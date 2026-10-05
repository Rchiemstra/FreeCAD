# SPDX-License-Identifier: LGPL-2.1-or-later

import time
import unittest
import FreeCAD
import FreeCADGui
import Mesh
from pivy import coin

from Test.GuiRecompute import close_document, recompute_document


class PivyTestCases(unittest.TestCase):
    def setUp(self):
        self.planarMesh = []
        FreeCAD.newDocument("MeshTest")
        self.doc = FreeCAD.ActiveDocument

    def _mesh_points(self):
        self.planarMesh.append([-16.097176, -29.891157, 15.987688])
        self.planarMesh.append([-16.176304, -29.859991, 15.947966])
        self.planarMesh.append([-16.071451, -29.900553, 15.912505])
        self.planarMesh.append([-16.092241, -29.893408, 16.020439])
        self.planarMesh.append([-16.007210, -29.926180, 15.967641])
        self.planarMesh.append([-16.064457, -29.904951, 16.090832])
        return Mesh.Mesh(self.planarMesh)

    def _show_mesh(self, mesh_object):
        Mesh.show(mesh_object)
        recompute_document(self.doc)
        FreeCADGui.updateGui()

    def _ray_pick(self, view):
        # Keep the action alive: the picked point belongs to it.
        self._pick_action = coin.SoRayPickAction(view.getSoRenderManager().getViewportRegion())
        self._pick_action.setRay(coin.SbVec3f(-16.05, 16.0, 16.0), coin.SbVec3f(0, -1, 0))
        self._pick_action.apply(view.getSoRenderManager().getSceneGraph())
        return self._pick_action.getPickedPoint()

    def _pick_diagnostics(self):
        feature = self.doc.ActiveObject
        return {
            "mustExecute": self.doc.mustExecute(),
            "readiness": self.doc.getMutationReadiness(),
            "facets": feature.Mesh.CountFacets if feature else None,
            "visible": feature.ViewObject.Visibility if feature else None,
        }

    def testRayPick(self):
        planarMeshObject = self._mesh_points()
        self._show_mesh(planarMeshObject)
        view = FreeCADGui.ActiveDocument.ActiveView.getViewer()
        # Live Coin catches up with a committed recompute on a later event
        # loop pass, so give the scene a few passes before picking fails.
        deadline = time.monotonic() + 5.0
        pp = self._ray_pick(view)
        while pp is None and time.monotonic() < deadline:
            FreeCADGui.updateGui()
            time.sleep(0.02)
            pp = self._ray_pick(view)
        self.assertIsNotNone(pp, self._pick_diagnostics())
        det = pp.getDetail()
        self.assertEqual(det.getTypeId(), coin.SoFaceDetail.getClassTypeId())
        det = coin.cast(det, det.getTypeId().getName().getString())
        self.assertEqual(det.getFaceIndex(), 1)

    def testPrimitiveCount(self):
        planarMeshObject = self._mesh_points()

        view = FreeCADGui.ActiveDocument.ActiveView
        view.setAxisCross(False)
        pc = coin.SoGetPrimitiveCountAction()
        pc.apply(view.getSceneGraph())
        pre_mesh_tc = pc.getTriangleCount()
        self._show_mesh(planarMeshObject)
        pc.apply(view.getSceneGraph())
        mesh_tc = pc.getTriangleCount() - pre_mesh_tc
        self.assertEqual(mesh_tc, 2)

    def tearDown(self):
        if self.doc.Name in FreeCAD.listDocuments():
            close_document(self.doc)
