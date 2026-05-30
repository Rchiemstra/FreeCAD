"""
Offline tests for SimWorkbench document read mode.
"""
from __future__ import annotations

import importlib.util
import json
import os
import sys
import types
from pathlib import Path


ROOT = Path(__file__).parent.parent
ADDON_DIR = ROOT / "addons" / "SimWorkbench"
COMMANDS_DIR = ADDON_DIR / "commands"
sys.path.insert(0, str(ADDON_DIR))
sys.path.insert(0, str(COMMANDS_DIR))


class _Signal:
    def __init__(self):
        self.callbacks = []

    def connect(self, callback):
        self.callbacks.append(callback)


class _QTimer:
    def __init__(self):
        self.timeout = _Signal()
        self.interval = None
        self.active = False

    def start(self, interval):
        self.interval = interval
        self.active = True

    def stop(self):
        self.active = False

    def isActive(self):
        return self.active


class _FakeIcon:
    def __init__(self, path):
        self.path = path


class _FakeAction:
    def __init__(self, icon=None, text="", parent=None):
        self.icon = icon
        self._text = text
        self._tool_tip = ""
        self._object_name = ""
        self._enabled = True
        self._checked = False
        self.triggered = _Signal()

    def setObjectName(self, name):
        self._object_name = name

    def objectName(self):
        return self._object_name

    def setCheckable(self, _value):
        pass

    def setChecked(self, value):
        self._checked = value

    def setIcon(self, icon):
        self.icon = icon

    def setText(self, text):
        self._text = text

    def text(self):
        return self._text

    def setToolTip(self, text):
        self._tool_tip = text

    def toolTip(self):
        return self._tool_tip

    def setEnabled(self, value):
        self._enabled = value


class _FakeToolBar:
    def __init__(self, actions=None):
        self._actions = list(actions or [])
        self._object_name = ""

    def actions(self):
        return list(self._actions)

    def insertAction(self, before, action):
        self._actions.insert(self._actions.index(before), action)

    def addAction(self, action):
        self._actions.append(action)

    def setObjectName(self, name):
        self._object_name = name


