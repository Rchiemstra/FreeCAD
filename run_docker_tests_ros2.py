#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the ROS 2 test suite in Docker and write one timestamped log."""

from __future__ import annotations

import argparse
from collections.abc import Iterable, Iterator
import datetime as dt
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys


DEFAULT_IMAGE = "ubuntu:noble"
CONTAINER_SRC = "/ros2"
CONTAINER_WS = "/ros2-workspace"
WS_VOLUME = "ros2-linux-workspace"

# Lines matching these are high-volume low-signal noise.
_NOISE_LINE_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"^\[\s*\d+%\] "),            # CMake: [ 12%] Building ...
    re.compile(r"^\[\d+/\d+\] "),            # Ninja: [42/1203] ...
    re.compile(
        r"^(Selecting previously unselected package|Unpacking |Setting up |Preparing to unpack )"
    ),
    re.compile(
        r"^(Reading package lists|Building dependency tree|Reading state information)\.{0,3}\s*$"
    ),
    re.compile(r"^(Get:\d|Ign:\d|Hit:\d|Fetched \d)"),  # apt-get update lines
    re.compile(r".*\bDownloading\b.*\b(KiB|MiB|GiB)\b"),
    re.compile(r"^\s*Downloaded\s"),
    re.compile(r"^\(Reading database"),
    re.compile(r"^#[-# ]{3,}"),
    # GTest assertion value context (repeat many times per failing test run)
    re.compile(r"\bevaluates to\b"),
    re.compile(r"\bWhich is:\s"),
    re.compile(r"^The difference between\b"),
    re.compile(r"^Expected equality of these values:\s*$"),
    re.compile(r",\s*where\s*$"),
    re.compile(r"^\s*\d+\s*$"),              # standalone integers (loop counters)
    # dpkg/debconf install noise
    re.compile(r"^Adding 'diversion of "),
    re.compile(r"^debconf:\s"),
    re.compile(r"^update-alternatives:\s"),
    re.compile(r"^invoke-rc\.d:\s"),
    # vcs clone progress (high volume during import)
    re.compile(r"^Cloning into '"),
    re.compile(r"^remote:\s"),
    re.compile(r"^Resolving deltas:\s"),
    re.compile(r"^Receiving objects:\s"),
    re.compile(r"^Compressing objects:\s"),
    re.compile(r"^Counting objects:\s"),
    # colcon progress timestamps: "[X.XXXs] ..." (informational status lines)
    re.compile(r"^\[\d+\.\d+s\]\s"),
    # rosdep update noise
    re.compile(r"^reading in sources list data from "),
    re.compile(r"^Hit\s"),
    re.compile(r"^Query rosdep"),
    # colcon/cmake configure spam: "-- ..." lines (not errors/warnings)
    re.compile(r"^-- (?!.*(?:fail|error|warn))", re.IGNORECASE),
    # GTest per-test passed/run lines — keep only failures
    re.compile(r"^\[ RUN\s+\]"),
    re.compile(r"^\[  PASSED  \]"),
    re.compile(r"^\[       OK \]"),
    re.compile(r"^\[----------\]"),
    re.compile(r"^\[==========\]"),
    re.compile(r"^\[ -------- \]"),
    # pytest passed/skipped per-test lines
    re.compile(r"\bPASSED\b"),
    re.compile(r"\bSKIPPED\b"),
    re.compile(r"\bxfail\b"),
    re.compile(r"\bxpass\b"),
)

_CTEST_START_LINE_RE = re.compile(r"^\s+Start\s+\d+:")
_CTEST_PASSED_RESULT_RE = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:")

_CMAKE_WARNING_PREFIXES: tuple[str, ...] = ("CMake Warning", "CMake Deprecation Warning")

_TIMESTAMP_RE = re.compile(r"^\(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+\)\s+")
_DEDUP_ALWAYS_EMIT = 2
_DEDUP_GLOBAL_CAP = 8

_FORCE_KEEP_SUBSTR: tuple[str, ...] = (
    "Required phase failed",
    "Test phase passed",
    "Test phase failed",
    "Install phase passed",
    "Install phase failed",
    "All Docker test phases passed",
    "One or more Docker test phases failed",
    "Traceback (most recent call last):",
    "The following tests FAILED",
    "Errors while running CTest",
    "Total Test time (real)",
    "System exit",
    # colcon test-result markers
    "had stderr output",
    "packages had stderr",
    "Summary:",
)

