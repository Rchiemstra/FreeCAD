# SPDX-License-Identifier: LGPL-2.1-or-later
"""Sketcher reference repair command and transient scene-only preview."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui
from pivy import coin

from SketchReferenceRepair import RepairError, broken_references, candidates, repair


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


class RepairDialog(QtGui.QDialog):
    def __init__(self, sketch):
        super().__init__(Gui.getMainWindow())
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.setWindowTitle(tr("Repair sketch references"))
        self.sketch = sketch
        self.preview = Preview(Gui.activeDocument().activeView())
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
        self.finished.connect(lambda _: self.preview.close())
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
        self.preview.clear()
        self.options.clear()
        self.ranked = []
        self.apply_button.setEnabled(False)
        if row < 0 or row >= len(self.refs):
            return
        try:
            self.ranked = candidates(self.sketch, self.refs[row])
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
        if ref_row < 0 or candidate_row < 0:
            return
        try:
            repair(self.sketch, [(self.refs[ref_row], self.ranked[candidate_row])])
        except (RepairError, RuntimeError, ReferenceError) as error:
            self.preview.clear()
            self.message.setText(str(error))
            return
        self.refresh()

    def closeEvent(self, event):
        self.preview.close()
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
        return len(selected) == 1 and selected[0].isDerivedFrom(
            "Sketcher::SketchObject"
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

    def modifyMenuBar(self):
        return [
            {
                "append": "Sketcher_RepairReferences",
                "menuItem": "Sketcher_ValidateSketch",
            }
        ]


menu_manipulator = RepairMenu()
Gui.addWorkbenchManipulator(menu_manipulator)
