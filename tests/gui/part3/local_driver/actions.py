# SPDX-License-Identifier: LGPL-2.1-or-later
"""Qt owner-thread personal-view actions for Part 3 LocalUserDriver."""

from __future__ import annotations

from typing import Any

SUPPORTED_ACTIONS = frozenset(
    {
        "preflight",
        "rotate_camera",
        "pan_view",
        "zoom_view",
        "fit_all",
        "select_object",
        "clear_selection",
        "expand_tree",
        "collapse_tree",
        "set_active_document",
        "pause_writes",
        "resume_writes",
        "view_state",
        "local_property_edit",
        "local_save",
        "provision_alpha_beta_fixture",
        "async_blocker_control",
        "close_document",
        "reset_property_editor",
    }
)


def execute(action: str, params: dict[str, Any]) -> dict[str, Any]:
    if action not in SUPPORTED_ACTIONS:
        raise ValueError(f"unsupported action: {action}")
    dispatch = {
        "preflight": _preflight,
        "rotate_camera": _rotate_camera,
        "pan_view": _pan_view,
        "zoom_view": _zoom_view,
        "fit_all": _fit_all,
        "select_object": _select_object,
        "clear_selection": _clear_selection,
        "expand_tree": _expand_tree,
        "collapse_tree": _collapse_tree,
        "set_active_document": _set_active_document,
        "pause_writes": _pause_writes,
        "resume_writes": _resume_writes,
        "view_state": _view_state,
        "local_property_edit": _local_property_edit,
        "local_save": _local_save,
        "provision_alpha_beta_fixture": _provision_alpha_beta_fixture,
        "async_blocker_control": _async_blocker_control,
        "close_document": _close_document,
        "reset_property_editor": _reset_property_editor,
    }
    return dispatch[action](params or {})


def _active_view():
    import FreeCADGui

    active_document = FreeCADGui.ActiveDocument
    if active_document is None:
        raise RuntimeError("no active GUI document")
    view = active_document.ActiveView
    if view is None:
        raise RuntimeError("no active 3D view")
    if hasattr(view, "setAnimationEnabled"):
        view.setAnimationEnabled(False)
    return view


def _require_pause_checkbox():
    import FreeCADGui
    from PySide import QtWidgets

    main_window = FreeCADGui.getMainWindow()
    if main_window is None:
        raise RuntimeError("pauseAgentWrites checkbox missing - hard setup failure")
    checkbox = main_window.findChild(QtWidgets.QCheckBox, "pauseAgentWrites")
    if checkbox is None:
        raise RuntimeError("pauseAgentWrites checkbox missing - hard setup failure")
    if not bool(checkbox.property("mcpAutomationPauseConnected")):
        raise RuntimeError(
            "pauseAgentWrites is not wired by FreeCADMCP - hard setup failure"
        )
    return checkbox


def _preflight(_params: dict[str, Any]) -> dict[str, Any]:
    try:
        from FreeCADMCP.lock_indicator_ops.document_changes_controls import (
            refresh_document_changes_controls,
        )
    except ImportError:
        from lock_indicator_ops.document_changes_controls import (  # type: ignore
            refresh_document_changes_controls,
        )
    refresh_document_changes_controls()
    checkbox = _require_pause_checkbox()
    return {
        "pause_checkbox_visible": bool(checkbox.isVisible()),
        "pause_checkbox_wired": True,
    }


