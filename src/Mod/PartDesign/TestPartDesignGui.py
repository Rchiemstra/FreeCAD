# SPDX-License-Identifier: LGPL-2.1-or-later

# **************************************************************************
#   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
#                                                                         *
#   This file is part of the FreeCAD CAx development system.              *
#                                                                         *
#   This program is free software; you can redistribute it and/or modify  *
#   it under the terms of the GNU Lesser General Public License (LGPL)    *
#   as published by the Free Software Foundation; either version 2 of     *
#   the License, or (at your option) any later version.                   *
#   for detail see the LICENCE text file.                                 *
#                                                                         *
#   FreeCAD is distributed in the hope that it will be useful,            *
#   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
#   GNU Library General Public License for more details.                  *
#                                                                         *
#   You should have received a copy of the GNU Library General Public     *
#   License along with FreeCAD; if not, write to the Free Software        *
#   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
#   USA                                                                   *
# **************************************************************************

import FreeCAD
import FreeCADGui
import os
import sys
import unittest
import Sketcher
import Part
import PartDesign
import PartDesignGui
import tempfile

from PySide import QtGui, QtCore
from PySide.QtGui import QApplication

from PartDesignTests.TestMaterial import TestMaterial
from PartDesignTests.TestAllowanceProfileGui import TestAllowanceProfileGui
from PartDesignTests.TestTaskFeatureCompletion import TestTaskFeatureCompletion
from PartDesignTests.TestActiveObject import TestActiveObject
from PartDesignTests.TestSuppressed import TestSuppressedStrikethrough
from Test.GuiRecompute import close_document, close_document_by_name, recompute_document, save_document_as


# timer runs this class in order to access modal dialog
class CallableCheckWorkflow:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        self.test.assertIsNotNone(dialog, "Dialog box could not be found")
        if dialog is not None:
            dialogcheck = CallableCheckDialogWasClosed(self.test)
            QtCore.QTimer.singleShot(500, dialogcheck)
            QtCore.QTimer.singleShot(0, dialog, QtCore.SLOT("accept()"))


class CallableCheckDialogWasClosed:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        self.test.assertIsNone(dialog, "Dialog box was not closed by accept()")


class CallableCheckWarning:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        self.test.assertIsNotNone(dialog, "Input dialog box could not be found")
        if dialog is not None:
            QtCore.QTimer.singleShot(0, dialog, QtCore.SLOT("accept()"))


class CallableComboBox:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        self.test.assertIsNotNone(dialog, "Warning dialog box could not be found")
        if dialog is not None:
            cbox = dialog.findChild(QtGui.QComboBox)
            self.test.assertIsNotNone(cbox, "ComboBox widget could not be found")
            if cbox is not None:
                QtCore.QTimer.singleShot(0, dialog, QtCore.SLOT("accept()"))


class CallableCheckExemptionDialog:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        if dialog is not None:
            dialogcheck = CallableCheckExemptionDialogWasClosed(self.test)
            QtCore.QTimer.singleShot(100, dialogcheck)
            QtCore.QTimer.singleShot(0, dialog, QtCore.SLOT("accept()"))


class CallableCheckExemptionDialogWasClosed:
    def __init__(self, test):
        self.test = test

    def __call__(self):
        dialog = QApplication.activeModalWidget()
        self.test.assertIsNone(dialog, "Dialog box was not closed by accept()")


App = FreeCAD
Gui = FreeCADGui


