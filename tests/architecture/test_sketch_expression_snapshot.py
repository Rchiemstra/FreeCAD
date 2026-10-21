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
    admission = source.split("SketchExpressionDiagnostics::readAdmission()", 1)[1].split(
        "SketchExpressionDiagnostics::refresh(", 1)[0]
    reservation = admission.index("App::LivePresentationRefreshScope")
    admitted = admission.index("->acquired()", reservation)
    assert "return" in admission[admitted:admission.index("getDocuments()")]
    assert admitted < admission.index("deferLivePresentationUpdates()")
    assert capture.index("readAdmission()") < capture.index("Base::PyGILStateLocker")
    assert "return false" in capture[:capture.index("getPyObject()")]
    assert "shared_ptr<const" in header
    assert "constraintErrors.clear()" not in capture
    solver = (GUI / "ViewProviderSketch.cpp").read_text().split(
        "void ViewProviderSketch::slotSolverUpdate()", 1)[1].split(
        "void ViewProviderSketch::slotConstraintAdded", 1)[0]
    assert solver.index("readAdmission()") < solver.index("getSketchObject()")
