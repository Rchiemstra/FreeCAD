# SPDX-License-Identifier: LGPL-2.1-or-later

"""Regression tests for workbench-local preference module imports."""

import importlib
import sys
import types
import unittest

import FreeCAD as App

if App.GuiUp:
    import FreeCADGui as Gui


class TestPreferencesImport(unittest.TestCase):
    @unittest.skipUnless(App.GuiUp, "requires the FreeCAD GUI")
    def test_assembly_workbench_initializes_with_cached_generic_preferences(self):
        conflicting_module = types.ModuleType("Preferences")
        previous_module = sys.modules.get("Preferences")
        previous_workbench = Gui.activeWorkbench().name()
        sys.modules["Preferences"] = conflicting_module
        try:
            Gui.activateWorkbench("AssemblyWorkbench")
            self.assertEqual(Gui.activeWorkbench().name(), "AssemblyWorkbench")
        finally:
            Gui.activateWorkbench(previous_workbench)
            if previous_module is None:
                sys.modules.pop("Preferences", None)
            else:
                sys.modules["Preferences"] = previous_module

    def test_unique_module_ignores_cached_generic_preferences(self):
        conflicting_module = types.ModuleType("Preferences")
        previous_module = sys.modules.get("Preferences")
        sys.modules["Preferences"] = conflicting_module
        try:
            assembly_preferences = importlib.import_module("AssemblyPreferences")
            self.assertIsNot(assembly_preferences, conflicting_module)
            self.assertTrue(hasattr(assembly_preferences, "PreferencesPage"))
            self.assertTrue(hasattr(assembly_preferences, "preferences"))
        finally:
            if previous_module is None:
                sys.modules.pop("Preferences", None)
            else:
                sys.modules["Preferences"] = previous_module


if __name__ == "__main__":
    unittest.main()
