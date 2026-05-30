"""
Global read/write mode toolbar button for FreeCAD.

The button is inserted next to FreeCAD's Save action when that toolbar is
available.  Green means read mode; red means normal write mode.
"""
from __future__ import annotations

import os

import read_mode


_toolbar_button = None


def _import_qt():
    try:
        from PySide6 import QtCore, QtGui, QtWidgets
        return QtCore, QtGui, QtWidgets
    except Exception:
        pass
    try:
        from PySide2 import QtCore, QtGui, QtWidgets
        return QtCore, QtGui, QtWidgets
    except Exception:
        pass
    from PySide import QtCore, QtGui
    return QtCore, QtGui, QtGui


def _action_text(action) -> str:
    text = ""
    for attr in ("objectName", "text", "toolTip"):
        try:
            value = getattr(action, attr)()
        except Exception:
            value = ""
        if value:
            text += " " + str(value)
    return text


def _is_save_action(action) -> bool:
    try:
        object_name = str(action.objectName())
    except Exception:
        object_name = ""
    if object_name == "Std_Save":
        return True

    try:
        label = str(action.text())
    except Exception:
        label = ""
    label = label.replace("&", "").replace("...", "").strip().lower()
    if label == "save":
        return True

    text = _action_text(action).replace("&", "").lower()
    return "save the active document" in text


class ReadModeToolbarButton:
    def __init__(self, freecad, freecad_gui, addon_dir: str):
        self.freecad = freecad
        self.freecad_gui = freecad_gui
        self.addon_dir = addon_dir
        self.qt_core, self.qt_gui, self.qt_widgets = _import_qt()
        self.action = None
        self.timer = None
        self._last_request_signature = None
        self.green_icon = os.path.join(addon_dir, "icons", "ReadModeEyeGreen.svg")
        self.red_icon = os.path.join(addon_dir, "icons", "ReadModeEyeRed.svg")

    def install(self) -> bool:
        main_window = self.freecad_gui.getMainWindow()
        if main_window is None:
            return False

        existing = self._find_existing_action(main_window)
        if existing is not None:
            self.action = existing
        else:
            action_cls = getattr(self.qt_gui, "QAction", None) or getattr(self.qt_widgets, "QAction")
            self.action = action_cls(self.qt_gui.QIcon(self.red_icon), "Write Mode", main_window)
            self.action.setObjectName("SimWB_ReadWriteModeToggle")
            self.action.setCheckable(True)
            self.action.triggered.connect(self._toggle_from_ui)

            inserted = self._insert_next_to_save(main_window)
            if not inserted:
                toolbar = main_window.addToolBar("Document Mode")
                toolbar.setObjectName("SimWB_DocumentModeToolbar")
                toolbar.addAction(self.action)

        read_mode.subscribe_mode_changes(lambda _mode, _path: self.update_action())
        self.timer = self.qt_core.QTimer()
        self.timer.timeout.connect(self.poll_agent_request)
        self.timer.timeout.connect(self.update_action)
        self._last_request_signature = read_mode.file_signature(read_mode.control_file_path())
        self.timer.start(1000)
        self.update_action()
        return True

    def _find_existing_action(self, main_window):
        toolbar_cls = getattr(self.qt_widgets, "QToolBar", None)
        if toolbar_cls is None:
            return None
        for toolbar in main_window.findChildren(toolbar_cls):
            for action in toolbar.actions():
                try:
                    if action.objectName() == "SimWB_ReadWriteModeToggle":
                        return action
                except Exception:
                    pass
        return None

    def _insert_next_to_save(self, main_window) -> bool:
        toolbar_cls = getattr(self.qt_widgets, "QToolBar", None)
        if toolbar_cls is None:
            return False
        for toolbar in main_window.findChildren(toolbar_cls):
            actions = list(toolbar.actions())
            for index, existing in enumerate(actions):
                if not _is_save_action(existing):
                    continue
                next_action = actions[index + 1] if index + 1 < len(actions) else None
                if next_action is not None:
                    toolbar.insertAction(next_action, self.action)
                else:
                    toolbar.addAction(self.action)
                return True
        return False

    def _toggle_from_ui(self, *_args) -> None:
        target = read_mode.MODE_WRITE if read_mode.current_mode() == read_mode.MODE_READ else read_mode.MODE_READ
        msg = read_mode.set_document_mode(target)
        self.freecad.Console.PrintMessage("[SimWorkbench] {}\n".format(msg))
        self._write_current_mode_request("ui")
        self.update_action()

    def _write_current_mode_request(self, source: str) -> None:
        try:
            control_file = read_mode.write_mode_request(
                read_mode.current_mode(),
                path=read_mode.watched_path(),
                source=source,
            )
            self._last_request_signature = read_mode.file_signature(control_file)
        except Exception as exc:
            self.freecad.Console.PrintWarning(
                "[SimWorkbench] Could not write document mode state: {}\n".format(exc)
            )

    def poll_agent_request(self) -> None:
        request = read_mode.read_mode_request()
        if request is None:
            return
        if request["signature"] == self._last_request_signature:
            return

        self._last_request_signature = request["signature"]
        msg = read_mode.set_document_mode(request["mode"], path=request["path"])
        self.freecad.Console.PrintMessage(
            "[SimWorkbench] Agent document mode request: {}\n".format(msg)
        )
        self.update_action()

    def update_action(self) -> None:
        if self.action is None:
            return

        is_read = read_mode.current_mode() == read_mode.MODE_READ
        icon_path = self.green_icon if is_read else self.red_icon
        label = "Read Mode" if is_read else "Write Mode"
        watched = read_mode.watched_path()
        if is_read and watched:
            tooltip = "Read mode: watching {}. Click to switch to write mode.".format(watched)
        else:
            tooltip = "Write mode: FreeCAD can edit and save. Click to watch the saved document."

        has_saved_doc = bool(read_mode.active_document_path())
        self.action.setIcon(self.qt_gui.QIcon(icon_path))
        self.action.setText(label)
        self.action.setToolTip(tooltip)
        self.action.setChecked(is_read)
        self.action.setEnabled(is_read or has_saved_doc)


def install_read_mode_toolbar(addon_dir: str) -> bool:
    global _toolbar_button
    if _toolbar_button is not None:
        return True

    import FreeCAD
    import FreeCADGui

    _toolbar_button = ReadModeToolbarButton(FreeCAD, FreeCADGui, addon_dir)
    return _toolbar_button.install()
