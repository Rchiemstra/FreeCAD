# SPDX-License-Identifier: LGPL-2.1-or-later

# test to check color per face when after restore

import FreeCAD as App
import Part
import base64
import json
import os
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET
import zipfile
import FreeCADGui as Gui
from BOPTools import BOPFeatures
from pivy import coin
from PySide import QtCore, QtWidgets
from Test.GuiRecompute import (
    _pump_gui_until,
    close_document,
    recompute_document,
    save_document_as,
)


class _SaveThreadProbe:
    def __init__(self):
        self.threads = []

    def dumps(self):
        self.threads.append(threading.get_ident())
        return {"thread": threading.get_ident()}

    def loads(self, state):
        pass


class ColorPerFaceTest(unittest.TestCase):
    def setUp(self):
        TempPath = tempfile.gettempdir()
        self.fileName = TempPath + os.sep + "ColorPerFaceTest.FCStd"
        self.doc = App.newDocument()

    def tearDown(self):
        close_document(self.doc)

    def testSaveGuiStateOnGuiThread(self):
        self.checkSaveGuiState(with_gil=False)

    def testSaveGuiStateWithGil(self):
        self.checkSaveGuiState(with_gil=True)

    def checkSaveGuiState(self, *, with_gil):
        # Async saves serialize the model on its owner thread. The separate
        # GuiDocument.xml callback must return to the GUI thread even while
        # posted events change the tree and serialize the active camera.
        box = self.doc.addObject("Part::FeaturePython", "Box")
        box.Shape = Part.makeBox(10, 10, 10)
        model_probe = _SaveThreadProbe()
        gui_probe = _SaveThreadProbe()
        box.Proxy = model_probe
        box.ViewObject.Proxy = gui_probe
        group = self.doc.addObject("App::DocumentObjectGroup", "Group")
        group.addObject(box)
        recompute_document(self.doc)

        view = Gui.activeDocument().activeView()
        tree = Gui.getMainWindow().findChild(QtWidgets.QTreeWidget)
        self.assertIsNotNone(tree)
        changes = []

        def change_gui_state():
            changes.append(None)
            if len(changes) % 2:
                view.viewFront()
                tree.collapseAll()
            else:
                view.viewAxonometric()
                tree.expandAll()
            view.getCamera()

        timer = QtCore.QTimer()
        timer.timeout.connect(change_gui_state)
        timer.start(0)
        try:
            if with_gil:
                # saveCopy() runs directly on the calling Python thread, which
                # holds the GIL when SaveDocFile dispatches to the GUI thread.
                errors = []

                def save_with_gil():
                    try:
                        self.doc.saveCopy(self.fileName)
                    except BaseException as error:
                        errors.append(error)

                worker = threading.Thread(target=save_with_gil, daemon=True)
                worker.start()
                _pump_gui_until(lambda: not worker.is_alive(), 30.0)
                worker.join()
                self.assertFalse(errors, errors)
                self.assertEqual(set(model_probe.threads), {worker.ident})
            else:
                save_document_as(self.doc, self.fileName)
        finally:
            timer.stop()

        self.assertTrue(changes)
        self.assertTrue(model_probe.threads)
        self.assertTrue(gui_probe.threads)
        gui_thread = threading.get_ident()
        self.assertNotIn(gui_thread, model_probe.threads)
        self.assertEqual(set(gui_probe.threads), {gui_thread})
        with zipfile.ZipFile(self.fileName) as archive:
            gui_xml = ET.fromstring(archive.read("GuiDocument.xml"))
        proxy = gui_xml.find(
            "./ViewProviderData/ViewProvider[@name='Box']/Properties/Property[@name='Proxy']/Python"
        )
        self.assertIsNotNone(proxy)
        self.assertEqual(
            json.loads(base64.b64decode(proxy.attrib["value"])), {"thread": gui_thread}
        )

    def testBox(self):
        box = self.doc.addObject("Part::Box", "Box")
        recompute_document(self.doc)

        box.ViewObject.DiffuseColor = [
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
        ]

        box.Visibility = False
        recompute_document(self.doc)

        save_document_as(self.doc, self.fileName)
        close_document(self.doc)

        self.doc = App.openDocument(self.fileName)
        box = self.doc.Box
        box.Visibility = True
        self.assertEqual(len(box.ViewObject.DiffuseColor), 6)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterial.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(box.ViewObject.RootNode)
        paths = sa.getPaths()

        mat = paths.get(1).getTail()
        self.assertEqual(mat.diffuseColor.getNum(), 6)

    def testBoxAndLink(self):
        box = self.doc.addObject("Part::Box", "Box")
        recompute_document(self.doc)

        box.ViewObject.DiffuseColor = [
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
        ]

        link = self.doc.addObject("App::Link", "Link")
        link.setLink(box)
        box.Visibility = False
        recompute_document(self.doc)

        save_document_as(self.doc, self.fileName)
        close_document(self.doc)

        self.doc = App.openDocument(self.fileName)
        box = self.doc.Box
        box.Visibility = True
        self.assertEqual(len(box.ViewObject.DiffuseColor), 6)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterial.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(box.ViewObject.RootNode)
        paths = sa.getPaths()

        mat = paths.get(1).getTail()
        self.assertEqual(mat.diffuseColor.getNum(), 6)

    def testTransparency(self):
        """
        If color per face is set then changing the transparency must not revert it
        """
        box = self.doc.addObject("Part::Box", "Box")
        recompute_document(self.doc)

        box.ViewObject.DiffuseColor = [
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 0.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
            (1.0, 1.0, 0.0, 1.0),
        ]

        box.ViewObject.Transparency = 35
        self.assertEqual(box.ViewObject.Transparency, 35)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterialBinding.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(box.ViewObject.RootNode)
        paths = sa.getPaths()

        bind = paths.get(1).getTail()
        self.assertEqual(bind.value.getValue(), bind.PER_PART)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterial.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(box.ViewObject.RootNode)
        paths = sa.getPaths()

        mat = paths.get(1).getTail()
        self.assertEqual(mat.diffuseColor.getNum(), 6)

    def testMultiFuse(self):
        """
        Both input objects are red. So, it's expected that the output object is red, too.
        """
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cylinder")
        box.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
        cyl.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
        recompute_document(self.doc)

        bp = BOPFeatures.BOPFeatures(self.doc)
        fuse = bp.make_multi_fuse([box.Name, cyl.Name])
        self.assertEqual(fuse.TypeId, "Part::MultiFuse")
        fuse.Refine = False
        recompute_document(self.doc)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterialBinding.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(fuse.ViewObject.RootNode)
        paths = sa.getPaths()

        bind = paths.get(1).getTail()
        self.assertEqual(bind.value.getValue(), bind.PER_PART)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterial.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(fuse.ViewObject.RootNode)
        paths = sa.getPaths()

        mat = paths.get(1).getTail()
        self.assertEqual(mat.diffuseColor.getNum(), 11)

        self.assertEqual(len(fuse.Shape.Faces), 11)
        self.assertEqual(len(fuse.ViewObject.DiffuseColor), 11)
        self.assertEqual(fuse.ViewObject.DiffuseColor[0], (1.0, 0.0, 0.0, 1.0))

    def testMultiFuseWithCustomDefaultShapeColor(self):
        """
        The FreeCAD Light and Dark preference packs change the default shape color. Fusing two
        red objects must still give a red result in that case.
        """
        params = App.ParamGet("User parameter:BaseApp/Preferences/View")
        had_color = "DefaultShapeColor" in params.GetUnsigneds()
        old_color = params.GetUnsigned("DefaultShapeColor")
        params.SetUnsigned("DefaultShapeColor", 0xADB5BDFF)  # FreeCAD Light
        try:
            box = self.doc.addObject("Part::Box", "Box")
            cyl = self.doc.addObject("Part::Cylinder", "Cylinder")
            box.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
            cyl.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
            recompute_document(self.doc)

            bp = BOPFeatures.BOPFeatures(self.doc)
            fuse = bp.make_multi_fuse([box.Name, cyl.Name])
            fuse.Refine = False
            recompute_document(self.doc)

            self.assertEqual(len(fuse.Shape.Faces), 11)
            self.assertEqual(len(fuse.ViewObject.DiffuseColor), 11)
            for color in fuse.ViewObject.DiffuseColor:
                self.assertEqual(color, (1.0, 0.0, 0.0, 1.0))
        finally:
            if had_color:
                params.SetUnsigned("DefaultShapeColor", old_color)
            else:
                params.RemUnsigned("DefaultShapeColor")

    def testMultiFuseSaveRestore(self):
        box = self.doc.addObject("Part::Box", "Box")
        cyl = self.doc.addObject("Part::Cylinder", "Cylinder")
        box.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
        cyl.ViewObject.ShapeColor = (1.0, 0.0, 0.0, 1.0)
        recompute_document(self.doc)

        bp = BOPFeatures.BOPFeatures(self.doc)
        fuse = bp.make_multi_fuse([box.Name, cyl.Name])
        self.assertEqual(fuse.TypeId, "Part::MultiFuse")
        fuse.Refine = False
        recompute_document(self.doc)

        fuse.ViewObject.DiffuseColor = [(1.0, 0.0, 0.0, 1.0)] * 11

        save_document_as(self.doc, self.fileName)
        close_document(self.doc)

        self.doc = App.openDocument(self.fileName)

        fuse = self.doc.ActiveObject
        self.assertEqual(len(fuse.Shape.Faces), 11)
        self.assertEqual(len(fuse.ViewObject.DiffuseColor), 11)
        self.assertEqual(fuse.ViewObject.DiffuseColor[0], (1.0, 0.0, 0.0, 1.0))

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterialBinding.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(fuse.ViewObject.RootNode)
        paths = sa.getPaths()

        bind = paths.get(1).getTail()
        self.assertEqual(bind.value.getValue(), bind.PER_PART)

        sa = coin.SoSearchAction()
        sa.setType(coin.SoMaterial.getClassTypeId())
        # We need an easier way to access nodes of a display mode
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(fuse.ViewObject.RootNode)
        paths = sa.getPaths()

        mat = paths.get(1).getTail()
        self.assertEqual(mat.diffuseColor.getNum(), 11)
