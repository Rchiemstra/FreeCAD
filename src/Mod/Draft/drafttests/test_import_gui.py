# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2013 Yorik van Havre <yorik@uncreated.net>              *
# *   Copyright (c) 2019 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de> *
# *   Copyright (c) 2025 FreeCAD Project Association                        *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

"""Unit tests for the Draft Workbench, GUI import tests."""

## @package test_import_gui
# \ingroup drafttests
# \brief Unit tests for the Draft Workbench, GUI import tests.

## \addtogroup drafttests
# @{

import unittest

from drafttests import auxiliary as aux
from drafttests import test_base


class DraftGuiImport(test_base.DraftTestCaseNoDoc):
    """Import the Draft graphical modules."""

    def test_import_gui_draftgui(self):
        """Import Draft TaskView GUI tools."""
        module = "DraftGui"
        imported = aux.import_test(module)
        self.assertTrue(imported, "Problem importing '{}'".format(module))

    def test_import_gui_draft_snap(self):
        """Import Draft snapping."""
        module = "draftguitools.gui_snapper"
        imported = aux.import_test(module)
        self.assertTrue(imported, "Problem importing '{}'".format(module))

    def test_import_gui_draft_tools(self):
        """Import Draft graphical commands."""
        module = "DraftTools"
        imported = aux.import_test(module)
        self.assertTrue(imported, "Problem importing '{}'".format(module))

    def test_import_gui_draft_trackers(self):
        """Import Draft tracker utilities."""
        module = "draftguitools.gui_trackers"
        imported = aux.import_test(module)
        self.assertTrue(imported, "Problem importing '{}'".format(module))


## @}


