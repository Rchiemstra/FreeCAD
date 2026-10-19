# SPDX-License-Identifier: LGPL-2.1-or-later
"""Transient in-edit picking for a single hanging external reference."""

import FreeCAD as App
import FreeCADGui as Gui
from pivy import coin
from PySide import QtCore, QtGui

from SketchReferenceRepair import RepairError, broken_references, picked_candidate
from SketchReferenceRepairGui import RepairDialog, RepairObserver, RepairOperation, tr


def selected_reference():
    """Resolve exactly one selected cached edge/vertex in the edited sketch."""
    document = Gui.activeDocument()
    edit = document.getInEdit() if document else None
    if edit is None or not edit.Object.isDerivedFrom("Sketcher::SketchObject"):
        return None
    sketch = edit.Object
    selected = Gui.Selection.getSelectionEx()
    if len(selected) != 1 or selected[0].Object is not sketch:
        return None
    names = selected[0].SubElementNames
    if len(names) != 1:
        return None
    name = names[0]
    try:
        if name.startswith("ExternalEdge"):
            geo_id = -int(name[len("ExternalEdge") :]) - 2
        elif name.startswith("Vertex"):
            geo_id, _position = sketch.getGeoVertexIndex(int(name[6:]) - 1)
        else:
            return None
    except (ValueError, IndexError, RuntimeError):
        return None
    reference = next(
        (
            ref
            for ref in broken_references(sketch)
            if ref.kind == "external" and ref.geo_id == geo_id
        ),
        None,
    )
    return (sketch, reference) if reference else None


