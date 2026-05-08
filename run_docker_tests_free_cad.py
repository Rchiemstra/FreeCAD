#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the FreeCAD test suite in Docker and write one timestamped log."""

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


DEFAULT_IMAGE = "ghcr.io/prefix-dev/pixi:0.59.0"
CONTAINER_WORKDIR = "/workspace"
PIXI_VOLUME = "freecad-linux-pixi"
BUILD_VOLUME_PREFIX = "freecad-linux-build"

# Lines matching these are usually high-volume and low signal (build progress, package IO).
_NOISE_LINE_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"^\[\s*\d+%\] "),  # CMake: [ 12%] Building ...
    re.compile(r"^\[\d+/\d+\] "),  # Ninja: [42/1203] ...
    re.compile(
        r"^(Selecting previously unselected package|Unpacking |Setting up |Preparing to unpack )"
    ),
    re.compile(
        r"^(Reading package lists|Building dependency tree|Reading state information)\.{0,3}\s*$"
    ),
    re.compile(r"^(Get:\d|Ign:\d|Hit:\d|Fetched \d)"),  # apt-get update lines
    re.compile(r".*\bDownloading\b.*\b(KiB|MiB|GiB)\b"),  # pixi / curl style progress
    re.compile(r"^\s*Downloaded\s"),  # pixi wheel summaries (often huge)
    re.compile(r"^\(Reading database"),  # dpkg/apt database scan progress
    re.compile(r"^Add property type:\s"),  # FreeCAD property-type registration (70+ lines/run)
    re.compile(r"^MbD:\s"),  # Multibody-dynamics solver verbose output
    re.compile(r"^\*{4}"),  # FEM decorative star banners (****...**** section headers)
    re.compile(r"^onChanged\s"),  # Document signal callbacks during tests
    re.compile(r"^#[-# ]{3,}"),  # #### Input #### / #### Result #### / ##########
)

# CTest prints thousands of per-test lines; keep failures and the final summary only.
_CTEST_START_LINE_RE = re.compile(r"^\s+Start\s+\d+:")
_CTEST_PASSED_RESULT_RE = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:")

# Verbose unittest: "test_foo (...)" or CAM-style "test02 (CAMTests....test02)" (same line may continue).
_UNITTEST_HEADER_LINE_RE = re.compile(r"^\s*test[\w.]+\s+\(\w+(?:\.\w+)+\)")
# Typical ISO CNC motion lines dumped during CAM tests (G0/G1 + axis words).
_GCODE_MOTION_LINE_RE = re.compile(
    r"^\s*G\d+(?:\s+[A-Z]-?\d*\.?\d+)*[\\]?\s*$",
    re.IGNORECASE,
)
# Full G-code program lines: modal codes, M-codes, program delimiters, parenthetical comments.
_GCODE_PROGRAM_LINE_RE = re.compile(
    r"^\s*(?:"
    r"%|"  # program delimiter
    r"\((?!test_)[^)]{0,200}\)|"  # G-code comment (not test annotation)
    r"(?:G\d+\s*)+[A-Z\s\d.+*/\[\]\-#]*[\\]?|"  # G-code with optional expressions
    r"M\d+(?:\s+[A-Z]\d+)?|"  # M-code
    r"N\d+\s+"  # sequence numbers
    r")\s*$",
    re.IGNORECASE,
)
_FREECAD_INTERNAL_TAG_RE = re.compile(r"^<[^>\n]{1,120}>")
_DOCSTYLE_LONG_DESC_RE = re.compile(
    r"(?i)^(Create|Convert|Verify|Tests that|Test the)\s+.+\.{2,}"
)
# OpenCascade-style verbose progress during booleans / history prepare.
# Use prefix-only matching so garbled "...ok" variants (e.g. "Preparing histok") also match.
_OCCT_PROGRESS_LINE_RE = re.compile(
    r"(?i)^\s*(?:"
    r"Preparing hist|"
    r"Post treatment of result shape|"
    r"Performing intersection of|"
    r"Initialization of Intersection|"
    r"Building result of SECTION"
    r")"
)
_FREECAD_SPLASH_LINE_RES: tuple[re.Pattern[str], ...] = (
    re.compile(r"FreeCAD \d+\.\d+\.\d+.*"),
    re.compile(r"\(C\) \d{4}-\d{4} FreeCAD contributors\s*"),
    re.compile(r"FreeCAD is free and open-source software licensed under.*"),
)

