# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: 2025 Furgo
# SPDX-FileNotice: Part of the FreeCAD project.

################################################################################
#                                                                              #
#   FreeCAD is free software: you can redistribute it and/or modify            #
#   it under the terms of the GNU Lesser General Public License as             #
#   published by the Free Software Foundation, either version 2.1              #
#   of the License, or (at your option) any later version.                     #
#                                                                              #
#   FreeCAD is distributed in the hope that it will be useful,                 #
#   but WITHOUT ANY WARRANTY; without even the implied warranty                #
#   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                    #
#   See the GNU Lesser General Public License for more details.                #
#                                                                              #
#   You should have received a copy of the GNU Lesser General Public           #
#   License along with FreeCAD. If not, see https://www.gnu.org/licenses       #
#                                                                              #
################################################################################

import Arch
import ArchSectionPlane
import Draft
import os
import FreeCAD as App
from bimtests import TestArchBase


class _SectionDimensionCommandLogic:
    """Deterministic Python command logic; these doubles do not prove native admission."""

    def _fixture(self):
        import threading
        from types import SimpleNamespace
        from unittest.mock import patch

        model = ArchSectionPlane._SectionPlane.__new__(ArchSectionPlane._SectionPlane)
        provider = ArchSectionPlane._ViewProviderSectionPlane.__new__(
            ArchSectionPlane._ViewProviderSectionPlane)

        class Object:
            def __setattr__(self, name, value):
                if name in ("PlaneLength", "PlaneHeight", "DisplayLength", "DisplayHeight"):
                    value = SimpleNamespace(Value=value)
                object.__setattr__(self, name, value)

            def getTypeIdOfProperty(self, name):
                return "App::PropertyLength"

        obj = Object()
        obj.Proxy, obj.ID, obj.Name, obj.Removing = model, 7, "Section", False
        obj.PlaneLength, obj.PlaneHeight, obj.PlaneDimensionsRevision = 1200, 700, 0
        obj.Placement = App.Placement()
        view = Object()
        view.Object, view.Proxy = obj, provider
        view.PropertiesList = ["DisplayLength", "DisplayHeight"]
        view.DisplayLength, view.DisplayHeight = 1200, 700
        view.CutView = False
        obj.ViewObject, provider.Object = view, obj

        class Document:
            def __init__(self):
                self.commands, self.results, self.waits = [], [], []
                self.busy = False
                self.Transacting = False
                self.wait_hook = None

            def collaborationIdentity(self):
                return dict(instance_id=11, lifecycle_epoch=1, state="Live")

            def getObject(self, name):
                return obj if name == obj.Name else None

            def commitCompatibilityMutationAsync(self, apply, **options):
                if options != dict(recompute=False, object_name="Section",
                                   label="Section plane dimensions"):
                    raise AssertionError(options)
                index = len(self.commands)
                def wait(timeout):
                    self.waits.append(index)
                    if self.wait_hook is not None:
                        self.wait_hook(index)
                    result = self.results.pop(0) if self.results else {"status": "Committed"}
                    if isinstance(result, BaseException):
                        raise result
                    if result["status"] == "Committed":
                        apply()
                    return result
                handle = {"state": "Running", "wait": wait}
                self.commands.append(handle)
                return handle

            def recomputeAsync(self):
                if self.busy:
                    raise RuntimeError("document execution lane is busy")
                return SimpleNamespace(wait=lambda timeout: {"state": "completed"})

        doc = Document()
        obj.Document = doc
        queued = []
        class QueuedThread:
            def __init__(self, target, **options):
                self.target = target
            def start(self):
                queued.append(self.target)
        app_patch = patch.object(ArchSectionPlane, "FreeCAD",
                                 SimpleNamespace(GuiUp=True, Vector=App.Vector))
        app_patch.start()
        self.addCleanup(app_patch.stop)
        thread_patch = patch.object(threading, "Thread", QueuedThread)
        thread_patch.start()
        self.addCleanup(thread_patch.stop)
        def drain():
            while queued:
                queued.pop(0)()
        # Only the presentation drawing is doubled. updateData itself is real;
        # its guard must prevent this callback from submitting another input.
        def present(vobj, prop):
            if not getattr(provider, "_presenting_dimensions", False):
                provider._submitDimensionInputs(vobj)
        provider.onChanged = present
        return model, provider, obj, view, doc, drain

    def _assert_model(self, model, obj, length, height):
        model.svgcache, model.shapecache = "old-svg", "old-shape"
        model.execute(obj)  # Python logic only, never counted as native execute.
        self.assertAlmostEqual(obj.Shape.Area, length * height)
        self.assertIsNone(model.svgcache)
        self.assertIsNone(model.shapecache)

    def test_completed_input_happy_and_same_value_dedup(self):
        model, provider, obj, view, doc, drain = self._fixture()
        provider._submitDimensionInputs(view)
        drain()
        provider._submitDimensionInputs(view)
        drain()
        self.assertEqual(len(doc.commands), 1)
        self._assert_model(model, obj, 1200, 700)

    def test_terminal_failed_input_allows_exactly_one_same_value_retry(self):
        model, provider, obj, view, doc, drain = self._fixture()
        view.DisplayLength, view.DisplayHeight = 900, 400
        doc.results = [{"status": "Rejected"}, {"status": "Committed"}]
        provider._submitDimensionInputs(view)
        with self.assertRaises(RuntimeError) as failed:
            drain()
        self.assertEqual((obj.PlaneLength.Value, obj.PlaneHeight.Value), (1200, 700))
        provider._submitDimensionInputs(view)  # Explicit user retry, identical values.
        self.assertEqual(len(doc.commands), 2)
        drain()
        provider._submitDimensionInputs(view)
        self.assertEqual(len(doc.commands), 2)
        self._assert_model(model, obj, 900, 400)
        with self.assertRaises(RuntimeError) as historical:
            provider.raiseDimensionCommandErrors()
        self.assertIs(historical.exception, failed.exception)

    def test_transient_busy_recompute_recovers_without_replaying_input(self):
        model, provider, obj, view, doc, drain = self._fixture()
        doc.busy = True
        provider.recomputeDimensions()
        with self.assertRaisesRegex(RuntimeError, "lane is busy") as failed:
            drain()
        self.assertEqual(len(doc.commands), 1)
        doc.busy = False
        provider.recomputeDimensions()
        drain()
        self.assertEqual(len(doc.commands), 1)
        self._assert_model(model, obj, 1200, 700)
        with self.assertRaises(RuntimeError) as historical:
            provider.raiseDimensionCommandErrors()
        self.assertIs(historical.exception, failed.exception)

    def test_timeout_keeps_unknown_input_until_real_terminal_then_explicit_retry(self):
        model, provider, obj, view, doc, drain = self._fixture()
        view.DisplayLength, view.DisplayHeight = 900, 400
        doc.results = [TimeoutError("future still outstanding"), {"status": "Rejected"}]
        def while_unknown(index):
            if len(doc.waits) == 2:
                self.assertEqual(index, 0)
                provider._submitDimensionInputs(view)
                self.assertEqual(len(doc.commands), 1, "unknown work must not be replayed")
                self.assertEqual((obj.PlaneLength.Value, obj.PlaneHeight.Value), (1200, 700))
        doc.wait_hook = while_unknown
        provider._submitDimensionInputs(view)
        with self.assertRaises(TimeoutError):
            drain()
        # The same observer may keep waiting. The second wait supplies a real
        # terminal rejection, before explicit retry; TimeoutError was history.
        self.assertEqual(doc.waits, [0, 0], "lost the outstanding future after timeout")
        self.assertEqual(len(doc.commands), 1)
        doc.wait_hook = None
        doc.results = [{"status": "Committed"}]
        provider._submitDimensionInputs(view)
        self.assertEqual(len(doc.commands), 2)
        drain()
        self._assert_model(model, obj, 900, 400)
        with self.assertRaises(TimeoutError):
            provider.raiseDimensionCommandErrors()

    def test_completed_input_undo_redo_uses_authoritative_properties_without_submission(self):
        model, provider, obj, view, doc, drain = self._fixture()
        provider._submitDimensionInputs(view)
        drain()
        initial_revision = obj.PlaneDimensionsRevision
        self._assert_model(model, obj, 1200, 700)
        view.DisplayLength, view.DisplayHeight = 900, 400
        provider._submitDimensionInputs(view)
        drain()
        edited_revision = obj.PlaneDimensionsRevision
        self._assert_model(model, obj, 900, 400)
        submissions = len(doc.commands)
        for length, height, revision in ((1200, 700, initial_revision),
                                         (900, 400, edited_revision)):
            # Native undo restores App properties, not the Python request tuple.
            obj.PlaneLength, obj.PlaneHeight, obj.PlaneDimensionsRevision = length, height, revision
            provider.updateData(obj, "PlaneLength")
            provider.updateData(obj, "PlaneHeight")
            provider.updateData(obj, "PlaneDimensionsRevision")
            self.assertEqual((view.DisplayLength.Value, view.DisplayHeight.Value), (length, height))
            self._assert_model(model, obj, length, height)
            self.assertEqual(len(doc.commands), submissions)