class DraftHatchImportIsolation(unittest.TestCase):
    """Register Hatch on the GUI thread while keeping owner imports isolated."""

    def _load_view_provider(self, importer):
        import importlib.util
        from pathlib import Path
        from unittest.mock import patch

        path = Path(__file__).resolve().parents[1] / "draftviewproviders" / "view_hatch.py"
        spec = importlib.util.spec_from_file_location("_draft_hatch_import_probe", path)
        module = importlib.util.module_from_spec(spec)
        with patch("builtins.__import__", side_effect=importer):
            spec.loader.exec_module(module)
        return module

    def test_import_does_not_load_gui_commands(self):
        import builtins
        from unittest.mock import patch

        original = builtins.__import__

        def guarded_import(name, *args, **kwargs):
            if name.startswith("draftguitools"):
                raise RuntimeError("model import attempted GUI command registration")
            return original(name, *args, **kwargs)

        with patch("draftutils.gui_utils.on_gui_thread", return_value=False):
            module = self._load_view_provider(guarded_import)
        self.assertTrue(callable(module.ViewProviderDraftHatch))

    def test_fresh_main_thread_draft_import_registers_hatch(self):
        import importlib
        import sys
        import FreeCADGui as Gui
        from unittest.mock import patch

        names = ("Draft", "draftmake.make_hatch", "draftviewproviders.view_hatch",
                 "draftguitools.gui_hatch")
        saved = {name: sys.modules.pop(name) for name in names if name in sys.modules}
        commands = set(Gui.listCommands())
        native_register = Gui.addCommand

        def register_if_new(name, command):
            # Other GUI tests may already have registered Hatch. In a clean
            # process exercise native registration; avoid duplicate diagnostics
            # when probing a fresh import in an existing GUI session.
            if name not in commands:
                native_register(name, command)

        try:
            with patch.object(Gui, "addCommand", side_effect=register_if_new) as register:
                importlib.import_module("Draft")
            self.assertIn("Draft_Hatch", [call.args[0] for call in register.call_args_list])
            self.assertIn("Draft_Hatch", Gui.listCommands())
        finally:
            # Keep newly imported main-thread modules cached, just as a macro
            # would. Removing them would register commands again on activation.
            sys.modules.update(saved)

    def test_fresh_owner_thread_draft_import_does_not_register_commands(self):
        import importlib
        import sys
        import threading
        import FreeCADGui as Gui
        from unittest.mock import patch

        names = ("Draft", "draftmake.make_hatch", "draftviewproviders.view_hatch",
                 "draftguitools.gui_hatch")
        saved = {name: sys.modules.pop(name) for name in names if name in sys.modules}
        errors = []

        def worker():
            try:
                importlib.import_module("Draft")
            except BaseException as exc:
                errors.append(exc)

        try:
            with patch.object(Gui, "addCommand", side_effect=AssertionError("owner registered GUI command")) as register:
                thread = threading.Thread(target=worker)
                thread.start()
                thread.join(timeout=30)
                self.assertFalse(thread.is_alive())
                self.assertFalse(errors, errors)
                register.assert_not_called()
        finally:
            for name in names:
                sys.modules.pop(name, None)
            sys.modules.update(saved)

    def test_edit_loads_task_panel_and_shows_dialog(self):
        import builtins
        from types import SimpleNamespace
        from unittest.mock import Mock, patch

        panel = SimpleNamespace(form=SimpleNamespace(
            File=Mock(), Pattern=Mock(), Scale=Mock(), Rotation=Mock(), Translate=Mock()))
        factory = Mock(return_value=panel)
        original = builtins.__import__

        def panel_import(name, *args, **kwargs):
            if name == "draftguitools.gui_hatch":
                return SimpleNamespace(Draft_Hatch_TaskPanel=factory)
            return original(name, *args, **kwargs)

        module = self._load_view_provider(panel_import)
        obj = SimpleNamespace(File="pattern.svg", Pattern="Lines", Scale=2, Rotation=30, Translate=True)
        vobj = SimpleNamespace(Object=obj)
        provider = module.ViewProviderDraftHatch(vobj)
        with patch("builtins.__import__", side_effect=panel_import), patch.object(module, "Gui") as gui:
            self.assertTrue(provider.setEdit(vobj, 0))
            gui.Control.showDialog.assert_called_once_with(panel)
        factory.assert_called_once_with(obj)
        panel.form.File.setFileName.assert_called_once_with("pattern.svg")
        self.assertIsNone(provider.setEdit(vobj, 1))

    def _check_factory_presentation(self, on_gui_thread):
        import importlib
        from types import SimpleNamespace
        from unittest.mock import Mock, patch

        class OwnerObject:
            @property
            def ViewObject(self):
                raise RuntimeError("ViewObject accessed off the GUI thread")

        for name, proxy, view, args in (
            ("rectangle", "Rectangle", "ViewProviderRectangle", (20, 10)),
        ):
            with self.subTest(factory=name):
                module = importlib.import_module("draftmake.make_" + name)
                obj = SimpleNamespace(ViewObject=object()) if on_gui_thread else OwnerObject()
                app = SimpleNamespace(GuiUp=True, ActiveDocument=Mock())
                app.ActiveDocument.addObject.return_value = obj
                with patch.object(module, "App", app), patch.object(module, proxy), \
                        patch.object(module, view) as provider, \
                        patch.object(module.gui_utils, "on_gui_thread", return_value=on_gui_thread), \
                        patch.object(module.gui_utils, "format_object") as formatter, \
                        patch.object(module.gui_utils, "select") as selector, \
                        patch.object(module.gui_utils, "_GuiSetup") as queued, \
                        patch.object(module.gui_utils, "_pending_gui_setups", set()):
                    self.assertIs(getattr(module, "make_" + name)(*args), obj)
                    if on_gui_thread:
                        provider.assert_called_once_with(obj.ViewObject)
                        formatter.assert_called_once_with(obj)
                        selector.assert_called_once_with(obj)
                        queued.assert_not_called()
                    else:
                        provider.assert_not_called()
                        formatter.assert_not_called()
                        selector.assert_not_called()
                        # This import control uses a fake model object; the real
                        # owner-thread tests exercise Qt delivery and catch-up.
                        queued.assert_called_once()
                        self.assertIs(queued.call_args.args[0], obj)
                        self.assertTrue(callable(queued.call_args.args[1]))
                        queued.return_value.wake.emit.assert_called_once_with()

    def test_factories_preserve_gui_presentation_on_main_thread(self):
        self._check_factory_presentation(True)

    def test_factories_do_not_access_view_objects_on_owner_thread(self):
        self._check_factory_presentation(False)
