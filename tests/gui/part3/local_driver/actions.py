# SPDX-License-Identifier: LGPL-2.1-or-later
"""Qt owner-thread personal-view actions for Part 3 LocalUserDriver."""

from __future__ import annotations

from typing import Any


_ASYNC_RECOMPUTE_HANDLES: dict[Any, Any] = {}


def _poll_async_recompute_handle(document, handle) -> None:
    from PySide import QtCore

    if _ASYNC_RECOMPUTE_HANDLES.get(document) is not handle:
        return
    if handle.done():
        _ASYNC_RECOMPUTE_HANDLES.pop(document, None)
        return
    QtCore.QTimer.singleShot(
        5,
        lambda: _poll_async_recompute_handle(document, handle),
    )


def _retain_async_recompute_handle(document, handle) -> None:
    from PySide import QtCore

    _ASYNC_RECOMPUTE_HANDLES[document] = handle
    QtCore.QTimer.singleShot(
        5,
        lambda: _poll_async_recompute_handle(document, handle),
    )


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
        "close_main_window",
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
        "close_main_window": _close_main_window,
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


def _view_state(params: dict[str, Any]) -> dict[str, Any]:
    import FreeCAD
    import FreeCADGui

    active_gui_document = FreeCADGui.ActiveDocument
    requested_name = str(params.get("document") or "")
    if not requested_name and active_gui_document is None:
        return {"active_document": None}
    if requested_name:
        document = FreeCAD.getDocument(requested_name)
        if document is None or str(document.Name) != requested_name:
            raise ValueError(f"document not found: {requested_name}")
        observed_gui_document = FreeCADGui.getDocument(requested_name)
    else:
        observed_gui_document = active_gui_document
        document = active_gui_document.Document
    active_name = (
        None
        if active_gui_document is None
        else str(active_gui_document.Document.Name)
    )
    view = (
        None if observed_gui_document is None else observed_gui_document.ActiveView
    )
    state = {
        "active_document": active_name,
        "observed_document": str(document.Name),
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


def _property_view_ancestor(widget):
    """The ``Gui::PropertyView`` that owns ``widget``, or ``None``.

    Ownership is structural rather than name-based: a PropertyView constructs
    its data editor as a descendant (PropertyView.cpp:93-94), so walking up to
    the owning view is what pins WHICH property view the harness is driving.
    """

    parent = widget.parentWidget()
    while parent is not None:
        if parent.metaObject().className() == "Gui::PropertyView":
            return parent
        parent = parent.parentWidget()
    return None


def _describe_widget(widget) -> str:
    view = _property_view_ancestor(widget)
    view_name = "none" if view is None else view.metaObject().className()
    view_visible = "n/a" if view is None else str(view.isVisible())
    return (
        f"{widget.metaObject().className()}(visible={widget.isVisible()}, "
        f"view={view_name}, view_visible={view_visible})"
    )


def _owned_by_visible_property_view(widget) -> bool:
    """Whether ``widget`` belongs to a PropertyView that can still rebuild.

    Visibility is checked on the OWNING VIEW, not on the editor page: a hidden
    PropertyView detaches from selection and builds up empty in hideEvent
    (PropertyView.cpp:188-197), and onTimer early-returns with an empty build
    while detached (PropertyView.cpp:391-396). The editor page itself may be the
    non-current tab and still be rebuilt correctly, so its own isVisible() is
    reported for diagnostics but is not a qualification test.
    """

    view = _property_view_ancestor(widget)
    return view is not None and view.isVisible()


def _property_editor_data():
    """Resolve the single usable Property Editor, or fail naming the ambiguity.

    The object name "propertyEditorData" is not unique in FreeCAD: every
    ``Gui::PropertyView`` names its data editor that way (PropertyView.cpp:93-94)
    and PropertyView is instantiated both in the Combo View (ComboView.cpp:56)
    and in the standalone dock (PropertyView.cpp:622). Taking the first match in
    Qt's traversal order could therefore bind to a detached view that never
    populates. This resolver requires exactly one candidate owned by a visible
    PropertyView and raises an explicit error otherwise (GRK-P3-088).
    """

    import FreeCADGui

    from PySide import QtWidgets

    main_window = FreeCADGui.getMainWindow()
    if main_window is None:
        raise RuntimeError("no main window")

    named = list(main_window.findChildren(QtWidgets.QWidget, "propertyEditorData"))
    tab = main_window.findChild(QtWidgets.QTabWidget, "propertyTab")
    if tab is not None:
        current = tab.currentWidget()
        if (
            current is not None
            and current.objectName() == "propertyEditorData"
            and all(current is not candidate for candidate in named)
        ):
            named.append(current)

    if not named:
        raise RuntimeError("propertyEditorData not found")

    usable = [widget for widget in named if _owned_by_visible_property_view(widget)]
    if not usable:
        raise RuntimeError(
            "no property editor owned by a visible Gui::PropertyView; candidates: "
            f"{[_describe_widget(widget) for widget in named]}"
        )
    if len(usable) > 1:
        raise RuntimeError(
            f"ambiguous property editor: {len(usable)} widgets named "
            "propertyEditorData are owned by a visible Gui::PropertyView; "
            f"candidates: {[_describe_widget(widget) for widget in usable]}"
        )
    return usable[0]


def _normalized_property_key(name: str) -> str:
    """Compare-safe key for a property name or an editor display label.

    Display labels are the property name camel-case split with spaces inserted
    (PropertyItem::setPropertyName, PropertyItem.cpp:569-596), so spaces are not
    significant; underscores are folded for the same reason the previous lookup
    folded them.
    """

    return str(name).replace(" ", "").replace("_", "")


def _camel_split_property_name(name: str) -> str:
    """Mirror step 1 of ``PropertyItem::setPropertyName`` (PropertyItem.cpp:578-590).

    A space is inserted before an uppercase character whose predecessor is
    lowercase - the exact rule FreeCAD applies before translating.
    """

    display = ""
    for char in str(name):
        if char.isupper() and display and display[-1].islower():
            display += " "
        display += char
    return display


def _expected_display_label(name: str) -> str:
    """The label the Property Editor will actually show for property ``name``.

    Mirrors ``PropertyItem::setPropertyName`` end to end
    (PropertyItem.cpp:569-596): camel-case split, then
    ``translate("App::Property", <split name>)``. FreeCAD calls
    ``QApplication::translate``; ``QCoreApplication.translate`` is the same
    static lookup against the same context and the same source string.

    Computing the EXPECTED label this way - instead of comparing a translated
    label against an untranslated property name - is what makes the binding
    check locale-independent. The model's DisplayRole cannot supply the raw
    name: ``PropertyItem::data`` returns ``displayName()`` for
    ``Qt::DisplayRole`` (PropertyItem.cpp:727), which is ``displayText``
    (:370-373), which is already translated (:595).
    """

    from PySide import QtCore

    return str(
        QtCore.QCoreApplication.translate(
            "App::Property", _camel_split_property_name(name)
        )
    )


def _property_label_keys(name: str) -> frozenset[str]:
    """Every form in which the editor could legitimately display ``name``.

    The translated label is what FreeCAD renders; the camel-split and raw forms
    are kept so an untranslated or partially translated catalogue still matches.
    All three describe the SAME property of the SAME object, so this widens
    nothing about object identity - a foreign object's rows still fail.
    """

    return frozenset(
        _normalized_property_key(form)
        for form in (
            str(name),
            _camel_split_property_name(name),
            _expected_display_label(name),
        )
    )


def _expected_property_keys(obj) -> frozenset[str]:
    """Accepted label keys for every property the requested object carries."""

    keys: set[str] = set()
    for name in obj.PropertiesList:
        keys.update(_property_label_keys(name))
    return frozenset(keys)


def _selected_object_pairs() -> list[tuple[str, str]]:
    """The live selection, read through the SAME call the rebuild consumes.

    ``PropertyView::onTimer`` builds the data editor from
    ``Gui::Selection().getSelectionEx("*")`` (PropertyView.cpp:427). Reading that
    identical list here is what makes the binding assertion below meaningful
    rather than merely suggestive.
    """

    import FreeCADGui

    return [
        (str(item.DocumentName), str(item.ObjectName))
        for item in FreeCADGui.Selection.getSelectionEx("*")
    ]


def _require_selection_is(document_name: str, object_name: str) -> None:
    """Fail loudly unless the selection is exactly the requested object."""

    selected = _selected_object_pairs()
    if selected != [(document_name, object_name)]:
        listed = [f"{doc}#{obj}" for doc, obj in selected]
        raise RuntimeError(
            "property editor selection mismatch: expected exactly "
            f"[{document_name}#{object_name}], live selection is {listed}"
        )


def _force_property_editor_rebuild(view) -> None:
    """Run FreeCAD's own rebuild slot synchronously, in this call frame.

    ``Gui::PropertyView::onTimer`` is declared ``public Q_SLOTS``
    (PropertyView.h:72-75) and is already invoked directly in-tree by
    ``PropertyView::setShowAll`` (PropertyView.cpp:173-186), so this is the
    product code path and not a test-only bypass. Invoking it with
    DirectConnection removes the dependence on the 100 ms single-shot timer
    (PropertyView.cpp:79-81, 369-378; ViewParams.cpp:400); its first act is
    ``timer->stop()`` (PropertyView.cpp:389), so a pending timer can no longer
    replace the content underneath the edit either.
    """

    from PySide import QtCore

    invoked = QtCore.QMetaObject.invokeMethod(
        view, "onTimer", QtCore.Qt.DirectConnection
    )
    if not invoked:
        raise RuntimeError(
            "could not invoke Gui::PropertyView::onTimer on "
            f"{view.metaObject().className()}"
        )


def _editor_property_labels(editor) -> list[str]:
    """Labels of the property rows: the direct children of the group rows."""

    from PySide import QtCore

    model = editor.model()
    root = QtCore.QModelIndex()
    labels: list[str] = []
    for group_row in range(model.rowCount(root)):
        group_index = model.index(group_row, 0, root)
        for row in range(model.rowCount(group_index)):
            labels.append(str(model.index(row, 0, group_index).data() or ""))
    return labels


def _property_editor_binding_defect(editor, property_name, expected_properties):
    """Why the editor is not presenting the requested object, or ``None``."""

    from PySide import QtCore

    labels = _editor_property_labels(editor)
    if not labels:
        group_count = editor.model().rowCount(QtCore.QModelIndex())
        return (
            f"{group_count} group row(s) carry no property rows - this is the "
            "emptied-group scaffolding left by PropertyModel::resetGroups "
            "(PropertyModel.cpp:305-341), not a build for this object"
        )
    foreign = sorted(
        {
            label
            for label in labels
            if _normalized_property_key(label) not in expected_properties
        }
    )
    if foreign:
        return (
            "editor is presenting properties that do not belong to the "
            f"requested object: {foreign}"
        )
    present = {_normalized_property_key(label) for label in labels}
    if not (_property_label_keys(property_name) & present):
        return (
            f"{property_name} (displayed as "
            f"{_expected_display_label(property_name)!r}) is absent from the "
            f"{len(labels)} property row(s) the editor is presenting"
        )
    return None


def _wait_for_property_editor(
    editor,
    document_name: str,
    object_name: str,
    property_name: str,
    expected_properties,
    timeout_s: float = 5.0,
) -> None:
    """Prove the editor is presenting ``document_name``/``object_name``.

    Readiness is no longer "the model has rows". That predicate is satisfied by
    content left over from an earlier build and says nothing about the object
    being edited (GRK-P3-083), which is how a stale editor holding another
    object's emptied groups passed the old check. Each attempt instead:

      1. asserts the live selection is exactly the requested object;
      2. forces FreeCAD's own rebuild synchronously from that selection;
      3. requires the resulting content to consist only of that object's
         properties and to include the requested one.

    No event-loop iteration runs between 1, 2 and 3, so the content inspected in
    step 3 is the content step 2 built from the selection step 1 verified. The
    bounded loop is a guard, not the mechanism: the rebuild is deterministic, so
    a healthy call satisfies the contract on its first attempt.
    """

    import time

    from PySide import QtWidgets

    view = _property_view_ancestor(editor)
    if view is None:
        raise RuntimeError(
            f"property editor has no Gui::PropertyView owner: {_describe_widget(editor)}"
        )

    deadline = time.monotonic() + timeout_s
    while True:
        _require_selection_is(document_name, object_name)
        if not view.isVisible():
            raise RuntimeError(
                "Gui::PropertyView is not visible, so it is detached from "
                "selection and cannot rebuild (PropertyView.cpp:188-197, 391-396)"
            )
        _force_property_editor_rebuild(view)
        defect = _property_editor_binding_defect(
            editor, property_name, expected_properties
        )
        if defect is None:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"property editor never bound to {document_name}#{object_name} "
                f"within {timeout_s:.1f}s: {defect}"
            )
        QtWidgets.QApplication.processEvents()
        time.sleep(0.05)


