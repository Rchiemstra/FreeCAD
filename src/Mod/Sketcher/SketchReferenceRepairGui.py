# SPDX-License-Identifier: LGPL-2.1-or-later
"""Sketcher reference repair command and transient scene-only preview."""

import threading
import time

import FreeCAD as App
import FreeCADGui as Gui
from pivy import coin
from PySide import QtCore, QtGui
from SketchReferenceRepair import (
    RepairError,
    apply_prepared,
    broken_references,
    check_repair,
    iter_candidates,
    prepare_repair,
)


def tr(text):
    return App.Qt.translate("Sketcher_RepairReferences", text)


class Preview:
    """Highlight a candidate without changing selection, visibility or the document."""

    def __init__(self, view):
        self.scene = view.getSceneGraph()
        self.node = None

    def clear(self):
        if self.node is not None:
            self.scene.removeChild(self.node)
            self.node = None

    def close(self):
        self.clear()
        self.scene = None

    def show(self, source, subelement):
        self.clear()
        shape = source.Shape.getElement(subelement).copy()
        parent = source.getGlobalPlacement().multiply(source.Placement.inverse())
        shape.transformShape(parent.toMatrix())
        node = coin.SoSeparator()
        color = coin.SoBaseColor()
        color.rgb = (1.0, 0.65, 0.0)
        style = coin.SoDrawStyle()
        style.lineWidth = 5
        style.pointSize = 9
        node.addChild(color)
        node.addChild(style)
        if shape.ShapeType == "Vertex":
            coordinates = coin.SoCoordinate3()
            coordinates.point.setValues(0, 1, [tuple(shape.Point)])
            node.addChild(coordinates)
            node.addChild(coin.SoPointSet())
        else:
            points, counts = [], []
            for edge in shape.Edges:
                segment = edge.discretize(Number=48)
                points.extend(tuple(p) for p in segment)
                counts.append(len(segment))
            coordinates = coin.SoCoordinate3()
            coordinates.point.setValues(0, len(points), points)
            lines = coin.SoLineSet()
            lines.numVertices.setValues(0, len(counts), counts)
            node.addChild(coordinates)
            node.addChild(lines)
        self.scene.addChild(node)
        self.node = node


_operations = set()


class RepairCancellation:
    """Session lifetime shared with the worker and native transaction callbacks."""

    def __init__(self):
        self.cancelled = threading.Event()

    def cancel(self):
        self.cancelled.set()

    def check(self):
        if self.cancelled.is_set():
            raise RepairError(tr("Reference repair cancelled"))


class RepairOperation(QtCore.QObject):
    """Resolve/commit a repair off Qt, then admit its presentation callback."""

    completed = QtCore.Signal(str)

    def __init__(self, sketch, choices, callback, cancellation=None):
        super().__init__()
        self.cancellation = cancellation or RepairCancellation()
        self.document = sketch.Document
        self.document_identity = self.document.collaborationIdentity()
        self.name, self.object_id = sketch.Name, sketch.ID
        self.choices = choices
        self.callback = callback
        self.error = ""
        self.timer = QtCore.QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.timeout.connect(self.present)
        self.completed.connect(self.finish, QtCore.Qt.QueuedConnection)
        _operations.add(self)
        threading.Thread(
            target=self.run, name="sketch-reference-repair", daemon=True
        ).start()

    def run(self):
        try:
            self.cancellation.check()
            doc = self.document
            if doc.collaborationIdentity() != self.document_identity:
                raise RepairError("Document changed; reopen the repair command")
            sketch = doc.getObject(self.name)
            if sketch is None or sketch.ID != self.object_id:
                raise RepairError("Sketch changed; reopen the repair command")
            choices = self.choices() if callable(self.choices) else self.choices
            self.cancellation.check()
            prepared = prepare_repair(sketch, choices)
            self.cancellation.check()

            def mutate():
                self.cancellation.check()
                apply_prepared(sketch, prepared)

            def postcondition():
                valid = check_repair(sketch, prepared)
                # Native code commits immediately after this callback. A close
                # during validation/mutation must abort the entire transaction.
                self.cancellation.check()
                return valid

            result = doc.commitCompatibilityMutationAsync(
                mutate,
                object_name=self.name,
                label="Repair sketch references",
                postcondition=postcondition,
            )
            if callable(result.get("wait")):
                result = result["wait"]()
            if result.get("status") != "Committed":
                raise RepairError(result.get("message", str(result)))
            error = ""
        except BaseException as exc:  # noqa: BLE001 - deliver interrupts to the GUI too
            error = str(exc) or type(exc).__name__
        self.completed.emit(error)

    @QtCore.Slot(str)
    def finish(self, error):
        self.error = error
        self.present()

    def present(self):
        if self.cancellation.cancelled.is_set():
            self.release()
            return
        try:
            if not Gui.tryRunPresentationCallback(
                lambda: self.callback(self.error), self.document
            ):
                self.timer.start(10)
                return
        except (ReferenceError, RuntimeError):
            # Document close/deletion also dismisses the owning dialog.
            pass
        self.release()

    def release(self):
        self.callback = None
        self.document = None
        _operations.discard(self)
        self.deleteLater()


