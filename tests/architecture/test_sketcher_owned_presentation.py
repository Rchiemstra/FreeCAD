"""Boundaries that must hold even when a Coin/Qt notification reenters edit mode."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUI = ROOT / "src/Mod/Sketcher/Gui"


def source(name):
    return (GUI / name).read_text()


def section(name, begin, end):
    return source(name).split(begin, 1)[1].split(end, 1)[0]


def test_redraw_retry_admits_before_resolving_document():
    retry = section("ViewProviderSketch.cpp", "void ViewProviderSketch::queueRedraw", "void ViewProviderSketch::setIsShownVirtualSpace")
    callback = retry.split("[documentName, objectName, session]", 1)[1]
    assert callback.index("refresh.acquired()") < callback.index("App::GetApplication().getDocument")
    refused = callback.split("!refresh.acquired()", 1)[1].split("App::GetApplication().getDocument", 1)[0]
    assert "schedule" in refused.lower() or "retry" in refused.lower()


def test_render_retains_manager_and_checks_session_after_notifications():
    header = source("ViewProviderSketch.h")
    assert "std::shared_ptr<EditModeCoinManager> editCoinManager" in header
    render = section("ViewProviderSketch.cpp", "void ViewProviderSketch::draw(bool", "void ViewProviderSketch::queueRedraw")
    assert "isRedrawCurrent" in render
    process = section("EditModeCoinManager.cpp", "void EditModeCoinManager::processGeometryConstraintsInformationOverlay", "void EditModeCoinManager::updateOverlayParameters")
    assert "isRedrawCurrent" in process


def test_delegates_use_owned_rows():
    row = section("TaskSketcherConstraints.cpp", "class ConstraintItem:", "class ExpressionDelegate:")
    # Construction may capture the model; all const row queries must be independent.
    queries = row.split("~ConstraintItem()", 1)[1]
    assert "sketch->Constraints" not in queries
    assert "sketch->getExpression" not in queries
    paint = section("TaskSketcherConstraints.cpp", "class ExpressionDelegate:", "ConstraintView::ConstraintView")
    assert "item->sketch->" not in paint
    visible = section("TaskSketcherElements.cpp", "bool isVisible() const", "QVariant data(int role)")
    assert "getSketchObject" not in visible


def test_removed_external_curve_is_checked_before_access():
    colors = section("EditModeGeometryCoinManager.cpp", "void EditModeGeometryCoinManager::updateGeometryColor", "void EditModeGeometryCoinManager::create")
    external = colors.split("else if (isExternal) {", 1)[1]
    assert external.index("if (!geom)") < external.index("geom->clone()")


def test_grouping_does_not_resolve_live_sketch_in_converters():
    for name in ("EditModeGeometryCoinConverter.cpp", "EditModeInformationOverlayCoinConverter.cpp", "EditModeGeometryCoinManager.cpp"):
        text = source(name)
        assert "obj->isInGroup" not in text, name
        assert "obj->getGroupHandleIfInGroup" not in text, name


def test_element_publication_reconciles_current_selection():
    publish = section("TaskSketcherElements.cpp", "// Drop buffered references", "void TaskSketcherElements::clearWidget")
    assert "Gui::Selection().getSelection" in publish
    assert "isStartingPointSelected" in publish
    assert "isMidPointSelected" in publish
