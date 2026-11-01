# SPDX-License-Identifier: LGPL-2.1-or-later
"""Boolean-family OCCT parallel mode is decided in one Part helper.

Literals ``SetRunParallel(true)`` / ``SetRunParallel(Standard_True)`` and any
``SetParallelMode(`` call belong in ``src/Mod/Part/App/BooleanParallel.cpp``
(and its header). Every other production source must go through
``Part::booleanRunParallel()`` or ``Part::setBooleanParallelMode()``.

The scan runs on each suppressed file as a whole. A call split across lines
is still an offender, and the reported line is the line of the match start.
"""

from __future__ import annotations

import os
from pathlib import Path
import re

import pytest

from test_geometry_native_adapters import _suppress_cpp


REPOSITORY = Path(__file__).resolve().parents[2]
HELPERS = {
    Path("src/Mod/Part/App/BooleanParallel.cpp"),
    Path("src/Mod/Part/App/BooleanParallel.h"),
}
SOURCE_SUFFIXES = {".cpp", ".h", ".cxx", ".hpp", ".hxx"}
RUN_PARALLEL = re.compile(r"SetRunParallel\s*\(\s*(?:true|Standard_True)\s*\)")
PARALLEL_MODE = re.compile(r"SetParallelMode\s*\(")


def _production_sources():
    root = REPOSITORY / "src"
    for dirpath, dirnames, filenames in os.walk(root):
        if "3rdParty" in Path(dirpath).relative_to(REPOSITORY).parts:
            dirnames.clear()
            continue
        if "3rdParty" in dirnames:
            dirnames.remove("3rdParty")
        for name in filenames:
            path = Path(dirpath) / name
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(REPOSITORY)
            if relative in HELPERS:
                continue
            yield path, relative


def _offender_hits(text: str) -> list[tuple[int, str]]:
    """Return ``(line, evidence)`` for parallel literals in suppressed *text*.

    Patterns run on the whole string, so whitespace before the parenthesis or
    the boolean literal may contain newlines. ``line`` is 1-based and comes
    from the match offset. ``evidence`` is that source line, stripped.
    """
    lines = text.splitlines()
    found: list[tuple[int, int, str]] = []
    for pattern in (RUN_PARALLEL, PARALLEL_MODE):
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            evidence = lines[line - 1].strip() if line <= len(lines) else ""
            found.append((match.start(), line, evidence))
    found.sort()
    return [(line, evidence) for _start, line, evidence in found]


def _offenders():
    found = []
    for path, relative in _production_sources():
        raw = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "SetRunParallel" not in raw and "SetParallelMode" not in raw:
            continue
        for line, evidence in _offender_hits(_suppress_cpp(raw)):
            found.append(f"{relative}:{line}:{evidence}")
    return found


def test_boolean_parallel_literals_stay_in_the_helper():
    offenders = _offenders()
    assert not offenders, "\n".join(offenders)


def test_helper_owns_the_process_wide_parallel_mode():
    helper = REPOSITORY / "src/Mod/Part/App/BooleanParallel.cpp"
    assert helper.is_file(), helper
    code = _suppress_cpp(helper.read_text(encoding="utf-8"))
    assert PARALLEL_MODE.search(code), "helper must be the only SetParallelMode caller"


@pytest.mark.parametrize(
    ("source", "line"),
    [
        (
            "void splitRun()\n{\n    algo.SetRunParallel(\n        true);\n}\n",
            3,
        ),
        (
            "void splitMode()\n{\n    options.SetParallelMode\n        (true);\n}\n",
            3,
        ),
        (
            "void single()\n{\n    algo.SetRunParallel(Standard_True);\n}\n",
            3,
        ),
    ],
    ids=(
        "run-parallel-argument-on-next-line",
        "parallel-mode-paren-on-next-line",
        "run-parallel-single-line",
    ),
)
def test_parallel_literals_match_across_newlines(source: str, line: int) -> None:
    hits = _offender_hits(_suppress_cpp(source))
    assert [number for number, _evidence in hits] == [line]


@pytest.mark.parametrize(
    "source",
    [
        "// SetRunParallel(true)\n",
        "/* SetParallelMode(\n    true) */\n",
        'const char* note = "SetRunParallel(Standard_True)";\n',
        'const char* note = "SetParallelMode(";\n',
        'const char* note = R"(SetRunParallel(true))";\n',
        "algo.SetRunParallel(Part::booleanRunParallel()); // SetRunParallel(true)\n",
    ],
    ids=(
        "line-comment",
        "block-comment",
        "string-run-parallel",
        "string-parallel-mode",
        "raw-string",
        "trailing-comment",
    ),
)
def test_comments_and_strings_are_not_parallel_offenders(source: str) -> None:
    assert _offender_hits(_suppress_cpp(source)) == []


def test_suppressed_trivia_does_not_move_a_real_call() -> None:
    source = "\n".join(
        [
            "// SetRunParallel(true)",
            'const char* note = "SetParallelMode(";',
            "algo.SetRunParallel(",
            "    true);",
        ]
    )
    assert [number for number, _evidence in _offender_hits(_suppress_cpp(source))] == [3]
