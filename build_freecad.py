#!/usr/bin/env python3
"""Small FreeCAD build wrapper for this checkout.

The script prefers Pixi because this repository already defines the FreeCAD
Conda toolchain there. It upgrades an outdated Pixi executable to the minimum
declared by the repository, then synchronizes the environment before building.
It falls back to system CMake/Git when --no-pixi is given, or when Pixi is not
on PATH.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
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


def run(
    command: list[str],
    *,
    cwd: Path,
    env: dict[str, str] | None,
    dry_run: bool,
    retries: int = 0,
) -> None:
    print(f"> {format_command(command)}", flush=True)
    if dry_run:
        return

    for attempt in range(retries + 1):
        completed = subprocess.run(command, cwd=cwd, env=env)
        if completed.returncode == 0:
            return
        if attempt < retries:
            print(
                f"Command failed with exit code {completed.returncode}; retrying once.",
                file=sys.stderr,
                flush=True,
            )

    raise SystemExit(completed.returncode)


def configure_environment() -> dict[str, str]:
    env = os.environ.copy()
    for name in ("CFLAGS", "CXXFLAGS", "DEBUG_CFLAGS", "DEBUG_CXXFLAGS"):
        env[name] = ""
    return env


def required_pixi_version(repo_root: Path) -> str | None:
    """Return the simple minimum version declared by requires-pixi, if any."""
    manifest = repo_root / "pixi.toml"
    try:
        contents = manifest.read_text(encoding="utf-8")
    except OSError:
        return None

    match = re.search(
        r'^\s*requires-pixi\s*=\s*["\']\s*>=\s*(\d+(?:\.\d+){1,2})',
        contents,
        flags=re.MULTILINE,
    )
    return match.group(1) if match else None


def installed_pixi_version(pixi: str) -> str | None:
    try:
        completed = subprocess.run(
            [pixi, "--version"],
            capture_output=True,
            text=True,
        )
    except OSError:
        return None

    output = f"{completed.stdout}\n{completed.stderr}"
    match = re.search(r"\bpixi\s+(\d+(?:\.\d+){1,2})", output)
    return match.group(1) if match else None


def version_key(version: str) -> tuple[int, int, int]:
    parts = [int(part) for part in version.split(".")]
    normalized = (parts + [0, 0, 0])[:3]
    return normalized[0], normalized[1], normalized[2]


def configure_preset(config: str, use_pixi: bool) -> str:
    if use_pixi:
        return f"conda-{host_platform_name()}-{config}"
    return config


def configure_fingerprint(
    repo_root: Path,
    *,
    preset: str,
    cmake_args: list[str],
    use_pixi: bool,
) -> str:
    """Identify inputs that can invalidate configure-time generated files."""
    inputs: dict[str, object] = {
        "preset": preset,
        "cmake_args": cmake_args,
        "use_pixi": use_pixi,
    }
    if use_pixi:
        for filename in ("pixi.toml", "pixi.lock"):
            path = repo_root / filename
            try:
                inputs[filename] = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError:
                inputs[filename] = None
    encoded = json.dumps(inputs, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def configured_with_fingerprint(stamp_file: Path, fingerprint: str) -> bool:
    try:
        stamp = json.loads(stamp_file.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return isinstance(stamp, dict) and stamp.get("fingerprint") == fingerprint


def write_configure_fingerprint(stamp_file: Path, fingerprint: str) -> None:
    stamp_file.parent.mkdir(parents=True, exist_ok=True)
    temporary = stamp_file.with_suffix(stamp_file.suffix + ".tmp")
    temporary.write_text(
        json.dumps({"fingerprint": fingerprint}, indent=2) + "\n",
        encoding="utf-8",
    )
    temporary.replace(stamp_file)


def ensure_compatible_pixi(pixi: str, repo_root: Path, dry_run: bool) -> None:
    required = required_pixi_version(repo_root)
    installed = installed_pixi_version(pixi)
    if required is None or installed is None:
        return
    if version_key(installed) >= version_key(required):
        return

    print(f"Pixi {installed} is older than the required {required}; updating Pixi.")
    run(
        [pixi, "self-update", "--version", required],
        cwd=repo_root,
        env=None,
        dry_run=dry_run,
    )


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
        help="Force CMake configure even when the cached environment is current.",
    )
    parser.add_argument(
        "--no-configure",
        action="store_true",
        help="Skip CMake configure. Fails if the build tree is not configured.",
    )
    parser.add_argument(
        "--configure-only",
        action="store_true",
        help="Configure the selected build tree without building it.",
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
    if args.configure_only and args.no_configure:
        parser.error("--configure-only and --no-configure cannot be used together")
    if args.configure_only and (args.target or args.clean_first or args.test or args.install):
        parser.error(
            "--configure-only cannot be combined with build targets, cleaning, tests, or install"
        )
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

    if args.no_configure and not cache_file.exists():
        raise SystemExit(
            f"{cache_file} does not exist. Run without --no-configure first."
        )

    prefix = command_prefix(not args.no_pixi)
    if not prefix and shutil.which("cmake") is None:
        raise SystemExit("cmake was not found on PATH. Install CMake or run with Pixi available.")

    configure_env = configure_environment()
    if prefix:
        ensure_compatible_pixi(prefix[0], repo_root, args.dry_run)
        # Keep the local environment in sync with pixi.toml/pixi.lock before
        # any tool from that environment is used for the build.
        run(
            [prefix[0], "install"],
            cwd=repo_root,
            env=None,
            dry_run=args.dry_run,
            retries=1,
        )

    preset = configure_preset(args.config, bool(prefix))
    fingerprint = configure_fingerprint(
        repo_root,
        preset=preset,
        cmake_args=args.cmake_arg,
        use_pixi=bool(prefix),
    )
    stamp_file = repo_root / build_dir / ".build_freecad_configure.json"
    needs_configure = not args.no_configure and (
        args.configure
        or not cache_file.exists()
        or not configured_with_fingerprint(stamp_file, fingerprint)
    )

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

        configure_cmd = prefix + ["cmake", "--preset", preset]
        if sys.platform.startswith("win") and preset.startswith("conda-windows-"):
            configure_cmd.extend(["-DCMAKE_GENERATOR_PLATFORM=", "-DCMAKE_GENERATOR_TOOLSET="])
        configure_cmd.extend(args.cmake_arg)
        run(configure_cmd, cwd=repo_root, env=configure_env, dry_run=args.dry_run)
        if not args.dry_run:
            write_configure_fingerprint(stamp_file, fingerprint)

    if args.configure_only:
        if args.dry_run:
            print("Configure dry run finished.")
        else:
            print("Configure finished successfully.")
        return 0

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
