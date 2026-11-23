# ***************************************************************************
# *   Copyright (c) 2006 Werner Mayer <werner.wm.mayer@gmx.de>              *
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
# *                                                                         *
# ***************************************************************************/

# Workbench test module

import FreeCAD, FreeCADGui, os, unittest
import tempfile
import subprocess
import time

from PySide import QtWidgets, QtCore
from PySide.QtWidgets import QApplication


class CallableCheckWarning:
    def __call__(self):
        diag = QApplication.activeModalWidget()
        if diag:
            QtCore.QTimer.singleShot(0, diag, QtCore.SLOT("accept()"))


class WorkbenchTestCase(unittest.TestCase):
    def setUp(self):
        self.Active = FreeCADGui.activeWorkbench()
        FreeCAD.Console.PrintLog(FreeCADGui.activeWorkbench().name())

    def testActivate(self):
        wbs = FreeCADGui.listWorkbenches()
        # this gives workbenches a possibility to detect that we're under test environment
        FreeCAD.TestEnvironment = True
        for i in wbs:
            try:
                print("Activate workbench '{}'".format(i))
                cobj = CallableCheckWarning()
                QtCore.QTimer.singleShot(500, cobj)
                if FreeCADGui.activeWorkbench().name() != i:
                    success = FreeCADGui.activateWorkbench(i)
                else:
                    # Cannot test activation of an already-active workbench
                    success = True
                FreeCAD.Console.PrintLog(
                    "Active: " + FreeCADGui.activeWorkbench().name() + " Expected: " + i + "\n"
                )
                self.assertTrue(success, "Test on activating workbench {0} failed".format(i))
            except Exception as e:
                self.fail("Loading of workbench '{0}' failed: {1}".format(i, e))
        del FreeCAD.TestEnvironment

    def testHandler(self):
        import __main__

        class UnitWorkbench(__main__.Workbench):
            MenuText = "Unittest"
            ToolTip = "Unittest"

            def Initialize(self):
                cmds = ["Test_Test"]
                self.appendToolbar("My Unittest", cmds)

            def GetClassName(self):
                return "Gui::PythonWorkbench"

        FreeCADGui.addWorkbench(UnitWorkbench())
        wbs = FreeCADGui.listWorkbenches()
        self.assertTrue("UnitWorkbench" in wbs, "Test on adding workbench handler failed")
        FreeCADGui.activateWorkbench("UnitWorkbench")
        FreeCADGui.updateGui()
        self.assertTrue(
            FreeCADGui.activeWorkbench().name() == "UnitWorkbench",
            "Test on loading workbench 'Unittest' failed",
        )
        FreeCADGui.removeWorkbench("UnitWorkbench")
        wbs = FreeCADGui.listWorkbenches()
        self.assertTrue(not "UnitWorkbench" in wbs, "Test on removing workbench handler failed")

    def testInvalidType(self):
        class MyExtWorkbench(FreeCADGui.Workbench):
            def Initialize(self):
                pass

            def GetClassName(self):
                return "App::Extension"

        FreeCADGui.addWorkbench(MyExtWorkbench())
        with self.assertRaises(TypeError):
            FreeCADGui.activateWorkbench("MyExtWorkbench")
        FreeCADGui.removeWorkbench("MyExtWorkbench")

    def tearDown(self):
        FreeCADGui.activateWorkbench(self.Active.name())
        FreeCAD.Console.PrintLog(self.Active.name())


class CommandTestCase(unittest.TestCase):
    def testPR6889(self):
        # Fixes a crash
        TempPath = tempfile.gettempdir()
        macroName = TempPath + os.sep + "testmacro.py"
        macroFile = open(macroName, "w")
        macroFile.write("print ('Hello, World!')")
        macroFile.close()

        name = FreeCADGui.Command.createCustomCommand(macroName)
        cmd = FreeCADGui.Command.get(name)
        cmd.run()