class RepairObserver(QtCore.QObject):
    removed = QtCore.Signal()

    def __init__(self, sketch, cancellation=None):
        super().__init__()
        self.cancellation = cancellation
        self.sketch = sketch
        self.document = sketch.Document

    def slotDeletedObject(self, obj):
        if obj is self.sketch:
            self.remove()

    def slotDeletedDocument(self, doc):
        if doc is self.document:
            self.remove()

    def remove(self):
        # App observers can run on the document owner before Qt receives the
        # close signal. Cancel synchronously without touching any GUI object.
        if self.cancellation is not None:
            self.cancellation.cancel()
        self.removed.emit()


class RepairDialog(QtGui.QDialog):
    def __init__(self, sketch):
        super().__init__(Gui.getMainWindow())
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.setWindowTitle(tr("Repair sketch references"))
        self.sketch = sketch
        self.document = sketch.Document
        self.cancellation = RepairCancellation()
        self.closed = False
        self.busy = False
        self.preview = Preview(Gui.getDocument(self.document.Name).activeView())
        self.observer = RepairObserver(sketch, self.cancellation)
        self.observer.removed.connect(self.close)
        App.addDocumentObserver(self.observer)
        self.rank_timer = QtCore.QTimer(self)
        self.rank_timer.setSingleShot(True)
        self.rank_timer.timeout.connect(self.rank_chunk)
        self.rank_iterator = None
        layout = QtGui.QVBoxLayout(self)
        layout.addWidget(
            QtGui.QLabel(tr("Select a broken reference and inspect a replacement."))
        )
        self.references = QtGui.QListWidget()
        self.options = QtGui.QListWidget()
        layout.addWidget(self.references)
        layout.addWidget(self.options)
        self.message = QtGui.QLabel()
        self.message.setWordWrap(True)
        layout.addWidget(self.message)
        buttons = QtGui.QDialogButtonBox(
            QtGui.QDialogButtonBox.Apply | QtGui.QDialogButtonBox.Close
        )
        self.apply_button = buttons.button(QtGui.QDialogButtonBox.Apply)
        self.apply_button.setText(tr("Repair selected reference"))
        self.apply_button.clicked.connect(self.apply)
        buttons.rejected.connect(self.close)
        layout.addWidget(buttons)
        self.references.currentRowChanged.connect(self.select_reference)
        self.options.currentRowChanged.connect(self.select_candidate)
        self.finished.connect(self.dispose)
        self.refresh()
        self.resize(620, 440)

    def refresh(self):
        self.preview.clear()
        self.refs = broken_references(self.sketch)
        self.references.clear()
        self.options.clear()
        self.ranked = []
        self.apply_button.setEnabled(False)
        for ref in self.refs:
            self.references.addItem(f"{ref.kind}: {ref.source}.{ref.subelement}")
        self.message.setText(tr("No broken references.") if not self.refs else "")
        if self.refs:
            self.references.setCurrentRow(0)

    def select_reference(self, row):
        self.rank_timer.stop()
        self.rank_iterator = None
        self.preview.clear()
        self.options.clear()
        self.ranked = []
        self.apply_button.setEnabled(False)
        if self.busy or row < 0 or row >= len(self.refs):
            return
        self.rank_iterator = iter_candidates(self.sketch, self.refs[row])
        self.rank_pending = []
        self.rank_timer.start(0)

    def rank_chunk(self):
        if self.closed or self.busy or self.rank_iterator is None:
            return
        try:
            deadline = time.monotonic() + 0.004
            while time.monotonic() < deadline:
                try:
                    self.rank_pending.append(next(self.rank_iterator))
                except StopIteration:
                    self.rank_iterator = None
                    break
            if self.rank_iterator is not None:
                self.rank_timer.start(0)
                return
            self.ranked = sorted(
                self.rank_pending, key=lambda c: (c.score, c.subelement)
            )
            for candidate in self.ranked:
                status = tr("compatible") if candidate.compatible else candidate.reason
                self.options.addItem(
                    f"{candidate.source}.{candidate.subelement}  ({candidate.score:.6g}) — {status}"
                )
            self.message.setText(
                tr("Equal compatible candidates are ambiguous and will be refused.")
                if self.ranked
                else tr(
                    "No replacement subelements are available on the source object."
                )
            )
        except (RepairError, RuntimeError, ReferenceError) as error:
            self.message.setText(str(error))

    def select_candidate(self, row):
        self.preview.clear()
        enabled = 0 <= row < len(self.ranked)
        self.apply_button.setEnabled(enabled and self.ranked[row].compatible)
        if enabled:
            selected = self.ranked[row]
            try:
                source = self.sketch.Document.getObject(selected.source)
                self.preview.show(source, selected.subelement)
            except (
                RuntimeError,
                ValueError,
                AttributeError,
                IndexError,
                ReferenceError,
            ):
                self.message.setText(
                    tr("Candidate changed; reopen the repair command.")
                )
                self.apply_button.setEnabled(False)

    def apply(self):
        ref_row, candidate_row = self.references.currentRow(), self.options.currentRow()
        if self.busy or ref_row < 0 or candidate_row < 0:
            return
        self.busy = True
        self.rank_timer.stop()
        self.preview.clear()
        self.apply_button.setEnabled(False)
        self.references.setEnabled(False)
        self.options.setEnabled(False)
        self.message.setText(tr("Repairing reference…"))
        RepairOperation(
            self.sketch,
            [(self.refs[ref_row], self.ranked[candidate_row])],
            self.applied,
            self.cancellation,
        )

    def applied(self, error):
        if self.closed:
            return
        self.busy = False
        self.references.setEnabled(True)
        self.options.setEnabled(True)
        if error:
            self.message.setText(error)
        else:
            self.refresh()

    def dispose(self, *args):
        if self.closed:
            return
        self.cancellation.cancel()
        self.closed = True
        self.rank_timer.stop()
        self.rank_iterator = None
        App.removeDocumentObserver(self.observer)
        self.preview.close()

    def closeEvent(self, event):
        self.dispose()
        super().closeEvent(event)