_CMAKE_WARNING_PREFIXES: tuple[str, ...] = ("CMake Warning", "CMake Deprecation Warning")

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
    re.compile(r"========== .+ =========="),  # docker script section banners only
    re.compile(r"(?i)\b(assertionerror|segmentation fault|core dumped)\b"),
    re.compile(r"(?i)^\s*\d+% tests passed"),  # ctest summary
    re.compile(r"(?i)^\s*\d+% tests failed"),  # alternate ctest wording
    re.compile(r"^Ran \d+ tests in "),  # unittest
    re.compile(r"^OK\b"),  # unittest summary, e.g. OK (skipped=…)
    re.compile(r"^FAILED\b"),
)


def _keep_for_signal_hint(line: str) -> bool:
    """True when the line likely reports an actionable error/failure/traceback for humans/agents."""
    if re.match(r'^\s*File "', line):
        return True
    if re.search(r"\b[A-Za-z_][A-Za-z0-9_]*Error:", line):
        return True
    if re.search(r"(?i)(^|\s)(error:|fatal error:|\bfailed\b)", line):
        return True
    if re.search(r"(?i)\((?:Failed|Timeout)\)", line):  # CTest: "12 - TestName (Failed)"
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
    """Strip trailing spaces and tabs before the line terminator (console / filtered log only)."""
    if line.endswith("\r\n"):
        return line[:-2].rstrip(" \t") + "\r\n"
    if line.endswith("\n"):
        return line[:-1].rstrip(" \t") + "\n"
    return line.rstrip(" \t")