def _find_property_value_index(editor, property_name: str):
    """Value index for ``property_name``, searched only inside group rows.

    Root rows are group headers, never properties, so they are not candidates.
    The search runs only after _wait_for_property_editor has proved the editor
    is presenting the requested object, and no event loop runs in between.

    Matching accepts any form in which FreeCAD could display the property, so
    the lookup does not depend on the UI language either.
    """

    from PySide import QtCore

    model = editor.model()
    root = QtCore.QModelIndex()
    accepted = _property_label_keys(property_name)

    def walk(parent_index):
        for row in range(model.rowCount(parent_index)):
            idx0 = model.index(row, 0, parent_index)
            if _normalized_property_key(str(idx0.data() or "")) in accepted:
                return model.index(row, 1, parent_index)
            found = walk(idx0)
            if found.isValid():
                return found
        return QtCore.QModelIndex()

    for group_row in range(model.rowCount(root)):
        index = walk(model.index(group_row, 0, root))
        if index.isValid():
            return index
    raise ValueError(f"property not found in Property Editor: {property_name}")


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
    stage_prepared = params.get("stage_prepared", False)
    if not isinstance(stage_prepared, bool):
        raise TypeError("stage_prepared must be a bool")

    document = FreeCAD.getDocument(document_name)
    obj = document.getObject(object_name)
    if obj is None:
        raise ValueError(f"object not found: {object_name}")
    if property_name not in obj.PropertiesList:
        raise ValueError(f"property not found on object: {property_name}")

    if stage_prepared:
        active = FreeCADGui.ActiveDocument
        active_name = None if active is None else str(active.Document.Name)
        if active_name != document_name:
            raise RuntimeError(
                "stage-prepared property edit active-document mismatch: "
                f"expected {document_name}, got {active_name}"
            )
    else:
        FreeCADGui.setActiveDocument(document_name)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(document_name, object_name)
        gui_document = FreeCADGui.getDocument(document_name)
        if gui_document is not None:
            gui_document.toggleTreeItem(obj, 2)
    FreeCADGui.updateGui()

    expected_properties = _expected_property_keys(obj)
    editor = _property_editor_data()
    _wait_for_property_editor(
        editor,
        document_name,
        object_name,
        property_name,
        expected_properties,
        timeout_s=8.0,
    )
    index = _find_property_value_index(editor, property_name)
    _require_selection_is(document_name, object_name)
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


def _close_main_window(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    main_window = FreeCADGui.getMainWindow()
    if main_window is None:
        raise RuntimeError("no main window")
    main_window.close()
    FreeCADGui.updateGui()
    return {"closed": True}


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
        retained = _ASYNC_RECOMPUTE_HANDLES.pop(document, None)
        if retained is not None:
            retained.cancel("async blocker control reset")
        blocker_api.resetBlocker()
        return {"action": action}
    if action == "release":
        blocker_api.releaseBlocker()
        return {"action": action}
    if action == "touch_and_queue_recompute":
        blocker_api.resetBlocker()
        blocker.touch()
        handle = document.recomputeAsync([blocker])
        _retain_async_recompute_handle(document, handle)
        return {"action": action, "queued": True}
    raise ValueError(f"unsupported async_blocker_control action: {action}")


def _local_save(_params: dict[str, Any]) -> dict[str, Any]:
    import FreeCADGui

    if FreeCADGui.ActiveDocument is None:
        raise RuntimeError("no active GUI document")
    FreeCADGui.runCommand("Std_Save")
    return {"command": "Std_Save"}