class RepairReferencesCommand:
    def GetResources(self):
        return {
            "MenuText": tr("Repair broken references…"),
            "ToolTip": tr("List, preview and repair missing sketch references"),
            "Pixmap": "Sketcher_External",
        }

    def IsActive(self):
        selected = Gui.Selection.getSelection()
        active = Gui.activeDocument()
        return (
            len(selected) == 1
            and active is not None
            and selected[0].Document is active.Document
            and selected[0].isDerivedFrom("Sketcher::SketchObject")
        )

    def Activated(self):
        if not self.IsActive():
            return
        self.dialog = RepairDialog(Gui.Selection.getSelection()[0])
        self.dialog.show()


command = RepairReferencesCommand()
Gui.addCommand("Sketcher_RepairReferences", command)


class RepairMenu:
    """Extend the native C++ workbench without editing its menu implementation."""

    def modifyContextMenu(self, recipient):
        from SketchReferenceReassignGui import command

        if recipient == "Sketch" and command.IsActive():
            return [{"insert": "Sketcher_ReassignBrokenReference", "menuItem": "Std_Delete"}]
        return []

    def modifyMenuBar(self):
        return [
            {
                "append": "Sketcher_RepairReferences",
                "menuItem": "Sketcher_ValidateSketch",
            }
        ]


menu_manipulator = RepairMenu()
Gui.addWorkbenchManipulator(menu_manipulator)

# Register the in-view command after the shared dialog/operation are available.
import SketchReferenceReassignGui  # noqa: E402,F401
