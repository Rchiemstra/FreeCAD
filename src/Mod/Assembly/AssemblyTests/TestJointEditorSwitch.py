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
        source.Visibility = False
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
            joint.Visibility = False
            self.joints.append(joint)
        self.doc.recompute()
        self.gui_doc.setEdit(self.assembly)
        Gui.updateGui()
        self.doc.commitTransaction()
        self.doc.clearUndos()
        self.references = [(j.Reference1, j.Reference2) for j in self.joints]
        self.placements = [p.Placement.copy() for p in self.parts]
        self.connectorPlacements = [
            (j.Placement1.copy(), j.Placement2.copy()) for j in self.joints
        ]
        self.jointProperties = [self.properties(joint) for joint in self.joints]

    def properties(self, joint):
        return {name: repr(joint.getPropertyByName(name)) for name in joint.PropertiesList
                if name != "Proxy"}

    def tearDown(self):
        if JointObject.activeTask is not None:
            JointObject.activeTask.reject()
        if Gui.Control.activeDialog():
            Gui.Control.closeDialog()
        self.gui_doc.resetEdit()
        App.closeDocument(self.doc.Name)

    def mouse(self, widget, point, button=QtCore.Qt.LeftButton,
              modifiers=QtCore.Qt.NoModifier):
        QtGui.QCursor.setPos(widget.mapToGlobal(point))
        for kind in (QtCore.QEvent.MouseButtonPress, QtCore.QEvent.MouseButtonRelease):
            event = QtGui.QMouseEvent(
                kind, QtCore.QPointF(point), QtCore.QPointF(widget.mapToGlobal(point)), button,
                button if kind == QtCore.QEvent.MouseButtonPress else QtCore.Qt.NoButton,
                modifiers,
            )
            QtWidgets.QApplication.sendEvent(widget, event)

    def key(self, widget, key, modifiers=QtCore.Qt.NoModifier):
        for kind in (QtCore.QEvent.KeyPress, QtCore.QEvent.KeyRelease):
            QtWidgets.QApplication.sendEvent(widget, QtGui.QKeyEvent(kind, key, modifiers))

    def finish(self, accept):
        role = QtWidgets.QDialogButtonBox.Ok if accept else QtWidgets.QDialogButtonBox.Cancel
        buttons = [box.button(role) for box in
                   Gui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox)]
        button = next(button for button in buttons if button and button.isVisible())
        self.mouse(button, button.rect().center())
        Gui.updateGui()

    def openJoint(self, joint, tree, expected=None):
        # Both paths use real tree selection before the editor entry point.
        items = []
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
        self.mouse(widget.viewport(), point)
        if tree:
            for kind in (QtCore.QEvent.MouseButtonDblClick, QtCore.QEvent.MouseButtonRelease):
                event = QtGui.QMouseEvent(
                    kind, QtCore.QPointF(point), QtCore.Qt.LeftButton,
                    QtCore.Qt.LeftButton, QtCore.Qt.NoModifier,
                )
                QtWidgets.QApplication.sendEvent(widget.viewport(), event)
        else:
            self.mouse(widget.viewport(), point, QtCore.Qt.RightButton)
            triggered = []
            offered = []

            def chooseEdit():
                menu = QtWidgets.QApplication.activePopupWidget()
                if isinstance(menu, QtWidgets.QMenu):
                    for action in menu.actions():
                        text = action.text().replace("&", "")
                        offered.append(text)
                        if text == App.Qt.translate("Assembly", "Edit Joint"):
                            triggered.append(action.text())
                            self.mouse(menu, menu.actionGeometry(action).center())
                            return
                    menu.close()

            # contextMenuEvent enters QMenu.exec(); choose its real action there.
            QtCore.QTimer.singleShot(0, chooseEdit)
            event = QtGui.QContextMenuEvent(
                QtGui.QContextMenuEvent.Mouse, point, widget.viewport().mapToGlobal(point)
            )
            QtWidgets.QApplication.sendEvent(widget.viewport(), event)
            self.assertTrue(triggered, "Right-click menu must offer Edit Joint: " + repr(offered))
        Gui.updateGui()
        self.assertIsNotNone(JointObject.activeTask)
        self.assertEqual(JointObject.activeTask.joint, expected or joint)

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
        self.finish(accept)
        self.assertFalse(Gui.Control.activeDialog())
        self.assertUnchanged()

    def test_tree_switch_then_ok(self):
        self.checkSwitch(tree=True, accept=True)

    def test_tree_switch_then_cancel(self):
        self.checkSwitch(tree=True, accept=False)

    def test_context_switch_then_ok(self):
        self.checkSwitch(tree=False, accept=True)

    def test_context_switch_then_cancel(self):
        self.checkSwitch(tree=False, accept=False)

    def changeType(self):
        combo = JointObject.activeTask.jForm.jointType
        self.key(combo, QtCore.Qt.Key_Down)
        Gui.updateGui()
        self.assertEqual(JointObject.activeTask.joint.JointType, "Revolute")

    def assertTypes(self, first, second):
        self.assertEqual([j.JointType for j in self.joints], [first, second])
        self.assertUnchanged()
        self.assertEqual([j.Visibility for j in self.joints], [False, False])

    def checkEditedSwitch(self, tree, accept):
        self.openJoint(self.joints[0], tree)
        self.changeType()
        self.openJoint(self.joints[1], tree)
        self.assertEqual(self.doc.UndoCount, 1, "A including cleanup is one finished edit")
        self.assertEqual([j.Visibility for j in self.joints], [False, True])
        self.changeType()
        self.finish(accept)
        self.assertFalse(Gui.Control.activeDialog())
        self.assertTypes("Revolute", "Revolute" if accept else "Fixed")
        self.assertEqual(self.doc.UndoCount, 2 if accept else 1)
        if accept:
            self.doc.undo()
            self.assertTypes("Revolute", "Fixed")
        self.doc.undo()
        self.assertTypes("Fixed", "Fixed")
        self.assertEqual(self.doc.UndoCount, 0)
        self.doc.redo()
        self.assertTypes("Revolute", "Fixed")
        if accept:
            self.doc.redo()
            self.assertTypes("Revolute", "Revolute")

    def test_tree_switch_commits_first_edit_then_ok(self):
        self.checkEditedSwitch(tree=True, accept=True)

    def test_tree_switch_commits_first_edit_then_cancel(self):
        self.checkEditedSwitch(tree=True, accept=False)

    def test_context_switch_commits_first_edit_then_ok(self):
        self.checkEditedSwitch(tree=False, accept=True)

    def test_context_switch_commits_first_edit_then_cancel(self):
        self.checkEditedSwitch(tree=False, accept=False)

    def deleteReferences(self, multiple=False):
        featureList = JointObject.activeTask.jForm.featureList
        point = featureList.visualItemRect(featureList.item(0)).center()
        self.mouse(featureList.viewport(), point)
        if multiple:
            self.mouse(featureList.viewport(), featureList.visualItemRect(featureList.item(1)).center(),
                       modifiers=QtCore.Qt.ControlModifier)
            self.assertEqual(len(featureList.selectedIndexes()), 2)
        self.key(featureList, QtCore.Qt.Key_Delete)
        Gui.updateGui()

    def checkInvalidSwitch(self, tree):
        self.openJoint(self.joints[0], tree)
        self.changeType()
        self.deleteReferences()
        self.assertEqual(len(JointObject.activeTask.refs), 1)
        self.assertIsNone(self.joints[0].Reference2)
        self.openJoint(self.joints[1], tree, expected=self.joints[0])
        # Failed validation must leave A's entire transaction available to Cancel.
        pendingUndoCount = self.doc.UndoCount
        self.finish(False)
        self.assertTypes("Fixed", "Fixed")
        for joint, properties in zip(self.joints, self.jointProperties):
            self.assertEqual(self.properties(joint), properties)
        self.assertEqual(pendingUndoCount, 0)
        self.assertEqual(self.doc.UndoCount, 0)

    def test_tree_failed_accept_then_cancel_restores_first_joint(self):
        self.checkInvalidSwitch(tree=True)

    def test_context_failed_accept_then_cancel_restores_first_joint(self):
        self.checkInvalidSwitch(tree=False)

    def test_delete_multiple_reference_rows(self):
        self.openJoint(self.joints[0], tree=True)
        self.deleteReferences(multiple=True)
        self.assertEqual(JointObject.activeTask.refs, [])
        self.assertIsNone(self.joints[0].Reference1)
        self.assertIsNone(self.joints[0].Reference2)
        self.finish(False)
        self.assertUnchanged()
        self.assertEqual(self.doc.UndoCount, 0)

    def viewport(self):
        # Quarter installs its Coin event filter on the graphics view itself.
        return Gui.activeDocument().activeView().graphicsView()

    def test_viewport_empty_space_keeps_references(self):
        self.openJoint(self.joints[0], tree=True)
        view = self.viewport()
        # Before this branch GreedySelection ignores empty-space clicks, leaving
        # the existing joint's references selected and the editor open.
        self.mouse(view, QtCore.QPoint(2, 2))
        Gui.updateGui()
        self.assertUnchanged()
        self.assertEqual(len(JointObject.activeTask.refs), 2)
        self.finish(False)
        self.assertEqual(self.doc.UndoCount, 0)

    def test_viewport_escape_cancels_edit(self):
        self.openJoint(self.joints[0], tree=True)
        self.changeType()
        # Before this branch Escape in the viewport rejects the edit, restoring
        # the joint and its references without adding an undo entry.
        self.key(self.viewport(), QtCore.Qt.Key_Escape)
        Gui.updateGui()
        self.assertIsNone(JointObject.activeTask)
        self.assertFalse(Gui.Control.activeDialog())
        self.assertTypes("Fixed", "Fixed")
        self.assertEqual(self.doc.UndoCount, 0)

    def test_viewport_deselect_removes_reference(self):
        # Keep a single visible component so the overlapping fixture is unambiguous.
        for part in self.parts[1:]:
            part.Visibility = False
        self.openJoint(self.joints[0], tree=True)
        view = Gui.activeDocument().activeView()
        view.viewLeft()
        view.fitAll()
        Gui.updateGui()
        widget = self.viewport()
        # Face1 is the box's left face. GreedySelection toggles a clicked selected
        # reference off, as before this branch; Cancel must restore that change.
        point = view.getPointOnScreen(App.Vector(0, 5, 5))
        self.mouse(widget, QtCore.QPoint(int(point[0]), widget.height() - 1 - int(point[1])),
                   modifiers=QtCore.Qt.ControlModifier)
        Gui.updateGui()
        self.assertEqual(len(JointObject.activeTask.refs), 1)
        self.assertIsNone(self.joints[0].Reference2)
        self.finish(False)
        self.assertUnchanged()
        self.assertEqual(self.doc.UndoCount, 0)
