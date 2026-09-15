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
import re
import sys
import subprocess
import os
import shutil
from collections.abc import Callable, Sequence
from pathlib import Path


# Suites Woodpecker pipeline 336 actually completed. Extra registered Gui
# names are allowed; missing any of these is a discovery miss, not a skip.
EXPECTED_GUI_SUITES: tuple[str, ...] = (
    "GuiDocument",
    "TestSpreadsheetWindowGui",
    "TestSketcherGui",
    "TestPartDesignGui",
    "TestPartGui",
    "MeshTestsGui",
    "TestDraftGui",
    "TestArchGui",
    "TestTechDrawGui",
    "TestImportGui",
    "TestOpenSCADGui",
    "TestMaterialsGui",
    "TestCAMGui",
)

# Diagnostic class units. Never a substitute for the aggregate TestSketcherGui.
SKETCHER_GUI_CLASS_UNITS: tuple[str, ...] = (
    "SketcherTests.TestConstraintPreselectionGui.SketcherGuiTestCases",
    "SketcherTests.TestDistanceLabelExtensionGui.TestDistanceLabelExtensionGui",
    "SketcherTests.TestConstraintCommandsGui.TestConstraintCommandsGui",
    "SketcherTests.TestOnViewParameterGui.TestOnViewParameterGui",
    "SketcherTests.TestPlacementUpdate.TestSketchPlacementUpdate",
    "SketcherTests.TestExternalFacePreselection.TestExternalFacePreselection",
    "SketcherTests.TestSketcherOffsetGui.TestSketcherOffsetGui",
)

_RAN_TESTS = re.compile(r"^Ran (\d+) tests?\b", re.MULTILINE)
# unittest's final status line: "OK", "OK (skipped=2)", "FAILED (failures=1, errors=2)".
# Anchored to the whole line so diagnostics like "FAILED to load ..." never match.
_OUTCOME = re.compile(r"^(OK|FAILED|NO TESTS RAN)(?:\s*\(([^)]*)\))?\s*$")

RunCommand = Callable[[list[str]], tuple[int, str]]


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


def parse_completed_test_count(output: str) -> int | None:
    """Return the last unittest ``Ran N test(s)`` count, or None if missing."""
    matches = list(_RAN_TESTS.finditer(output))
    if not matches:
        return None
    return int(matches[-1].group(1))


def units_for_module(mod: str) -> list[str]:
    """Run the registered module first. Split Sketcher classes only as extras."""
    if mod == "TestSketcherGui":
        return [mod, *SKETCHER_GUI_CLASS_UNITS]
    return [mod]


def discover_gui_suites(
    listing_rc: int,
    listing_output: str,
    expected_suites: Sequence[str] = EXPECTED_GUI_SUITES,
) -> tuple[int, list[str], str]:
    """Return (exit_code, gui_suite_names, error). exit_code 0 means discovery is usable."""
    if listing_rc != 0:
        return (
            1 if listing_rc < 0 else listing_rc,
            [],
            f"Test discovery failed with exit code {_format_rc(listing_rc)}.",
        )
    if "Registered test units:" not in listing_output:
        return 2, [], "Test discovery output is missing 'Registered test units:'."
    tests = parse_registered_tests(listing_output)
    if not tests:
        return 2, [], "Test discovery listed no registered units."
    gui_tests = [t for t in tests if "Gui" in t]
    missing = [name for name in expected_suites if name not in gui_tests]
    if missing:
        return (
            3,
            gui_tests,
            "Expected GUI suites missing from discovery: " + ", ".join(missing) + ".",
        )
    if not gui_tests:
        return 3, [], "No GUI tests found in registered tests."
    return 0, gui_tests, ""


