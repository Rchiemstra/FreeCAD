#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the gz-sim test suite in Docker and write one timestamped log."""

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
CONTAINER_SRC = "/gz-sim"
CONTAINER_BUILD = "/gz-sim-build"
BUILD_VOLUME = "gz-sim-linux-build"

# Lines matching these are high-volume low-signal noise.
_NOISE_LINE_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"^\[\s*\d+%\] "),           # CMake: [ 12%] Building ...
    re.compile(r"^\[\d+/\d+\] "),           # Ninja: [42/1203] ...
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
    # GTest assertion value context (repeat thousands of times per failing test run)
    re.compile(r"\bevaluates to\b"),                   # "X evaluates to Y"
    re.compile(r"\bWhich is:\s"),                      # "  Which is: 0"
    re.compile(r"^The difference between\b"),           # GTest NEAR assertion header
    re.compile(r"^Expected equality of these values:\s*$"),
    re.compile(r",\s*where\s*$"),                      # word-wrapped GTest continuation
    # Standalone integers (GTest loop iteration counters)
    re.compile(r"^\s*\d+\s*$"),
    # dpkg/debconf install noise
    re.compile(r"^Adding 'diversion of "),
    re.compile(r"^debconf:\s"),
    re.compile(r"^update-alternatives:\s"),
    re.compile(r"^invoke-rc\.d:\s"),
    # gz-sim [info]/[debug] runtime telemetry — keep [error]/[warn] for signal
    re.compile(r"\)\s+\[(?:info|debug)\]\s+\["),
    # xkbcomp keysym warnings from Xvfb startup — not fatal, not actionable
    re.compile(r"^The XKEYBOARD keymap compiler \(xkbcomp\) reports:"),
    re.compile(r"^> Warning:\s+Could not resolve keysym XF86"),
    re.compile(r"^Errors from xkbcomp are not fatal to the X server"),
    # Qt / XDG runtime dir note — informational only (may appear bare or after a timestamp)
    re.compile(r"QStandardPaths: XDG_RUNTIME_DIR not set"),
)

_CTEST_START_LINE_RE = re.compile(r"^\s+Start\s+\d+:")
_CTEST_PASSED_RESULT_RE = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:")

_CMAKE_WARNING_PREFIXES: tuple[str, ...] = ("CMake Warning", "CMake Deprecation Warning")

# Deduplication: strip leading timestamp before comparing lines for identity.
_TIMESTAMP_RE = re.compile(r"^\(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+\)\s+")
_DEDUP_ALWAYS_EMIT = 2   # emit first N adjacent identical occurrences; suppress the rest
_DEDUP_GLOBAL_CAP = 8    # total number of times any unique line may be emitted

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
)

_FORCE_KEEP_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"========== .+ =========="),
    re.compile(r"(?i)\b(assertionerror|segmentation fault|core dumped)\b"),
    re.compile(r"(?i)^\s*\d+% tests passed"),
    re.compile(r"(?i)^\s*\d+% tests failed"),
    re.compile(r"^FAILED\b"),
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

    # tier 6: compiler source-context lines (caret markers, blank pipe lines)
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
        """Return a trailing dedup note if lines were suppressed at the end of input."""
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

        # Deduplicate: normalize away timestamps, then apply adjacent + global caps.
        key = _TIMESTAMP_RE.sub("", out).strip()
        prefix = ""
        if key:
            # Adjacent run dedup: emit first N, suppress the rest with a note.
            if key == self._dedup_key:
                self._dedup_count += 1
                if self._dedup_count > _DEDUP_ALWAYS_EMIT:
                    return None  # suppress; note emitted when key changes
            else:
                held = self._dedup_count - _DEDUP_ALWAYS_EMIT
                if held > 0:
                    prefix = f"    ... ({held} more identical lines omitted)\n"
                self._dedup_key = key
                self._dedup_count = 0

            # Global cap: suppress after _DEDUP_GLOBAL_CAP total emissions.
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


def find_gz_sim_root(start: Path) -> tuple[Path, Path]:
    """Return (repo_root, gz_sim_src) by walking up from *start*."""
    for candidate in (start, *start.parents):
        gz_sim = candidate / "src" / "3rdParty" / "gz-sim"
        if (candidate / "pixi.toml").is_file() or (candidate / ".gitmodules").is_file():
            return candidate, gz_sim
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
    return log_root / f"{now:%H%M%S_%Y%m%d}_{seed}_gz-sim.log"


_TEST_SUITE_REGEX: dict[str, str] = {
    "UNIT": "^UNIT_",
    "INTEGRATION": "^INTEGRATION_",
}