def _load_module(module_name: str, path: Path):
    spec = importlib.util.spec_from_file_location(module_name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_file_signature_detects_existing_file(tmp_path):
    from read_mode import file_signature

    fcstd = tmp_path / "model.FCStd"
    fcstd.write_text("first", encoding="utf-8")

    first = file_signature(str(fcstd))
    assert first is not None
    assert first[1] == len("first")

    fcstd.write_text("larger content", encoding="utf-8")
    second = file_signature(str(fcstd))
    assert second is not None
    assert second != first
    assert second[1] == len("larger content")


def test_same_path_normalises_absolute_paths(tmp_path):
    from read_mode import same_path

    path = tmp_path / "model.FCStd"
    assert same_path(str(path), os.path.join(str(tmp_path), ".", "model.FCStd"))
    assert not same_path(str(path), str(tmp_path / "other.FCStd"))


def test_mode_request_round_trip(monkeypatch, tmp_path):
    import read_mode

    control_file = tmp_path / "read_mode_control.json"
    fcstd = tmp_path / "model.FCStd"
    fcstd.write_text("model", encoding="utf-8")
    monkeypatch.setenv("SIMWORKBENCH_READ_MODE_CONTROL", str(control_file))

    written = read_mode.write_mode_request("read", path=str(fcstd), source="agent")
    request = read_mode.read_mode_request()

    assert written == str(control_file)
    assert request["mode"] == "read"
    assert request["path"] == str(fcstd)
    assert request["source"] == "agent"
    assert request["signature"] is not None


def test_agent_script_writes_mode_request(monkeypatch, tmp_path):
    control_file = tmp_path / "agent_control.json"
    monkeypatch.setenv("SIMWORKBENCH_READ_MODE_CONTROL", str(control_file))

    script = _load_module(
        "set_freecad_document_mode_test",
        ROOT / "scripts" / "set_freecad_document_mode.py",
    )
    result = script.main(["write"])

    assert result == 0
    payload = json.loads(control_file.read_text(encoding="utf-8"))
    assert payload["mode"] == "write"
    assert payload["source"] == "agent"


def test_start_and_stop_read_mode_with_fake_freecad(monkeypatch, tmp_path):
    import read_mode

    fcstd = tmp_path / "model.FCStd"
    fcstd.write_text("model", encoding="utf-8")

    messages = []

    class Console:
        @staticmethod
        def PrintMessage(message):
            messages.append(message)

        @staticmethod
        def PrintWarning(message):
            messages.append(message)

    active_doc = types.SimpleNamespace(FileName=str(fcstd), Name="Doc")
    freecad = types.SimpleNamespace(
        ActiveDocument=active_doc,
        Console=Console,
        listDocuments=lambda: {"Doc": active_doc},
        closeDocument=lambda _name: None,
        openDocument=lambda path: types.SimpleNamespace(Name="Reloaded", FileName=path),
        setActiveDocument=lambda _name: None,
    )
    freecad_gui = types.SimpleNamespace(
        getDocument=lambda _name: None,
        SendMsgToActiveView=lambda _msg: None,
    )
    qt_module = types.SimpleNamespace(QtCore=types.SimpleNamespace(QTimer=_QTimer))

    monkeypatch.setitem(sys.modules, "FreeCAD", freecad)
    monkeypatch.setitem(sys.modules, "FreeCADGui", freecad_gui)
    monkeypatch.setitem(sys.modules, "PySide6", qt_module)
    read_mode._watcher = None

    started = read_mode.start_read_mode()
    assert "Read mode watching" in started
    assert read_mode.is_read_mode_running()
    assert read_mode._watcher.timer.interval == 1000

    stopped = read_mode.stop_read_mode()
    assert "Read mode stopped" in stopped
    assert not read_mode.is_read_mode_running()


def test_sim_commands_registers_read_mode_command(monkeypatch):
    registered = {}

    class Console:
        @staticmethod
        def PrintMessage(_message):
            pass

    freecad = types.SimpleNamespace(
        ActiveDocument=types.SimpleNamespace(FileName="model.FCStd"),
        Console=Console,
    )

    class FreeCADGui:
        @staticmethod
        def addCommand(name, command):
            registered[name] = command

    monkeypatch.setitem(sys.modules, "FreeCAD", freecad)
    monkeypatch.setitem(sys.modules, "FreeCADGui", FreeCADGui)

    mod = _load_module("sim_commands_read_mode_test", COMMANDS_DIR / "sim_commands.py")

    assert "SimWB_ToggleReadMode" in registered
    assert "SimWB_ToggleReadMode" in mod.TOOLBAR_COMMANDS
    assert registered["SimWB_ToggleReadMode"].IsActive()
    resources = registered["SimWB_ToggleReadMode"].GetResources()
    assert resources["MenuText"] == "Toggle Document Read Mode"
    assert "external agent" in resources["ToolTip"]
    assert os.path.isfile(resources["Pixmap"])

    for command_name in mod.TOOLBAR_COMMANDS:
        pixmap = registered[command_name].GetResources()["Pixmap"]
        assert os.path.isfile(pixmap)
        assert "media-" not in pixmap


def test_read_mode_toolbar_inserts_after_save(monkeypatch):
    import read_mode_toolbar

    save_action = _FakeAction(text="Save")
    other_action = _FakeAction(text="Open")
    toolbar = _FakeToolBar([other_action, save_action])

    class MainWindow:
        def findChildren(self, cls):
            return [toolbar] if cls is _FakeToolBar else []

        def addToolBar(self, _name):
            return _FakeToolBar()

    messages = []

    class Console:
        @staticmethod
        def PrintMessage(message):
            messages.append(message)

        @staticmethod
        def PrintWarning(message):
            messages.append(message)

    freecad = types.SimpleNamespace(
        ActiveDocument=types.SimpleNamespace(FileName="model.FCStd"),
        Console=Console,
    )
    freecad_gui = types.SimpleNamespace(getMainWindow=lambda: MainWindow())
    qt_core = types.SimpleNamespace(QTimer=_QTimer)
    qt_gui = types.SimpleNamespace(QAction=_FakeAction, QIcon=_FakeIcon)
    qt_widgets = types.SimpleNamespace(QToolBar=_FakeToolBar)

    monkeypatch.setattr(read_mode_toolbar, "_import_qt", lambda: (qt_core, qt_gui, qt_widgets))
    button = read_mode_toolbar.ReadModeToolbarButton(freecad, freecad_gui, str(ADDON_DIR))

    assert button.install()
    actions = toolbar.actions()
    assert actions[actions.index(save_action) + 1].objectName() == "SimWB_ReadWriteModeToggle"
    assert actions[actions.index(save_action) + 1].icon.path.endswith("ReadModeEyeRed.svg")