_FORCE_KEEP_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"========== .+ =========="),
    re.compile(r"(?i)\b(assertionerror|segmentation fault|core dumped)\b"),
    re.compile(r"(?i)^\s*\d+% tests passed"),
    re.compile(r"(?i)^\s*\d+% tests failed"),
    re.compile(r"^FAILED\b"),
    # colcon test-result summary lines
    re.compile(r"^\d+ packages? (had|finished)"),
    re.compile(r"^\s*\[  FAILED  \]"),        # GTest failure summary
    re.compile(r"\bFAILED\b.*\(Failed\)"),    # colcon/CTest failure list
)


def _keep_for_signal_hint(line: str) -> bool:
    """True when the line likely reports an actionable error or failure."""
    if re.match(r'^\s*File "', line):
        return True
    if re.search(r"\b[A-Za-z_][A-Za-z0-9_]*Error:", line):
        return True
    if re.search(r"(?i)(^|\s)(error:|fatal error:|\bfailed\b)", line):
        return True
    if re.search(r"(?i)\((?:Failed|Timeout)\)", line):
        return True
    if re.search(r"(?i)\*\*\*.*\b(?:failed|timeout)\b", line):
        return True
    if re.search(r"(?i) \.\.\. (fail|error)\s*$", line):
        return True
    if re.search(
        r"(?i)\b(traceback|exception\b|assertionerror|segmentation fault|core dumped)\b",
        line,
    ):
        return True
    return False


def trim_trailing_spaces_before_newline(line: str) -> str:
    if line.endswith("\r\n"):
        return line[:-2].rstrip(" \t") + "\r\n"
    if line.endswith("\n"):
        return line[:-1].rstrip(" \t") + "\n"
    return line.rstrip(" \t")


def keep_log_line(line: str) -> bool:
    """Return False if *line* is mostly noise."""
    # tier 1: force-keep
    if any(s in line for s in _FORCE_KEEP_SUBSTR):
        return True
    if any(r.search(line) for r in _FORCE_KEEP_RES):
        return True

    # tier 2: signal-hint
    if _keep_for_signal_hint(line):
        return True

    # tier 3: CTest per-test chatter
    if _CTEST_START_LINE_RE.match(line):
        return False
    if _CTEST_PASSED_RESULT_RE.search(line) and re.search(r"(?i)\bpassed\b", line):
        return False
    if _CTEST_PASSED_RESULT_RE.search(line) and re.search(r"\*\*\*(Skipped|Not Run)", line):
        return False

    # tier 4: high-volume regex noise
    if any(r.search(line) for r in _NOISE_LINE_RES):
        return False

    # tier 5: cmake configure spam
    stripped = line.lstrip()
    if stripped.startswith("--"):
        if not re.search(r"(?i)(fail|error|warn)", stripped):
            return False

    # tier 6: compiler source-context lines
    if re.match(r"^\s*\d+ \|", line):
        return False
    if re.match(r"^\s+\|\s+\^", line):
        return False
    if re.match(r"^\s+\|\s*$", line):
        return False
    if re.match(r"^\d+ warnings? generated\.?\s*$", line, re.IGNORECASE):
        return False

    # tier 7: dot-only progress and standalone "ok"
    if re.fullmatch(r"\.+", line.strip()):
        return False
    if re.fullmatch(r"ok\s*", line.strip()):
        return False

    # tier 8: progress lines with ASCII ellipsis
    if "..." in line and not _keep_for_signal_hint(line):
        return False

    # tier 9: blank separator lines
    if re.match(r"^\s*=+\s*$", line):
        return False

    return True


