#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
run_gui_tests.py

List registered tests via `FreeCAD -t`, filter for GUI tests (names containing 'Gui'), and run each
GUI test module using the specified FreeCAD executable.

Usage:
  run_gui_tests.py [FREECAD_EXEC]

If FREECAD_EXEC is omitted the script falls back to 'FreeCAD' on PATH.
If FREECAD_EXEC is a directory containing bin/FreeCAD, that binary is used.
If FREECAD_EXEC is an executable path, it is used directly.

This script returns 0 if all GUI modules run successfully. Otherwise it returns the last non-zero
exit code.
"""

from __future__ import annotations
import sys
import subprocess
import os
import shutil
from pathlib import Path


def find_executable(arg: str | None) -> str:
    """Return the FreeCAD executable path to use.

    If `arg` is None or empty, returns the plain name 'FreeCAD' which will be looked up on PATH. If
    `arg` is a directory and contains `bin/FreeCAD` that binary will be returned. If `arg` is a file
    path it is returned as-is. Otherwise the original argument is returned.

    Common use cases: use the FreeCAD binary from a build directory or an installed FreeCAD prefix.
    """
    if not arg:
        return "FreeCAD"
    p = Path(arg)
    if p.is_dir():
        candidate = p / "bin" / "FreeCAD"
        if candidate.exists():
            return str(candidate)
    if p.is_file():
        return str(p)
    # fallback: return as-is (may be on PATH)
    return arg


def validate_executable(path: str) -> tuple[bool, str]:
    """Return (ok, message). Checks if the executable exists or is likely on PATH.

    This is best effort: if a bare name is given (e.g. 'FreeCAD') we can't stat it here, so we
    accept it but warn. If the path points to a file, we check executability. Also warn if the name
    looks like the CLI-only 'FreeCADCmd'.
    """
    p = Path(path)
    if p.is_file():
        if not os.access(str(p), os.X_OK):
            return False, f"File exists but is not executable: {path}"
        if p.name.endswith("FreeCADCmd"):
            return (
                True,
                (
                    "Warning: executable looks like 'FreeCADCmd' (CLI); GUI tests require the GUI "
                    "binary 'FreeCAD'."
                ),
            )
        return True, ""
    # Bare name or non-existent path: accept but warn
    if p.name.endswith("FreeCADCmd"):
        return (
            True,
            (
                "Warning: executable name looks like 'FreeCADCmd' (CLI); GUI tests require the GUI "
                "binary 'FreeCAD'."
            ),
        )
    return True, ""


def _format_rc(rc: int) -> str:
    """Describe a FreeCAD child exit, including Python's negative signal codes.

    A SIGSEGV child becomes ``returncode == -11``; ``sys.exit(-11)`` is 245.
    """
    if rc < 0:
        return f"{rc} (killed by signal {-rc})"
    if rc == 245:
        return "245 (unsigned SIGSEGV / sys.exit(-11))"
    return str(rc)


def _ensure_headless_gl() -> None:
    os.environ.setdefault("QT_QPA_PLATFORM", "xcb")


def _log(message: str, *, error: bool = False) -> None:
    stream = sys.stderr if error else sys.stdout
    print(message, file=stream, flush=True)


def _with_crash_helper(cmd: list[str]) -> list[str]:
    """Prefix catchsegv when available so a GUI SIGSEGV prints a backtrace."""
    catchsegv = shutil.which("catchsegv")
    if catchsegv:
        return [catchsegv, *cmd]
    return cmd


def run_and_capture(cmd: list[str]) -> tuple[int, str]:
    """Run `cmd` and return (returncode, combined stdout+stderr string).

    If the executable is not found a return code of 127 is returned and a small error string is
    provided as output to aid diagnosis.
    """
    try:
        proc = subprocess.run(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=False
        )
        return proc.returncode, proc.stdout
    except FileNotFoundError:
        return 127, f"Executable not found: {cmd[0]}\n"


def parse_registered_tests(output: str) -> list[str]:
    """Parse output from `FreeCAD -t` and return a list of registered test unit names.

    The function looks for the section starting with the literal 'Registered test units:' and
    then collects non-empty, stripped lines from that point onwards as test names.
    """
    lines = output.splitlines()
    tests: list[str] = []
    started = False
    for ln in lines:
        if not started:
            if "Registered test units:" in ln:
                started = True
            continue
        s = ln.strip()
        if not s:
            # allow blank lines but keep going
            continue
        tests.append(s)
    return tests


def main(argv: list[str]) -> int:
    """Entry point: run GUI test modules registered in the FreeCAD executable.

    Returns the last non-zero exit code from any GUI test module, or 0 on success.
    """
    _ensure_headless_gl()
    exec_arg = argv[1] if len(argv) > 1 else None
    freecad_exec = find_executable(exec_arg)

    _log(f"Using FreeCAD executable: {freecad_exec}")
    _log(
        "headless env "
        f"QT_QPA_PLATFORM={os.environ.get('QT_QPA_PLATFORM')} "
        f"LIBGL_ALWAYS_SOFTWARE={os.environ.get('LIBGL_ALWAYS_SOFTWARE')}"
    )

    ok, msg = validate_executable(freecad_exec)
    if msg:
        _log(msg, error=True)
    if not ok:
        _log(f"Aborting: invalid FreeCAD executable: {freecad_exec}", error=True)
        return 3

    code, out = run_and_capture([freecad_exec, "-t"])
    if code != 0:
        _log(
            f"Warning: listing tests returned exit code {_format_rc(code)}; "
            "attempting to parse output anyway",
            error=True,
        )
        _log(out, error=True)

    tests = parse_registered_tests(out)
    if not tests:
        # A 7-second "success" here is how pipelines 341-343 went green without
        # running any GUI module. Treat an empty list as a miss unless listing
        # itself was a clean zero with truly no units (should not happen here).
        if code != 0:
            _log("No registered tests found after a failed listing.", error=True)
            return 1 if code < 0 else code
        _log("No registered tests found; exiting with success.")
        return 0

    gui_tests = [t for t in tests if "Gui" in t]
    if not gui_tests:
        _log("No GUI tests found in registered tests; nothing to run.")
        return 0

    _log("Found GUI test modules:")
    for t in gui_tests:
        _log(f"  {t}")

    last_rc = 0
    for mod in gui_tests:
        _log(f"\nRunning GUI tests for module: {mod}")
        rc, out = run_and_capture(_with_crash_helper([freecad_exec, "-t", mod]))
        _log(out)
        if rc != 0:
            _log(f"Module {mod} exited with code {_format_rc(rc)}", error=True)
            last_rc = rc
            if rc < 0 or rc == 245:
                _log(
                    f"Stopping after {mod}: FreeCAD GUI child segfaulted.",
                    error=True,
                )
                gdb = shutil.which("gdb")
                if gdb:
                    _log(f"Re-running {mod} under gdb for a backtrace.", error=True)
                    _, gdb_out = run_and_capture(
                        [
                            gdb,
                            "-batch",
                            "-return-child-result",
                            "-ex",
                            "set pagination off",
                            "-ex",
                            "run",
                            "-ex",
                            "thread apply all bt 30",
                            "--args",
                            freecad_exec,
                            "-t",
                            mod,
                        ]
                    )
                    _log(gdb_out, error=True)
                return 1

    return last_rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
