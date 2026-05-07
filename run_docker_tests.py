#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the FreeCAD test suite in Docker and write one timestamped log."""

from __future__ import annotations

import argparse
import datetime as dt
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys


DEFAULT_IMAGE = "ghcr.io/prefix-dev/pixi:0.59.0"
CONTAINER_WORKDIR = "/workspace"
PIXI_VOLUME = "freecad-linux-pixi"
BUILD_VOLUME_PREFIX = "freecad-linux-build"


def find_repo_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / "pixi.toml").is_file() and (candidate / "CMakeLists.txt").is_file():
            return candidate
    raise RuntimeError("Could not find the FreeCAD repository root.")


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
    return log_root / f"{now:%H%M%S_%Y%m%d}_{seed}.log"


def container_script(config: str, include_install_gui: bool) -> str:
    build_dir = f"build/{config}"
    configure_task = f"configure-{config}"
    build_task = f"build-{config}"
    install_task = f"install-{config}"

    install_gui_step = ""
    if include_install_gui:
        install_gui_step = """
if [ "$install_status" -eq 0 ]; then
    run_test "FreeCAD GUI tests on install" \\
        pixi run /bin/bash .github/scripts/xvfb_run.sh -a -s "-screen 0 1024x768x24" -- \\
        python .github/scripts/run_gui_tests.py FreeCAD
fi
"""

    return f"""set -uo pipefail

export QTWEBENGINE_DISABLE_SANDBOX=1
export XDG_RUNTIME_DIR="${{XDG_RUNTIME_DIR:-/tmp/runtime-root}}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR" || true

# UTF-8 so Python unittest can print non-ASCII test descriptions (e.g. CAM docstrings with °).
export LANG=C.UTF-8
export LC_ALL=C.UTF-8
export PYTHONUTF8=1

overall_status=0

section() {{
    printf '\\n========== %s ==========' "$1"
    printf '\\n'
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
pwd
pixi --version
pixi run git config --global --add safe.directory {CONTAINER_WORKDIR} || true

# Host Xvfb from apt (conda sysroot Xvfb is EL7-linked and breaks on modern glibc/openssl).
if command -v apt-get >/dev/null 2>&1 && ! [ -x /usr/bin/Xvfb ]; then
    run_required "Install xvfb (apt)" bash -c 'export DEBIAN_FRONTEND=noninteractive && apt-get update -qq && apt-get install -y -qq xvfb'
fi

run_required "Initialize submodules" pixi run initialize
run_required "Configure {config}" pixi run {configure_task}
run_required "Build {config}" pixi run {build_task}

run_test "src/Tools Python tests" \\
    pixi run python -m unittest discover -s src/Tools/tests -p "test_*.py"

run_test "C++ CTest suite" \\
    pixi run ctest --test-dir {build_dir} --output-on-failure

run_test "FreeCAD CLI tests on build dir" \\
    pixi run {build_dir}/bin/FreeCADCmd -t 0

run_test "FreeCAD GUI tests on build dir" \\
    pixi run /bin/bash .github/scripts/xvfb_run.sh -a -s "-screen 0 1024x768x24" -- \\
    python .github/scripts/run_gui_tests.py {build_dir}

run_test "Coin node snapshot visual tests" \\
    pixi run env FC_VISUAL_OUT_DIR=/tmp/FreeCADTesting/CoinNodeSnapshots \\
    /bin/bash .github/scripts/xvfb_run.sh -a -s "-screen 0 1024x768x24" -- \\
    {build_dir}/bin/FreeCAD -t TestCoinNodeSnapshots

section "CMake install"
pixi run {install_task}
install_status=$?
if [ "$install_status" -ne 0 ]; then
    printf '\\nInstall phase failed with exit code %s\\n' "$install_status"
    if [ "$overall_status" -eq 0 ]; then
        overall_status="$install_status"
    fi
else
    printf '\\nInstall phase passed\\n'
fi

if [ "$install_status" -eq 0 ]; then
    run_test "FreeCAD CLI tests on install" pixi run FreeCADCmd -t 0
fi
{install_gui_step}
section "Result"
if [ "$overall_status" -eq 0 ]; then
    printf 'All Docker test phases passed\\n'
else
    printf 'One or more Docker test phases failed; first failure exit code: %s\\n' "$overall_status"
fi
exit "$overall_status"
"""


def docker_command(args: argparse.Namespace, repo_root: Path, seed: str) -> list[str]:
    command = [
        "docker",
        "run",
        "--rm",
        "--name",
        f"freecad-tests-{seed.lower()}",
        "--workdir",
        CONTAINER_WORKDIR,
        "--mount",
        f"type=bind,source={repo_root},target={CONTAINER_WORKDIR}",
        "--mount",
        f"type=volume,source={PIXI_VOLUME},target={CONTAINER_WORKDIR}/.pixi",
        "--mount",
        f"type=volume,source={BUILD_VOLUME_PREFIX}-{args.config},target={CONTAINER_WORKDIR}/build",
        args.image,
        "bash",
        "-lc",
        container_script(args.config, not args.skip_install_gui),
    ]
    return command


def run_and_log(command: list[str], log_path: Path, repo_root: Path, args: argparse.Namespace) -> int:
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"Started: {dt.datetime.now().isoformat(timespec='seconds')}\n")
        log.write(f"Repository: {repo_root}\n")
        log.write(f"Docker image: {args.image}\n")
        log.write(f"Config: {args.config}\n")
        log.write(f"Command: {shlex.join(command)}\n\n")
        log.flush()

        process = subprocess.Popen(
            command,
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

        assert process.stdout is not None
        try:
            for line in process.stdout:
                print(line, end="")
                log.write(line)
                log.flush()
        except KeyboardInterrupt:
            process.terminate()
            log.write("\nInterrupted by user; terminated Docker process.\n")
            log.flush()
            return 130

        return process.wait()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Execute the FreeCAD test suite in Docker and save output to .log/<TIME_DATE_SEED>.log."
    )
    parser.add_argument(
        "--image",
        default=DEFAULT_IMAGE,
        help=f"Docker image to use. Default: {DEFAULT_IMAGE}",
    )
    parser.add_argument(
        "--config",
        choices=("release", "debug"),
        default="release",
        help="Pixi/CMake build configuration to test. Default: release",
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
        "--skip-install-gui",
        action="store_true",
        help="Skip GUI tests against the installed FreeCAD binary.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if shutil.which("docker") is None:
        print("docker was not found on PATH.", file=sys.stderr)
        return 127

    repo_root = find_repo_root(Path(__file__).resolve())
    seed = make_seed(args.seed)
    log_path = make_log_path(repo_root, args.log_dir, seed)
    command = docker_command(args, repo_root, seed)

    print(f"Writing Docker test output to {log_path}")
    print(f"Using Docker image {args.image}")
    return run_and_log(command, log_path, repo_root, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