class _FilteredLogPipeline:
    """Streaming filter state (CMake warning blocks, blank collapse, deduplication)."""

    __slots__ = (
        "_skipping_cmake_warning",
        "_skipping_did_not_run",
        "_skipping_bash_script",
        "_prev_blank",
        "_dedup_key",
        "_dedup_count",
        "_global_seen",
    )

    def __init__(self) -> None:
        self._skipping_cmake_warning = False
        self._skipping_did_not_run = False
        self._skipping_bash_script = False
        self._prev_blank = False
        self._dedup_key: str = ""
        self._dedup_count: int = 0
        self._global_seen: dict[str, int] = {}

    def flush_dedup(self) -> str:
        held = self._dedup_count - _DEDUP_ALWAYS_EMIT
        if held > 0:
            self._dedup_count = 0
            return f"    ... ({held} more identical lines omitted)\n"
        return ""

    def feed(self, raw_line: str) -> str | None:
        """Return text to append to a filtered log/console stream, or None to omit."""
        if not self._skipping_bash_script and raw_line.startswith("Command:") and "bash -lc '" in raw_line:
            self._skipping_bash_script = True
            return None
        if self._skipping_bash_script:
            if raw_line.rstrip("\r\n") == "'":
                self._skipping_bash_script = False
            return None

        if self._skipping_cmake_warning:
            sl = raw_line.strip()
            if sl.startswith("CMake Error"):
                self._skipping_cmake_warning = False
            elif re.match(r"^\s*=+\s*$", raw_line) or "==========" in raw_line or raw_line.startswith("--"):
                self._skipping_cmake_warning = False
            else:
                return None

        if self._skipping_did_not_run:
            if re.match(r"^\s+\d+\s+-\s+", raw_line):
                return None
            if not raw_line.strip():
                return None
            self._skipping_did_not_run = False

        sl = raw_line.strip()
        if any(sl.startswith(p) for p in _CMAKE_WARNING_PREFIXES) and "CMake Error" not in raw_line:
            self._skipping_cmake_warning = True
            return None

        if sl == "The following tests did not run:":
            self._skipping_did_not_run = True
            return None

        if not keep_log_line(raw_line):
            return None

        out = trim_trailing_spaces_before_newline(raw_line)

        key = _TIMESTAMP_RE.sub("", out).strip()
        prefix = ""
        if key:
            if key == self._dedup_key:
                self._dedup_count += 1
                if self._dedup_count > _DEDUP_ALWAYS_EMIT:
                    return None
            else:
                held = self._dedup_count - _DEDUP_ALWAYS_EMIT
                if held > 0:
                    prefix = f"    ... ({held} more identical lines omitted)\n"
                self._dedup_key = key
                self._dedup_count = 0

            emitted = self._global_seen.get(key, 0)
            if emitted >= _DEDUP_GLOBAL_CAP:
                return None
            self._global_seen[key] = emitted + 1

        if out.strip() == "":
            if self._prev_blank:
                return None
            self._prev_blank = True
        else:
            self._prev_blank = False
        return prefix + out if prefix else out


def filter_log_lines(lines: Iterable[str]) -> Iterator[str]:
    pipe = _FilteredLogPipeline()
    for raw_line in lines:
        out = pipe.feed(raw_line)
        if out is not None:
            yield out
    note = pipe.flush_dedup()
    if note:
        yield note


def filter_log(text: str) -> str:
    return "".join(filter_log_lines(text.splitlines(True)))


def find_ros2_root(start: Path) -> tuple[Path, Path]:
    """Return (repo_root, ros2_src) by walking up from *start*."""
    for candidate in (start, *start.parents):
        ros2 = candidate / "src" / "3rdParty" / "ros2"
        if (candidate / "pixi.toml").is_file() or (candidate / ".gitmodules").is_file():
            return candidate, ros2
    raise RuntimeError("Could not find the FreeCAD repository root (expected pixi.toml or .gitmodules).")


def make_seed(seed: str | None) -> str:
    if seed is None:
        return f"{random.SystemRandom().randrange(0, 0xFFFFFFFF):08x}"
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", seed):
        raise ValueError("Seed may only contain letters, numbers, underscore, dot, or dash.")
    return seed


def make_log_path(repo_root: Path, log_dir: str, seed: str) -> Path:
    now = dt.datetime.now()
    log_root = Path(log_dir)
    if not log_root.is_absolute():
        log_root = repo_root / log_root
    log_root.mkdir(parents=True, exist_ok=True)
    return log_root / f"{now:%H%M%S_%Y%m%d}_{seed}_ros2.log"