def keep_log_line(line: str) -> bool:  # noqa: C901  (complexity is inherent to a filter)
    """Return False if *line* is mostly noise (safe to drop for a short human/LLM-friendly log).

    Lines containing ``...`` are dropped unless :func:`_keep_for_signal_hint` finds errors,
    tracebacks, ``*Error:``, CTest ``***Failed`` / ``***Timeout``, etc.

    Lines mentioning the Docker workdir ``/workspace`` or the ``[]`` placeholder are dropped
    for the same reason once diagnostics have been ruled out (see signal hint).
    """
    # ── tier 1: force-keep (phase markers, CTest summaries, tracebacks) ──────────────────────
    if any(s in line for s in _FORCE_KEEP_SUBSTR):
        return True
    if any(r.search(line) for r in _FORCE_KEEP_RES):
        return True

    # ── tier 2: unconditional early drops (noise even when words like "failed" appear) ───────

    # FreeCAD tagged traces at line start (e.g. "<Sketch> SketchObject.cpp(...)").
    if _FREECAD_INTERNAL_TAG_RE.match(line.strip()):
        return False

    # FreeCAD tagged traces with a leading numeric timestamp (e.g. "2.7e-08 <App> ...").
    if re.match(r"^\d[\d.e+-]*\s+<[^>]+>", line):
        return False

    # OCCT progress lines — drop before signal-hint because garbled variants embed real error
    # fragments (e.g. "Preparing histo<Sketch> ... Failed to make face") that would be kept.
    if _OCCT_PROGRESS_LINE_RE.match(line):
        return False

    # Docstring-style verbose test description prints: "Verify X...", "Create X...", etc.
    # These prints from test methods may embed FreeCAD error tags that signal_hint would keep.
    # Use a broader pattern here (no trailing-dots requirement) compared to _DOCSTYLE_LONG_DESC_RE.
    if re.match(r"(?i)^(?:Create|Convert|Verify|Tests that|Test the)\s+\w[\w\s-]{10,}", line.strip()):
        return False

    # Python FreeCAD-style module logger: "ModuleName.LEVEL: message".
    # Covers SetupSheet.INFO, Processor.WARNING, machine.WARNING, DrillCycleExpander.DEBUG, etc.
    # These are test-context diagnostic messages, not primary failure indicators.
    if re.match(r"^[A-Za-z][A-Za-z0-9_.]+\.(?:DEBUG|INFO|WARNING|ERROR):", line):
        return False

    # CAM migration / FreeCAD migration logger messages.
    if "migration.INFO:" in line or "migration.WARNING:" in line:
        return False

    # ── tier 3: test-result lines — must be resolved BEFORE signal-hint ──────────────────────

    # Explicit test FAIL/ERROR suffix — keep unconditionally (real failures).
    if re.search(r" \.\.\. (?:FAIL|ERROR)\s*$", line):
        return True

    # Passed / skipped test output lines — drop even if description contains "exception" etc.
    if re.search(r" \.\.\. ok\s*$", line):
        return False
    if re.search(r"(?i) \.\.\. skipped", line):
        return False

    # Drop test-method headers (with or without inline continuation content).
    # "testFoo (mod.Class.testFoo) ... <Sketch>...: Failed to make face ..." is noise.
    if _UNITTEST_HEADER_LINE_RE.match(line):
        return False

    # Drop any line where a FreeCAD internal tag appears after " ... " (docstring-style output).
    if re.search(r" \.\.\. <[^>]{1,80}>", line):
        return False

    # Drop lines containing "... Module.LEVEL:" (test description + embedded logger warning)
    # BEFORE signal-hint, since the embedded "Failed" in the message is not a real test failure.
    if re.search(r"\.\.\.\s+[A-Za-z][A-Za-z0-9_.]+\.(?:WARNING|INFO|DEBUG|ERROR):", line):
        return False

    # Spreadsheet expected test-error messages (test verifies formula errors, not real failures).
    # Use substring check because the line may be prefixed with "Recompute...." noise.
    if "Spreadsheet: One or more cells" in line:
        return False

    # ── tier 4: signal-hint keep (real errors, tracebacks, assertions) ───────────────────────
    if _keep_for_signal_hint(line):
        return True

    # ── tier 5: path / placeholder drops (diagnostics already ruled out above) ───────────────
    if "/workspace" in line and not line.startswith("In file included from") and ": warning:" not in line:
        return False
    if "[]" in line:
        return False

    # ── tier 6: high-volume regex noise patterns ──────────────────────────────────────────────
    if any(r.search(line) for r in _NOISE_LINE_RES):
        return False

    # CTest per-test chatter.
    if _CTEST_START_LINE_RE.match(line):
        return False
    if _CTEST_PASSED_RESULT_RE.search(line) and re.search(r"(?i)\bpassed\b", line):
        return False
    if _CTEST_PASSED_RESULT_RE.search(line) and re.search(r"\*\*\*(Skipped|Not Run)", line):
        return False

    # Dot-only progress lines and standalone "ok".
    if re.fullmatch(r"\.+", line.strip()):
        return False
    if re.fullmatch(r"ok\s*", line.strip()):
        return False

    # Progress / chatter lines using ASCII ellipsis.
    if "..." in line and not _keep_for_signal_hint(line):
        return False

    # CAM / CNC specific noise.
    if _GCODE_MOTION_LINE_RE.match(line):
        return False
    if re.match(r"^\s*Processing\.+", line):
        return False
    if _OCCT_PROGRESS_LINE_RE.match(line):
        return False
    # Full G-code program lines (beyond motion-only lines above).
    if _GCODE_PROGRAM_LINE_RE.match(line):
        return False

    if re.search(r"(?i)\bAdded Group\b", line):
        return False

    # Progress percentage markers: "(50 %)", "(50 %)\tok", garbled "(ok" fragments.
    if re.match(r"^\s*\(\s*\d+\s*%\s*\)\s*(?:ok\s*)?$", line):
        return False
    if re.match(r"^\s+\((?:ok)?\s*$", line):
        return False

    if re.match(r"(?i)^\s*Importing\b", line):
        return False
    if re.match(r"(?i)^Import\s+.+\.\.\.\s*$", line):
        return False
    if re.search(r"\bTest Testing\b", line):
        return False
    if _DOCSTYLE_LONG_DESC_RE.match(line.strip()):
        return False
    if re.search(r"\.\.\..*FreeCAD\s+\d+\.\d+", line):
        return False

    # Spreadsheet / expression chatter.
    if line.startswith("Spreadsheet: One or more cells"):
        return False
    if line.startswith("in expression:"):
        return False

    if re.match(r"(?i)^Migrating\s+.+\bdone\.?\s*$", line.strip()):
        return False

    # GUI test runner chatter.
    if line.startswith("Using FreeCAD executable:"):
        return False
    if line.startswith("Found GUI test modules:"):
        return False
    if line.startswith("Running GUI tests for module:"):
        return False
    if re.match(r"^ {3}Test\w+Gui\s*$", line):
        return False

    # FreeCAD splash lines.
    _splash = line.strip()
    if any(r.match(_splash) for r in _FREECAD_SPLASH_LINE_RES):
        return False

    if line.startswith("✨ Pixi task"):
        return False
    if re.match(r"^\s*=+\s*$", line):
        return False
    if "Now run 'cmake --build" in line:
        return False
    if re.fullmatch(r"git\s*", line.strip()):
        return False
    if line.startswith("debconf:") or line.startswith("invoke-rc.d:"):
        return False
    if line.startswith("Processing triggers for"):
        return False
    if line.startswith("Label Time Summary:") or "sec*proc" in line:
        return False

    # Compiler warning / note / caret noise.
    # NOTE: warning: lines are kept so agents can spot build regressions.
    if re.match(r"^\s*\d+ \|", line):
        return False
    if re.match(r"^\s+\|\s+\^", line):  # caret-indicator line
        return False
    if re.match(r"^\s+\|\s*$", line):
        return False
    if re.match(r"^\d+ warnings? generated\.?\s*$", line, re.IGNORECASE):
        return False

    # FreeCAD CLI / GUI test verbose output.
    if re.match(r"^  Temporary document ", line):
        return False
    if re.match(r"^  Test '", line):
        return False
    if re.match(r"^  Try importing ", line):
        return False
    if re.match(r"^import Test[A-Za-z]", line):
        return False
    if re.match(r"^Recompute", line):
        return False
    if re.match(r"^  Occasionally crashes", line):
        return False
    if re.match(r"^  [a-zA-Z_][a-zA-Z0-9_]*=", line):
        return False

    # ── tier 7: additional high-volume test-output patterns ──────────────────────────────────

    # CAM migration / property spam (often concatenated on same line with other content).
    if "New property added to" in line:
        return False
    if "Stock Material property is deprecated" in line:
        return False

    # CAM test repeated startup marker.
    if line.strip() == "Startup!":
        return False

    # MbD solver "Time = 0" companion line.
    if re.fullmatch(r"Time\s*=\s*\d+(\.\d+)?", line.strip()):
        return False

    # CAM adaptive / drill / spindle output.
    if re.match(
        r"^(Tool Diameter:|Min step size:|Spindle speed will be controlled|Boundary check:|"
        r"Invalid substitution strings will be ignored|\*\* Processing region:)",
        line,
    ):
        return False
    if line.strip() == "All cleared.":
        return False

    # CAM machine compatibility / toolhead messages.
    if re.match(r"^Machine\b.+\b(?:compatible|toolhead)\b", line):
        return False
    if re.match(r"^ Choose from ", line):
        return False

    # CAM test section labels ("=== Pentagram Test: ... ===").
    if re.match(r"^=== .+ ===\s*$", line):
        return False

    # CAM G-code axis-label lines ("Angled rectangle long axis G-code:").
    if re.match(r"^(?:Angled rectangle|long axis|short axis)\b.*G-code:\s*$", line):
        return False

    # TestPathLog test-logger lines (TestPathLog.LEVEL: ... and TestPathLog(line).method(...)).
    if re.match(r"^TestPathLog[.(]", line):
        return False

    # "Show editor = N" / lone "here" debug prints in CAM tests.
    if re.match(r"^Show editor\s*=\s*\d+\s*$", line):
        return False
    if line.strip() == "here":
        return False

    # FEM test verbose output.
    if re.match(
        r"^(doc objects count:|PropertyPostDataObject::SaveDocFile:|"
        r"load (?:old|master head) document objects|AddGroupElements:|"
        r"One monster input file)",
        line,
    ):
        return False
    if "One monster input file" in line:
        return False
    if re.match(r"^(Writing|Input file:).*[Ff][Ee][Mm]", line):
        return False
    if re.match(r"^(Getting mesh data time|Writing time CalculiX input file):", line):
        return False
    if re.match(r"^\[1,\s*\(1,\s*2,\s*3", line):
        return False

    # Sketcher solver / geometry messages (expected behavior in passing tests).
    if re.match(r"^(Invalid solution from |Updating geometry: Error build geometry)", line):
        return False

    # TechDraw test verbose output.
    if re.match(
        r"^(making a page|Page created|making a projection group|Group created|"
        r"adding views|added (?:Front|Left|Right|Top|Bottom|Rear)|removed |"
        r"testing getItemByLabel|Item Label:|recomputing document|"
        r"Anchor values set|Front.Anchor recomputed)",
        line,
    ):
        return False
    if re.match(r"^(View:|adding balloon)", line):
        return False

    # Document / thread test verbose output.
    if re.match(r"^Call from Python thread", line):
        return False
    if re.match(r"^(making |finished |box created|sphere created|Fusion created)\b", line):
        return False

    # DXF import information block.
    if re.match(
        r"^(DXF version:|File encoding:|File units:|Manual scaling factor:|Final scaling:|"
        r"Performance:|Import settings:|Entity counts:|System Blocks:|"
        r"Unsupported features:)",
        line,
    ):
        return False
    if re.match(r"^\s+-\s+(?:Import|C\+\+|Total|Final scaling|Use |Join|Manual|Entity|Other)", line):
        return False
    if re.match(r"^\s*\(\*\)", line):
        return False

    # FreeCAD object-type / namespace qualified prefix (e.g. "App::FeatureTest: ...").
    if re.match(r"^[A-Z][A-Za-z0-9_]*::[A-Za-z0-9_]+:", line):
        return False

    # Chamfer / Fillet / PartDesign operation warnings during passing tests.
    if re.match(r"^(?:Chamfer|Fillet|Revolution):\s", line):
        return False

    # CAM G-code drill test output.
    if re.match(r"^(?:starting position:|retract mode:)", line):
        return False
    if line.strip() in ("Testing drilling", "G80"):
        return False

    # Test Execution Times block (Adaptive / FEM per-test timing).
    if re.match(r"^Test Execution Times:\s*$", line):
        return False
    if re.match(r"^\s+\d+\.\d+s\s+\(\s*\d+\.\d+%\)", line):
        return False
    if re.match(r"^Total:\s+\d+\.\d+s", line):
        return False

    # Expected path geometry warning during CAM tests.
    if re.match(r"^Unable to resolve tool down linking path", line):
        return False

    # Tool property warnings during CAM tests.
    if re.match(r"^The selected tool has no\b", line):
        return False

    # CalculiX bare float lines (C <float>).
    if re.match(r"^C\s+\d+\.\d+\s*$", line):
        return False

    # Optional FEM solver backend missing.
    if re.match(r"^Module \w+ not found\.", line):
        return False

    # FEM object name headers (e.g. "FemMaterial:", "Fix_XYZ:", "ConstraintForce:").
    if re.match(r"^[A-Za-z][A-Za-z0-9_]+:\s*$", line):
        return False

    # FEM indented type annotations: "    Type: Fem::MaterialCommon, Name: ...".
    if re.match(r"^\s+Type:\s+[A-Z]", line):
        return False

    # FEM element set dictionaries e.g. [{'ccx_elset': ..., 'mat_obj_name': ...}].
    if re.match(r"^\[{", line):
        return False

    # VTK timestamped log lines (ERR|/WARN| format from FEM visualization).
    if re.match(r"^[\t ]*\d{4}-\d{2}-\d{2}\s+\d{2}:\d{2}:\d{2}\.\d+\s+\(", line):
        return False

    # "File written to ..." from postprocessor tests.
    if re.match(r"^File written to ", line):
        return False

    # "skipped '...'" lowercase from skip-reason direct prints.
    if re.match(r"^skipped '", line):
        return False

    # Standalone simple values printed in test output.
    if re.fullmatch(r"\d+", line.strip()):
        return False
    if re.match(r"^TestWorkbench[A-Z]\s*$", line):
        return False
    if line.strip() in ("testpath", "Automatic PASS"):
        return False

    # Placement debug prints from assembly tests.
    if re.match(r"^\s*(?:plc|targetPlc) '", line):
        return False

    # Thread test output.
    if re.match(r"^on thread ", line):
        return False

    # "Printing error/warning/message" labels (FreeCAD console API test output).
    if re.match(r"^\s+Printing (?:error|warning|message)\s*$", line):
        return False

    # Attribute access messages from FeatureTest.
    if re.match(r"^Attribute:\s+No such attribute\b", line):
        return False

    # FEM material property names printed one per line.
    if re.match(
        r"^(?:AngleOfFriction|CompressiveStrength|Density|ShearModulus|"
        r"UltimateTensileStrength|YieldStrength|YoungsModulus|Stiffness|"
        r"ThermalExpansionCoefficient|SpecificHeat|ThermalConductivity|PoissonRatio|"
        r"ElectricalConductivity|FlexuralStrength|ImpactStrength|"
        r"ThermalResistivity|FractureToughness|RelativePermittivity)\s*$",
        line,
    ):
        return False

    # Garbled unittest header: "(test_name (Module.Class.test_name)".
    if re.match(r"^\s*\(test_\w+\s+\(", line):
        return False

    # Standalone garbled completion fragment: "Iok" (truncated "Initializing … ok").
    if re.fullmatch(r"Iok\s*", line):
        return False

    # Try importing… (FEM / module import attempt lines without leading spaces).
    if re.match(r"^Try importing\b", line):
        return False

    # Garbled progress prefixes: "      (36 %)machine.WARNING:...", "(N %)Some text".
    if re.match(r"^\s*\(\d+", line) and not _keep_for_signal_hint(line):
        return False

    # Garbled "Processing…ok" / "migration…ok" / "Material…ok" lines from terminal output mixing.
    if re.match(r"^(?:Proc|Proce|Proces|Process|Processi|Processin|Postprocess|Material|Solver|Mesh|Fem|Iok)\w*ok\s*$", line):
        return False
    # Broad single-word garbled ok: capital-led word (no spaces) ending in "ok".
    if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*ok\s*", line):
        return False
    if re.match(r"^(?:migr|migra|migrat|Try\s+imp)\w*ok\s*$", line):
        return False
    if "migration.IN" in line and not _keep_for_signal_hint(line):
        return False

    # PartDesign body / shape messages (garbled with progress prefix or standalone).
    if re.match(r"^\s*\(?Body\d+:\s", line):
        return False

    # FreeCAD internal object message blocks inside test output.
    if re.match(r"^\s*\((?:App|Part|Sketcher|PartDesign|CAM|FEM|TechDraw)::", line):
        return False

    # Draft module verbose result prints.
    if re.match(
        r"^(?:Found \d+|Found several|Found closed|Found only|Found points|"
        r"Unable to (?:downgrade|upgrade))\b",
        line,
    ):
        return False
    if re.match(r"^\s+\d+:\s+Result '", line):
        return False
    if re.match(r"^\s+The last object", line):
        return False
    if re.match(r"^FreeCAD objects created:", line):
        return False

    # Hash-bar section markers and "not implemented" labels in test output.
    if re.match(r"^#[-# ]*$", line):
        return False
    if re.match(r"^#\s+This test is not implemented", line):
        return False

    # Debug print assignments: "; p2=..." from test debug output.
    if re.match(r"^;\s+\w+\s*=", line):
        return False

    # Bytes literals printed from unit / conversion tests.
    if re.match(r"^b'[^']*'\s*$", line):
        return False

    # FEM mesh / solver verbose output.
    if re.match(
        r"^(?:Beam rotations|std search:|binary search:|Checking FEM inp f|"
        r"Constraint:\s+\w+.*-->\s+We're going to search|Count finite elements|"
        r"Save FreeCAD file for\b|Materials\s*$|"
        r"Fix_[A-Z]+:\s*$|ConstraintFix\w+|ElectricalConductivity|"
        r"Preparing\b.*\[{|\w+beam_axis_m\b|Get mesh data for\b)",
        line,
    ):
        return False

    # Elmer / Z88 FEM solver verbose output.
    if re.match(
        r"^(?:Saved unit schema:|"
        r"Write (?:elmer|z88) input files to:|"
        r"The (?:FreeCAD standard|SI) unit schema|"
        r"Test writing (?:STARTINFO|case) file|"
        r"Writing time input file:|"
        r"Reset unit schema back to|"
        r"'Coordinate Scaling Revert)",
        line,
    ):
        return False
    # Z88 file list literals: ['z88i5.txt', ...]
    if re.match(r"^\['z88", line):
        return False
    # Garbled FEM comparison and Gmsh lines.
    if re.match(r"^(?:Comparing\b|Filling splits of Gmsh\b|ProcGmsh\b)", line):
        return False

    # FEM: missing external Gmsh binary (CI or minimal installs); examples/tests use fallback meshes.
    if re.match(
        r"^(?:Unexpected error when creating mesh: )?"
        r"(?:Gmsh binary not found\.|Configured Gmsh binary '\$[^']+' not found\.)",
        line,
    ):
        return False

    # Garbled terminal fragment lines: lowercase fragment + apostrophe (e.g. "rial' for 375-16_Tap").
    if re.match(r"^[a-z][a-z]+'\s+for\s+\d", line):
        return False
    # Garbled short path fragments that start with hex-like chars and contain a slash.
    if re.match(r"^[a-z0-9]{4,}/\w+\.", line):
        return False
    # Garbled "Filling…" + uppercase (e.g. "FillinGmsh …") or "Processi…" + uppercase.
    if re.match(r"^(?:Fillin[A-Z]|Processi[A-Z])", line):
        return False
    # Garbled "P[['...list..."]] / "Prep[{..." / "Proc[{..." terminal mixing.
    if re.match(r"^P\w*\[\[|^P\w*\[{", line):
        return False

    # Postprocessor test description + embedded logger warning (line has "... Module.LEVEL:").
    if re.search(r"\.\.\.\s+[A-Za-z][A-Za-z0-9_.]+\.(?:WARNING|INFO|DEBUG):", line):
        return False

    # FEM material property values printed with units (e.g. "PoissonRatio 0.3", "YoungsModulus 2.1e+08 kg/(mm*s^2)").
    if re.match(
        r"^(?:PoissonRatio|YoungsModulus|SpecificHeat|ThermalConductivity|"
        r"ThermalExpansionCoefficient|Density|Stiffness|CompressiveStrength|"
        r"UltimateTensileStrength|YieldStrength|ShearModulus|AngleOfFriction|"
        r"ElectricalConductivity|FlexuralStrength)\s+[-\d.]",
        line,
    ):
        return False

    # FEM material appearance properties.
    if re.match(r"^(?:AmbientColor|DiffuseColor|EmissiveColor|SpecularColor|Shininess|Transparency)\s", line):
        return False

    # Sweet Home 3D importer verbose output.
    if re.match(
        r"^(?:Creating \d+ '|Creating slab#|Decorating \d+|"
        r"Successfully imported home|orting wall#|The joint width is too small|"
        r"No <\w+> tag found)",
        line,
    ):
        return False

    # WebGL / template test expected-failure messages.
    if re.match(r"^(?:Custom webgl template file|Export cancelled:|Using custom template file|Successfully written /tmp/)", line):
        return False

    # CTest / test-session separator lines (e.g. "ccx_cantilever_beam_pipe-------").
    if re.match(r"^[a-z][a-z0-9_]+[-]{3,}\s*$", line):
        return False

    # Set literals printed from CAM machine tests.
    if re.match(r"^\{['\"\w]", line) and line.strip().endswith("}"):
        return False
    if re.match(r"^Please add -", line):
        return False

    # Multi-word garbled ok lines (terminal output mixing).
    if re.match(r"^Call from Pyth", line):
        return False
    if re.match(r"^Get mesh data for\b", line):
        return False

    # TechDraw DrawView verbose test progress output.
    if re.match(r"^(?:DrawView\w+\s+test[:\s]|testing\s+DrawView|DVDTest\.)", line):
        return False

    # TopoShape test lifecycle markers.
    if re.match(r"^(?:running\s+\w+Test\b|\w+Test\s+finished\s*$|\w+Test:\s+setUp)", line):
        return False

    # Verbose test description prints from Arch / PartDesign test methods.
    # "Test phase passed/failed" is force-kept earlier (tier 1) so the negative lookahead
    # here is a safety net only.
    if re.match(r"^Test\s+(?!phase\b)", line):
        return False

    # PartDesign / geometry attachment offset conversion noise.
    if re.match(r"^Converting attachment offset of\b", line):
        return False

    # PartDesign body with empty tip shape (expected during tests, not a real error).
    if re.match(r"^Body:\s+Tip shape is empty", line):
        return False

    # Short quoted-string debug prints: 'abcef', 'abc_ef', etc.
    if re.match(r"^'[^']{0,30}'\s*$", line):
        return False

    # Lines ending with Unicode ellipsis (…) are Sweet Home 3D / importer progress noise.
    if line.rstrip().endswith("\u2026"):
        return False

    # Indented verbose test output (object names, descriptions, property values).
    # At this point signal_hint already ran and returned False, so there are no error signals.
    # Real traceback frames ("  File "...") are caught earlier by signal_hint; CTest failure
    # lines ("  42 - Foo (Failed)") are also kept earlier.
    if re.match(r"^[\t ]{2}|^\t", line):  # 2+ spaces OR 1+ tabs → verbose indent
        return False

    # 1-space-indented debug assignment prints (e.g. " numOfSteps = obj.NumberOfSteps - 14").
    if re.match(r"^ \w+ = \w+\.", line):
        return False

    stripped = line.lstrip()
    if stripped.startswith("--"):
        # CMake configure spam: "-- Found X:", "-- Checking Y", etc.
        if not re.search(r"(?i)(fail|error|warn)", stripped):
            return False
    return True


