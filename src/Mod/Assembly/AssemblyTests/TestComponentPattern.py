# SPDX-License-Identifier: LGPL-2.1-or-later
"""Component patterns use real PartDesign cuts and the production Assembly solver."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Assembly
import Part
import Sketcher


class TestComponentPattern(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ComponentPatternTest")
        self.assembly = Assembly.createAssembly(self.doc)
        self.body = self.doc.addObject("PartDesign::Body", "Flange")
        outline = self.body.newObject("Sketcher::SketchObject", "Outline")
        outline.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 35), False)
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = outline
        pad.Length = 5
        self.doc.recompute()
        self.hole = self.body.newObject("Sketcher::SketchObject", "Hole")
        self.hole.Placement.Base.z = 5
        self.hole.addGeometry(
            Part.Circle(App.Vector(20, 0, 0), App.Vector(0, 0, 1), 2), False
        )
        self.radiusConstraint = self.hole.addConstraint(
            Sketcher.Constraint("DistanceX", 0, 3, 20.0)
        )
        pocket = self.body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = self.hole
        pocket.Length = 5
        self.doc.recompute()
        self.source = self.body.newObject("PartDesign::PolarPattern", "HolePattern")
        self.source.Originals = [pocket]
        self.source.Axis = (self.hole, ["N_Axis"])
        self.source.Angle = 360
        self.source.Occurrences = 6
        self.body.Tip = self.source
        self.flange = self.assembly.newObject("App::Link", "FlangeComponent")
        self.flange.LinkedObject = self.body
        cylinder = self.doc.addObject("Part::Cylinder", "BoltShape")
        cylinder.Radius = 2
        cylinder.Height = 5
        self.bolt = self.assembly.newObject("App::Link", "Bolt")
        self.bolt.LinkedObject = cylinder
        self.bolt.Placement.Base = App.Vector(20, 0, 0)
        Assembly.createGroundedJoint(self.assembly, self.flange, recompute=False)
        self.doc.recompute()
        self.assertTrue(self.source.Shape.isValid())
        # Six full depth cylindrical cuts, independent of the component implementation.
        self.assertAlmostEqual(
            self.source.Shape.Volume, math.pi * (35**2 - 6 * 2**2) * 5, places=5
        )
        self.assertAlmostEqual(
            self.flange.Shape.Volume, self.source.Shape.Volume, places=5
        )
        self.flushGui()

    def flushGui(self):
        if App.GuiUp:
            import FreeCADGui as Gui
            from PySide import QtCore

            Gui.updateGui()
            QtCore.QCoreApplication.processEvents()

    def tearDown(self):
        self.flushGui()
        App.closeDocument(self.doc.Name)
        self.flushGui()

    def create(self, **kwargs):
        from ComponentPattern import createComponentPattern

        pattern = createComponentPattern(
            self.assembly,
            self.bolt,
            self.source,
            patternComponent=self.flange,
            **kwargs,
        )
        self.flushGui()
        return pattern

    def assertPolar(self, pattern, count, radius=20, angle=360):
        self.assertEqual(len(pattern.Copies), count - 1)
        bolts = [self.bolt] + list(pattern.Copies)
        for k, bolt in enumerate(bolts):
            a = math.radians(k * angle / (count if angle == 360 else count - 1))
            expected = self.flange.Placement.multVec(
                App.Vector(radius * math.cos(a), radius * math.sin(a), 0)
            )
            self.assertLess((bolt.Placement.Base - expected).Length, 1e-5)
            rotation = (
                self.flange.Placement.Rotation
                * App.Rotation(App.Vector(0, 0, 1), math.degrees(a))
                * self.flange.Placement.Rotation.inverted()
                * self.bolt.Placement.Rotation
            )
            self.assertTrue(bolt.Placement.Rotation.isSame(rotation, 1e-6))
            self.assertIs(bolt.getLinkedObject(), self.bolt.getLinkedObject())

    def test_polar_and_count_angle_changes(self):
        pattern = self.create()
        self.assertPolar(pattern, 6)
        self.assertFalse(pattern.PlacementDriver.Visibility)
        names = [o.Name for o in pattern.Copies]
        self.source.Occurrences = 8
        self.doc.recompute()
        self.assertPolar(pattern, 8)
        self.assertEqual([o.Name for o in pattern.Copies[:5]], names)
        self.source.Angle = 180
        self.doc.recompute()
        self.assertPolar(pattern, 8, angle=180)
        self.source.Occurrences = 3
        self.doc.recompute()
        self.assertPolar(pattern, 3, angle=180)
        self.assertTrue(all(self.doc.getObject(n) is None for n in names[2:]))

    def test_linear_source(self):
        linear = self.body.newObject("PartDesign::LinearPattern", "LinearHoles")
        linear.Originals = self.source.Originals
        linear.Direction = (self.hole, ["V_Axis"])
        linear.Length = 18
        linear.Occurrences = 4
        self.source = linear
        self.body.Tip = linear
        pattern = self.create()
        self.doc.recompute()
        self.assertEqual(len(pattern.Copies), 3)
        for k, bolt in enumerate([self.bolt] + list(pattern.Copies)):
            self.assertLess(
                (bolt.Placement.Base - App.Vector(20, 6 * k, 0)).Length, 1e-6
            )
        linear.Length = 15
        linear.Reversed = True
        self.doc.recompute()
        self.assertLess(
            (pattern.Copies[-1].Placement.Base - App.Vector(20, -15, 0)).Length, 1e-6
        )

    def test_explicit_parameters(self):
        from ComponentPattern import createComponentPattern

        pattern = createComponentPattern(self.assembly, self.bolt, count=6, angle=360)
        self.assertPolar(pattern, 6)
        pattern.PatternType = "Linear"
        pattern.Direction = App.Vector(0, 1, 0)
        pattern.Length = 25
        self.doc.recompute()
        self.assertLess(
            (pattern.Copies[-1].Placement.Base - App.Vector(20, 25, 0)).Length, 1e-6
        )

    def test_save_reload(self):
        pattern = self.create(joints=[self.seedJoint()])
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "pattern.FCStd")
            name = pattern.Name
            self.doc.recompute()
            self.doc.saveAs(path)
            self.flushGui()
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            self.assembly = self.doc.getObject("Assembly")
            self.bolt = self.doc.getObject("Bolt")
            self.flange = self.doc.getObject("FlangeComponent")
            self.source = self.doc.getObject("HolePattern")
            pattern = self.doc.getObject(name)
            self.source.Occurrences = 8
            self.doc.recompute()
            self.assertPolar(pattern, 8)
            self.assertEqual(len(pattern.JointCopies), 7)
            self.assertEqual(self.assembly.solve(), 0)
            self.assertFalse("Invalid" in pattern.State)
            self.assertFalse(pattern.PlacementDriver.Visibility)
            self.assertFalse(pattern.ParameterSource.Visibility)

    def seedJoint(self):
        # Select the actual cylindrical wall of the first hole through its assembly link.
        for i, face in enumerate(self.source.Shape.Faces, 1):
            if (
                isinstance(face.Surface, Part.Cylinder)
                and abs(face.Surface.Radius - 2) < 1e-7
            ):
                if abs(face.CenterOfMass.x - 20) < 1e-5:
                    import UtilsAssembly

                    ref1 = (self.flange, [f"Face{i}", f"Face{i}"])
                    ref2 = (self.bolt, ["Face1", "Face1"])
                    first = UtilsAssembly.getJcsGlobalPlc(
                        UtilsAssembly.findPlacement(ref1), ref1
                    )
                    second = UtilsAssembly.getJcsGlobalPlc(
                        UtilsAssembly.findPlacement(ref2), ref2
                    )
                    return Assembly.createJoint(
                        self.assembly,
                        "Fixed",
                        ref1,
                        ref2,
                        Offset2=second.inverse() * first,
                        solve=False,
                        presolve=False,
                    )
        self.fail("seed hole wall missing")

    def test_joint_copies_follow_faces_and_radius_after_solve(self):
        joint = self.seedJoint()
        pattern = self.create(joints=[joint])
        self.assertEqual(len(pattern.JointCopies), 5)
        for copy in pattern.JointCopies:
            self.assertNotEqual(copy.Reference1[1], joint.Reference1[1])
            self.assertIn(copy.Reference2[0], pattern.Copies)
        self.assertEqual(self.assembly.solve(), 0)
        self.assertPolar(pattern, 6)
        self.hole.setDatum(self.radiusConstraint, 24.0)
        self.source.Occurrences = 8
        self.doc.recompute()
        self.assertEqual(len(pattern.JointCopies), 7)
        self.assertEqual(self.assembly.solve(), 0)
        self.assertPolar(pattern, 8, radius=24)
        for bolt in pattern.Copies:
            placement = bolt.Placement
            placement.Base += App.Vector(0, 0, 2)
            bolt.Placement = placement
            self.assertAlmostEqual(bolt.Placement.Base.z, 2, places=5)
        self.assertEqual(self.assembly.solve(), 0)
        self.assertPolar(pattern, 8, radius=24)

    def test_seed_joint_tracks_radius_without_joint_copying(self):
        self.seedJoint()
        pattern = self.create()
        self.assertEqual(len(pattern.JointCopies), 0)
        self.hole.setDatum(self.radiusConstraint, 24.0)
        self.doc.recompute()
        self.assertPolar(pattern, 6, radius=24)

    def test_transformed_flange(self):
        self.flange.Placement = App.Placement(
            App.Vector(11, 7, 3), App.Rotation(App.Vector(1, 0, 0), 30)
        )
        self.bolt.Placement = self.flange.Placement * self.bolt.Placement
        pattern = self.create()
        self.assertPolar(pattern, 6)

    def test_imported_pattern_source(self):
        external = App.newDocument("ExternalFlange")
        try:
            with tempfile.TemporaryDirectory() as directory:
                body = external.copyObject(self.body, True)
                external.recompute()
                external.saveAs(os.path.join(directory, "flange.FCStd"))
                self.doc.saveAs(os.path.join(directory, "assembly.FCStd"))
                self.flange.LinkedObject = body
                self.source = external.getObject("HolePattern")
                pattern = self.create()
                self.assertPolar(pattern, 6)
                self.assertTrue(pattern.ReferenceDrivers)
                self.assertTrue(all(not o.Visibility for o in pattern.ReferenceDrivers))
                self.source.Occurrences = 8
                external.recompute()
                self.doc.recompute()
                self.assertPolar(pattern, 8)
                # Origin axes are the usual source for an imported polar pattern.
                axis = next(o for o in body.Origin.OriginFeatures if o.Role == "Z_Axis")
                self.source.Axis = (axis, [""])
                external.recompute()
                self.doc.recompute()
                self.assertPolar(pattern, 8)
        finally:
            App.closeDocument(external.Name)

    def test_delete_removes_only_generated_objects(self):
        pattern = self.create(joints=[self.seedJoint()])
        names = [o.Name for o in pattern.Copies] + [o.Name for o in pattern.JointCopies]
        names.append(pattern.PlacementDriver.Name)
        names.append(pattern.ParameterSource.Name)
        self.doc.removeObject(pattern.Name)
        self.doc.recompute()
        self.assertTrue(all(self.doc.getObject(name) is None for name in names))
        self.assertIsNotNone(self.doc.getObject(self.bolt.Name))
        self.assertIsNotNone(self.doc.getObject(self.source.Name))

    def test_invalid_seed_is_rejected_without_objects(self):
        from ComponentPattern import createComponentPattern

        before = len(self.doc.Objects)
        with self.assertRaises(ValueError):
            createComponentPattern(self.assembly, self.body, self.source)
        self.assertEqual(len(self.doc.Objects), before)


@unittest.skipUnless(App.GuiUp, "requires GUI")
class TestComponentPatternGui(unittest.TestCase):
    setUp = TestComponentPattern.setUp
    tearDown = TestComponentPattern.tearDown
    assertPolar = TestComponentPattern.assertPolar
    flushGui = TestComponentPattern.flushGui

    def test_command(self):
        import FreeCADGui as Gui

        Gui.activateWorkbench("AssemblyWorkbench")
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.bolt)
        Gui.Selection.addSelection(self.source)
        Gui.runCommand("Assembly_ComponentPattern", 0)
        import CommandComponentPattern

        panel = CommandComponentPattern.activeTask
        self.assertIsNotNone(panel)
        self.assertTrue(panel.accept())
        import time
        from PySide import QtCore

        deadline = time.monotonic() + 30
        while not panel.recomputeHandle.done() and time.monotonic() < deadline:
            QtCore.QCoreApplication.processEvents()
            time.sleep(0.01)
        self.assertTrue(panel.recomputeHandle.done(), panel.recomputeHandle.status())
        Gui.updateGui()
        patterns = [
            o for o in self.doc.Objects if getattr(o, "Type", "") == "ComponentPattern"
        ]
        self.assertEqual(len(patterns), 1)
        self.assertIs(patterns[0].PatternSource, self.source)
        self.assertPolar(patterns[0], 6)
        Gui.Selection.clearSelection()