def container_script(build_type: str, packages: list[str] | None = None) -> str:
    packages_select = ""
    packages_label = ""
    if packages:
        pkg_list = " ".join(packages)
        packages_select = f" --packages-select {pkg_list}"
        packages_label = f" [packages: {pkg_list}]"
    return f"""set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export ROS_DISTRO=rolling

overall_status=0

section() {{
    printf '\\n========== %s ==========\\n' "$1"
}}

run_required() {{
    section "$1"
    shift
    "$@"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '\\nRequired phase failed with exit code %s\\n' "$rc"
        exit "$rc"
    fi
}}

run_test() {{
    section "$1"
    shift
    "$@"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '\\nTest phase failed with exit code %s\\n' "$rc"
        if [ "$overall_status" -eq 0 ]; then
            overall_status="$rc"
        fi
    else
        printf '\\nTest phase passed\\n'
    fi
}}

section "Environment"
uname -a
cmake --version || true
echo "Build type: {build_type}"

run_required "Set up ROS 2 apt repository" bash -c '
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends curl gnupg lsb-release ca-certificates
    curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.asc \\
        | gpg --dearmor -o /usr/share/keyrings/ros-archive-keyring.gpg
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \\
        http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" \\
        > /etc/apt/sources.list.d/ros2.list
    apt-get update -qq
'

run_required "Install build dependencies" bash -c '
    apt-get install -y -qq --no-install-recommends \\
        cmake make ninja-build gcc g++ git python3-pip \\
        python3-colcon-common-extensions \\
        python3-rosdep python3-vcstool \\
        libasio-dev libtinyxml2-dev libssl-dev
'

run_required "Import ROS 2 source tree" bash -c '
    mkdir -p {CONTAINER_WS}/src
    if [[ ! -f {CONTAINER_WS}/src/.repos-imported ]]; then
        vcs import {CONTAINER_WS}/src < {CONTAINER_SRC}/ros2.repos
        touch {CONTAINER_WS}/src/.repos-imported
    else
        echo "Source tree already imported; skipping vcs import."
    fi
'

run_required "Install rosdep dependencies" bash -c '
    # Disable recommended/suggested packages to avoid pulling in systemd, Qt5,
    # X11 and other GUI stacks that are not needed for a headless build/test and
    # whose post-install scripts can kill the running bash process inside Docker.
    printf "APT::Install-Recommends \\"false\\";\\nAPT::Install-Suggests \\"false\\";\\n" \
        > /etc/apt/apt.conf.d/99-no-recommends
    rosdep init 2>/dev/null || true
    rosdep update --rosdistro rolling
    rosdep install --from-paths {CONTAINER_WS}/src --ignore-src -y \\
        --rosdistro rolling \\
        --skip-keys "fastcdr urdfdom_headers rpyutils \\
            rti-connext-dds-6.0.1 rti-connext-dds-7.7.0 \\
            connext_cmake_module rti_connext_dds_cmake_module \\
            rviz2 rviz_rendering rviz_default_plugins rviz_ogre_vendor \\
            rviz_visual_testing_framework \\
            rqt rqt_gui rqt_gui_cpp rqt_gui_py \\
            rqt_action rqt_bag rqt_bag_plugins rqt_console rqt_graph \\
            rqt_image_view rqt_msg rqt_plot rqt_reconfigure rqt_service_caller \\
            rqt_shell rqt_srv rqt_tf_tree rqt_topic \\
            qt_gui_cpp qt_gui_core \\
            libogre-1.12-dev"
'

# rpyutils has no noble system package (rosdep skips it) and was not cloned
# during the initial vcs import (the src/.repos-imported sentinel prevents
# re-import on subsequent runs).  Fix by cloning it explicitly, then copying
# the Python package into /usr/lib/python3/dist-packages/ — that path is
# unconditionally in sys.path for every python3 process, including cmake
# custom-command subprocesses that ignore PYTHONPATH overrides from the shell.
run_required "Bootstrap rpyutils from source" bash -c '
    [ -d {CONTAINER_WS}/src/ros2/rpyutils/.git ] || \\
        git clone -b rolling https://github.com/ros2/rpyutils.git \\
                  {CONTAINER_WS}/src/ros2/rpyutils
    colcon build \\
        --base-paths {CONTAINER_WS} \\
        --build-base {CONTAINER_WS}/build \\
        --install-base {CONTAINER_WS}/install \\
        --symlink-install \\
        --packages-select rpyutils \\
        --event-handlers console_cohesion+
    RPYUTILS_SRC="{CONTAINER_WS}/src/ros2/rpyutils/rpyutils"
    RPYUTILS_DST="/usr/lib/python3/dist-packages/rpyutils"
    [ -d "$RPYUTILS_DST" ] || cp -r "$RPYUTILS_SRC" "$RPYUTILS_DST"
    python3 -c "from rpyutils import add_dll_directories_from_env" && echo "rpyutils OK"
    rm -f  {CONTAINER_WS}/build/rosidl_generator_py/CMakeCache.txt
    rm -rf {CONTAINER_WS}/build/rosidl_generator_py/CMakeFiles
'

run_required "Build ROS 2{packages_label}" bash -c '
    colcon build \\
        --base-paths {CONTAINER_WS} \\
        --build-base {CONTAINER_WS}/build \\
        --install-base {CONTAINER_WS}/install \\
        --symlink-install \\
        --cmake-args -DCMAKE_BUILD_TYPE={build_type} \\
        --packages-skip rmw_connextdds connext_cmake_module rti_connext_dds_cmake_module \\
            qt_gui_cpp qt_gui_core \\
            rviz2 rviz_rendering rviz_default_plugins rviz_ogre_vendor \\
            rviz_visual_testing_framework \\
            rqt rqt_gui rqt_gui_cpp rqt_gui_py \\
            rqt_action rqt_bag rqt_bag_plugins rqt_console rqt_graph \\
            rqt_image_view rqt_msg rqt_plot rqt_reconfigure rqt_service_caller \\
            rqt_shell rqt_srv rqt_tf_tree rqt_topic{packages_select} \\
        --event-handlers console_cohesion+
'

run_test "ROS 2 colcon test suite{packages_label}" bash -c '
    source {CONTAINER_WS}/install/setup.bash
    colcon test \\
        --base-paths {CONTAINER_WS} \\
        --build-base {CONTAINER_WS}/build \\
        --install-base {CONTAINER_WS}/install \\
        --packages-skip rmw_connextdds connext_cmake_module rti_connext_dds_cmake_module \\
            qt_gui_cpp qt_gui_core \\
            rviz2 rviz_rendering rviz_default_plugins rviz_ogre_vendor \\
            rviz_visual_testing_framework \\
            rqt rqt_gui rqt_gui_cpp rqt_gui_py \\
            rqt_action rqt_bag rqt_bag_plugins rqt_console rqt_graph \\
            rqt_image_view rqt_msg rqt_plot rqt_reconfigure rqt_service_caller \\
            rqt_shell rqt_srv rqt_tf_tree rqt_topic{packages_select} \\
        --event-handlers console_cohesion+
    colcon test-result \\
        --base-paths {CONTAINER_WS} \\
        --test-result-base {CONTAINER_WS}/build \\
        --verbose
'

section "Result"
if [ "$overall_status" -eq 0 ]; then
    printf 'All Docker test phases passed\\n'
else
    printf 'One or more Docker test phases failed; first failure exit code: %s\\n' "$overall_status"
fi
exit "$overall_status"
"""


