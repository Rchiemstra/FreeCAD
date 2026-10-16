# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2021 Werner Mayer <werner.wm.mayer@gmx.de>              *
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
# ***************************************************************************/


import math, os, sys, unittest
import FreeCAD
import Part
import Sketcher

App = FreeCAD


class TestSketchExpression(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("TestSketchExpr")

    # See https://forum.freecad.org/viewtopic.php?f=3&t=64699
    # and https://forum.freecad.org/viewtopic.php?f=10&t=64718
    def testConstraintWithExpression(self):
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")

        geoList = []
        geoList.append(Part.LineSegment(App.Vector(0, 5, 0), App.Vector(5, 5, 0)))
        geoList.append(Part.LineSegment(App.Vector(5, 5, 0), App.Vector(5, 0, 0)))
        geoList.append(Part.LineSegment(App.Vector(5, 0, 0), App.Vector(0, 0, 0)))
        geoList.append(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(0, 5, 0)))
        sketch.addGeometry(geoList, False)

        conList = []
        conList.append(Sketcher.Constraint("Coincident", 0, 2, 1, 1))
        conList.append(Sketcher.Constraint("Coincident", 1, 2, 2, 1))
        conList.append(Sketcher.Constraint("Coincident", 2, 2, 3, 1))
        conList.append(Sketcher.Constraint("Coincident", 3, 2, 0, 1))
        conList.append(Sketcher.Constraint("Horizontal", 0))
        conList.append(Sketcher.Constraint("Horizontal", 2))
        conList.append(Sketcher.Constraint("Vertical", 1))
        conList.append(Sketcher.Constraint("Vertical", 3))
        sketch.addConstraint(conList)
        del geoList, conList

        # Check that circular references get detected
        length = sketch.addConstraint(Sketcher.Constraint("Distance", 0, 6.0))
        height = sketch.addConstraint(Sketcher.Constraint("Distance", 3, 5.0))
        sketch.renameConstraint(length, "Length")
        sketch.renameConstraint(height, "Height")

        sketch.setExpression("Constraints[{}]".format(height), ".Constraints.Length")

        with self.assertRaises(RuntimeError) as context:
            sketch.setExpression("Constraints.Length", ".Constraints.Length * 2")
        assert ".Constraints.Length reference creates a cyclic dependency." in str(
            context.exception
        )

        with self.assertRaises(RuntimeError) as context:
            sketch.setExpression("Constraints.Length", ".Constraints.Height * 2")
        assert ".Constraints.Length reference creates a cyclic dependency." in str(
            context.exception
        )

    def _externalConstraintLifecycle(self, mode):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            existingDocuments = set(FreeCAD.listDocuments())
            source = FreeCAD.newDocument("SrcDoc")
            try:
                sheet = source.addObject("Spreadsheet::Sheet", "Dims")
                sheet.set("A1", "12")
                sheet.setAlias("A1", "PadLen")
                sheet.set("A2", "30")
                sheet.setAlias("A2", "Wall")
                source.recompute()
                path = os.path.join(directory, "SrcDoc.FCStd")
                source.saveAs(path)
                sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
                sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(30, 0, 0)))
                sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(0, 12, 0)))
                width = sketch.addConstraint(Sketcher.Constraint("Distance", 0, 30.0))
                height = sketch.addConstraint(Sketcher.Constraint("Distance", 1, 12.0))
                sketch.renameConstraint(width, "W")
                sketch.renameConstraint(height, "H")
                self.Doc.saveAs(os.path.join(directory, "DepDoc.FCStd"))
                sketch.setExpression("Constraints[{}]".format(width), "SrcDoc#<<Dims>>.Wall")
                sketch.setExpression("Constraints.H", "SrcDoc#<<Dims>>.PadLen")
                if mode == "relabel":
                    source.Label = "Source dimensions"
                    sketch.setExpression("Constraints.H", "<<Source dimensions>>#<<Dims>>.PadLen")
                self.Doc.recompute()
                self.Doc.save()
                self.Doc.UndoMode = 1
                if mode == "relabel":
                    source.Label = "Renamed dimensions"
                    source.save()
                    bindings = {p.lstrip("."): e for p, e in sketch.ExpressionEngine}
                    self.assertIn("<<Renamed dimensions>>#", bindings["Constraints.H"])
                if mode == "save_as":
                    path = os.path.join(directory, "RenamedSource.FCStd")
                    source.saveAs(path)
                if mode != "open":
                    FreeCAD.closeDocument("SrcDoc")
                    self.Doc.recompute()
                if mode == "name_collision":
                    otherDirectory = os.path.join(directory, "other")
                    os.mkdir(otherDirectory)
                    other = FreeCAD.newDocument("SrcDoc")
                    other.saveAs(os.path.join(otherDirectory, "SrcDoc.FCStd"))
                if mode in ("save_as", "name_collision"):
                    source = FreeCAD.openDocument(path)
                    self.assertNotEqual(source.Name, "SrcDoc")
                    self.Doc.recompute()
                    # Document-name remapping is upstream behavior. These cases
                    # only require safe reopen, recompute and manual repair.
                    self.assertEqual(len(sketch.ExpressionEngine), 2)
                    sketch.setExpression("Constraints.W", None)
                    sketch.setExpression("Constraints.H", None)
                    self.Doc.recompute()
                    self.assertNotIn("Invalid", sketch.State)
                    return
                partial = mode in ("partial", "relabel", "undo_last")
                if partial:
                    sketch.setExpression("Constraints.W", None)
                    self.Doc.recompute()
                    self.Doc.purgeTouched()
                if mode == "undo_last":
                    self.Doc.openTransaction("Clear last closed-source expression")
                    sketch.setExpression("Constraints.H", None)
                    self.Doc.commitTransaction()
                    self.assertEqual(sketch.ExpressionEngine, [])
                    self.Doc.undo()
                    self.assertEqual(len(sketch.ExpressionEngine), 1)
                    self.Doc.redo()
                    self.assertEqual(sketch.ExpressionEngine, [])
                    self.Doc.undo()
                    self.assertEqual(len(sketch.ExpressionEngine), 1)
                    self.Doc.purgeTouched()
                if mode in ("reopen", "partial", "relabel", "undo_last"):
                    source = FreeCAD.openDocument(path)
                    self.Doc.recompute()
                    self.assertNotIn("Invalid", sketch.State)
                    self.assertEqual(sketch.getDatum(width).Value, 30)
                    # Verify the runtime dependency was reattached, too.
                    source.Dims.set("A1" if partial else "A2", "18" if partial else "42")
                    source.recompute()
                    self.Doc.recompute()
                    self.assertEqual(sketch.getDatum(height if partial else width).Value,
                                     18 if partial else 42)
                else:
                    self.Doc.openTransaction("Clear width expression")
                    sketch.setExpression("Constraints.W", None)
                    self.Doc.commitTransaction()
                    self.assertFalse(any(p.lstrip(".") == "Constraints.W" for p, _ in sketch.ExpressionEngine))
                    self.Doc.undo()
                    self.assertTrue(any(p.lstrip(".") == "Constraints.W" for p, _ in sketch.ExpressionEngine))
                    self.Doc.redo()
                    self.assertFalse(any(p.lstrip(".") == "Constraints.W" for p, _ in sketch.ExpressionEngine))
                    sketch.setExpression("Constraints.H", None)
                    sketch.setDatum(width, App.Units.Quantity("35 mm"))
                    self.Doc.recompute()
                    self.assertNotIn("Invalid", sketch.State)
                    self.assertEqual(sketch.getDatum(width).Value, 35)
            finally:
                for name in list(FreeCAD.listDocuments()):
                    if name not in existingDocuments:
                        FreeCAD.closeDocument(name)

    def testExternalConstraintHappyOpen(self):
        self._externalConstraintLifecycle("open")

    def testExternalConstraintSourceReopened(self):
        self._externalConstraintLifecycle("reopen")

    def testExternalConstraintClearSourceClosed(self):
        self._externalConstraintLifecycle("closed")

    def testExternalConstraintRetainedBindingSourceReopened(self):
        self._externalConstraintLifecycle("partial")

    def testExternalConstraintRelabeledSourcePartialClearReopened(self):
        self._externalConstraintLifecycle("relabel")

    def testExternalConstraintLastClosedBindingUndoReopened(self):
        self._externalConstraintLifecycle("undo_last")

    def testExternalConstraintSourceSaveAsDifferentBasenameSafe(self):
        self._externalConstraintLifecycle("save_as")

    def testExternalConstraintSourceSameNameDifferentFileSafe(self):
        self._externalConstraintLifecycle("name_collision")

    def _closedSourceCommitRepair(self, closed):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            source = FreeCAD.newDocument("SrcDoc")
            try:
                dims = source.addObject("Spreadsheet::Sheet", "Dims")
                for cell, value, alias in (("A1", "30", "Wall"), ("A2", "12", "PadLen")):
                    dims.set(cell, value)
                    dims.setAlias(cell, alias)
                source.recompute()
                source.saveAs(os.path.join(directory, "SrcDoc.FCStd"))
                sk = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
                for end, value, name in ((App.Vector(30, 0, 0), 30, "W"), (App.Vector(0, 15, 0), 15, "H")):
                    geo = sk.addGeometry(Part.LineSegment(App.Vector(), end))
                    index = sk.addConstraint(Sketcher.Constraint("Distance", geo, value))
                    sk.renameConstraint(index, name)
                other = self.Doc.addObject("Part::Box", "Other")
                other.addProperty("App::PropertyLink", "Profile")
                other.Profile = sk
                self.Doc.saveAs(os.path.join(directory, "DepDoc.FCStd"))
                sk.setExpression("Constraints.W", "SrcDoc#<<Dims>>.Wall")
                sk.setExpression("Constraints.H", "SrcDoc#<<Dims>>.Wall / 2")
                other.setExpression("Length", "SrcDoc#<<Dims>>.PadLen")
                self.Doc.recompute()
                self.Doc.save()
                self.Doc.UndoMode = 1
                if closed:
                    FreeCAD.closeDocument("SrcDoc")
                def commit(callback, deferred=False):
                    before = self.Doc.UndoCount
                    result = self.Doc.commitCompatibilityMutation(callback, recompute=not deferred)
                    self.assertEqual(result["status"], "Committed", result)
                    self.assertEqual(self.Doc.UndoCount, before + 1)
                commit(lambda: sk.setExpression("Constraints.W", None), deferred=True)
                self.Doc.recompute()
                self.Doc.purgeTouched()
                beforeErrors = [(o.Name, list(o.State), o.getStatusString()) for o in self.Doc.Objects]
                result = self.Doc.commitCompatibilityMutation(
                    lambda: sk.setExpression("Constraints.H", None))
                self.assertNotEqual(result["status"], "RollbackFailed", result)
                self.assertFalse(self.Doc.getMutationReadiness().get("quarantined"), result)
                # Failure is an ordinary rollback; source-free repair can follow.
                if not result["committed"]:
                    self.assertEqual([(o.Name, list(o.State), o.getStatusString()) for o in self.Doc.Objects], beforeErrors)
                    commit(lambda: sk.setExpression("Constraints.H", None), deferred=True)
                commit(lambda: sk.setDatum(0, App.Units.Quantity("35 mm")), deferred=True)
                self.assertEqual(sk.getDatum(0).Value, 35)
                self.Doc.undo()
                self.assertEqual(sk.getDatum(0).Value, 30)
                self.Doc.redo()
                self.assertEqual(sk.getDatum(0).Value, 35)
                commit(lambda: other.setExpression("Length", None), deferred=True)
                self.Doc.recompute()
                self.assertNotIn("Invalid", sk.State)
                self.assertNotIn("Invalid", other.State)
            finally:
                if "SrcDoc" in FreeCAD.listDocuments():
                    FreeCAD.closeDocument("SrcDoc")

    def testExternalConstraintCommitRepairHappyOpen(self):
        self._closedSourceCommitRepair(False)

    def testExternalConstraintCommitRepairMissingSource(self):
        self._closedSourceCommitRepair(True)

    def tearDown(self):
        # comment out to omit closing document for debugging
        FreeCAD.closeDocument(self.Doc.Name)
