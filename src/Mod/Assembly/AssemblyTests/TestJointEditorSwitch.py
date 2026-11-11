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
        # A failed button click leaves the editor open. Each step must run even
        # when an earlier one raises, or the next test inherits that dialog.
        try:
            if JointObject.activeTask is not None:
                JointObject.activeTask.reject()
        finally:
            try:
                if Gui.Control.activeDialog():
                    Gui.Control.closeDialog()
            finally:
                try:
                    self.gui_doc.resetEdit()
                finally:
                    try:
                        App.closeDocument(self.doc.Name)
                    finally:
                        Gui.updateGui()

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
        # showDialog leaves OK/Cancel enabled under a hidden OverlayRight until
        # OverlayManager's debounced layout timer runs (DockOverlayDelay, 200ms).
        # Context-menu input restarts that timer, so an immediate isVisible()
        # lookup raises StopIteration. Re-fetch the button after every pump and
        # click it before the next one; a box kept across the pump is deleted
        # when the panel is replaced.
        role = QtWidgets.QDialogButtonBox.Ok if accept else QtWidgets.QDialogButtonBox.Cancel
        elapsed = QtCore.QElapsedTimer()
        elapsed.start()
        while True:
            clicked = False
            for box in Gui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
                button = box.button(role)
                if button is not None and button.isVisible():
                    self.mouse(button, button.rect().center())
                    clicked = True
                    break
            # Drop the wrappers before pumping. The panel is destroyed on the
            # next event-loop turn when the edited joint changes.
            box = None
            button = None
            if clicked:
                break
            if elapsed.elapsed() >= 5000:
                self.fail(self.missingButton(role))
            loop = QtCore.QEventLoop()
            QtCore.QTimer.singleShot(10, loop.quit)
            loop.exec_()
        Gui.updateGui()

    def missingButton(self, role):
        task = JointObject.activeTask
        joint = task.joint.Name if task is not None else None
        boxes = []
        for box in Gui.getMainWindow().findChildren(QtWidgets.QDialogButtonBox):
            hidden = None
            widget = box
            while widget is not None:
                if hidden is None and (widget.isHidden() or not widget.isVisible()):
                    hidden = (widget.metaObject().className(), widget.objectName(),
                              widget.isVisible(), widget.isHidden())
                widget = widget.parentWidget()
            boxes.append({
                "hidden_ancestor": hidden,
                "buttons": [(button.text(), button.isVisible(), button.isEnabled())
                            for button in box.buttons()],
            })
        roleName = "OK" if role == QtWidgets.QDialogButtonBox.Ok else "Cancel"
        return (
            "No visible " + roleName + " button for the joint editor after 5s"
            + "; joint=" + repr(joint)
            + "; dialog=" + repr(Gui.Control.activeTaskDialog())
            + "; button_boxes=" + repr(boxes)
        )

    def openJoint(self, joint, tree, expected=None):
        if not tree:
            # TreeWidget synchronizes the connector selection on a deferred
            # timer. Let it finish before making a fresh, single-item selection.
            timeout = App.ParamGet("User parameter:BaseApp/Preferences/TreeView").GetInt(
                "SelectionTimeout", 100
            )
            loop = QtCore.QEventLoop()
            QtCore.QTimer.singleShot(max(1, timeout) + 20, loop.quit)
            loop.exec_()
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
            Gui.updateGui()
            # Selecting B can rebuild/expand tree rows through selection observers.
            item = widget.findItems(
                joint.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
            )[0]
            widget.scrollToItem(item)
            rect = widget.visualItemRect(item)
            point = QtCore.QPoint(rect.right() - 10, rect.center().y())
            self.mouse(widget.viewport(), point, QtCore.Qt.RightButton)
            Gui.updateGui()
            item = widget.findItems(
                joint.Label, QtCore.Qt.MatchExactly | QtCore.Qt.MatchRecursive
            )[0]
            rect = widget.visualItemRect(item)
            point = QtCore.QPoint(rect.right() - 10, rect.center().y())
            self.assertEqual([item.text(0) for item in widget.selectedItems()], [joint.Label])
            triggered = []
            offered = []
            attempts = []
            # A singleShot retry still queued when QMenu.exec returns clicks or
            # closes a menu belonging to a later test. Stop it on every exit.
            pending = {"active": True}
            chooseTimer = QtCore.QTimer()
            chooseTimer.setSingleShot(True)

            def chooseEdit():
                if not pending["active"]:
                    return
                menus = [w for w in QtWidgets.QApplication.topLevelWidgets()
                         if isinstance(w, QtWidgets.QMenu) and w.isVisible()]
                if not menus and len(attempts) < 50:
                    attempts.append(None)
                    chooseTimer.start(10)
                    return
                for menu in menus:
                    for action in menu.actions():
                        text = action.text().replace("&", "")
                        offered.append(text)
                        if text == App.Qt.translate("Assembly", "Edit Joint"):
                            action.triggered.connect(
                                lambda checked=False, text=text: triggered.append(text)
                            )
                            self.mouse(menu, menu.actionGeometry(action).center())
                            menu.close()
                            return
                for menu in menus:
                    menu.close()

            chooseTimer.timeout.connect(chooseEdit)
            # contextMenuEvent enters QMenu.exec(); choose its real action there.
            chooseTimer.start(0)
            event = QtGui.QContextMenuEvent(
                QtGui.QContextMenuEvent.Mouse, point, widget.viewport().mapToGlobal(point)
            )
            try:
                QtWidgets.QApplication.sendEvent(widget.viewport(), event)
            finally:
                pending["active"] = False
                chooseTimer.stop()
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
        # UndoCount includes the active transaction: one finished A edit and
        # B's pending edit (which has temporarily made B visible).
        self.assertEqual(self.doc.UndoCount, 2, "One finished edit plus B's pending edit")
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

    def referenceRowTarget(self, row):
        # OverlayManager's app event filter retargets a mouse event when
        # QCursor is on a transparent OverlayTabWidget (the dock is still
        # sliding in). The feature row then never becomes selected. A hidden
        # dock is safe: the cursor is not on an overlay, so the event reaches
        # the viewport. Re-fetch every attempt; do not keep the widgets.
        featureList = JointObject.activeTask.jForm.featureList
        item = featureList.item(row)
        viewport = featureList.viewport()
        point = featureList.visualItemRect(item).center() if item is not None else None
        chain = []
        hitList = False
        hitOverlay = False
        if point is not None:
            widget = QtWidgets.QApplication.widgetAt(viewport.mapToGlobal(point))
            while widget is not None:
                chain.append((widget.metaObject().className(), widget.objectName()))
                if widget.objectName() == "featureList" or (
                    widget.objectName() == "qt_scrollarea_viewport"
                    and widget.parentWidget() is not None
                    and widget.parentWidget().objectName() == "featureList"
                ):
                    hitList = True
                if widget.metaObject().className() == "Gui::OverlayTabWidget":
                    hitOverlay = True
                widget = widget.parentWidget()
        ready = item is not None and (hitList or not hitOverlay)
        return ready, viewport, point, chain, featureList

    def clickReferenceRow(self, row, modifiers=QtCore.Qt.NoModifier):
        elapsed = QtCore.QElapsedTimer()
        elapsed.start()
        chain = []
        featureList = None
        while True:
            ready, viewport, point, chain, featureList = self.referenceRowTarget(row)
            if ready:
                self.mouse(viewport, point, modifiers=modifiers)
                return
            size = None
            if featureList is not None:
                size = (featureList.width(), featureList.height(), featureList.isVisible())
            viewport = None
            featureList = None
            if elapsed.elapsed() >= 5000:
                self.fail(
                    "Feature row "
                    + str(row)
                    + " is covered by a transparent overlay after 5s; list="
                    + repr(size)
                    + "; widgetAt="
                    + repr(chain)
                )
            loop = QtCore.QEventLoop()
            QtCore.QTimer.singleShot(10, loop.quit)
            loop.exec_()

    def deleteReferences(self, multiple=False):
        self.clickReferenceRow(0)
        if multiple:
            self.clickReferenceRow(1, modifiers=QtCore.Qt.ControlModifier)
            featureList = JointObject.activeTask.jForm.featureList
            self.assertEqual(len(featureList.selectedIndexes()), 2)
        self.key(JointObject.activeTask.jForm.featureList, QtCore.Qt.Key_Delete)
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
        if tree:
            self.finish(False)
        else:
            # The context menu leaves the model tree raised. The viewport's
            # existing Escape shortcut invokes the same Cancel/reject path.
            self.key(self.viewport(), QtCore.Qt.Key_Escape)
            Gui.updateGui()
        self.assertFalse(Gui.Control.activeDialog())
        self.assertTypes("Fixed", "Fixed")
        for joint, properties in zip(self.joints, self.jointProperties):
            self.assertEqual(self.properties(joint), properties)
        self.assertEqual(pendingUndoCount, 1, "Only A's edit is available, still pending")
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
        view.setAnimationEnabled(False)
        view.viewLeft()
        view.fitAll()
        Gui.updateGui()
        widget = self.viewport()
        # Face1 is the box's left face. GreedySelection toggles a clicked selected
        # reference off, as before this branch; Cancel must restore that change.
        point = view.getPointOnScreen(App.Vector(0, 5, 5))
        picked = view.getObjectInfo((int(point[0]), int(point[1])))
        self.assertIsNotNone(picked, "The deselection click must hit the component")
        self.mouse(widget, QtCore.QPoint(int(point[0]), widget.height() - 1 - int(point[1])),
                   modifiers=QtCore.Qt.ControlModifier)
        Gui.updateGui()
        self.assertEqual(len(JointObject.activeTask.refs), 1, repr(picked))
        self.assertIsNone(self.joints[0].Reference2)
        self.finish(False)
        self.assertUnchanged()
        self.assertEqual(self.doc.UndoCount, 0)