class TestArchSectionPlane(_SectionDimensionCommandLogic, TestArchBase.TestArchBase):

    def _makeBox(self, length=1000, width=2000, height=3000):
        box = self.document.addObject("Part::Box", "SectionPlaneFitBox")
        box.Length = length
        box.Width = width
        box.Height = height
        self.document.recompute()
        return box

    def test_makeSectionPlane(self):
        """Test the makeSectionPlane function."""
        operation = "Testing makeSectionPlane function"
        self.printTestMessage(operation)

        section_plane = Arch.makeSectionPlane(name="TestSectionPlane")
        self.assertIsNotNone(
            section_plane, "makeSectionPlane failed to create a section plane object."
        )
        self.assertEqual(
            section_plane.Label, "TestSectionPlane", "Section plane label is incorrect."
        )

    def testSectionPlaneFitUsesLocalAxesAfterRotateY(self):
        """Resize-to-fit dimensions follow the rotated section plane axes."""

        box = self._makeBox()
        placement = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 1, 0), 90))

        length, height = ArchSectionPlane.getSectionPlaneFit([box], placement)

        self.assertAlmostEqual(length, 3300)
        self.assertAlmostEqual(height, 2300)

    def testSectionPlaneFitCombinesMultipleObjects(self):
        """Resize-to-fit covers the combined bounds of all objects."""

        box = self._makeBox()
        second_box = self._makeBox()
        second_box.Placement.Base = App.Vector(2000, 500, -100)
        self.document.recompute()

        length, height = ArchSectionPlane.getSectionPlaneFit([box, second_box], App.Placement())

        self.assertAlmostEqual(length, 3300)
        self.assertAlmostEqual(height, 2800)

    def testSectionPlaneFitUsesLocalAxesAfterRotateZ(self):
        """Resize-to-fit handles in-plane rotations without swapping axes."""

        box = self._makeBox()
        placement = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 0, 1), 90))

        length, height = ArchSectionPlane.getSectionPlaneFit([box], placement)

        self.assertAlmostEqual(length, 2200)
        self.assertAlmostEqual(height, 1200)

    def testSectionPlaneCenterRecentersLocalBounds(self):
        """Recenter moves the placement base to the local bound-box center."""

        box = self._makeBox()
        placement = App.Placement(App.Vector(500, -250, 125), App.Rotation(App.Vector(0, 1, 0), 90))
        center = ArchSectionPlane.getSectionPlaneCenter([box], placement)
        recentered = App.Placement(center, placement.Rotation)

        local_boundbox = ArchSectionPlane.getSectionPlaneLocalBoundBox([box], recentered)

        self.assertAlmostEqual(local_boundbox.Center.x, 0)
        self.assertAlmostEqual(local_boundbox.Center.y, 0)
        self.assertAlmostEqual(local_boundbox.Center.z, 0)

    def testTechDrawViewGeneration(self):
        """Tests the whole TD view generation workflow"""

        # Create a few objects
        points = [App.Vector(0.0, 0.0, 0.0), App.Vector(2000.0, 0.0, 0.0)]
        line = Draft.make_wire(points)
        wall = Arch.makeWall(line, height=2000)
        wpl = App.Placement(App.Vector(500, 0, 1500), App.Vector(1, 0, 0), -90)
        win = Arch.makeWindowPreset(
            "Fixed",
            width=1000.0,
            height=1000.0,
            h1=50.0,
            h2=50.0,
            h3=50.0,
            w1=100.0,
            w2=50.0,
            o1=0.0,
            o2=50.0,
            placement=wpl,
        )
        win.Hosts = [wall]
        profile = Arch.makeProfile([169, "HEA", "HEA100", "H", 100.0, 96.0, 5.0, 8.0])
        column = Arch.makeStructure(profile, height=2000.0)
        column.Profile = "HEA100"
        column.Placement.Base = App.Vector(500.0, 600.0, 0.0)
        level = Arch.makeFloor()
        level.addObjects([wall, column])
        App.ActiveDocument.recompute()

        # Create a drawing view
        section = Arch.makeSectionPlane(level)
        drawing = Arch.make2DDrawing()
        view = Draft.make_shape_2d_view(section)
        cut = Draft.make_shape_2d_view(section)
        cut.InPlace = False
        cut.ProjectionMode = "Cutfaces"
        drawing.addObjects([view, cut])
        App.ActiveDocument.recompute()

        # Create a TD page
        tpath = os.path.join(
            App.getResourceDir(), "Mod", "TechDraw", "Templates", "ISO", "A3_Landscape_blank.svg"
        )
        page = App.ActiveDocument.addObject("TechDraw::DrawPage", "Page")
        template = App.ActiveDocument.addObject("TechDraw::DrawSVGTemplate", "Template")
        template.Template = tpath
        page.Template = template
        view = App.ActiveDocument.addObject("TechDraw::DrawViewDraft", "DraftView")
        view.Source = drawing
        page.addView(view)
        view.Scale = 1.0
        view.X = "20cm"
        view.Y = "15cm"
        App.ActiveDocument.recompute()
        assert True

    def testShape2DViewGeneration(self):
        """Tests Draft_Shape2DView face with hole creation"""

        # Create a wall based on a clock-wise wire starting at the lower left corner.
        # Such a wire would previously result in an invalid face in the Shape2DView.
        wire = Draft.make_wire(
            [
                App.Vector(0, 0, 0),
                App.Vector(0, 1000, 0),
                App.Vector(2000, 1000, 0),
                App.Vector(2000, 0, 0),
            ],
            closed=True,
        )
        wire.MakeFace = False
        wall = Arch.makeWall(wire, height=3000, width=200)
        App.ActiveDocument.recompute()

        section = Arch.makeSectionPlane(wall)
        shp_view = Draft.make_shape_2d_view(section)
        shp_view.InPlace = False
        shp_view.ProjectionMode = "Cutfaces"
        App.ActiveDocument.recompute()

        face = shp_view.Shape.Faces[0]
        self.assertTrue(face.isValid())

        area_expected = 1200 * 2200 - 800 * 1800
        self.assertAlmostEqual(face.Area, area_expected)


    def testSectionPlaneHeadlessDefaultGeometry(self):
        import unittest
        if App.GuiUp:
            raise unittest.SkipTest("Run this reference in the separate headless suite")
        section = Arch.makeSectionPlane()
        self.document.recompute()
        self.assertEqual(section.getStatusString(), "Valid")
        self.assertAlmostEqual(section.Shape.Area, 1)
        self.assertIsNone(section.Proxy.svgcache)
        self.assertIsNone(section.Proxy.shapecache)