# ---------------------------------------------------------------------------
# define the test cases to test the FreeCAD PartDesign module
# ---------------------------------------------------------------------------
class PartDesignGuiTestCases(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("SketchGuiTest")

    def testRefuseToMoveSingleFeature(self):
        FreeCAD.Console.PrintMessage(
            "Testing refuse to move the feature with dependencies from one body to another\n"
        )
        self.BodySource = self.Doc.addObject("PartDesign::Body", "Body")
        Gui.activateView("Gui::View3DInventor", True)
        Gui.activeView().setActiveObject("pdbody", self.BodySource)

        self.BoxObj = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.BoxObj.Length = 10.0
        self.BoxObj.Width = 10.0
        self.BoxObj.Height = 10.0
        self.BodySource.addObject(self.BoxObj)

        recompute_document(App.ActiveDocument)

        self.Sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        self.Sketch.AttachmentSupport = (self.BoxObj, ("Face3",))
        self.Sketch.MapMode = "FlatFace"
        self.BodySource.addObject(self.Sketch)

        geoList = []
        geoList.append(Part.LineSegment(App.Vector(2.0, 8.0, 0), App.Vector(8.0, 8.0, 0)))
        geoList.append(Part.LineSegment(App.Vector(8.0, 8.0, 0), App.Vector(8.0, 2.0, 0)))
        geoList.append(Part.LineSegment(App.Vector(8.0, 2.0, 0), App.Vector(2.0, 2.0, 0)))
        geoList.append(Part.LineSegment(App.Vector(2.0, 2.0, 0), App.Vector(2.0, 8.0, 0)))
        self.Sketch.addGeometry(geoList, False)
        conList = []
        conList.append(Sketcher.Constraint("Coincident", 0, 2, 1, 1))
        conList.append(Sketcher.Constraint("Coincident", 1, 2, 2, 1))
        conList.append(Sketcher.Constraint("Coincident", 2, 2, 3, 1))
        conList.append(Sketcher.Constraint("Coincident", 3, 2, 0, 1))
        conList.append(Sketcher.Constraint("Horizontal", 0))
        conList.append(Sketcher.Constraint("Horizontal", 2))
        conList.append(Sketcher.Constraint("Vertical", 1))
        conList.append(Sketcher.Constraint("Vertical", 3))
        self.Sketch.addConstraint(conList)

        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.Pad.Profile = self.Sketch
        self.Pad.Length = 10.000000
        self.Pad.Length2 = 100.000000
        self.Pad.Type = 0
        self.Pad.UpToFace = None
        self.Pad.Reversed = 0
        self.Pad.SideType = "One side"
        self.Pad.Offset = 0.000000

        self.BodySource.addObject(self.Pad)

        recompute_document(self.Doc)
        Gui.ActiveDocument.ActiveView.sendMessage("ViewFit")

        self.BodyTarget = self.Doc.addObject("PartDesign::Body", "Body")

        Gui.Selection.addSelection(App.ActiveDocument.Pad)
        cobj = CallableCheckWarning(self)
        QtCore.QTimer.singleShot(500, cobj)
        Gui.runCommand("PartDesign_MoveFeature")
        # assert dependencies of the Sketch
        self.assertEqual(len(self.BodySource.Group), 3, "Source body feature count is wrong")
        self.assertEqual(len(self.BodyTarget.Group), 0, "Target body feature count is wrong")

    def testMoveSingleFeature(self):
        FreeCAD.Console.PrintMessage("Testing moving one feature from one body to another\n")
        self.BodySource = self.Doc.addObject("PartDesign::Body", "Body")
        Gui.activateView("Gui::View3DInventor", True)
        Gui.activeView().setActiveObject("pdbody", self.BodySource)

        self.Sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        self.BodySource.addObject(self.Sketch)
        self.Sketch.AttachmentSupport = (self.BodySource.Origin.OriginFeatures[3], [""])
        self.Sketch.MapMode = "FlatFace"

        geoList = []
        geoList.append(
            Part.LineSegment(
                App.Vector(-10.000000, 10.000000, 0), App.Vector(10.000000, 10.000000, 0)
            )
        )
        geoList.append(
            Part.LineSegment(
                App.Vector(10.000000, 10.000000, 0), App.Vector(10.000000, -10.000000, 0)
            )
        )
        geoList.append(
            Part.LineSegment(
                App.Vector(10.000000, -10.000000, 0), App.Vector(-10.000000, -10.000000, 0)
            )
        )
        geoList.append(
            Part.LineSegment(
                App.Vector(-10.000000, -10.000000, 0), App.Vector(-10.000000, 10.000000, 0)
            )
        )
        self.Sketch.addGeometry(geoList, False)
        conList = []
        conList.append(Sketcher.Constraint("Coincident", 0, 2, 1, 1))
        conList.append(Sketcher.Constraint("Coincident", 1, 2, 2, 1))
        conList.append(Sketcher.Constraint("Coincident", 2, 2, 3, 1))
        conList.append(Sketcher.Constraint("Coincident", 3, 2, 0, 1))
        conList.append(Sketcher.Constraint("Horizontal", 0))
        conList.append(Sketcher.Constraint("Horizontal", 2))
        conList.append(Sketcher.Constraint("Vertical", 1))
        conList.append(Sketcher.Constraint("Vertical", 3))
        self.Sketch.addConstraint(conList)

        self.Pad = self.Doc.addObject("PartDesign::Pad", "Pad")
        self.BodySource.addObject(self.Pad)
        self.Pad.Profile = self.Sketch
        self.Pad.Length = 10.000000
        self.Pad.Length2 = 100.000000
        self.Pad.Type = 0
        self.Pad.UpToFace = None
        self.Pad.Reversed = 0
        self.Pad.SideType = "One side"
        self.Pad.Offset = 0.000000

        recompute_document(self.Doc)
        Gui.ActiveDocument.ActiveView.sendMessage("ViewFit")

        self.BodyTarget = self.Doc.addObject("PartDesign::Body", "Body")

        Gui.Selection.addSelection(App.ActiveDocument.Pad)
        cobj = CallableComboBox(self)
        QtCore.QTimer.singleShot(500, cobj)
        Gui.runCommand("PartDesign_MoveFeature")
        # assert dependencies of the Sketch
        recompute_document(self.Doc)

        self.assertFalse(
            self.Sketch.AttachmentSupport[0][0] in self.BodySource.Origin.OriginFeatures
        )
        self.assertTrue(
            self.Sketch.AttachmentSupport[0][0] in self.BodyTarget.Origin.OriginFeatures
        )
        self.assertEqual(len(self.BodySource.Group), 0, "Source body feature count is wrong")
        self.assertEqual(len(self.BodyTarget.Group), 2, "Target body feature count is wrong")

    def tearDown(self):
        close_document_by_name("SketchGuiTest")


class PartDesignTransformed(unittest.TestCase):
    def setUp(self):
        self.Doc = App.newDocument("PartDesignTransformed")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.BoxObj = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.BoxObj.Length = 10.0
        self.BoxObj.Width = 10.0
        self.BoxObj.Height = 10.0
        recompute_document(App.ActiveDocument)
        # not adding box to the body to imitate undertermined workflow
        tempDir = tempfile.gettempdir()
        self.TempDoc = os.path.join(tempDir, "PartDesignTransformed.FCStd")
        if os.path.exists(self.TempDoc):
            os.remove(self.TempDoc)
        save_document_as(App.ActiveDocument, self.TempDoc)
        close_document_by_name("PartDesignTransformed")

    def tearDown(self):
        # closing doc
        if App.ActiveDocument is not None and App.ActiveDocument.Name == PartDesignTransformed:
            close_document_by_name("PartDesignTransformed")
        # print ("omit closing document for debugging")

    def testMultiTransformCase(self):
        App.Console.PrintMessage("Testing applying MultiTransform to the Box outside the body\n")
        App.open(self.TempDoc)
        App.setActiveDocument("PartDesignTransformed")
        Gui.Selection.addSelection(App.ActiveDocument.Box)

        workflowcheck = CallableCheckWorkflow(self)
        QtCore.QTimer.singleShot(500, workflowcheck)
        Gui.runCommand("PartDesign_MultiTransform")

        close_document_by_name("PartDesignTransformed")


class CreateSketch(unittest.TestCase):

    def testPDCreateSketch(self):
        App.Console.PrintMessage("Testing the creation of a sketch\n")
        param = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/PartDesign")
        useAttachmentSaved = param.GetBool("NewSketchUseAttachmentDialog", False)
        param.SetBool("NewSketchUseAttachmentDialog", False)
        App.newDocument()
        App.activeDocument().addObject("PartDesign::Body", "Body")
        App.ActiveDocument.getObject("Body").Label = "Body"
        App.ActiveDocument.getObject("Body").AllowCompound = True
        FreeCADGui.activateView("Gui::View3DInventor", True)
        FreeCADGui.activeView().setActiveObject("pdbody", App.activeDocument().Body)
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.runCommand("Std_OrthographicCamera", 1)
        workflowcheck = CallableCheckExemptionDialog(self)
        QtCore.QTimer.singleShot(100, workflowcheck)
        FreeCADGui.runCommand("PartDesign_CompSketches", 0)
        activeDialog = FreeCADGui.Control.activeDialog()
        self.assertIsNotNone(activeDialog)
        if activeDialog is not None:
            FreeCADGui.Control.closeDialog()
        close_document(App.ActiveDocument)
        param.SetBool("NewSketchUseAttachmentDialog", useAttachmentSaved)


# class PartDesignGuiTestCases(unittest.TestCase):
#   def setUp(self):
#       self.Doc = FreeCAD.newDocument("SketchGuiTest")
#
#   def testBoxCase(self):
#       self.Box = self.Doc.addObject('PartDesign::SketchObject','SketchBox')
#       self.Box.addGeometry(Part.LineSegment(App.Vector(-99.230339,36.960674,0),App.Vector(69.432587,36.960674,0)))
#       self.Box.addGeometry(Part.LineSegment(App.Vector(69.432587,36.960674,0),App.Vector(69.432587,-53.196629,0)))
#       self.Box.addGeometry(Part.LineSegment(App.Vector(69.432587,-53.196629,0),App.Vector(-99.230339,-53.196629,0)))
#       self.Box.addGeometry(Part.LineSegment(App.Vector(-99.230339,-53.196629,0),App.Vector(-99.230339,36.960674,0)))
#
#   def tearDown(self):
#       #closing doc
#       FreeCAD.closeDocument("SketchGuiTest")


class TestShapeBinder(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestShapeBinder")

    def testDefaultColor(self):
        """
        A shape binder uses a different default color than a Part feature.
        This color must still be set after its creation.
        """
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        self.Body.addObject(self.Box)
        recompute_document(self.Doc)
        binder = self.Doc.addObject("PartDesign::ShapeBinder", "ShapeBinder")
        binder.Support = [(self.Box, "Face1")]

        grp = App.ParamGet("User parameter:BaseApp/Preferences/Mod/PartDesign")
        packed_color = grp.GetUnsigned("DefaultDatumColor", 0xFFD70099)
        r, g, b, a = binder.ViewObject.ShapeColor
        color = (
            int(r * 255.0 + 0.5) << 24
            | int(g * 255.0 + 0.5) << 16
            | int(b * 255.0 + 0.5) << 8
            | int(a * 255.0 + 0.5)
        )

        self.assertEqual(packed_color, color)

    def tearDown(self):
        close_document(self.Doc)


class TestSubShapeBinder(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestSubShapeBinder")

    def tearDown(self):
        close_document(self.Doc)

    def testDefaultColor(self):
        """
        A sub-shape binder uses a different default color than a Part feature.
        This color must still be set after its creation.
        """
        body = self.Doc.addObject("PartDesign::Body", "Body")
        box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        body.addObject(box)

        recompute_document(self.Doc)
        binder = body.newObject("PartDesign::SubShapeBinder", "Binder")
        binder.Support = [(box, ("Face1"))]

        grp = App.ParamGet("User parameter:BaseApp/Preferences/Mod/PartDesign")
        packed_color = grp.GetUnsigned("DefaultDatumColor", 0xFFD70099)
        r, g, b, a = binder.ViewObject.ShapeColor
        color = (
            int(r * 255.0 + 0.5) << 24
            | int(g * 255.0 + 0.5) << 16
            | int(b * 255.0 + 0.5) << 8
            | int(a * 255.0 + 0.5)
        )

        self.assertEqual(packed_color, color)


class TestDatumPlane(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDatumPlane")

    def tearDown(self):
        close_document(self.Doc)

    def testDefaultColor(self):
        """
        A datum object uses a different default color than a Part feature.
        This color must still be set after its creation.
        """
        body = self.Doc.addObject("PartDesign::Body", "Body")
        box = self.Doc.addObject("PartDesign::AdditiveBox", "Box")
        body.addObject(box)

        recompute_document(self.Doc)
        datum = body.newObject("PartDesign::Plane", "DatumPlane")
        datum.AttachmentSupport = [(box, "Face6")]
        datum.MapMode = "FlatFace"
        recompute_document(self.Doc)

        grp = App.ParamGet("User parameter:BaseApp/Preferences/Mod/PartDesign")
        packed_color = grp.GetUnsigned("DefaultDatumColor", 0xFFD70099)
        r, g, b, a = datum.ViewObject.ShapeColor
        color = (
            int(r * 255.0 + 0.5) << 24
            | int(g * 255.0 + 0.5) << 16
            | int(b * 255.0 + 0.5) << 8
            | int(a * 255.0 + 0.5)
        )

        self.assertEqual(packed_color, color)


class TestBinderDatumDisplay(unittest.TestCase):
    def setUp(self):
        self.Doc = App.newDocument("BinderDatumDisplay")
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        close_document(self.Doc)

    @staticmethod
    def _nodes(root, typeName):
        from pivy import coin

        search = coin.SoSearchAction()
        search.setType(coin.SoType.fromName(typeName))
        search.setInterest(coin.SoSearchAction.ALL)
        search.setSearchingAll(True)
        search.apply(root)
        return [path.getTail() for path in search.getPaths()]

    def _binders(self, support, refine=True):
        for kind in ("SubShapeBinder", "ShapeBinder"):
            binder = self.Doc.addObject("PartDesign::" + kind, "Binder")
            if kind == "SubShapeBinder":
                binder.Refine = refine
            binder.Support = support
            recompute_document(self.Doc)
            binder.ViewObject.show()
            Gui.updateGui()
            yield binder

    def _points(self, binder, minimum):
        coords = self._nodes(binder.ViewObject.RootNode, "SoCoordinate3")[0]
        self.assertGreaterEqual(coords.point.getNum(), minimum)
        return [App.Vector(*point.getValue()) for point in coords.point.getValues()]

    def _checkPlane(self, binder, placement):
        points = self._points(binder, 4)
        faces = self._nodes(binder.ViewObject.RootNode, "SoBrepFaceSet")[0]
        self.assertEqual(list(faces.partIndex.getValues()), [2])
        size = 50
        local = placement.inverse() * binder.Placement
        points = [local.multVec(point) for point in points]
        for point in points:
            self.assertAlmostEqual(point.z, 0, places=4)
            self.assertLessEqual(abs(point.x), size + 1e-4)
            self.assertLessEqual(abs(point.y), size + 1e-4)
        self.assertAlmostEqual(min(p.x for p in points), -size, places=4)
        self.assertAlmostEqual(max(p.x for p in points), size, places=4)
        self.assertAlmostEqual(min(p.y for p in points), -size, places=4)
        self.assertAlmostEqual(max(p.y for p in points), size, places=4)
        self.assertGreater(binder.Shape.BoundBox.DiagonalLength, 1e90)
        self.assertEqual(len(binder.Shape.Faces), 1)
        self.assertEqual(len(binder.Shape.Face1.Edges), 0)
        # The rendered face retains the Face1 selection detail.
        self._checkTopology(binder)
        self._checkPick(
            binder, "Face1", placement.Base, placement.Rotation.multVec(App.Vector(0, 0, 1))
        )

    def _checkTopology(self, binder):
        root = binder.ViewObject.RootNode
        faces = self._nodes(root, "SoBrepFaceSet")[0]
        edges = self._nodes(root, "SoBrepEdgeSet")[0]
        vertices = self._nodes(root, "SoBrepPointSet")[0]
        coords = self._nodes(root, "SoCoordinate3")[0]
        self.assertEqual(faces.partIndex.getNum(), len(binder.Shape.Faces))
        self.assertEqual(list(edges.coordIndex.getValues()).count(-1), len(binder.Shape.Edges))
        self.assertEqual(
            coords.point.getNum() - vertices.startIndex.getValue(), len(binder.Shape.Vertexes)
        )

    def _checkPick(self, binder, element, point, normal):
        from pivy import coin

        path = coin.SoPath()
        detail = binder.ViewObject.getDetailPath(element, path)
        self.assertIsNotNone(detail)
        # Use the detail returned for highlighting on the actual picked path.
        pick = coin.SoRayPickAction(coin.SbViewportRegion(100, 100))
        pick.setRay(coin.SbVec3f(*(point + normal * 10)), coin.SbVec3f(*(-normal)))
        pick.setPickAll(True)
        pick.apply(path)
        hits = [
            p for p in pick.getPickedPointList()
            if binder.ViewObject.getElementPicked(p) == element
        ]
        self.assertTrue(hits, element)
        picked = hits[0]
        picked.setDetail(detail, picked.getPath().getTail())
        self.assertEqual(binder.ViewObject.getElementPicked(picked), element)

    def testOriginXZPlane(self):
        source = self.Doc.XZ_Plane
        for binder in self._binders([(source, [""])]):
            with self.subTest(binder=binder.TypeId):
                self._checkPlane(binder, source.Placement)

    def testRotatedDatumPlane(self):
        source = self.Body.newObject("PartDesign::Plane", "DatumPlane")
        source.Placement = App.Placement(App.Vector(11, 22, 33), App.Rotation(0, 30, 0))
        for binder in self._binders([(source, [""])]):
            with self.subTest(binder=binder.TypeId):
                self._checkPlane(binder, source.Placement)

    def testRotatedOriginPlaneInPart(self):
        part = self.Doc.addObject("App::Part", "Part")
        part.Placement = App.Placement(App.Vector(17, 23, 31), App.Rotation(20, 10, 15))
        part.addObject(self.Body)
        self.Body.Placement = App.Placement(App.Vector(7, 8, 9), App.Rotation(0, 30, 0))
        source = self.Doc.XZ_Plane
        binder = self.Doc.addObject("PartDesign::SubShapeBinder", "Binder")
        path = self.Body.Name + "." + self.Body.Origin.Name + "." + source.Name + "."
        binder.Support = [(part, [path])]
        recompute_document(self.Doc)
        binder.ViewObject.show()
        Gui.updateGui()
        self._checkPlane(binder, part.Placement * self.Body.Placement * source.Placement)

    def testDatumLine(self):
        source = self.Body.newObject("PartDesign::Line", "DatumLine")
        source.Placement = App.Placement(App.Vector(11, 22, 33), App.Rotation(0, 30, 0))
        size = 50
        for binder in self._binders([(source, [""])]):
            with self.subTest(binder=binder.TypeId):
                points = self._points(binder, 2)
                local = source.Placement.inverse() * binder.Placement
                points = [local.multVec(point) for point in points]
                for point in points:
                    self.assertAlmostEqual(point.x, 0, places=4)
                    self.assertAlmostEqual(point.y, 0, places=4)
                    self.assertLessEqual(abs(point.z), size + 1e-4)
                self.assertAlmostEqual(min(p.z for p in points), -size, places=4)
                self.assertAlmostEqual(max(p.z for p in points), size, places=4)
                self.assertGreater(binder.Shape.BoundBox.DiagonalLength, 1e90)
                self.assertEqual(len(binder.Shape.Vertexes), 0)
                self._checkTopology(binder)

    def testMixedCompound(self):
        datum = self.Body.newObject("PartDesign::Plane", "DatumPlane")
        datum.Placement = App.Placement(App.Vector(11, 22, 33), App.Rotation(0, 30, 0))
        recompute_document(self.Doc)
        source = self.Doc.addObject("Part::Feature", "Mixed")
        finite = Part.makePlane(12, 8, App.Vector(100, 200, 300))
        # Infinite face first: temporary boundary edges would shift all finite edge indices.
        infinite = datum.Shape.copy()
        infinite.reverse()
        source.Shape = Part.makeCompound([infinite, finite])
        source.Placement = App.Placement(App.Vector(7, 13, 19), App.Rotation(10, 20, 30))
        for binder in self._binders([(source, ["Face1", "Face2"])], refine=False):
            with self.subTest(binder=binder.TypeId):
                points = self._points(binder, 8)
                faces = self._nodes(binder.ViewObject.RootNode, "SoBrepFaceSet")[0]
                self.assertEqual(list(faces.partIndex.getValues()), [2, 2])
                self._checkTopology(binder)
                indices = list(faces.coordIndex.getValues())
                local = source.Placement.inverse() * binder.Placement
                finitePoints = [local.multVec(points[i]) for i in indices[8:] if i >= 0]
                for point in finitePoints:
                    self.assertAlmostEqual(point.z, 300, places=4)
                    self.assertGreaterEqual(point.x, 100 - 1e-4)
                    self.assertLessEqual(point.x, 112 + 1e-4)
                    self.assertGreaterEqual(point.y, 200 - 1e-4)
                    self.assertLessEqual(point.y, 208 + 1e-4)
                datumLocal = datum.Placement.inverse() * local
                for i in indices[:8]:
                    if i >= 0:
                        point = datumLocal.multVec(points[i])
                        self.assertAlmostEqual(point.z, 0, places=4)
                        self.assertAlmostEqual(abs(point.x), 50, places=4)
                        self.assertAlmostEqual(abs(point.y), 50, places=4)
                a, b, c = [points[i] for i in indices[:3]]
                normal = local.Rotation.multVec((b - a).cross(c - a))
                expected = datum.Placement.Rotation.multVec(App.Vector(0, 0, -1))
                self.assertGreater(normal.dot(expected), 9999)
                self.assertEqual(len(binder.Shape.Face1.Edges), 0)
                self.assertAlmostEqual(binder.Shape.Face2.Area, 96)
                self.assertGreater(binder.Shape.BoundBox.DiagonalLength, 1e90)
                world = source.Placement
                normal = world.Rotation.multVec(App.Vector(0, 0, 1))
                self._checkPick(binder, "Face2", world.multVec(finite.CenterOfMass), normal)
                edge = binder.Shape.Edge1
                self._checkPick(binder, "Edge1", edge.CenterOfMass, normal)
