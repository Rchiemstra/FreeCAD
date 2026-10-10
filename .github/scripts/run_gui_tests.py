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
import struct
import sys
import subprocess
import os
import shutil
import tempfile
import zlib
from collections import defaultdict
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
    SIGABRT is ``-6``.
    """
    if rc < 0:
        return f"{rc} (killed by signal {-rc})"
    if rc == 245:
        return "245 (unsigned SIGSEGV / sys.exit(-11))"
    return str(rc)


def _died_from_signal(rc: int) -> bool:
    """True when the child was killed by a signal, including SIGABRT (-6)."""
    return rc < 0 or rc == 245


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


def _line_buffered(cmd: list[str]) -> list[str]:
    """Force line-buffered FreeCAD stdout/stderr when piped (Docker CI)."""
    stdbuf = shutil.which("stdbuf")
    if stdbuf:
        return [stdbuf, "-oL", "-eL", *cmd]
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
    if _died_from_signal(rc):
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


# fcrash v1, written from the signal handler. See src/Base/CrashReporter/Format.h.
# Little-endian header is 128 bytes; the first 77 are the fields below.
_FCRASH_MAGIC = 0x52434346  # 'FCCR'
_FCRASH_HEADER = struct.Struct("<IIQQqIIIIIIIIIIBBBBB")
_FCRASH_FRAME = struct.Struct("<QQII")
_FCRASH_NO_STRING = 0xFFFFFFFF
_SIGNAL_NAMES = {4: "SIGILL", 6: "SIGABRT", 7: "SIGBUS", 8: "SIGFPE", 11: "SIGSEGV"}

# Printed by FreeCADCmd between markers so startup logs can be ignored.
_CRASH_DECODE_PY = """
import FreeCAD as App
print("FCRASH_BEGIN")
reports = App.getCrashReports()
if not reports:
    print("FCRASH_EMPTY")
else:
    for report in reports:
        print("SYMBOLICATED %s" % int(bool(report.symbolicated)))
        print("FAULT %s %s" % (report.fault_code, report.fault_name))
        for frame in report.stack_frames:
            offset = frame.module_offset
            offset_text = "" if offset is None else format(int(offset), "x")
            symbol = frame.symbol or ""
            location = ""
            if frame.file:
                line = 0 if frame.line is None else int(frame.line)
                location = "%s:%s" % (frame.file, line)
            module = frame.module or ""
            print("FRAME\\t%s\\t%s\\t%s\\t%s" % (offset_text, module, symbol, location))
