# SPDX-License-Identifier: LGPL-2.1-or-later
"""GUI integration and real --diff startup coverage (registered in InitGui)."""

import json
import os
import subprocess
import textwrap
import unittest
from pathlib import Path
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import modeldiff as Diff
import TestModelDiff as Fixtures


class ModelDiffGuiTests(unittest.TestCase):
    setUp = Fixtures.ModelDiffTests.setUp
    tearDown = Fixtures.ModelDiffTests.tearDown
    box = Fixtures.ModelDiffTests.box
    save = Fixtures.ModelDiffTests.save
    compare = Fixtures.ModelDiffTests.compare
    changes = Fixtures.ModelDiffTests.changes
    _external_pair = Fixtures.ModelDiffTests._external_pair
    test_available_external_link = Fixtures.ModelDiffTests.test_available_external_link
    test_open_edited_external_file_untouched = (
        Fixtures.ModelDiffTests.test_open_edited_external_file_untouched
    )
    test_missing_external_link = Fixtures.ModelDiffTests.test_missing_external_link

    def test_colours_groups_and_faint_common(self):
        self.assertIn("TestModelDiffGui", App.__unit_test__)
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        shown = Diff.show_result(self.compare())
        Gui.updateGui()
        roles = {obj.DiffRole: obj for obj in shown.Changed.Group}
        self.assertEqual(set(roles), {"Added", "Removed", "Unchanged"})
        self.assertNotEqual(
            roles["Added"].ViewObject.ShapeColor, roles["Removed"].ViewObject.ShapeColor
        )
        self.assertEqual(roles["Unchanged"].ViewObject.Transparency, 85)
        self.assertEqual(shown.FileName, "")
        self.assertTrue(all(obj.ViewObject.Visibility for obj in roles.values()))

    def test_hidden_intermediates_not_rendered(self):
        box = self.box()
        hidden = self.box("Intermediate")
        hidden.ViewObject.Visibility = False
        self.save()
        hidden.Length = 30
        box.Length = 20
        self.save(True)
        result = self.compare()
        self.assertEqual({v.name for v in result.visuals}, {"Box"})
        self.assertIn("Intermediate", {obj.name for obj in result.objects})

    def test_hidden_part_hides_children(self):
        part = self.doc.addObject("App::Part", "Container")
        box = self.box()
        part.addObject(box)
        part.ViewObject.Visibility = False
        self.save()
        box.Length = 20
        self.save(True)
        self.assertEqual(self.compare().visuals, [])

    def test_hidden_link_does_not_hide_target(self):
        visible = self.doc.addObject("App::Part", "VisiblePart")
        box = self.box()
        visible.addObject(box)
        hidden = self.doc.addObject("App::Part", "HiddenPart")
        link = self.doc.addObject("App::Link", "Instance")
        link.setLink(visible)
        hidden.addObject(link)
        hidden.ViewObject.Visibility = False
        visible.ViewObject.Visibility = box.ViewObject.Visibility = True
        self.save()
        box.Length = 20
        self.save(True)
        self.assertEqual({v.name for v in self.compare().visuals}, {"Box"})

    def test_report_view_and_shared_entry(self):
        from PySide import QtWidgets

        box = self.box()
        self.save()
        box.Length = 20
        self.save(True)
        result = self.compare()
        with patch.object(App.Console, "PrintMessage") as message:
            shown = Diff.run_cli(self.local, self.remote)
        message.assert_called_once_with(result.report())
        self.assertEqual(shown.DiffReport, result.report())
        report = Gui.getMainWindow().findChild(QtWidgets.QDockWidget, "Report view")
        self.assertIsNotNone(report)
        self.assertFalse(report.isHidden())

    def test_mesh_overlay_colours(self):
        import Mesh

        mesh = self.doc.addObject("Mesh::Feature", "Mesh")
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(10, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save()
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(20, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save(True)
        shown = Diff.show_result(self.compare())
        self.assertEqual(len(shown.Changed.Group), 2)
        old, new = shown.Changed.Group
        self.assertNotEqual(old.ViewObject.ShapeColor, new.ViewObject.ShapeColor)
        self.assertEqual(old.TypeId, "Mesh::Feature")

    def test_visibility_change_is_reported(self):
        box = self.box()
        box.ViewObject.Visibility = False
        self.save()
        box.ViewObject.Visibility = True
        self.save(True)
        result = self.compare()
        self.assertEqual({v.role for v in result.visuals}, {"Added"})
        change = next(obj for obj in result.objects if obj.name == "Box")
        self.assertEqual(change.properties["View.Visibility"], ("false", "true"))

    def test_cli_gui_startup(self):
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        expected = self.compare().report()
        # Observe the real startup entry point from a child process. The shim
        # delegates to the installed implementation, records the returned GUI
        # document, then exits the child without a save-confirmation dialog.
        shim_dir = Path(self.tmp.name) / "shim"
        shim_dir.mkdir()
        output = Path(self.tmp.name) / "gui-result.json"
        (shim_dir / "Init.py").write_text("# Test-only startup observer\n")
        (shim_dir / "InitGui.py").write_text("import diff_cli_observer\n")
        (shim_dir / "diff_cli_observer.py").write_text(textwrap.dedent(f"""\
            import json, os
            import FreeCAD as App
            import modeldiff
            _original_run_cli = modeldiff.run_cli
            def run_cli(local, remote):
                doc = _original_run_cli(local, remote)
                from PySide import QtCore
                def record():
                    data = {{'report': doc.DiffReport, 'filename': doc.FileName,
                            'sources': [o.SourceName for o in doc.Changed.Group],
                            'roles': sorted(o.DiffRole for o in doc.Changed.Group),
                            'groups': sorted(o.Name for o in doc.Objects
                                             if o.TypeId == 'App::DocumentObjectGroup'),
                            'documents': len(App.listDocuments())}}
                    with open({str(output)!r}, 'w') as stream:
                        json.dump(data, stream)
                    os._exit(0)
                QtCore.QTimer.singleShot(200, record)
                return doc
            modeldiff.run_cli = run_cli
        """))
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCAD")
        process = subprocess.run(
            check=False,
            args=[binary, "-M", str(shim_dir), "--diff", self.local, self.remote],
            text=True,
            capture_output=True,
            timeout=180,
            env={**os.environ, "QTWEBENGINE_DISABLE_SANDBOX": "1"},
        )
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertTrue(output.exists(), process.stdout + process.stderr)
        observed = json.loads(output.read_text())
        self.assertEqual(observed["report"], expected)
        self.assertEqual(observed["filename"], "")
        self.assertEqual(observed["roles"], ["Added", "Removed", "Unchanged"])
        self.assertEqual(
            observed["groups"], ["Added", "Changed", "Removed", "Unchanged"]
        )
        self.assertEqual(observed["sources"], ["Box"] * 3)
        self.assertEqual(observed["documents"], 1)
