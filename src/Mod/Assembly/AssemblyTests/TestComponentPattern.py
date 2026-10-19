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

    def test_required_input_deletion_cleans_generated_objects(self):
        for input_name in ("Bolt", "HolePattern"):
            with self.subTest(input=input_name):
                pattern = self.create(joints=[self.seedJoint()])
                names = [o.Name for o in pattern.Copies + pattern.JointCopies]
                self.doc.removeObject(input_name)
                self.doc.recompute()
                self.assertTrue(all(self.doc.getObject(n) is None for n in names))
                self.assertFalse(pattern.Copies)
                self.assertFalse(pattern.JointCopies)
                self.assertIn("missing", pattern.StatusMessage.lower())
            if input_name == "Bolt":
                # Rebuild the fixture for the independent source deletion case.
                self.tearDown()
                self.setUp()

    def test_nested_seed_and_later_placement_change(self):
        from ComponentPattern import createComponentPattern

        nested = Assembly.createAssembly(self.doc)
        self.assembly.addObject(nested)
        nested.Placement = App.Placement(
            App.Vector(7, 11, 4), App.Rotation(App.Vector(0, 0, 1), 35)
        )
        nested.addObject(self.bolt)
        self.assembly.Placement = App.Placement(
            App.Vector(3, -8, 6), App.Rotation(App.Vector(1, 0, 0), 20)
        )
        pattern = createComponentPattern(
            self.assembly, self.bolt, count=3, patternType="Linear", length=12
        )
        for shift in (App.Vector(), App.Vector(2, 3, 1)):
            self.bolt.Placement.Base += shift
            self.doc.recompute()
            seed = nested.Placement * self.bolt.Placement
            for k, copy in enumerate(pattern.Copies, 1):
                expected = App.Placement(App.Vector(6 * k, 0, 0), App.Rotation()) * seed
                self.assertTrue(copy.Placement.isSame(expected, 1e-6))

    def test_grounded_seed_and_later_placement_change(self):
        from ComponentPattern import createComponentPattern

        Assembly.createGroundedJoint(self.assembly, self.bolt, recompute=False)
        pattern = createComponentPattern(self.assembly, self.bolt, count=3)
        self.assertEqual(len(pattern.Copies), 2)
        self.assertFalse(pattern.JointCopies)
        self.bolt.Placement.Base = App.Vector(25, 0, 0)
        self.doc.recompute()
        self.assertPolar(pattern, 3, radius=25)

    def test_count_shrink_removes_copied_joints(self):
        pattern = self.create(joints=[self.seedJoint()])
        removed = [o.Name for o in pattern.JointCopies[1:] + pattern.Copies[1:]]
        self.source.Occurrences = 2
        self.doc.recompute()
        self.assertEqual(len(pattern.JointCopies), 1)
        self.assertEqual(len(pattern.Copies), 1)
        self.assertTrue(all(self.doc.getObject(n) is None for n in removed))

    def test_mirrored_source(self):
        mirror = self.body.newObject("PartDesign::Mirrored", "MirroredHoles")
        mirror.Originals = self.source.Originals
        mirror.MirrorPlane = (self.hole, ["V_Axis"])
        self.source = mirror
        self.body.Tip = mirror
        self.doc.recompute()
        pattern = self.create()
        self.assertEqual(len(pattern.Copies), 1)
        self.assertLess((pattern.Copies[0].Placement.Base - App.Vector(-20, 0, 0)).Length, 1e-6)
        self.assertAlmostEqual(pattern.Copies[0].Shape.Volume, self.bolt.Shape.Volume, places=5)
        # Check the entire reflected geometry, including handedness.
        reflected = self.bolt.Shape.copy()
        matrix = App.Matrix()
        matrix.A11 = -1
        reflected.transformShape(matrix)
        self.assertAlmostEqual(pattern.Copies[0].Shape.common(reflected).Volume, reflected.Volume, places=5)

    def test_multitransform_source_composes_helpers(self):
        linear = self.body.newObject("PartDesign::LinearPattern", "LinearHelper")
        linear.Direction = (self.hole, ["V_Axis"])
        linear.Length = 12
        linear.Occurrences = 3
        mirror = self.body.newObject("PartDesign::Mirrored", "MirrorHelper")
        mirror.MirrorPlane = (self.hole, ["V_Axis"])
        multi = self.body.newObject("PartDesign::MultiTransform", "MultiHoles")
        multi.Originals = self.source.Originals
        multi.Transformations = [linear, mirror]
        self.source = multi
        self.body.Tip = multi
        self.doc.recompute()
        pattern = self.create()
        expected = [(20, 6), (20, 12), (-20, 0), (-20, 6), (-20, 12)]
        self.assertEqual(len(pattern.Copies), len(expected))
        for copy, (x, y) in zip(pattern.Copies, expected):
            self.assertLess((copy.Placement.Base - App.Vector(x, y, 0)).Length, 1e-6)
        linear.Occurrences = 2
        linear.Length = 10
        self.doc.recompute()
        self.assertEqual(len(pattern.Copies), 3)
        self.assertLess((pattern.Copies[-1].Placement.Base - App.Vector(-20, 10, 0)).Length, 1e-6)

    def test_translated_errors_and_unsupported_source(self):
        import ComponentPattern
        from unittest.mock import patch

        before = len(self.doc.Objects)
        with patch.object(ComponentPattern, "translate", side_effect=lambda context, text: "TR:" + text):
            with self.assertRaisesRegex(ValueError, "TR:"):
                ComponentPattern.createComponentPattern(self.assembly, self.bolt, self.hole)
        self.assertEqual(len(self.doc.Objects), before)

    def test_invalid_explicit_parameters_leave_no_objects(self):
        from ComponentPattern import createComponentPattern

        for kwargs in ({"count": 0}, {"axis": App.Vector()},
                       {"patternType": "Linear", "length": 0},
                       {"patternType": "Linear", "direction": App.Vector()}):
            with self.subTest(parameters=kwargs):
                before = len(self.doc.Objects)
                with self.assertRaises(ValueError):
                    createComponentPattern(self.assembly, self.bolt, **kwargs)
                self.assertEqual(len(self.doc.Objects), before)

    def test_standard_suite_registers_both_classes(self):
        import TestAssemblyWorkbench

        self.assertIs(TestAssemblyWorkbench.TestComponentPattern, TestComponentPattern)
        self.assertIs(TestAssemblyWorkbench.TestComponentPatternGui, TestComponentPatternGui)

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

    seedJoint = TestComponentPattern.seedJoint

    def waitForPanel(self, panel):
        import time
        from PySide import QtCore

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            QtCore.QCoreApplication.processEvents()
            if panel.recomputeHandle.done() and not getattr(panel, "_pending", False):
                break
            time.sleep(0.01)
        self.assertTrue(panel.recomputeHandle.done(), panel.recomputeHandle.status())
        self.assertFalse(getattr(panel, "_pending", False))
        self.flushGui()

    def openPanel(self):
        import FreeCADGui as Gui
        import CommandComponentPattern

        self.doc.UndoMode = 1
        Gui.activateWorkbench("AssemblyWorkbench")
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.bolt)
        Gui.Selection.addSelection(self.source)
        Gui.runCommand("Assembly_ComponentPattern", 0)
        return CommandComponentPattern.activeTask

    def test_command_undo_redo_and_no_synchronous_recompute(self):
        import FreeCADGui as Gui
        from unittest.mock import patch
        from PySide import QtCore
        import CommandComponentPattern

        joint = self.seedJoint()
        self.doc.UndoMode = 1
        panel = self.openPanel()
        panel.joints.item(panel.availableJoints.index(joint)).setCheckState(QtCore.Qt.Checked)
        before = {o.Name for o in self.doc.Objects}

        class AsyncOnlyDocument:
            def __getattr__(wrapper, name):
                if name == "recompute":
                    raise AssertionError("command called synchronous recompute")
                return getattr(self.doc, name)

        class AssemblyFacade:
            Document = AsyncOnlyDocument()
            def __getattr__(wrapper, name):
                return getattr(self.assembly, name)

        real_create = CommandComponentPattern.createComponentPattern
        def create(assembly, *args, **kwargs):
            self.assertIs(kwargs.get("recompute"), False)
            return real_create(self.assembly, *args, **kwargs)

        panel.assembly = AssemblyFacade()
        with patch.object(CommandComponentPattern, "createComponentPattern", side_effect=create):
            self.assertTrue(panel.accept())
            self.waitForPanel(panel)
        self.assertIsNone(CommandComponentPattern.activeTask)
        pattern = next(o for o in self.doc.Objects if getattr(o, "Type", "") == "ComponentPattern")
        self.assertEqual(len(pattern.JointCopies), 5)
        pattern_name = pattern.Name
        generated = {o.Name for o in self.doc.Objects} - before
        self.assertTrue(generated)
        self.doc.undo()
        self.assertEqual({o.Name for o in self.doc.Objects}, before)
        self.doc.redo()
        self.assertEqual({o.Name for o in self.doc.Objects}, before | generated)
        self.assertPolar(self.doc.getObject(pattern_name), 6)
        Gui.Selection.clearSelection()

    def test_command_async_failure_rolls_back_and_reports(self):
        import CommandComponentPattern
        from unittest.mock import patch

        panel = self.openPanel()
        before = {o.Name for o in self.doc.Objects}
        real_create = CommandComponentPattern.createComponentPattern
        def invalid(*args, **kwargs):
            pattern = real_create(*args, **kwargs)
            pattern.UseSource = False
            pattern.Axis = App.Vector()
            return pattern
        with patch.object(CommandComponentPattern, "createComponentPattern", side_effect=invalid):
            self.assertTrue(panel.accept())
            self.waitForPanel(panel)
        self.assertIs(CommandComponentPattern.activeTask, panel)
        self.assertTrue(panel.error.text())
        self.assertEqual({o.Name for o in self.doc.Objects}, before)
        panel.reject()

    def test_command_translated_kind_retains_internal_key(self):
        import CommandComponentPattern
        from unittest.mock import patch

        panel = self.openPanel()
        panel.reject()
        with patch.object(CommandComponentPattern, "translate", side_effect=lambda context, text: "TR:" + text):
            panel = CommandComponentPattern.TaskComponentPattern(self.assembly)
        self.assertEqual(panel.kind.itemText(0), "TR:Polar")
        self.assertEqual(panel.kind.itemData(0), "Polar")
        self.assertEqual(panel.kind.itemData(1), "Linear")
        panel.form.deleteLater()

    def test_command_rejects_invalid_inputs(self):
        panel = self.openPanel()
        panel.source.setCurrentIndex(0)
        before = {o.Name for o in self.doc.Objects}
        for kind, widget in ((0, panel.axis), (1, panel.direction)):
            panel.kind.setCurrentIndex(kind)
            widget.setText("0, 0, 0")
            self.assertFalse(panel.accept())
            self.assertTrue(panel.error.text())
            self.assertEqual({o.Name for o in self.doc.Objects}, before)
            widget.setText("1, 0, 0")
        panel.length.setValue(0)
        self.assertFalse(panel.accept())
        panel.reject()

    def test_command(self):
        import FreeCADGui as Gui

        self.doc.UndoMode = 1
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