def docker_command(args: argparse.Namespace, ros2_src: Path, seed: str) -> list[str]:
    return [
        "docker",
        "run",
        "--rm",
        "--name",
        f"ros2-tests-{seed.lower()}",
        "--workdir",
        CONTAINER_WS,
        "--mount",
        f"type=bind,source={ros2_src},target={CONTAINER_SRC}",
        "--mount",
        f"type=volume,source={WS_VOLUME},target={CONTAINER_WS}",
        args.image,
        "bash",
        "-c",
        container_script(args.build_type, args.packages or None),
    ]


def run_and_log(command: list[str], log_path: Path, ros2_src: Path, args: argparse.Namespace) -> int:
    filtered_path = (
        log_path.with_name(f"{log_path.stem}.filtered{log_path.suffix}") if args.filtered_log else None
    )

    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"Started: {dt.datetime.now().isoformat(timespec='seconds')}\n")
        log.write(f"ROS 2 source: {ros2_src}\n")
        log.write(f"Docker image: {args.image}\n")
        log.write(f"Build type: {args.build_type}\n")
        log.write(f"Command: {shlex.join(command)}\n\n")
        log.flush()

        filtered_file = None
        if filtered_path is not None:
            filtered_file = filtered_path.open("w", encoding="utf-8", errors="replace")
            filtered_file.write(
                f"# Noise-reduced copy of Docker output. Full log: {log_path.name}\n"
                f"# Generated: {dt.datetime.now().isoformat(timespec='seconds')}\n\n"
            )
            filtered_file.flush()

        process = subprocess.Popen(
            command,
            cwd=str(ros2_src.parent.parent.parent),  # repo root
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

        assert process.stdout is not None
        pipe: _FilteredLogPipeline | None = (
            None if args.full_console and filtered_file is None else _FilteredLogPipeline()
        )
        try:
            for line in process.stdout:
                log.write(line)
                log.flush()
                filtered_chunk = pipe.feed(line) if pipe is not None else None
                if filtered_file is not None and filtered_chunk is not None:
                    filtered_file.write(filtered_chunk)
                    filtered_file.flush()
                if args.full_console:
                    print(line, end="")
                elif filtered_chunk is not None:
                    print(filtered_chunk, end="")
        except KeyboardInterrupt:
            process.terminate()
            log.write("\nInterrupted by user; terminated Docker process.\n")
            log.flush()
            if filtered_file is not None:
                filtered_file.write("\nInterrupted by user; terminated Docker process.\n")
                filtered_file.flush()
            return 130
        finally:
            if filtered_file is not None:
                filtered_file.close()

        rc = process.wait()
        if filtered_path is not None:
            print(f"Noise-reduced log: {filtered_path}", file=sys.stderr)
        return rc


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build and test ROS 2 in Docker (Ubuntu Noble + ROS 2 apt packages) "
            "and save output to .log/<TIME_DATE_SEED>_ros2.log."
        )
    )
    parser.add_argument(
        "--image",
        default=DEFAULT_IMAGE,
        help=f"Docker image to use. Default: {DEFAULT_IMAGE}",
    )
    parser.add_argument(
        "--build-type",
        choices=("RelWithDebInfo", "Release", "Debug"),
        default="RelWithDebInfo",
        help="CMake build type. Default: RelWithDebInfo",
    )
    parser.add_argument(
        "--log-dir",
        default=".log",
        help="Directory for generated log files, relative to the repo root unless absolute. Default: .log",
    )
    parser.add_argument(
        "--seed",
        help="Optional filename/container seed. Defaults to a random 8-digit hex value.",
    )
    parser.add_argument(
        "--full-console",
        action="store_true",
        help="Print the complete Docker stream to the terminal (default: filtered output only).",
    )
    parser.add_argument(
        "--filtered-log",
        action="store_true",
        help="Also write <stem>.filtered.log next to the main log.",
    )
    parser.add_argument(
        "--packages",
        nargs="+",
        metavar="PACKAGE",
        help=(
            "Optional list of colcon package names to build and test "
            "(e.g. rclcpp rcl). Builds and tests all packages when omitted."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if shutil.which("docker") is None:
        print("docker was not found on PATH.", file=sys.stderr)
        return 127

    repo_root, ros2_src = find_ros2_root(Path(__file__).resolve())

    if not (ros2_src / "ros2.repos").is_file():
        print(
            f"ros2 submodule not initialised at {ros2_src}.\n"
            "Run:  git submodule update --init --recursive src/3rdParty/ros2",
            file=sys.stderr,
        )
        return 1

    seed = make_seed(args.seed)
    log_path = make_log_path(repo_root, args.log_dir, seed)
    command = docker_command(args, ros2_src, seed)

    print(f"Writing ROS 2 Docker test output to {log_path}")
    print(f"Using Docker image: {args.image}")
    print(f"ROS 2 source: {ros2_src}")
    return run_and_log(command, log_path, ros2_src, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