class _FilteredLogPipeline:
    """Streaming filter state (CMake warning blocks, CTest skip listings, blank collapse)."""

    __slots__ = (
        "_skipping_cmake_warning",
        "_skipping_did_not_run",
        "_skipping_bash_script",
        "_prev_blank",
    )

    def __init__(self) -> None:
        self._skipping_cmake_warning = False
        self._skipping_did_not_run = False
        self._skipping_bash_script = False
        self._prev_blank = False

    def feed(self, raw_line: str) -> str | None:
        """Return text to append to a filtered log/console stream, or None to omit."""

        # Skip the embedded bash script body that appears in "Command: docker run ... bash -lc '...'"
        # metadata blocks written to the log.  The opening Command: line itself is dropped (not
        # useful in filtered output); each subsequent line of the script body is dropped until the
        # closing lone-apostrophe line is consumed.
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
            elif (
                re.match(r"^\s*=+\s*$", raw_line)
                or "==========" in raw_line
                or raw_line.startswith("--")
            ):
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
        if out.strip() == "":
            if self._prev_blank:
                return None
            self._prev_blank = True
        else:
            self._prev_blank = False
        return out


def filter_log_lines(lines: Iterable[str]) -> Iterator[str]:
    """Yield only lines that belong in a noise-reduced log."""
    pipe = _FilteredLogPipeline()
    for raw_line in lines:
        out = pipe.feed(raw_line)
        if out is not None:
            yield out


def filter_log(text: str) -> str:
    """Return *text* with noisy lines removed (newline-preserving)."""
    return "".join(filter_log_lines(text.splitlines(True)))


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
    filtered_path = log_path.with_name(f"{log_path.stem}.filtered{log_path.suffix}") if args.filtered_log else None

    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"Started: {dt.datetime.now().isoformat(timespec='seconds')}\n")
        log.write(f"Repository: {repo_root}\n")
        log.write(f"Docker image: {args.image}\n")
        log.write(f"Config: {args.config}\n")
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
            cwd=str(repo_root),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )

        assert process.stdout is not None
        pipe: _FilteredLogPipeline | None = None if args.full_console and filtered_file is None else _FilteredLogPipeline()
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
    parser.add_argument(
        "--filter-console",
        action="store_true",
        help="Compatibility option (no-op): filtered console output is already the default. "
        "Use --full-console to print the complete Docker stream.",
    )
    parser.add_argument(
        "--full-console",
        action="store_true",
        help="Print the complete Docker stream to the terminal. Default is filtered output only; "
        "the full stream is always written to the log file.",
    )
    parser.add_argument(
        "--filtered-log",
        action="store_true",
        help="Also write <stem>.filtered.log next to the main log (same rules as filter_log()).",
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