print("FCRASH_END")
"""

Symbolize = Callable[[str, int], str | None]


def crash_report_search_roots() -> list[Path]:
    """Directories that can contain a ``CrashReports`` folder for this process.

    ``Application::initCrashReporter`` writes ``crash-<time>-<pid>.fcrash`` under
    ``getUserAppDataDir() + "CrashReports"``. The FreeCAD and FreeCADCmd mains set
    ``AppDataSkipVendor``, so on Linux that data dir is ``$FREECAD_USER_DATA`` when
    set, else ``$FREECAD_USER_HOME``, else ``$XDG_DATA_HOME/FreeCAD`` (default
    ``~/.local/share/FreeCAD``) plus ``v<major>-<minor>`` when the tree is versioned.
    Searching the FreeCAD data root finds ``v27-1/CrashReports`` without hard-coding
    the version.
    """
    data = os.environ.get("FREECAD_USER_DATA")
    home = os.environ.get("FREECAD_USER_HOME")
    if data:
        roots = [Path(data)]
    elif home:
        roots = [Path(home)]
    else:
        xdg = os.environ.get("XDG_DATA_HOME")
        base = Path(xdg) if xdg else Path.home() / ".local" / "share"
        roots = [base / "FreeCAD"]
    unique: list[Path] = []
    seen: set[Path] = set()
    for root in roots:
        if root in seen:
            continue
        seen.add(root)
        unique.append(root)
    return unique


def _fcrash_files_under(roots: Sequence[Path]) -> set[Path]:
    found: set[Path] = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.fcrash"):
            if path.is_file():
                found.add(path.resolve())
    return found


def _string_at(table: bytes, offset: int) -> str | None:
    if offset == _FCRASH_NO_STRING:
        return None
    if offset < 0 or offset + 2 > len(table):
        raise ValueError(f"string offset {offset} is outside the string table")
    (length,) = struct.unpack_from("<H", table, offset)
    start = offset + 2
    end = start + length
    if length > 4096 or end > len(table):
        raise ValueError(f"string at {offset} does not fit in the string table")
    return table[start:end].decode("utf-8", "replace")


def parse_fcrash(path: Path) -> dict:
    """Parse one v1 ``.fcrash`` file into fault metadata and stack frames.

    Each frame keeps the module path stored in the file (a full path on Linux)
    and the module-relative offset. Symbolication is separate.
    """
    data = path.read_bytes()
    if len(data) < _FCRASH_HEADER.size + 4:
        raise ValueError(f"crash report is too small: {path}")
    if len(data) > (1 << 20):
        raise ValueError(f"crash report is larger than 1 MiB: {path}")
    header = _FCRASH_HEADER.unpack_from(data, 0)
    (
        magic,
        version,
        fault_address,
        _thread_id,
        _timestamp,
        process_id,
        code,
        frame_count,
        file_size,
        flags,
        frame_table_offset,
        string_table_offset,
        _build_id,
        _suffix,
        _minidump,
        _os_id,
        _arch,
        _major,
        _minor,
        _patch,
    ) = header
    if magic != _FCRASH_MAGIC:
        raise ValueError(f"unexpected fcrash magic in {path}")
    if version != 1:
        raise ValueError(f"unsupported fcrash version {version} in {path}")
    if file_size != len(data):
        raise ValueError(f"fcrash size mismatch in {path}")
    if frame_count > 128:
        raise ValueError(f"fcrash frame count {frame_count} is too large")
    frame_bytes = frame_count * _FCRASH_FRAME.size
    if frame_table_offset + frame_bytes > string_table_offset:
        raise ValueError(f"fcrash frame table does not fit in {path}")
    if string_table_offset + 4 > file_size:
        raise ValueError(f"fcrash string table does not fit in {path}")
    checksum = struct.unpack_from("<I", data, file_size - 4)[0]
    calculated = zlib.crc32(data[: file_size - 4]) & 0xFFFFFFFF
    string_table = data[string_table_offset : file_size - 4]
    frames: list[dict] = []
    for index in range(frame_count):
        raw_address, module_offset, module_string, _pad = _FCRASH_FRAME.unpack_from(
            data, frame_table_offset + index * _FCRASH_FRAME.size
        )
        module = _string_at(string_table, module_string) or ""
        frames.append(
            {
                "address": raw_address,
                "offset": module_offset,
                "module": module,
            }
        )
    return {
        "path": path,
        "code": code,
        "fault_address": fault_address,
        "pid": process_id,
        "partial": calculated != checksum or bool(flags & (1 << 1)),
        "frames": frames,
    }


def _freecad_cmd(freecad_exec: str) -> str | None:
    path = Path(freecad_exec)
    candidates: list[Path] = []
    if path.name.endswith("FreeCADCmd"):
        candidates.append(path)
    if path.name == "FreeCAD":
        candidates.append(path.with_name("FreeCADCmd"))
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which("FreeCADCmd")


def _symbols_from_freecad(freecad_exec: str, report_path: Path) -> dict[int, list[str]]:
    """Ask FreeCADCmd to parse and symbolicate a copy of ``report_path``.

    Startup scans ``$FREECAD_USER_DATA/CrashReports`` and moves the copy into
    ``archive/``. The original file is left in place. Empty when FreeCADCmd is
    missing or the report does not match this build (no symbolication).
    """
    cmd = _freecad_cmd(freecad_exec)
    if not cmd:
        return {}
    try:
        with tempfile.TemporaryDirectory(prefix="fc-crash-decode-") as tmp:
            data_dir = Path(tmp)
            dest_dir = data_dir / "CrashReports"
            dest_dir.mkdir()
            shutil.copy2(report_path, dest_dir / report_path.name)
            script_path = data_dir / "decode_crash.py"
            script_path.write_text(_CRASH_DECODE_PY, encoding="utf-8")
            env = os.environ.copy()
            env["FREECAD_USER_DATA"] = str(data_dir)
            env.setdefault("QT_QPA_PLATFORM", "offscreen")
            proc = subprocess.run(
                [cmd, "-c", f"exec(open({str(script_path)!r}).read())"],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                check=False,
                env=env,
                timeout=120,
            )
    except (OSError, subprocess.TimeoutExpired):
        return {}
    text = proc.stdout or ""
    begin = text.find("FCRASH_BEGIN")
    end = text.find("FCRASH_END")
    if begin < 0 or end < begin or "FCRASH_EMPTY" in text[begin:end]:
        return {}
    if "SYMBOLICATED 1" not in text[begin:end]:
        return {}
    symbols: dict[int, list[str]] = defaultdict(list)
    for line in text[begin:end].splitlines():
        if not line.startswith("FRAME\t"):
            continue
        parts = line.split("\t")
        if len(parts) < 5 or not parts[1]:
            continue
        try:
            offset = int(parts[1], 16)
        except ValueError:
            continue
        symbol = parts[3].strip()
        location = parts[4].strip()
        if not symbol and not location:
            continue
        rendered = symbol
        if location:
            rendered = f"{symbol} at {location}".strip()
        symbols[offset].append(rendered)
    return symbols


def _symbolize_native(module: str, offset: int) -> str | None:
    """Resolve ``module+offset`` with addr2line, then gdb, when the file exists."""
    if not module or not Path(module).is_file():
        return None
    address = hex(offset)
    addr2line = shutil.which("addr2line")
    if addr2line:
        proc = subprocess.run(
            [addr2line, "-f", "-C", "-i", "-p", "-e", module, address],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
            timeout=30,
        )
        rendered = []
        for line in (proc.stdout or "").splitlines():
            text = line.strip()
            if not text or text == "??" or text.startswith("?? ") or text.startswith("??:"):
                continue
            rendered.append(text)
        if rendered:
            return " / ".join(rendered)
    gdb = shutil.which("gdb")
    if not gdb:
        return None
    proc = subprocess.run(
        [gdb, "-batch", "-nx", "-ex", "set pagination off", "-ex", f"info line *{address}", module],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
        timeout=30,
    )
    for line in (proc.stdout or "").splitlines():
        stripped = line.strip()
        if stripped.startswith("Line ") or " is in " in stripped:
            return stripped
    return None


def format_crash_report(
    path: Path,
    *,
    freecad_exec: str | None = None,
    symbolize: Symbolize | None = None,
) -> str:
    """Return a readable backtrace for one ``.fcrash`` file.

    Frames always include module + offset. Symbols come from the injected
    ``symbolize`` callable, otherwise from FreeCAD's parser
    (``App.getCrashReports`` via FreeCADCmd) and then addr2line or gdb.
    """
    parsed = parse_fcrash(path)
    freecad_symbols: dict[int, list[str]] = {}
    if symbolize is None and freecad_exec:
        freecad_symbols = _symbols_from_freecad(freecad_exec, path)
    name = _SIGNAL_NAMES.get(parsed["code"], "signal")
    partial = " partial" if parsed["partial"] else ""
    count = len(parsed["frames"])
    noun = "frame" if count == 1 else "frames"
    lines = [
        (
            f"Crash report {parsed['path']}: {name} ({parsed['code']}) "
            f"pid={parsed['pid']}{partial}, {count} {noun}"
        )
    ]
    for index, frame in enumerate(parsed["frames"]):
        module = frame["module"] or "?"
        line = f"#{index} {module}+0x{frame['offset']:x}"
        symbol: str | None = None
        if symbolize is not None:
            symbol = symbolize(module, frame["offset"])
        else:
            queued = freecad_symbols.get(frame["offset"])
            if queued:
                symbol = queued.pop(0)
            if not symbol:
                symbol = _symbolize_native(module, frame["offset"])
        if symbol:
            line = f"{line} {symbol}"
        lines.append(line)
    return "\n".join(lines)


def _emit_crash_reports(
    roots: Sequence[Path],
    before: set[Path],
    *,
    freecad_exec: str,
    symbolize: Symbolize | None,
) -> None:
    new_files = sorted(_fcrash_files_under(roots) - before)
    if not new_files:
        _log("No crash report (.fcrash) found.", error=True)
        return
    for path in new_files:
        try:
            text = format_crash_report(path, freecad_exec=freecad_exec, symbolize=symbolize)
        except (OSError, ValueError, struct.error, subprocess.TimeoutExpired) as exc:
            text = f"Could not decode crash report {path}: {exc}"
        _log(text, error=True)


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
    crash_reports_dir: Path | None = None,
    symbolize: Symbolize | None = None,
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

    report_roots = (
        [crash_reports_dir] if crash_reports_dir is not None else crash_report_search_roots()
    )
    last_rc = 0
    for mod in gui_tests:
        units = units_for_module(mod)
        if len(units) > 1:
            _log(
                f"\nRunning registered suite {mod}, then {len(units) - 1} class units"
            )
        for unit in units:
            _log(f"\nRunning GUI tests for module: {unit}")
            before_reports = _fcrash_files_under(report_roots)
            cmd = _line_buffered([freecad_exec, "-t", unit])
            rc, out = run(_with_crash_helper(cmd) if crash_helper else cmd)
            _log(out)
            eval_rc, eval_err = evaluate_unit_result(rc, out, mandatory=unit in expected_suites)
            if eval_rc != 0:
                _log(f"Module {unit}: {eval_err}", error=True)
                last_rc = eval_rc
                if _died_from_signal(rc):
                    _log(
                        f"Stopping after {unit}: FreeCAD GUI child segfaulted.",
                        error=True,
                    )
                    _emit_crash_reports(
                        report_roots,
                        before_reports,
                        freecad_exec=freecad_exec,
                        symbolize=symbolize,
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
