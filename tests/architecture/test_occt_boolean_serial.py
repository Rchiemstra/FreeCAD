# SPDX-License-Identifier: LGPL-2.1-or-later
"""Boolean-family OCCT parallel mode is decided in one Part helper.

Literals ``SetRunParallel(true)`` / ``SetRunParallel(Standard_True)`` and any
``SetParallelMode(`` call belong in ``src/Mod/Part/App/BooleanParallel.cpp``
(and its header). Every other production source must go through
``Part::booleanRunParallel()`` or ``Part::setBooleanParallelMode()``.
"""

from __future__ import annotations

import os
from pathlib import Path
import re

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


def _offenders():
    found = []
    for path, relative in _production_sources():
        raw = path.read_text(encoding="utf-8", errors="surrogateescape")
        if "SetRunParallel" not in raw and "SetParallelMode" not in raw:
            continue
        text = _suppress_cpp(raw)
        for lineno, line in enumerate(text.splitlines(), 1):
            if RUN_PARALLEL.search(line) or PARALLEL_MODE.search(line):
                found.append(f"{relative}:{lineno}:{line.strip()}")
    return found


def test_boolean_parallel_literals_stay_in_the_helper():
    offenders = _offenders()
    assert not offenders, "\n".join(offenders)


def test_helper_owns_the_process_wide_parallel_mode():
    helper = REPOSITORY / "src/Mod/Part/App/BooleanParallel.cpp"
    assert helper.is_file(), helper
    code = _suppress_cpp(helper.read_text(encoding="utf-8"))
    assert PARALLEL_MODE.search(code), "helper must be the only SetParallelMode caller"
