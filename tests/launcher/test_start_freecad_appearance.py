"""Appearance parity between normal and authenticated FreeCAD profiles."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "start_freecad_appearance_under_test", REPO / "tools/launcher/start_freecad_impl.py"
)
launcher = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(launcher)


def parameters(main: str = "", view: str = "", extra: str = "") -> str:
    return f'''<FCParameters><FCParamGroup Name="Root"><FCParamGroup Name="BaseApp">
      <FCParamGroup Name="Preferences">
        <FCParamGroup Name="MainWindow">{main}</FCParamGroup>
        <FCParamGroup Name="View">{view}</FCParamGroup>{extra}
      </FCParamGroup></FCParamGroup></FCParamGroup></FCParameters>'''


class AppearanceSyncTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / "normal profile/user.cfg"
        self.destination = self.directory / "isolated/user.cfg"
        self.source.parent.mkdir()
        self.destination.parent.mkdir()

    def test_updates_theme_and_colors_without_importing_history_layout_or_mcp(self):
        self.source.write_text(parameters(
            '<FCText Name="Theme">FreeCAD Dark</FCText><FCText Name="Geometry">normal layout</FCText>',
            '<FCUInt Name="BackgroundColor" Value="522133503"/><FCText Name="NavigationStyle">normal navigation</FCText>',
            '<FCParamGroup Name="RecentFiles"><FCText Name="MRU0">private.FCStd</FCText></FCParamGroup>'
            '<FCParamGroup Name="FreeCADMCP"><FCInt Name="rpc_port" Value="9875"/></FCParamGroup>',
        ))
        self.destination.write_text(parameters(
            '<FCText Name="Theme">Classic</FCText><FCText Name="Geometry">isolated layout</FCText>',
            '<FCUInt Name="BackgroundColor" Value="1364283903"/><FCText Name="NavigationStyle">isolated navigation</FCText>',
            '<FCParamGroup Name="FreeCADMCP"><FCInt Name="rpc_port" Value="9876"/></FCParamGroup>',
        ))
        original = self.source.read_bytes()
        self.assertTrue(launcher._sync_appearance_config(self.source, self.destination))
        content = self.destination.read_text()
        self.assertIn('FreeCAD Dark', content)
        self.assertIn('522133503', content)
        self.assertIn('isolated layout', content)
        self.assertIn('isolated navigation', content)
        self.assertIn('Value="9876"', content)
        self.assertNotIn('private.FCStd', content)
        self.assertEqual(self.source.read_bytes(), original)

    def test_refresh_removes_stale_visual_overrides_and_is_idempotent(self):
        self.source.write_text(parameters('<FCText Name="Theme">Classic</FCText>'))
        self.destination.write_text(parameters(
            '<FCText Name="Theme">Dark</FCText><FCText Name="StyleSheet">old.qss</FCText>',
            '<FCUInt Name="BackgroundColor" Value="1"/>',
        ))
        launcher._sync_appearance_config(self.source, self.destination)
        self.assertNotIn('old.qss', self.destination.read_text())
        self.assertNotIn('BackgroundColor', self.destination.read_text())
        original = self.destination.stat().st_mtime_ns
        self.assertFalse(launcher._sync_appearance_config(self.source, self.destination))
        self.assertEqual(self.destination.stat().st_mtime_ns, original)

    def test_new_profile_inherits_accents_editor_and_workbench_colors(self):
        self.source.write_text(parameters(extra='''
          <FCParamGroup Name="Themes"><FCUInt Name="ThemeAccentColor1" Value="42"/></FCParamGroup>
          <FCParamGroup Name="Editor"><FCUInt Name="Keyword" Value="43"/>
            <FCInt Name="TabSize" Value="2"/></FCParamGroup>
          <FCParamGroup Name="Mod"><FCParamGroup Name="Sketcher"><FCParamGroup Name="General">
            <FCUInt Name="GridLineColor" Value="44"/>
            <FCBool Name="AutoRecompute" Value="0"/>
          </FCParamGroup></FCParamGroup></FCParamGroup>'''))
        self.assertTrue(launcher._sync_appearance_config(self.source, self.destination))
        content = self.destination.read_text()
        for key in ('ThemeAccentColor1', 'Keyword', 'GridLineColor'):
            self.assertIn(key, content)
        self.assertNotIn('AutoRecompute', content)
        self.assertNotIn('TabSize', content)
        ET.parse(self.destination)

    def test_invalid_source_preserves_existing_profile(self):
        self.destination.write_text(parameters())
        original = self.destination.read_bytes()
        for content in ('<malformed', '<otherFormat/>'):
            self.source.write_text(content)
            with self.assertRaises(SystemExit):
                launcher._sync_appearance_config(self.source, self.destination)
            self.assertEqual(self.destination.read_bytes(), original)

    def test_config_query_uses_selected_build_and_its_versioned_profile(self):
        self.source.write_text(parameters('<FCText Name="Theme">FreeCAD Dark</FCText>'))
        executable = self.directory / "build/bin/FreeCAD"
        command = executable.with_name("FreeCADCmd")
        command.parent.mkdir(parents=True)
        command.touch()
        with mock.patch.object(launcher, "_launch_env", return_value={"DISPLAY": ":0"}), mock.patch.object(
            launcher.subprocess, "run", return_value=SimpleNamespace(returncode=0, stdout=str(self.source.parent) + "\n")
        ) as run:
            launcher._sync_appearance_preferences(executable, self.destination.parent)
        self.assertEqual(run.call_args.args[0], [str(command), "--get-config", "UserConfigPath"])
        self.assertIn('FreeCAD Dark', self.destination.read_text())


if __name__ == "__main__":
    unittest.main()
