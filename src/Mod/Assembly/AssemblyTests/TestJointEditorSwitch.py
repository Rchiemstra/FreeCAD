# SPDX-License-Identifier: LGPL-2.1-or-later

"""Joint task replacement must preserve existing references (issue #32967)."""

import unittest

import FreeCAD as App
import JointObject

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets


@unittest.skipUnless(App.GuiUp, "Requires the Assembly GUI")
class TestJointEditorSwitch(unittest.TestCase):
    def setUp(self):
        Gui.activateWorkbench("AssemblyWorkbench")
        self.doc = App.newDocument("JointEditorSwitch")
        self.doc.UndoMode = 1
        self.gui_doc = Gui.getDocument(self.doc)
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        group = self.assembly.newObject("Assembly::JointGroup", "Joints")
        source = self.doc.addObject("Part::Box", "Source")
        self.parts = [self.assembly.newObject("App::Link", "Part") for _ in range(3)]
        for part in self.parts:
            part.LinkedObject = source
        self.doc.recompute()
        self.joints = []
        for index in range(2):
            joint = group.newObject("App::FeaturePython", "Joint")
            JointObject.Joint(joint, 0)
            JointObject.ViewProviderJoint(joint.ViewObject)
            joint.Proxy.setJointConnectors(
                joint,
                [(part, ["Face1", "Vertex1"]) for part in self.parts[index:index + 2]],
            )
            joint.Label = "SwitchJoint" + str(index)
            self.joints.append(joint)
        self.doc.recompute()
        self.gui_doc.setEdit(self.assembly)
        Gui.updateGui()
        self.doc.clearUndos()
        self.references = [(j.Reference1, j.Reference2) for j in self.joints]
        self.placements = [p.Placement.copy() for p in self.parts]
        self.connectorPlacements = [
            (j.Placement1.copy(), j.Placement2.copy()) for j in self.joints
        ]

    def tearDown(self):
        if JointObject.activeTask is not None:
            JointObject.activeTask.reject()
        if Gui.Control.activeDialog():
            Gui.Control.closeDialog()
        self.gui_doc.resetEdit()
        App.closeDocument(self.doc.Name)

    def openJoint(self, joint, tree):
        if tree:
            # Use real clicks: the preceding selection change is part of the
            # regression, before TreeWidget opens its automatic "Edit" transaction.
            for widget in Gui.getMainWindow().findChildren(QtWidgets.QTreeWidget):
                items = widget.findItems(
                    joint.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
                )
                if items:
                    break
            self.assertTrue(items, "Joint must be present in the model tree")
            child = widget
            parent = child.parentWidget()
            while parent is not None:
                if isinstance(parent, QtWidgets.QTabWidget):
                    parent.setCurrentWidget(child)
                if isinstance(parent, QtWidgets.QDockWidget):
                    parent.show()
                child, parent = parent, parent.parentWidget()
            widget.expandAll()
            Gui.updateGui()
            # Showing or expanding the model dock can rebuild its items.
            item = widget.findItems(
                joint.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
            )[0]
            widget.scrollToItem(item)
            Gui.updateGui()
            item = widget.findItems(
                joint.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
            )[0]
            rect = widget.visualItemRect(item)
            point = QtCore.QPoint(rect.right() - 10, rect.center().y())
            for kind in (
                QtCore.QEvent.MouseButtonPress,
                QtCore.QEvent.MouseButtonRelease,
                QtCore.QEvent.MouseButtonDblClick,
                QtCore.QEvent.MouseButtonRelease,
            ):
                event = QtGui.QMouseEvent(
                    kind, QtCore.QPointF(point), QtCore.Qt.LeftButton,
                    QtCore.Qt.LeftButton, QtCore.Qt.NoModifier,
                )
                QtWidgets.QApplication.sendEvent(widget.viewport(), event)
        else:
            # The Edit Joint context-menu action invokes editJoint directly.
            self.assertTrue(joint.ViewObject.Proxy.editJoint(joint.ViewObject))
        Gui.updateGui()
        self.assertIsNotNone(JointObject.activeTask)
        self.assertEqual(JointObject.activeTask.joint, joint)

    def assertUnchanged(self):
        for joint, references in zip(self.joints, self.references):
            self.assertEqual((joint.Reference1, joint.Reference2), references)
        for joint, placements in zip(self.joints, self.connectorPlacements):
            self.assertTrue(joint.Placement1.isSame(placements[0], 1e-7))
            self.assertTrue(joint.Placement2.isSame(placements[1], 1e-7))
        for part, placement in zip(self.parts, self.placements):
            self.assertTrue(part.Placement.isSame(placement, 1e-7))

    def checkSwitch(self, tree, accept):
        self.openJoint(self.joints[0], tree)
        self.openJoint(self.joints[1], tree)
        self.assertUnchanged()
        task = Gui.Control.activeTaskDialog()
        if accept:
            task.accept()
        else:
            task.reject()
        Gui.updateGui()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertUnchanged()
        # A real edit after the switch supplies an undo entry even when both
        # editors were accepted without changing any joint settings.
        self.doc.openTransaction("Change part label")
        self.parts[0].Label = "Changed label"
        self.doc.commitTransaction()
        Gui.updateGui()
        self.doc.undo()
        self.assertUnchanged()
        self.doc.redo()
        self.assertUnchanged()

    def test_tree_switch_then_ok(self):
        self.checkSwitch(tree=True, accept=True)

    def test_tree_switch_then_cancel(self):
        self.checkSwitch(tree=True, accept=False)

    def test_context_switch_then_ok(self):
        self.checkSwitch(tree=False, accept=True)

    def test_context_switch_then_cancel(self):
        self.checkSwitch(tree=False, accept=False)

    def checkEditedSwitch(self, tree, accept):
        originalLabels = [joint.Label for joint in self.joints]
        self.openJoint(self.joints[0], tree)
        self.joints[0].Label = "Edited first joint"
        self.openJoint(self.joints[1], tree)
        self.assertEqual(self.joints[0].Label, "Edited first joint")
        self.assertUnchanged()
        self.joints[1].Label = "Edited second joint"
        task = Gui.Control.activeTaskDialog()
        if accept:
            task.accept()
        else:
            task.reject()
        Gui.updateGui()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertUnchanged()
        self.assertEqual(self.joints[0].Label, "Edited first joint")
        self.assertEqual(
            self.joints[1].Label, "Edited second joint" if accept else originalLabels[1]
        )
        finalLabels = [joint.Label for joint in self.joints]
        undoCount = self.doc.UndoCount
        self.assertGreater(undoCount, 0)
        # Tree double-click also creates an automatic transaction. Check every
        # entry produced by the switch, including panel cleanup, in both directions.
        for _ in range(undoCount):
            self.doc.undo()
            self.assertUnchanged()
        self.assertEqual([joint.Label for joint in self.joints], originalLabels)
        for _ in range(undoCount):
            self.doc.redo()
            self.assertUnchanged()
        self.assertEqual([joint.Label for joint in self.joints], finalLabels)

    def test_tree_switch_commits_first_edit_then_ok(self):
        self.checkEditedSwitch(tree=True, accept=True)

    def test_tree_switch_commits_first_edit_then_cancel(self):
        self.checkEditedSwitch(tree=True, accept=False)

    def test_context_switch_commits_first_edit_then_ok(self):
        self.checkEditedSwitch(tree=False, accept=True)

    def test_context_switch_commits_first_edit_then_cancel(self):
        self.checkEditedSwitch(tree=False, accept=False)

    def test_edit_reference_delete_after_selection_clear(self):
        self.openJoint(self.joints[0], tree=False)
        task = JointObject.activeTask
        Gui.Selection.clearSelection()
        self.assertUnchanged()
        task.jForm.featureList.setCurrentRow(0)
        task.deleteSelectedItems()
        self.assertEqual(len(task.refs), 1)
        self.assertIsNone(self.joints[0].Reference2)
        Gui.Control.activeTaskDialog().reject()
        Gui.updateGui()
        self.assertUnchanged()
