# SPDX-License-Identifier: LGPL-2.1-or-later
"""Sketch diagnostics must reserve document reads before capturing presentation."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUI = ROOT / "src/Mod/Sketcher/Gui"


def test_sketch_diagnostics_read_admission_and_immutable_publication():
    source = (GUI / "SketchExpressionDiagnostics.cpp").read_text()
    header = (GUI / "SketchExpressionDiagnostics.h").read_text()
    capture = source.split("SketchExpressionDiagnostics::refresh(", 1)[1].split(
        "SketchExpressionDiagnostics::reason(", 1)[0]
    reservation = capture.index("App::LivePresentationRefreshScope")
    admitted = capture.index(".acquired()", reservation)
    assert "return" in capture[admitted:capture.index("Base::PyGILStateLocker")]
    assert admitted < capture.index("deferLivePresentationUpdates()")
    assert admitted < capture.index("getPyObject()")
    assert "shared_ptr<const" in header
    assert "constraintErrors.clear()" not in capture