def container_script(build_type: str, test_suite: str = "ALL",
                     test_numbers: list[int] | None = None) -> str:
    ctest_filter = ""
    if test_suite in _TEST_SUITE_REGEX:
        ctest_filter = f' -R "{_TEST_SUITE_REGEX[test_suite]}"'
    if test_numbers:
        # -I ,,,N1,N2,N3 runs only tests with those global CTest numbers
        ctest_filter += " -I ,,," + ",".join(str(n) for n in test_numbers)
    suite_label = f" ({test_suite})" if test_suite != "ALL" else ""
    if test_numbers:
        suite_label += f" [#{','.join(str(n) for n in test_numbers)}]"
    return f"""set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export DISPLAY=:99
export MESA_GL_VERSION_OVERRIDE=3.3
export RENDER_ENGINE_VALUES=ogre2
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

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

run_required "Set up OSRF apt repository" bash -c '
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends curl lsb-release gnupg ca-certificates
    curl -fsSL https://packages.osrfoundation.org/gazebo.gpg \
        -o /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] \
        http://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/gazebo-stable.list
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] \
        http://packages.osrfoundation.org/gazebo/ubuntu-nightly $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/gazebo-nightly.list
    apt-get update -qq
'

run_required "Install build dependencies" bash -c '
    apt-get install -y -qq --no-install-recommends \
        cmake ninja-build gcc g++ git \
        binutils-dev freeglut3-dev libbenchmark-dev libdwarf-dev libdw-dev \
        libfreeimage-dev libglew-dev \
        libgz-rotary-cmake-dev libgz-rotary-common-dev libgz-rotary-fuel-tools-dev \
        libgz-rotary-gui-dev libgz-rotary-math-eigen3-dev libgz-rotary-msgs-dev \
        libgz-rotary-physics-dev libgz-rotary-plugin-dev libgz-rotary-rendering-dev \
        libgz-rotary-sensors-dev libgz-rotary-tools-dev libgz-rotary-transport-dev \
        libgz-rotary-utils-cli-dev libogre-1.9-dev libogre-next-2.3-dev \
        libprotobuf-dev libprotoc-dev libgz-rotary-sdformat-dev libtinyxml2-dev \
        libwebsockets-dev libxi-dev libxmu-dev libpython3-dev \
        python3-gz-rotary-math python3-gz-rotary-msgs python3-gz-rotary-transport \
        python3-pybind11 python3-pytest python3-gz-rotary-sdformat \
        qml6-module-qt-labs-folderlistmodel qml6-module-qt-labs-settings \
        qml6-module-qt5compat-graphicaleffects qml6-module-qtqml-models \
        qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
        qml6-module-qtquick-layouts qml6-module-qtquick \
        qt6-5compat-dev qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
        uuid-dev xvfb x11-utils mesa-utils
'

run_required "Configure gz-sim" bash -c '
    mkdir -p {CONTAINER_BUILD}
    cmake -S {CONTAINER_SRC} -B {CONTAINER_BUILD} \
        -G Ninja \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_BUILD_TYPE={build_type} \
        -DBUILD_DOCS=OFF \
        -DSKIP_PYBIND11=ON
'

run_required "Build gz-sim" cmake --build {CONTAINER_BUILD} -- -j$(nproc)

run_required "Install gz-sim" cmake --install {CONTAINER_BUILD}

run_required "Fix line endings in shell scripts and CSV files" bash -c '
    find {CONTAINER_SRC}/src/cmd -name "*.sh" -print0 | xargs -0 -r sed -i "s/\\r//"
    find {CONTAINER_SRC}/test/worlds -name "*.csv" -print0 | xargs -0 -r sed -i "s/\\r//; s/^\\xef\\xbb\\xbf//"
'

run_required "Deploy test plugins" bash -c '
    gz_plugin_dir=$(find /usr/lib -type d -name "plugins" 2>/dev/null | grep "gz-sim" | head -1)
    if [ -z "$gz_plugin_dir" ]; then
        printf "WARNING: Could not find installed gz-sim plugin directory\\n"
        exit 0
    fi
    printf "Deploying test plugins to: %s\\n" "$gz_plugin_dir"

    # Deploy all NON-MockSystem test plugins to the install dir.
    # SimulationRunner_TEST (no InternalFixture) finds plugins only via the install dir.
    # MockSystem is intentionally excluded: SystemLoader_TEST.FromPluginPathEnv
    # verifies that MockSystem is NOT findable from the default install path.
    find {CONTAINER_BUILD} -maxdepth 6 \\( \\
        -name "libTestModelSystem.so"    -o \\
        -name "libTestSensorSystem.so"   -o \\
        -name "libTestWorldSystem.so"    -o \\
        -name "libTestVisualSystem.so"   -o \\
        -name "libEventTriggerSystem.so" -o \\
        -name "libNullSystem.so"         -o \\
        -name "libTestSystem.so"         \\
    \\) -exec cp {{}} "$gz_plugin_dir/" \\;

    # InternalFixture::SetUp() (used by Server_TEST) sets GZ_SIM_SYSTEM_PLUGIN_PATH
    # to GZ_SIM_TEST_SYSTEM_PLUGIN_PATH, which is baked at cmake-configure time as
    #   ${{CMAKE_BINARY_DIR}}/${{GZ_LIB_INSTALL_DIR}}/gz-sim/plugins:${{CMAKE_BINARY_DIR}}/${{GZ_LIB_INSTALL_DIR}}
    # On multiarch Linux GZ_LIB_INSTALL_DIR is e.g. "lib/x86_64-linux-gnu", whereas
    # CMAKE_LIBRARY_OUTPUT_DIRECTORY is just "lib/" (no multiarch subdir).
    # Bridge the gap by copying all test plugins into the multiarch build lib dir so
    # that InternalFixture can find MockSystem via GZ_SIM_TEST_SYSTEM_PLUGIN_PATH.
    gz_lib_rel=$(printf "%s" "$gz_plugin_dir" | sed "s|^/usr/||; s|/gz-[^/]*/plugins.*||")
    build_lib_dir="{CONTAINER_BUILD}/$gz_lib_rel"
    mkdir -p "$build_lib_dir"
    find {CONTAINER_BUILD}/lib -maxdepth 1 \\( \\
        -name "libTestModelSystem.so"    -o \\
        -name "libTestSensorSystem.so"   -o \\
        -name "libTestWorldSystem.so"    -o \\
        -name "libTestVisualSystem.so"   -o \\
        -name "libMockSystem.so"         -o \\
        -name "libEventTriggerSystem.so" -o \\
        -name "libNullSystem.so"         -o \\
        -name "libTestSystem.so"         \\
    \\) -exec cp {{}} "$build_lib_dir/" \\;
    printf "Test plugins staged (install: %s, build-lib: %s)\\n" "$gz_plugin_dir" "$build_lib_dir"
'

section "Start Xvfb for rendering tests"
Xvfb :99 -screen 0 1280x1024x24 &
sleep 1

run_test "gz-sim CTest suite{suite_label}" \
    ctest --test-dir {CONTAINER_BUILD} --output-on-failure{ctest_filter}

section "Result"
if [ "$overall_status" -eq 0 ]; then
    printf 'All Docker test phases passed\\n'
else
    printf 'One or more Docker test phases failed; first failure exit code: %s\\n' "$overall_status"
fi
exit "$overall_status"
"""