def parse_final_outcome(output: str) -> tuple[str | None, dict[str, int]]:
    """Return the unittest status that follows the last ``Ran N tests`` line.

    ``("OK", {"skipped": 2})`` for ``OK (skipped=2)``; ``(None, {})`` when the
    last summary has no status line after it (truncated or killed child).
    """
    matches = list(_RAN_TESTS.finditer(output))
    if not matches:
        return None, {}
    for line in output[matches[-1].end() :].splitlines():
        m = _OUTCOME.match(line.strip())
        if not m:
            continue
        counts: dict[str, int] = {}
        for item in (m.group(2) or "").split(","):
            key, sep, value = item.partition("=")
            if sep and value.strip().isdigit():
                counts[key.strip()] = int(value)
        return m.group(1), counts
    return None, {}


def evaluate_unit_result(rc: int, output: str, *, mandatory: bool = False) -> tuple[int, str]:
    """Require a completed, successful unittest run, not only a zero child exit.

    ``mandatory`` suites must also execute at least one test that is not skipped.
    """
    if rc < 0 or rc == 245:
        return 1, f"GUI child segfaulted ({_format_rc(rc)})."
    completed = parse_completed_test_count(output)
    if completed is None:
        return 4, "Module produced no 'Ran N tests' summary."
    if completed < 1:
        return 4, "Module completed 0 tests."
    if rc != 0:
        return rc, f"Module exited with code {_format_rc(rc)}."
    outcome, counts = parse_final_outcome(output)
    if outcome is None:
        return 4, f"Module ran {completed} tests but printed no final OK/FAILED outcome."
    if outcome != "OK":
        return 1, f"Module reported {outcome} ({', '.join(f'{k}={v}' for k, v in counts.items())})."
    if mandatory and counts.get("skipped", 0) >= completed:
        return 4, f"Mandatory suite skipped all {completed} tests."
    return 0, ""


def _rerun_under_gdb(freecad_exec: str, unit: str, run: RunCommand) -> str:
    gdb = shutil.which("gdb")
    if not gdb:
        return ""
    _log(f"Re-running {unit} under gdb for a backtrace.", error=True)
    _, gdb_out = run(
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
            unit,
        ]
    )
    return gdb_out


def run_gui_modules(
    freecad_exec: str,
    *,
    run: RunCommand = run_and_capture,
    expected_suites: Sequence[str] = EXPECTED_GUI_SUITES,
    crash_helper: bool = True,
) -> int:
    """Discover GUI suites and run them. Non-zero means the e2e gate failed."""
    listing_rc, listing_out = run([freecad_exec, "-t"])
    if listing_rc != 0:
        _log(
            f"Warning: listing tests returned exit code {_format_rc(listing_rc)}.",
            error=True,
        )
        _log(listing_out, error=True)
    disc_rc, gui_tests, disc_err = discover_gui_suites(
        listing_rc, listing_out, expected_suites
    )
    if disc_rc != 0:
        _log(disc_err, error=True)
        return disc_rc

    _log("Found GUI test modules:")
    for t in gui_tests:
        _log(f"  {t}")

    last_rc = 0
    for mod in gui_tests:
        units = units_for_module(mod)
        if len(units) > 1:
            _log(
                f"\nRunning registered suite {mod}, then {len(units) - 1} class units"
            )
        for unit in units:
            _log(f"\nRunning GUI tests for module: {unit}")
            cmd = [freecad_exec, "-t", unit]
            rc, out = run(_with_crash_helper(cmd) if crash_helper else cmd)
            _log(out)
            eval_rc, eval_err = evaluate_unit_result(rc, out, mandatory=unit in expected_suites)
            if eval_rc != 0:
                _log(f"Module {unit}: {eval_err}", error=True)
                last_rc = eval_rc
                if rc < 0 or rc == 245:
                    _log(
                        f"Stopping after {unit}: FreeCAD GUI child segfaulted.",
                        error=True,
                    )
                    gdb_out = _rerun_under_gdb(freecad_exec, unit, run)
                    if gdb_out:
                        _log(gdb_out, error=True)
                    return 1
    return last_rc


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

    return run_gui_modules(freecad_exec)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
