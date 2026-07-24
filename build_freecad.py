#!/usr/bin/env python3
"""Small FreeCAD build wrapper for this checkout.

The script prefers Pixi because this repository already defines the FreeCAD
Conda toolchain there. It falls back to system CMake/Git when --no-pixi is
given, or when Pixi is not on PATH.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


def host_platform_name() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linux"
    raise SystemExit(f"Unsupported host platform: {sys.platform}")


def command_prefix(use_pixi: bool) -> list[str]:
    if not use_pixi:
        return []

    pixi = shutil.which("pixi")
    if pixi:
        return [pixi, "run"]

    print("pixi was not found on PATH; falling back to system tools.", file=sys.stderr)
    return []


def format_command(command: list[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(command)
    return shlex.join(command)


def run(command: list[str], *, cwd: Path, env: dict[str, str] | None, dry_run: bool) -> None:
    print(f"> {format_command(command)}", flush=True)
    if dry_run:
        return

    completed = subprocess.run(command, cwd=cwd, env=env)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def configure_environment() -> dict[str, str]:
    env = os.environ.copy()
    for name in ("CFLAGS", "CXXFLAGS", "DEBUG_CFLAGS", "DEBUG_CXXFLAGS"):
        env[name] = ""
    return env


def flatten_targets(targets: list[list[str]]) -> list[str]:
    return [target for group in targets for target in group]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Configure and build FreeCAD from this source checkout.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--config",
        choices=("release", "debug"),
        default="release",
        help="Build configuration to use.",
    )
    parser.add_argument(
        "--target",
        action="append",
        default=[],
        nargs="+",
        metavar="NAME",
        help="CMake target to build, for example SketcherGui. May be repeated.",
    )
    parser.add_argument(
        "-j",
        "--jobs",
        type=int,
        metavar="N",
        help="Maximum parallel build jobs.",
    )
    parser.add_argument(
        "--configure",
        action="store_true",
        help="Run CMake configure even when build/<config>/CMakeCache.txt exists.",
    )
    parser.add_argument(
        "--no-configure",
        action="store_true",
        help="Skip CMake configure. Fails if the build tree is not configured.",
    )
    parser.add_argument(
        "--cmake-arg",
        action="append",
        default=[],
        metavar="ARG",
        help="Extra CMake configure argument. Use --cmake-arg=-DNAME=VALUE.",
    )
    parser.add_argument(
        "--clean-first",
        action="store_true",
        help="Ask CMake to clean the requested target before building.",
    )
    parser.add_argument(
        "--test",
        action="store_true",
        help="Run ctest after building.",
    )
    parser.add_argument(
        "--test-filter",
        metavar="REGEX",
        help="Only run tests matching this ctest -R regular expression.",
    )
    parser.add_argument(
        "--ctest-arg",
        action="append",
        default=[],
        metavar="ARG",
        help="Extra ctest argument. Use --ctest-arg=--verbose for dashed values.",
    )
    parser.add_argument(
        "--install",
        action="store_true",
        help="Run cmake --install after building.",
    )
    parser.add_argument(
        "--skip-submodules",
        action="store_true",
        help="Skip git submodule update before configure.",
    )
    parser.add_argument(
        "--no-pixi",
        action="store_true",
        help="Use system Git/CMake/CTest instead of pixi run.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing them.",
    )
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    if args.configure and args.no_configure:
        parser.error("--configure and --no-configure cannot be used together")
    if args.cmake_arg and args.no_configure:
        parser.error("--cmake-arg requires configure; remove --no-configure")
    if (args.test_filter or args.ctest_arg) and not args.test:
        parser.error("--test-filter and --ctest-arg require --test")
    if args.jobs is not None and args.jobs < 1:
        parser.error("--jobs must be greater than zero")

    repo_root = Path(__file__).resolve().parent
    if not (repo_root / "CMakePresets.json").exists():
        raise SystemExit(f"CMakePresets.json was not found next to {__file__}")

    build_dir = Path("build") / args.config
    cache_file = repo_root / build_dir / "CMakeCache.txt"
    needs_configure = (
        args.configure
        or bool(args.cmake_arg)
        or (not args.no_configure and not cache_file.exists())
    )

    if args.no_configure and not cache_file.exists():
        raise SystemExit(
            f"{cache_file} does not exist. Run without --no-configure first."
        )

    prefix = command_prefix(not args.no_pixi)
    if not prefix and shutil.which("cmake") is None:
        raise SystemExit("cmake was not found on PATH. Install CMake or run with Pixi available.")

    configure_env = configure_environment()
    if needs_configure:
        if not args.skip_submodules:
            if not prefix and shutil.which("git") is None:
                raise SystemExit("git was not found on PATH. Install Git or use --skip-submodules.")
            run(
                prefix + ["git", "submodule", "update", "--init", "--recursive"],
                cwd=repo_root,
                env=configure_env,
                dry_run=args.dry_run,
            )

        preset = f"conda-{host_platform_name()}-{args.config}"
        configure_cmd = prefix + ["cmake", "--preset", preset]
        if sys.platform.startswith("win") and preset.startswith("conda-windows-"):
            configure_cmd.extend(["-DCMAKE_GENERATOR_PLATFORM=", "-DCMAKE_GENERATOR_TOOLSET="])
        configure_cmd.extend(args.cmake_arg)
        run(configure_cmd, cwd=repo_root, env=configure_env, dry_run=args.dry_run)

    targets = flatten_targets(args.target)
    build_cmd = prefix + ["cmake", "--build", str(build_dir)]
    if args.clean_first:
        build_cmd.append("--clean-first")
    if args.jobs:
        build_cmd.extend(["--parallel", str(args.jobs)])
    if targets:
        build_cmd.extend(["--target", *targets])
    run(build_cmd, cwd=repo_root, env=None, dry_run=args.dry_run)

    if args.test:
        ctest_cmd = prefix + ["ctest", "--test-dir", str(build_dir), "--output-on-failure"]
        if args.jobs:
            ctest_cmd.extend(["--parallel", str(args.jobs)])
        if args.test_filter:
            ctest_cmd.extend(["-R", args.test_filter])
        ctest_cmd.extend(args.ctest_arg)
        run(ctest_cmd, cwd=repo_root, env=None, dry_run=args.dry_run)

    if args.install:
        install_cmd = prefix + ["cmake", "--install", str(build_dir)]
        run(install_cmd, cwd=repo_root, env=None, dry_run=args.dry_run)

    if args.dry_run:
        print("Dry run finished.")
    else:
        print("Build script finished successfully.")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