def docker_command(args: argparse.Namespace, gz_sim_src: Path, seed: str) -> list[str]:
    return [
        "docker",
        "run",
        "--rm",
        "--name",
        f"gz-sim-tests-{seed.lower()}",
        "--workdir",
        CONTAINER_SRC,
        "--mount",
        f"type=bind,source={gz_sim_src},target={CONTAINER_SRC}",
        "--mount",
        f"type=volume,source={BUILD_VOLUME},target={CONTAINER_BUILD}",
        args.image,
        "bash",
        "-lc",
        container_script(args.build_type, args.test_suite, args.tests or None),
    ]


def run_and_log(command: list[str], log_path: Path, gz_sim_src: Path, args: argparse.Namespace) -> int:
    filtered_path = (
        log_path.with_name(f"{log_path.stem}.filtered{log_path.suffix}") if args.filtered_log else None
    )

    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"Started: {dt.datetime.now().isoformat(timespec='seconds')}\n")
        log.write(f"gz-sim source: {gz_sim_src}\n")
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
            cwd=str(gz_sim_src.parent.parent.parent),  # repo root
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
            "Build and test gz-sim in Docker (Ubuntu Noble + OSRF packages) "
            "and save output to .log/<TIME_DATE_SEED>_gz-sim.log."
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
        "--test-suite",
        choices=("ALL", "UNIT", "INTEGRATION"),
        default="ALL",
        help=(
            "CTest label group to run. "
            "UNIT runs tests matching '^UNIT_', "
            "INTEGRATION runs tests matching '^INTEGRATION_', "
            "ALL runs the full suite (default)."
        ),
    )
    parser.add_argument(
        "tests",
        nargs="*",
        type=int,
        metavar="TEST_NUMBER",
        help=(
            "Optional list of CTest test numbers to run (e.g. 129 131 135). "
            "Runs only the specified tests. Can be combined with --test-suite."
        ),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if shutil.which("docker") is None:
        print("docker was not found on PATH.", file=sys.stderr)
        return 127

    repo_root, gz_sim_src = find_gz_sim_root(Path(__file__).resolve())

    if not (gz_sim_src / "CMakeLists.txt").is_file():
        print(
            f"gz-sim submodule not initialised at {gz_sim_src}.\n"
            "Run:  git submodule update --init --recursive src/3rdParty/gz-sim",
            file=sys.stderr,
        )
        return 1

    seed = make_seed(args.seed)
    log_path = make_log_path(repo_root, args.log_dir, seed)
    command = docker_command(args, gz_sim_src, seed)

    print(f"Writing gz-sim Docker test output to {log_path}")
    print(f"Using Docker image: {args.image}")
    print(f"gz-sim source: {gz_sim_src}")
    return run_and_log(command, log_path, gz_sim_src, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
