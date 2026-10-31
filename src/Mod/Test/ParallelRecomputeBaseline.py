# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileNotice: Part of the FreeCAD project.

import os
import unittest

import FreeCAD as App


def fixture_dir():
    """Packaged TestData, not a walk up to the source tree.

    CMake copies ``data/tests/parallel-recompute/*.FCStd`` to
    ``Mod/Test/TestData/parallel-recompute`` for the build tree and installs
    that same relative path. ``FreeCAD.getHomePath()`` is the build directory
    or the install prefix.
    """
    packaged = os.path.join(
        App.getHomePath(), "Mod", "Test", "TestData", "parallel-recompute"
    )
    if not os.path.isdir(packaged):
        raise AssertionError(
            "parallel-recompute fixtures are not installed with the Test module at "
            + packaged
        )
    return packaged


def open_fixture(name):
    return App.openDocument(os.path.join(fixture_dir(), name))


class ParallelRecomputeBaseline(unittest.TestCase):
    """Serial results of the small documents M1 reuses as compatibility baselines."""

    def tearDown(self):
        for name in list(App.listDocuments()):
            App.closeDocument(name)

    def assertVolume(self, shape, expected):
        self.assertFalse(shape.isNull())
        self.assertTrue(shape.isValid())
        self.assertEqual(len(shape.Solids), 1)
        self.assertAlmostEqual(shape.Volume, expected, delta=1e-4)

    def assertBoxGeometry(self, box, volume):
        """Rebuilt solid matches the box parameters.

        Element names and topology equivalence (which face is which) stay
        outstanding; this pins volume, validity, and the parameter bounding box.
        """
        self.assertVolume(box.Shape, volume)
        bounds = box.Shape.BoundBox
        self.assertAlmostEqual(bounds.XLength, box.Length.Value, delta=1e-6)
        self.assertAlmostEqual(bounds.YLength, box.Width.Value, delta=1e-6)
        self.assertAlmostEqual(bounds.ZLength, box.Height.Value, delta=1e-6)

    def forceRecompute(self, document):
        rebuilt = document.recompute(None, True)
        self.assertGreater(rebuilt, 0)
        return rebuilt

    def testFixturesResolveFromPackagedTestData(self):
        directory = os.path.normpath(fixture_dir())
        packaged = os.path.normpath(
            os.path.join(App.getHomePath(), "Mod", "Test", "TestData")
        )
        # commonpath drops a leading "./" that getHomePath() can carry.
        self.assertEqual(os.path.commonpath([directory, packaged]), packaged)
        self.assertTrue(os.path.isfile(os.path.join(directory, "part-siblings.FCStd")))

    def testFeatureTestCleanDocumentStaysSettled(self):
        document = open_fixture("featuretest-abz-clean.FCStd")
        source = document.getObject("A")
        dependent = document.getObject("B")
        independent = document.getObject("Z")
        self.assertEqual(dependent.Source1.Name, "A")
        self.assertEqual(source.ExecCount, 1)
        self.assertEqual(dependent.ExecCount, 1)
        self.assertEqual(independent.ExecCount, 1)
        self.assertNotIn("Touched", source.State)
        document.recompute()
        self.assertEqual(source.ExecCount, 1)
        self.assertEqual(dependent.ExecCount, 1)
        self.assertEqual(independent.ExecCount, 1)
        self.assertNotIn("Invalid", source.State)
        self.forceRecompute(document)
        self.assertEqual(source.ExecCount, 2)
        self.assertEqual(dependent.ExecCount, 2)
        self.assertEqual(independent.ExecCount, 2)
        self.assertNotIn("Touched", source.State)
        self.assertNotIn("Invalid", source.State)

    def testFeatureTestDirtyDocumentRecomputesSourceAndDependent(self):
        document = open_fixture("featuretest-abz-dirty.FCStd")
        source = document.getObject("A")
        dependent = document.getObject("B")
        independent = document.getObject("Z")
        self.assertIn("Touched", source.State)
        self.assertNotIn("Touched", dependent.State)
        self.assertNotIn("Touched", independent.State)
        self.assertEqual(source.ExecCount, 1)
        self.assertEqual(dependent.ExecCount, 1)
        self.assertEqual(independent.ExecCount, 1)
        recomputed = document.recompute()
        self.assertEqual(recomputed, 2)
        self.assertEqual(source.ExecCount, 2)
        self.assertEqual(dependent.ExecCount, 2)
        self.assertEqual(independent.ExecCount, 1)
        self.assertNotIn("Touched", source.State)
        self.assertNotIn("Invalid", source.State)
        self.forceRecompute(document)
        self.assertEqual(source.ExecCount, 3)
        self.assertEqual(dependent.ExecCount, 3)
        self.assertEqual(independent.ExecCount, 2)
        self.assertNotIn("Touched", source.State)

    def testPartSiblingsKeepBoxVolumes(self):
        document = open_fixture("part-siblings.FCStd")
        left = document.getObject("A")
        right = document.getObject("B")
        self.assertBoxGeometry(left, 6000)
        self.assertBoxGeometry(right, 120)
        self.assertEqual(document.recompute(), 0)
        self.assertBoxGeometry(left, 6000)
        self.assertBoxGeometry(right, 120)
        self.forceRecompute(document)
        self.assertBoxGeometry(left, 6000)
        self.assertBoxGeometry(right, 120)

    def testPartChainCutVolume(self):
        document = open_fixture("part-chain.FCStd")
        cut = document.getObject("Cut")
        self.assertEqual(cut.TypeId, "Part::Cut")
        self.assertFalse(cut.Refine)
        self.assertEqual(cut.Base.Name, "Base")
        self.assertEqual(cut.Tool.Name, "Tool")
        self.assertVolume(document.getObject("Base").Shape, 8000)
        self.assertVolume(document.getObject("Tool").Shape, 4000)
        self.assertVolume(cut.Shape, 4000)
        self.assertEqual(document.recompute(), 0)
        self.assertVolume(cut.Shape, 4000)
        self.forceRecompute(document)
        self.assertVolume(document.getObject("Base").Shape, 8000)
        self.assertVolume(document.getObject("Tool").Shape, 4000)
        self.assertVolume(cut.Shape, 4000)

    def testPartAbzCutAndIndependentBox(self):
        document = open_fixture("part-abz.FCStd")
        cut = document.getObject("B")
        self.assertEqual(cut.TypeId, "Part::Cut")
        self.assertEqual(cut.Base.Name, "A")
        self.assertEqual(document.getObject("Z").TypeId, "Part::Box")
        self.assertBoxGeometry(document.getObject("A"), 1000)
        self.assertVolume(cut.Shape, 500)
        self.assertBoxGeometry(document.getObject("Z"), 24)
        self.assertEqual(document.recompute(), 0)
        self.assertVolume(cut.Shape, 500)
        self.assertBoxGeometry(document.getObject("Z"), 24)
        self.forceRecompute(document)
        self.assertBoxGeometry(document.getObject("A"), 1000)
        self.assertVolume(cut.Shape, 500)
        self.assertBoxGeometry(document.getObject("Z"), 24)

    def testInvalidBoxStaysInvalid(self):
        document = open_fixture("part-invalid-box.FCStd")
        box = document.getObject("Invalid")
        self.assertEqual(box.TypeId, "Part::Box")
        self.assertAlmostEqual(box.Length.Value, 0.0, delta=1e-12)
        self.assertIn("Invalid", box.State)
        self.assertTrue(box.Shape.isNull())
        document.recompute()
        self.assertIn("Invalid", box.State)
        self.assertTrue(box.Shape.isNull())
        document.recompute(None, True)
        self.assertIn("Invalid", box.State)
        self.assertTrue(box.Shape.isNull())
        self.assertAlmostEqual(box.Length.Value, 0.0, delta=1e-12)

    def testDirtyBoxKeepsEditedLength(self):
        document = open_fixture("part-dirty-box.FCStd")
        box = document.getObject("Dirty")
        self.assertAlmostEqual(box.Length.Value, 20.0, delta=1e-9)
        self.assertIn("Touched", box.State)
        self.assertBoxGeometry(box, 2000)
        document.recompute()
        self.assertNotIn("Touched", box.State)
        self.assertNotIn("Invalid", box.State)
        self.assertBoxGeometry(box, 2000)
        self.forceRecompute(document)
        self.assertNotIn("Touched", box.State)
        self.assertNotIn("Invalid", box.State)
        self.assertBoxGeometry(box, 2000)
