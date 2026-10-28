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


def test_solver_dispatches_to_gui_before_diagnostic_admission_and_retries_refusal():
    solver = (GUI / "ViewProviderSketch.cpp").read_text().split(
        "void ViewProviderSketch::slotSolverUpdate()", 1)[1].split(
        "void ViewProviderSketch::slotConstraintAdded", 1)[0]
    dispatch = solver.index("Gui::scheduleGuiSingleShot(0")
    admission = solver.index("expressionDiagnostics.readAdmission()")
    assert solver.index("!App::DocumentWouldBlock::isGuiThread()") < dispatch < admission
    refused = solver[admission:solver.index("getSketchObject()")]
    assert "!admission" in refused
    assert "queueRedraw(false, true)" in refused


def test_constraint_diagnostic_roles_capture_owned_row_inputs():
    source = (GUI / "TaskSketcherConstraints.cpp").read_text()
    row = source.split("class ConstraintItem:", 1)[1].split("class ExpressionDelegate:", 1)[0]
    queries = row.split("~ConstraintItem()", 1)[1]
    assert "sketch->" not in queries
    assert "presentation.capture(" in queries
    publish = source.split("void TaskSketcherConstraints::slotConstraintsChanged()", 1)[1]
    assert publish.index("it->inputs = inputs") < publish.index("it->capturePresentation()")
