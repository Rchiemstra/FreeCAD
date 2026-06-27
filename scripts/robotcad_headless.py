# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Shared RobotCAD/CROSS setup for FreeCADCmd (no GUI, no MCP).

Used by ``build_arm_2dof_fcstd.py``, ``export_arm_2dof_fcstd.py``, and
``e2e/export_robotcad_fcstd.py``.
"""
from __future__ import annotations

import os
import sys
import types
from pathlib import Path
from typing import Any, Optional, Tuple

OVERCROSS_MOD_NAME = "freecad.overcross"


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def overcross_mod_path() -> Path:
    candidates: list[Path] = []
    if os.name == "nt":
        appdata = os.environ.get("APPDATA", "")
        candidates.append(Path(appdata) / "FreeCAD" / "v1-2" / "Mod" / OVERCROSS_MOD_NAME)
        candidates.append(Path(appdata) / "FreeCAD" / "Mod" / OVERCROSS_MOD_NAME)
    candidates.append(Path.home() / ".local/share/FreeCAD/Mod" / OVERCROSS_MOD_NAME)
    for path in candidates:
        if path.is_dir():
            return path
    return candidates[0]


def pixi_site_packages() -> Optional[Path]:
    """Pixi env ``Lib/site-packages`` (provides ``freecad.utils``)."""
    for entry in sys.path:
        norm = entry.replace("\\", "/")
        if ".pixi" in norm and "Library" in entry:
            site = Path(entry).parent.parent / "Lib" / "site-packages"
            if (site / "freecad" / "utils.py").is_file():
                return site
    # Fallback relative to this file (repo layout)
    candidate = repo_root() / ".pixi" / "envs" / "default" / "Lib" / "site-packages"
    if (candidate / "freecad" / "utils.py").is_file():
        return candidate
    return None


def ensure_freecad_cross_importable() -> None:
    """Insert OVERCROSS Mod + pixi site-packages; clear shadowing ``freecad`` cache."""
    mod = overcross_mod_path()
    if mod.is_dir() and str(mod) not in sys.path:
        sys.path.insert(0, str(mod))

    site = pixi_site_packages()
    if site is not None and str(site) not in sys.path:
        sys.path.insert(1, str(site))

    for name in list(sys.modules.keys()):
        if name == "freecad" or name.startswith("freecad."):
            del sys.modules[name]


def ensure_freecad_gui_stub() -> None:
    """OVERCROSS imports FreeCADGui; stub it for FreeCADCmd."""
    if "FreeCADGui" in sys.modules:
        return

    class _Bar:
        def setValue(self, *_a, **_k):
            pass

        def show(self, *_a, **_k):
            pass

        def hide(self, *_a, **_k):
            pass

        def close(self, *_a, **_k):
            pass

    class _StatusBar:
        def setStyleSheet(self, *_a, **_k):
            pass

        def addWidget(self, *_a, **_k):
            pass

    class _MainWindow:
        def statusBar(self):
            return _StatusBar()

    gui = types.ModuleType("FreeCADGui")
    gui.Selection = types.SimpleNamespace(getSelection=lambda: [])
    gui.getMainWindow = lambda: _MainWindow()
    gui.ActiveDocument = None
    gui.doCommand = lambda *_a, **_k: None
    gui.addModule = lambda *_a, **_k: None
    gui.updateLocale = lambda *_a, **_k: None
    gui.PySideUic = types.SimpleNamespace(loadUi=lambda *_a, **_k: None)
    sys.modules["FreeCADGui"] = gui

    import freecad.cross.freecadgui_utils as fg

    fg.get_progress_bar = lambda *a, **k: _Bar()


def fix_overcross_mod_paths(wb_utils: Any, robot_proxy_mod: Any) -> Path:
    """Point OVERCROSS ``MOD_PATH`` at ``freecad.overcross`` (not ``freecad.robotcad``)."""
    mod = overcross_mod_path()
    if not mod.is_dir():
        raise FileNotFoundError(
            f"OVERCROSS not installed at {mod}. Run scripts/install_robotcad_cross.ps1"
        )

    wb_utils.MOD_PATH = mod
    wb_utils.RESOURCES_PATH = mod / "resources"
    wb_utils.UI_PATH = wb_utils.RESOURCES_PATH / "ui"
    wb_utils.ICON_PATH = wb_utils.RESOURCES_PATH / "icons"
    wb_utils.MODULES_PATH = mod / "modules"
    wb_utils.ROS2_CONTROLLERS_PATH = wb_utils.MODULES_PATH / "ros2_controllers"
    wb_utils.SDFORMAT_PATH = wb_utils.MODULES_PATH / "sdformat"
    wb_utils.SDFORMAT_SDF_TEMPLATES_PATH = wb_utils.SDFORMAT_PATH / "sdf"
    wb_utils.ROBOT_DESCRIPTIONS_REPO_PATH = wb_utils.MODULES_PATH / "robot_descriptions"
    wb_utils.ROBOT_DESCRIPTIONS_MODULE_PATH = (
        wb_utils.ROBOT_DESCRIPTIONS_REPO_PATH / "robot_descriptions"
    )
    wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH = wb_utils.MODULES_PATH / "Dynamic_World_Generator"
    wb_utils.DYNAMIC_WORLD_GENERATOR_MODULE_PATH = (
        wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH / "code"
    )
    wb_utils.DYNAMIC_WORLD_GENERATOR_WORLDS_GAZEBO_PATH = (
        wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH / "worlds" / "gazebo"
    )

    for name in (
        "MOD_PATH",
        "MODULES_PATH",
        "ROS2_CONTROLLERS_PATH",
        "SDFORMAT_PATH",
        "ROBOT_DESCRIPTIONS_REPO_PATH",
        "DYNAMIC_WORLD_GENERATOR_REPO_PATH",
    ):
        setattr(robot_proxy_mod, name, getattr(wb_utils, name))

    return mod


def patch_export_urdf_ignore_bug(robot_proxy_mod: Any) -> None:
    """
    OVERCROSS: ``export_urdf(interactive=False)`` never sets ``ignore``/``write``/``overwrite``.

    Patch the method in-place (no Qt / GuiUp) by re-execing source with an ``else`` branch.
    """
    import inspect
    import textwrap

    src = inspect.getsource(robot_proxy_mod.RobotProxy.export_urdf)
    needle = "            diag.close()\n        if set(ignore)"
    fix = (
        "            diag.close()\n"
        "        else:\n"
        "            ignore = []\n"
        "            write = list(write_files)\n"
        "            overwrite = []\n"
        "        if set(ignore)"
    )
    if needle not in src or fix in src:
        return

    src = textwrap.dedent(src.replace(needle, fix, 1))
    namespace: dict = {}
    exec(compile(src, "<export_urdf_patch>", "exec"), robot_proxy_mod.__dict__, namespace)
    robot_proxy_mod.RobotProxy.export_urdf = namespace["export_urdf"]


def apply_export_patches(wb_utils: Any, robot_proxy_mod: Any, gen_root: Path) -> None:
    """Headless export: skip git UI, overwrite dialog, ROS workspace prompts."""

    class _AutoOverwriteDialog:
        def __init__(self, _output_path, write_files):
            self._write_files = list(write_files)

        def exec_(self):
            return [], self._write_files, []

        def close(self):
            pass

    def _install_auto_overwrite_dialog():
        for mod in sys.modules.values():
            if mod is None:
                continue
            if hasattr(mod, "FileOverwriteConfirmationDialog"):
                setattr(mod, "FileOverwriteConfirmationDialog", _AutoOverwriteDialog)
        try:
            import freecad.cross.ui.file_overwrite_confirmation_dialog as fod_mod

            fod_mod.FileOverwriteConfirmationDialog = _AutoOverwriteDialog
        except Exception:
            pass
        robot_proxy_mod.FileOverwriteConfirmationDialog = _AutoOverwriteDialog

    def _noop_git_init_submodules(*_args, **_kwargs):
        return

    def _noop_progress_bar(*_args, **_kwargs):
        class _Bar:
            def show(self):
                pass

            def setValue(self, _v):
                pass

            def close(self):
                pass

        return _Bar()

    def _get_rel_and_abs_path(path: str, ask_user_fill_workspace: bool = True):
        p = Path(path)
        if p.is_absolute():
            return str(p), p
        return str(p), p.resolve()

    _install_auto_overwrite_dialog()
    wb_utils.git_init_submodules = _noop_git_init_submodules
    robot_proxy_mod.git_init_submodules = _noop_git_init_submodules
    wb_utils.get_progress_bar = _noop_progress_bar
    wb_utils.get_rel_and_abs_path = _get_rel_and_abs_path
    robot_proxy_mod.get_rel_and_abs_path = _get_rel_and_abs_path

    try:
        import freecad.cross.wb_gui_utils as wb_gui

        wb_gui.get_ros_workspace = lambda _old="": gen_root
    except Exception:
        pass

    def _minimal_controllers_yaml(self, *_a, **_k):
        return {"controller_manager": {"ros__parameters": {"update_rate": 250}}}

    def _noop_copy_custom_worlds(self, _destination_path=None):
        return None

    robot_proxy_mod.RobotProxy.get_robot_controllers_yaml = _minimal_controllers_yaml
    robot_proxy_mod.RobotProxy.copy_custom_worlds = _noop_copy_custom_worlds

    try:
        from PySide.QtWidgets import QMessageBox

        _yes = QMessageBox.Yes
        QMessageBox.question = lambda *_a, **_k: _yes
    except Exception:
        pass


def export_fcstd_to_urdf(
    fcstd: Path,
    out_dir: Path,
    *,
    robot_name: Optional[str] = None,
) -> Tuple[Path, Any]:
    """
    Open *fcstd*, export URDF via RobotCAD, return ``(urdf_path, robot_object)``.

    Must run inside FreeCADCmd / FreeCAD Python.
    """
    ensure_freecad_cross_importable()
    ensure_freecad_gui_stub()

    import FreeCAD
    import freecad.cross.robot_proxy as robot_proxy_mod
    import freecad.cross.wb_utils as wb_utils
    from freecad.cross.ros.utils import split_package_path
    from freecad.cross.wb_utils import get_urdf_path, is_robot

    fix_overcross_mod_paths(wb_utils, robot_proxy_mod)
    patch_export_urdf_ignore_bug(robot_proxy_mod)

    fcstd = fcstd.resolve()
    out_dir = out_dir.resolve()
    robot_name = robot_name or fcstd.stem
    pkg_name = f"{robot_name}_description"
    gen_root = out_dir
    output_path = (gen_root / pkg_name).resolve()

    doc = FreeCAD.openDocument(str(fcstd))
    try:
        import FreeCADGui

        FreeCADGui.ActiveDocument = doc
    except Exception:
        pass

    robot_objs = [o for o in doc.Objects if is_robot(o)]
    if not robot_objs:
        raise RuntimeError(f"No Cross::Robot in {fcstd}")

    robot = robot_objs[0]
    output_path.mkdir(parents=True, exist_ok=True)
    robot.OutputPath = str(output_path)
    doc.recompute()

    if not hasattr(robot, "Proxy") or robot.Proxy is None:
        raise RuntimeError("Cross::Robot has no Proxy")

    apply_export_patches(wb_utils, robot_proxy_mod, gen_root)

    import types

    robot.Proxy.export_urdf = types.MethodType(
        robot_proxy_mod.RobotProxy.export_urdf, robot.Proxy
    )
    robot.Proxy.get_robot_controllers_yaml = types.MethodType(
        robot_proxy_mod.RobotProxy.get_robot_controllers_yaml, robot.Proxy
    )
    robot.Proxy.copy_custom_worlds = types.MethodType(
        robot_proxy_mod.RobotProxy.copy_custom_worlds, robot.Proxy
    )

    robot.Proxy.export_urdf(interactive=False)

    _, _, description_package_path = split_package_path(output_path)
    urdf_path = Path(get_urdf_path(robot, description_package_path))
    return urdf_path, robot