class PickSession(QtGui.QDialog):
    """Consume picking events before Sketcher's edit handler can drag geometry.

    Transparency is a scene-only override. No view property, visibility,
    selection gate, edit transaction or constraint is changed while choosing.
    """

    def __init__(self, owner, sketch, reference):
        super().__init__(Gui.getMainWindow(), QtCore.Qt.Tool)
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.owner, self.sketch, self.reference = owner, sketch, reference
        self.document = sketch.Document
        self.gui_document = Gui.getDocument(self.document.Name)
        self.view = self.gui_document.activeView()
        self.viewport = self.view.graphicsView().viewport()
        self.closed = self.busy = False
        self.setWindowTitle(tr("Reassign broken reference"))
        layout = QtGui.QVBoxLayout(self)
        self.message = QtGui.QLabel(
            tr("Pick a replacement edge or vertex in the 3D view. Esc cancels.")
        )
        self.message.setWordWrap(True)
        layout.addWidget(self.message)
        self.fallback_button = QtGui.QPushButton(tr("Ranked candidates…"))
        self.fallback_button.clicked.connect(self.fallback)
        layout.addWidget(self.fallback_button)
        self.cancel_button = QtGui.QPushButton(tr("Cancel"))
        self.cancel_button.clicked.connect(self.close)
        layout.addWidget(self.cancel_button)
        # Sketcher also renders its edit separator directly in the viewer.
        # Override that subtree without changing an undoable view property.
        search = coin.SoSearchAction()
        search.setName("Sketch_EditRoot")
        search.setSearchingAll(True)
        search.apply(self.view.getSceneGraph())
        path = search.getPath()
        if path is None:
            raise RepairError("Sketch edit scene is unavailable; reopen the sketch")
        self.root = path.getTail()
        # getTail() is borrowed; leaving edit mode releases the native owner
        # before our lifecycle timer dismisses the picker.
        self.root.ref()
        self.material = coin.SoMaterial()
        self.material.setName("ReferenceRepairTransparency")
        for field in (
            "ambientColor",
            "diffuseColor",
            "specularColor",
            "emissiveColor",
            "shininess",
        ):
            getattr(self.material, field).setIgnored(True)
        self.material.transparency = 0.65
        self.material.setOverride(True)
        self.root.insertChild(self.material, 0)
        self.observer = RepairObserver(sketch)
        self.observer.removed.connect(self.close)
        App.addDocumentObserver(self.observer)
        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.check_edit)
        self.timer.start(100)
        self.application = QtGui.QApplication.instance()
        self.application.installEventFilter(self)
        self.finished.connect(self.dispose)
        self.show()

    def check_edit(self):
        try:
            edit = self.gui_document.getInEdit()
            if (
                Gui.activeDocument() is not self.gui_document
                or edit is None
                or edit.Object is not self.sketch
            ):
                self.close()
        except (RuntimeError, ReferenceError):
            self.close()

    def eventFilter(self, watched, event):
        if self.closed:
            return False
        kind = event.type()
        if kind in (QtCore.QEvent.KeyPress, QtCore.QEvent.KeyRelease):
            if event.key() == QtCore.Qt.Key_Escape:
                if kind == QtCore.QEvent.KeyPress and not self.busy:
                    self.close()
                return True
        if watched is not self.viewport:
            return False
        if kind in (
            QtCore.QEvent.MouseButtonPress,
            QtCore.QEvent.MouseButtonRelease,
            QtCore.QEvent.MouseButtonDblClick,
        ):
            if event.button() == QtCore.Qt.LeftButton:
                if kind == QtCore.QEvent.MouseButtonRelease and not self.busy:
                    # Qt viewport positions use logical pixels with a top-left origin;
                    # Coin uses physical pixels with a bottom-left origin.
                    ratio = self.viewport.devicePixelRatioF()
                    pos = event.pos()
                    point = (
                        round(pos.x() * ratio),
                        self.view.getSize()[1] - 1 - round(pos.y() * ratio),
                    )
                    self.pick(point)
                return True
            if event.button() == QtCore.Qt.RightButton:
                if kind == QtCore.QEvent.MouseButtonRelease and not self.busy:
                    self.close()
                return True
        # Avoid Sketcher preselection/drag state changes while choosing. Wheel and
        # middle-button navigation remain available.
        if kind == QtCore.QEvent.MouseMove and not event.buttons():
            return True
        return False

    def pick(self, point):
        self.check_edit()
        if self.closed or self.busy:
            return
        hits = self.view.getObjectsInfo(point) or []
        expected = self.sketch.getExternalReferenceType(self.reference.geo_id)
        for hit in hits:
            if hit.get("Document") != self.document.Name:
                continue
            source = self.document.getObject(hit.get("Object", ""))
            subelement = hit.get("Component", "")
            if source is None or source is self.sketch:
                continue
            if not subelement.startswith(expected):
                continue
            # Resolve and validate the source in the same worker used by the
            # dialog. Exporting a large BRep for its revision stamp can be slow.
            sketch, reference = self.sketch, self.reference

            def choices():
                return [
                    (reference, picked_candidate(sketch, reference, source, subelement))
                ]

            self.busy = True
            self.fallback_button.setEnabled(False)
            self.cancel_button.setEnabled(False)
            self.message.setText(tr("Repairing reference…"))
            RepairOperation(sketch, choices, self.applied)
            return
        self.message.setText(
            tr("No external edge or vertex here. Pick again or use ranked candidates.")
        )

    def applied(self, error):
        if self.closed:
            return
        self.busy = False
        if error:
            self.fallback_button.setEnabled(True)
            self.cancel_button.setEnabled(True)
            self.message.setText(error)
        else:
            self.close()

    def fallback(self):
        if self.busy or self.closed:
            return
        self.close()
        self.owner.dialog = RepairDialog(self.sketch)
        dialog = self.owner.dialog
        for row, reference in enumerate(dialog.refs):
            if reference == self.reference:
                dialog.references.setCurrentRow(row)
                break
        dialog.show()

    def dispose(self, *args):
        if self.closed:
            return
        self.closed = True
        self.timer.stop()
        self.application.removeEventFilter(self)
        App.removeDocumentObserver(self.observer)
        if self.material is not None:
            if self.root.findChild(self.material) >= 0:
                self.root.removeChild(self.material)
            self.material = None
        self.root.unref()
        self.root = None

    def closeEvent(self, event):
        self.dispose()
        super().closeEvent(event)


class ReassignBrokenReferenceCommand:
    session = None

    def GetResources(self):
        return {
            "MenuText": tr("Reassign broken reference"),
            "ToolTip": tr("Pick replacement geometry while preserving constraints"),
            "Pixmap": "Sketcher_External",
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return (
            self.session is None or self.session.closed
        ) and selected_reference() is not None

    def Activated(self):
        if not self.IsActive():
            return
        sketch, reference = selected_reference()
        self.session = PickSession(self, sketch, reference)


command = ReassignBrokenReferenceCommand()
Gui.addCommand("Sketcher_ReassignBrokenReference", command)