class TestNavigationStyle(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("CreateTest")
        self.View = FreeCADGui.getDocument(self.Doc).ActiveView

    def test00ViewerNoArgMethodBeforeAndAfterVarargs(self):
        viewer = self.View.getViewer()
        self.assertFalse(viewer.isSpinning())
        with self.assertRaises(TypeError):
            viewer.isSpinning(True)
        self.assertIsNotNone(viewer.getNavigationStyle())
        self.assertFalse(viewer.isSpinning())

    def test01ViewerDispatchOrdersInFreshProcesses(self):
        # The handler caches are process-wide. Each child starts a fresh
        # interpreter so the two access orders exercise independent caches.
        marker = "FREECAD_TEST_VIEWER_DISPATCH_ORDER"
        order = os.environ.get(marker)
        if order:
            viewer = self.View.getViewer()
            if order == "noargs-first":
                self.assertFalse(viewer.isSpinning())
                self.assertIsNotNone(viewer.getNavigationStyle())
            elif order == "varargs-first":
                self.assertIsNotNone(viewer.getNavigationStyle())
                self.assertFalse(viewer.isSpinning())
            else:
                self.fail("unexpected dispatcher test order")
            with self.assertRaises(TypeError):
                viewer.isSpinning(True)
            with self.assertRaises(ValueError):
                viewer.setPickRadius(-1)
            with self.assertRaises(TypeError):
                viewer.renderToImage(unknown=True)
            image = viewer.renderToImage(width=32, height=24, samples=0)
            self.assertFalse(image.isNull())
            self.assertEqual((image.width(), image.height()), (32, 24))
            self.assertFalse(viewer.isSpinning())
            return

        for order in ("noargs-first", "varargs-first"):
            with self.subTest(order=order):
                environment = os.environ.copy()
                environment[marker] = order
                with tempfile.TemporaryDirectory(prefix="freecad-viewer-order-") as home:
                    environment["FREECAD_USER_HOME"] = home
                    result = subprocess.run(
                        [QtCore.QCoreApplication.applicationFilePath(), "-t",
                         "Workbench.TestNavigationStyle.test01ViewerDispatchOrdersInFreshProcesses"],
                        env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, timeout=60, check=False,
                    )
                output = result.stdout
                self.assertEqual(result.returncode, 0, output)
                self.assertIn("Ran 1 test in ", output)
                self.assertIn("\nOK\n", output)
                self.assertNotIn("skipped=", output.lower())
                # Preserve all child diagnostics for the outer CI log gate.
                for line in output.splitlines():
                    print("viewer-dispatch-child: " + line)

    def testSavedViewerMethodsRejectDestroyedView(self):
        from Test.GuiRecompute import close_document

        document = FreeCAD.newDocument("SavedViewerMethodLifetime")
        try:
            gui_document = FreeCADGui.getDocument(document)
            view = gui_document.ActiveView
            viewer = view.getViewer()
            widget = view.graphicsView()
            destroyed = []
            widget.destroyed.connect(lambda *_args: destroyed.append(True))

            noargs = viewer.isSpinning
            varargs = viewer.getNavigationStyle
            keywords = viewer.renderToImage
            self.assertFalse(noargs())
            self.assertIsNotNone(varargs())
            with self.assertRaises(TypeError):
                keywords(unknown=True)

            close_document(document)
            deadline = time.monotonic() + 10.0
            while not destroyed and time.monotonic() < deadline:
                QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
                QtWidgets.QApplication.processEvents()
                time.sleep(0.01)
            self.assertTrue(destroyed, "viewer QObject was not destroyed")
            for method, args, kw in ((noargs, (), {}),
                                     (varargs, (), {}),
                                     (keywords, (), {"width": 32, "height": 24})):
                with self.subTest(method=method.__name__):
                    with self.assertRaisesRegex(RuntimeError, "deleted viewer"):
                        method(*args, **kw)
        finally:
            # close_document is idempotent if the normal close already finished.
            close_document(document)

    def testInvalidStyle(self):
        # Coin deliberately reports this invalid call as a debug warning.
        # Exercise it in a child FreeCAD process so the warning can be
        # asserted precisely without being confused with unexpected warnings
        # from the rest of this registered GUI suite.
        marker = "FREECAD_TEST_INVALID_NAVIGATION_CHILD"
        if os.environ.get(marker) == "1":
            original = self.View.getNavigationType()
            self.View.setNavigationType("App::Extension")
            self.assertEqual(self.View.getNavigationType(), original)
            return

        environment = os.environ.copy()
        environment[marker] = "1"
        with tempfile.TemporaryDirectory(prefix="freecad-nav-warning-") as home:
            environment["FREECAD_USER_HOME"] = home
            result = subprocess.run(
                [QtCore.QCoreApplication.applicationFilePath(), "-t",
                 "Workbench.TestNavigationStyle.testInvalidStyle"],
                env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=60, check=False,
            )
        output = result.stdout
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("Ran 1 test in ", output)
        self.assertIn("\nOK\n", output)
        self.assertNotIn("skipped=", output.lower())
        diagnostics = [line for line in output.splitlines()
                       if "Coin warning" in line or "Coin error" in line]
        expected = ("Coin warning in View3DInventorViewer::setNavigationType(): "
                    "Navigation object must be of type NavigationStyle.")
        if FreeCAD.ConfigGet("Debug") == "1":
            self.assertEqual(diagnostics, [expected], output)
        else:
            self.assertEqual(diagnostics, [], output)
        # Relay all other output so the enclosing CI log gate still sees any
        # unexpected Qt, VTK, Coin or FreeCAD diagnostics from the child.
        for line in output.splitlines():
            if line != expected:
                print("navigation-negative-child: " + line)

    def testOrientationLockBlocksProgrammaticOrientationChanges(self):
        viewer = self.View.getViewer()
        navigation = viewer.getNavigationStyle()
        self.View.setAnimationEnabled(False)
        initial = self.View.getCameraOrientation()
        target_rotation = initial.multiply(FreeCAD.Rotation(FreeCAD.Vector(0, 1, 0), 90))
        self.assertFalse(target_rotation.isSame(initial, 1e-6))
        target = target_rotation.Q

        navigation.setOrientationLocked(True)
        self.View.setViewDirection((1.0, 0.0, 0.0))
        self.assertTrue(self.View.getCameraOrientation().isSame(initial, 1e-6))
        self.View.setCameraOrientation(target)
        self.assertTrue(self.View.getCameraOrientation().isSame(initial, 1e-6))

        navigation.setOrientationLocked(False)
        self.View.setCameraOrientation(target)
        self.assertFalse(self.View.getCameraOrientation().isSame(initial, 1e-6))

    def testOrientationLockAllowsTranslationOnlyViewPosition(self):
        navigation = self.View.getViewer().getNavigationStyle()
        initial = self.View.viewPosition()
        target = FreeCAD.Placement(
            FreeCAD.Vector(initial.Base.x + 10.0, initial.Base.y + 20.0, initial.Base.z + 30.0),
            initial.Rotation,
        )

        navigation.setOrientationLocked(True)
        self.View.viewPosition(target, 0, 0)
        moved = self.View.viewPosition()

        self.assertTrue(moved.Rotation.isSame(initial.Rotation, 1e-6))
        self.assertGreater((moved.Base - initial.Base).Length, 1e-6)

    def testOrientationLockDoesNotBlockCameraTypeChanges(self):
        navigation = self.View.getViewer().getNavigationStyle()
        current = self.View.getCameraType()
        target = "Orthographic" if current == "Perspective" else "Perspective"

        navigation.setOrientationLocked(True)
        self.View.setCameraType(target)

        self.assertEqual(self.View.getCameraType(), target)

    def testNavigationStyleSwitchPreservesRotationFlags(self):
        viewer = self.View.getViewer()
        navigation = viewer.getNavigationStyle()
        current = self.View.getNavigationType()
        styles = [style for style in self.View.listNavigationTypes() if style != current]

        self.assertTrue(styles)
        self.assertNotIn("Gui::NavigationStateChart", styles)
        navigation.setRotationEnabled(False)
        navigation.setOrientationLocked(True)

        self.View.setNavigationType(styles[0])
        self.assertEqual(self.View.getNavigationType(), styles[0])
        navigation = viewer.getNavigationStyle()

        self.assertFalse(navigation.isRotationEnabled())
        self.assertTrue(navigation.isOrientationLocked())

    def testRotationDisableStopsActiveSpinAnimation(self):
        viewer = self.View.getViewer()
        navigation = viewer.getNavigationStyle()

        self.View.startAnimating(0.0, 1.0, 0.0, 1.0)
        self.assertTrue(viewer.isSpinning())

        navigation.setRotationEnabled(False)

        self.assertFalse(viewer.isSpinning())

    def tearDown(self):
        FreeCAD.closeDocument("CreateTest")