def _rotate_camera(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD

    view = _active_view()
    named_view = str(params.get("named_view") or "").strip()
    if named_view:
        method = getattr(view, f"view{named_view[:1].upper()}{named_view[1:]}", None)
        if not callable(method):
            raise ValueError(f"unknown named view: {named_view}")
        method()
    else:
        yaw = float(params.get("yaw", 15.0))
        pitch = float(params.get("pitch", 10.0))
        roll = float(params.get("roll", 0.0))
        orientation = FreeCAD.Rotation(yaw, pitch, roll)
        view.setCameraOrientation(orientation)
    return {
        "camera_orientation": str(view.getCameraOrientation()),
    }


def _pan_view(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD

    view = _active_view()
    placement = view.viewPosition()
    if placement is None:
        raise RuntimeError("viewPosition returned no placement")
    delta = FreeCAD.Vector(
        float(params.get("dx", 0.1)),
        float(params.get("dy", 0.0)),
        float(params.get("dz", 0.0)),
    )
    moved = FreeCAD.Placement(placement.Base + delta, placement.Rotation)
    view.viewPosition(moved, 0, 0)
    after = view.viewPosition()
    return {
        "view_position": str(after),
    }


def _zoom_view(params: dict[str, Any]) -> dict[str, Any]:
    view = _active_view()
    direction = str(params.get("direction") or "in").lower()
    if direction == "out":
        view.zoomOut()
    else:
        view.zoomIn()
    return {"direction": direction}


def _fit_all(params: dict[str, Any]) -> dict[str, Any]:
    view = _active_view()
    factor = float(params.get("factor", 1.0))
    view.fitAll(factor)
    return {"factor": factor}


def _select_object(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    document_name = str(params.get("document") or "")
    object_name = str(params.get("object") or "")
    if not document_name or not object_name:
        raise ValueError("document and object are required")
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(document_name, object_name)
    return {
        "selection_count": len(FreeCADGui.Selection.getSelection()),
        "selected_object": object_name,
    }


def _clear_selection(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    FreeCADGui.Selection.clearSelection()
    return {"selection_count": 0}


def _expand_tree(params: dict[str, Any]) -> dict[str, Any]:
    return _toggle_tree(params, mod=2)


def _collapse_tree(params: dict[str, Any]) -> dict[str, Any]:
    return _toggle_tree(params, mod=1)


def _toggle_tree(params: dict[str, Any], *, mod: int) -> dict[str, Any]:
    import FreeCAD
    import FreeCADGui

    document_name = str(params.get("document") or "")
    object_name = str(params.get("object") or "")
    if not document_name or not object_name:
        raise ValueError("document and object are required")
    document = FreeCAD.getDocument(document_name)
    obj = document.getObject(object_name)
    if obj is None:
        raise ValueError(f"object not found: {object_name}")
    gui_document = FreeCADGui.getDocument(document_name)
    gui_document.toggleTreeItem(obj, mod)
    return {"document": document_name, "object": object_name, "mod": mod}


def _set_active_document(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    document_name = str(params.get("document") or "")
    if not document_name:
        raise ValueError("document is required")
    FreeCADGui.setActiveDocument(document_name)
    active = FreeCADGui.ActiveDocument
    return {
        "active_document": None if active is None else str(active.Document.Name),
    }


def _pause_writes(_params: dict[str, Any]) -> dict[str, Any]:
    checkbox = _require_pause_checkbox()
    checkbox.setChecked(True)
    return {"paused": bool(checkbox.isChecked())}


def _resume_writes(_params: dict[str, Any]) -> dict[str, Any]:
    checkbox = _require_pause_checkbox()
    checkbox.setChecked(False)
    return {"paused": bool(checkbox.isChecked())}


def _view_state(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD
    import FreeCADGui

    active_document = FreeCADGui.ActiveDocument
    if active_document is None:
        return {"active_document": None}
    document = active_document.Document
    view = active_document.ActiveView
    state = {
        "active_document": str(document.Name),
        "selection": [
            item.ObjectName for item in FreeCADGui.Selection.getSelectionEx(str(document.Name))
        ],
        "touched": bool(document.isTouched()),
    }
    pending = getattr(document, "hasPendingFileChanges", None)
    state["modified"] = bool(pending()) if callable(pending) else bool(document.isTouched())
    file_state = document.getFileChangeState()
    if isinstance(file_state, dict):
        state["file_change_state"] = file_state
    identity = document.collaborationIdentity()
    uid = getattr(document, "Uid", None)
    uid_value = getattr(uid, "Value", uid)
    state["identity_selector"] = {
        "document_uid": str(uid_value or ""),
        "document_instance_id": int(identity["instance_id"]),
        "lifecycle_epoch": int(identity["lifecycle_epoch"]),
        "document_name": str(document.Name),
    }
    if view is not None:
        state["camera_orientation"] = str(view.getCameraOrientation())
        position = view.viewPosition()
        if position is not None:
            state["view_position"] = str(position)
    return state


def _property_editor_data():
    import FreeCADGui
    from PySide import QtWidgets

    main_window = FreeCADGui.getMainWindow()
    if main_window is None:
        raise RuntimeError("no main window")
    editor = main_window.findChild(QtWidgets.QWidget, "propertyEditorData")
    if editor is None:
        tab = main_window.findChild(QtWidgets.QTabWidget, "propertyTab")
        if tab is not None:
            widget = tab.currentWidget()
            if widget is not None and widget.objectName() == "propertyEditorData":
                editor = widget
    if editor is None:
        raise RuntimeError("propertyEditorData not found")
    return editor


def _wait_for_property_editor(editor, timeout_s: float = 5.0) -> None:
    import time

    from PySide import QtWidgets

    model = editor.model()
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        QtWidgets.QApplication.processEvents()
        if model.rowCount() > 0:
            return
        time.sleep(0.05)
    raise RuntimeError("property editor did not populate")


def _find_property_value_index(editor, property_name: str):
    from PySide import QtCore

    model = editor.model()
    root = QtCore.QModelIndex()
    normalized_name = property_name.replace("_", "")

    def walk(parent_index):
        for row in range(model.rowCount(parent_index)):
            idx0 = model.index(row, 0, parent_index)
            if model.rowCount(idx0) > 0:
                found = walk(idx0)
                if found.isValid():
                    return found
            label = str(idx0.data() or "")
            if label == property_name or label.replace(" ", "") == normalized_name:
                return model.index(row, 1, parent_index)
        return QtCore.QModelIndex()

    index = walk(root)
    if not index.isValid():
        raise ValueError(f"property not found in Property Editor: {property_name}")
    return index


def _apply_property_editor_value(editor, index, value) -> None:
    import FreeCAD

    from PySide import QtCore, QtWidgets

    model = editor.model()
    current = model.data(index, QtCore.Qt.EditRole)
    if isinstance(value, bool):
        variant = value
    elif isinstance(value, int) and not isinstance(value, bool):
        variant = int(value)
    elif hasattr(current, "Unit") or type(current).__name__.endswith("Quantity"):
        variant = FreeCAD.Units.Quantity(float(value), FreeCAD.Units.Length)
    else:
        variant = float(value)

    if not model.setData(index, variant, QtCore.Qt.EditRole):
        raise RuntimeError("property editor refused setData")
    QtWidgets.QApplication.processEvents()


def _local_property_edit(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD
    import FreeCADGui

    document_name = str(params.get("document") or "")
    object_name = str(params.get("object") or "")
    property_name = str(params.get("property") or "")
    if not document_name or not object_name or not property_name:
        raise ValueError("document, object, and property are required")
    if "value" not in params:
        raise ValueError("value is required")

    document = FreeCAD.getDocument(document_name)
    obj = document.getObject(object_name)
    if obj is None:
        raise ValueError(f"object not found: {object_name}")
    if property_name not in obj.PropertiesList:
        raise ValueError(f"property not found on object: {property_name}")

    FreeCADGui.setActiveDocument(document_name)
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.Selection.addSelection(document_name, object_name)
    gui_document = FreeCADGui.getDocument(document_name)
    if gui_document is not None:
        gui_document.toggleTreeItem(obj, 2)
    FreeCADGui.updateGui()

    editor = _property_editor_data()
    _wait_for_property_editor(editor, timeout_s=8.0)
    index = _find_property_value_index(editor, property_name)
    _apply_property_editor_value(editor, index, params["value"])

    after_value = getattr(obj, property_name)
    expected = params["value"]
    if isinstance(expected, bool):
        if bool(after_value) != bool(expected):
            raise RuntimeError(
                f"property editor did not apply {property_name}: "
                f"expected {expected!r}, got {after_value!r}"
            )
    elif isinstance(expected, int) and not isinstance(expected, bool):
        if int(after_value) != int(expected):
            raise RuntimeError(
                f"property editor did not apply {property_name}: "
                f"expected {expected!r}, got {after_value!r}"
            )
    else:
        if float(after_value) != float(expected):
            raise RuntimeError(
                f"property editor did not apply {property_name}: "
                f"expected {expected!r}, got {after_value!r}"
            )
    return {
        "document": document_name,
        "object": object_name,
        "property": property_name,
        "value": after_value,
    }


_PROP_NO_RECOMPUTE = 16


def _provision_alpha_beta_fixture(params: dict[str, Any]) -> dict[str, Any]:
    """Create StressBox.AlphaValue and SecondBox.BetaValue for Part 3 integration."""

    import FreeCAD

    document_name = str(params.get("document") or "")
    if not document_name:
        raise ValueError("document is required")
    alpha = int(params.get("alpha", 0))
    beta = int(params.get("beta", 0))
    with_blocker = bool(params.get("with_async_blocker", False))

    if document_name in FreeCAD.listDocuments():
        document = FreeCAD.getDocument(document_name)
    else:
        document = FreeCAD.newDocument(document_name)

    stress = document.getObject("StressBox")
    if stress is None:
        stress = document.addObject("App::FeatureTest", "StressBox")
    if "AlphaValue" not in stress.PropertiesList:
        stress.addProperty(
            "App::PropertyInteger",
            "AlphaValue",
            "Stress",
            "",
        )
    stress.AlphaValue = alpha

    second = document.getObject("SecondBox")
    if second is None:
        second = document.addObject("App::DocumentObject", "SecondBox")
    if "BetaValue" not in second.PropertiesList:
        second.addProperty(
            "App::PropertyInteger",
            "BetaValue",
            "Data",
            "",
            _PROP_NO_RECOMPUTE,
        )
    second.BetaValue = beta

    blocker_name = None
    if with_blocker:
        blocker = document.getObject("RecomputeBlocker")
        if blocker is None:
            blocker = document.addObject(
                "App::FeatureTestAsyncBlocker",
                "RecomputeBlocker",
            )
        blocker_name = str(blocker.Name)
        _async_blocker_reset()

    document.recompute()
    return {
        "document": document_name,
        "alpha": int(stress.AlphaValue),
        "beta": int(second.BetaValue),
        "async_blocker": blocker_name,
    }


def _reset_property_editor(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    active = FreeCADGui.ActiveDocument
    if active is not None:
        try:
            active.resetEdit()
        except Exception:
            pass
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.updateGui()
    return {"reset": True}


def _close_document(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD
    import FreeCADGui

    document_name = str(params.get("document") or "")
    if not document_name:
        raise ValueError("document is required")
    closed = False
    if document_name in FreeCAD.listDocuments():
        FreeCAD.closeDocument(document_name)
        closed = document_name not in FreeCAD.listDocuments()
    FreeCADGui.Selection.clearSelection()
    FreeCADGui.updateGui()
    return {"document": document_name, "closed": closed}


def _async_blocker_reset() -> None:
    import FreeCAD

    blocker_api = getattr(FreeCAD, "FeatureTestAsyncBlocker", None)
    if blocker_api is None:
        raise RuntimeError("FreeCAD.FeatureTestAsyncBlocker is unavailable")
    blocker_api.resetBlocker()
    blocker_api.releaseBlocker()


def _async_blocker_control(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD

    action = str(params.get("action") or "")
    document_name = str(params.get("document") or "")
    if not document_name:
        raise ValueError("document is required")
    document = FreeCAD.getDocument(document_name)
    blocker = document.getObject("RecomputeBlocker")
    if blocker is None:
        raise ValueError("RecomputeBlocker missing")
    blocker_api = getattr(FreeCAD, "FeatureTestAsyncBlocker", None)
    if blocker_api is None:
        raise RuntimeError("FreeCAD.FeatureTestAsyncBlocker is unavailable")

    if action == "reset":
        blocker_api.resetBlocker()
        return {"action": action}
    if action == "release":
        blocker_api.releaseBlocker()
        return {"action": action}
    if action == "touch_and_queue_recompute":
        blocker_api.resetBlocker()
        blocker.touch()
        FreeCAD.queueRecomputeRequest(
            FreeCAD.RecomputeRequest.fromDocumentObject(blocker)
        )
        return {"action": action, "queued": True}
    raise ValueError(f"unsupported async_blocker_control action: {action}")


def _local_save(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    if FreeCADGui.ActiveDocument is None:
        raise RuntimeError("no active GUI document")
    FreeCADGui.runCommand("Std_Save")
    return {"command": "Std_Save"}
