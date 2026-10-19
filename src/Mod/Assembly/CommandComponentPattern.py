# SPDX-License-Identifier: LGPL-2.1-or-later
"""Assembly toolbar command and task panel for associative component patterns."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

import UtilsAssembly
from ComponentPattern import createComponentPattern, _PATTERN_TYPES

translate = App.Qt.translate
activeTask = None


def _assembly():
    assembly = UtilsAssembly.activeAssembly()
    if assembly:
        return assembly
    doc = App.ActiveDocument
    if doc:
        assemblies = [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"]
        if len(assemblies) == 1:
            return assemblies[0]
    return None


class CommandComponentPattern:
    def GetResources(self):
        return {
            "Pixmap": "Part_PolarLinkArray",
            "MenuText": QtCore.QT_TRANSLATE_NOOP("Assembly", "Pattern component"),
            "ToolTip": QtCore.QT_TRANSLATE_NOOP(
                "Assembly",
                "Pattern a component using a PartDesign pattern or explicit parameters",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return _assembly() is not None and not Gui.Control.activeDialog()

    def Activated(self):
        global activeTask
        assembly = _assembly()
        if assembly is None:
            return
        activeTask = TaskComponentPattern(assembly)
        dialog = Gui.Control.showDialog(activeTask)
        if dialog is not None:
            dialog.setAutoCloseOnDeletedDocument(True)
            dialog.setDocumentName(assembly.Document.Name)


class TaskComponentPattern:
    def __init__(self, assembly):
        self.assembly = assembly
        self._pending = False
        self._rejecting = False
        self._timer = QtCore.QTimer()
        self._timer.setInterval(10)
        self._timer.timeout.connect(self._recomputed)
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Pattern component"))
        layout = QtWidgets.QFormLayout(self.form)
        self.components = [
            o
            for o in UtilsAssembly.getMovablePartsWithin(assembly)
            if UtilsAssembly.isLink(o) and not UtilsAssembly.isLinkArray(o)
        ]
        self.sources = [None] + [
            o
            for doc in App.listDocuments().values()
            for o in doc.Objects
            if o.TypeId in _PATTERN_TYPES
        ]
        self.component = QtWidgets.QComboBox()
        self.source = QtWidgets.QComboBox()
        self.owner = QtWidgets.QComboBox()
        self.component.addItems([o.Label for o in self.components])
        self.source.addItems(
            [translate("Assembly", "Explicit parameters")]
            + [o.Document.Label + ": " + o.Label for o in self.sources[1:]]
        )
        self.owner.addItems(
            [translate("Assembly", "Detect automatically")]
            + [o.Label for o in self.components]
        )
        layout.addRow(translate("Assembly", "Component"), self.component)
        layout.addRow(translate("Assembly", "Pattern source"), self.source)
        layout.addRow(translate("Assembly", "Patterned component"), self.owner)
        self.parameters = QtWidgets.QWidget()
        fields = QtWidgets.QFormLayout(self.parameters)
        fields.setContentsMargins(0, 0, 0, 0)
        self.kind = QtWidgets.QComboBox()
        self.kind.addItem(translate("Assembly", "Polar"), "Polar")
        self.kind.addItem(translate("Assembly", "Linear"), "Linear")
        self.count = QtWidgets.QSpinBox()
        self.count.setRange(1, 10000)
        self.count.setValue(6)
        self.angle = QtWidgets.QDoubleSpinBox()
        self.angle.setRange(-36000, 36000)
        self.angle.setValue(360)
        self.angle.setSuffix(" °")
        self.length = QtWidgets.QDoubleSpinBox()
        self.length.setRange(0, 1e9)
        self.length.setValue(100)
        self.length.setSuffix(" mm")
        self.center = QtWidgets.QLineEdit("0, 0, 0")
        self.axis = QtWidgets.QLineEdit("0, 0, 1")
        self.direction = QtWidgets.QLineEdit("1, 0, 0")
        for label, widget in (
            ("Type", self.kind),
            ("Count", self.count),
            ("Angle", self.angle),
            ("Length", self.length),
            ("Center (x, y, z)", self.center),
            ("Axis (x, y, z)", self.axis),
            ("Direction (x, y, z)", self.direction),
        ):
            fields.addRow(translate("Assembly", label), widget)
        layout.addRow(self.parameters)
        self.joints = QtWidgets.QListWidget()
        layout.addRow(translate("Assembly", "Copy selected joints"), self.joints)
        self.error = QtWidgets.QLabel()
        self.error.setWordWrap(True)
        layout.addRow(self.error)
        selected = Gui.Selection.getSelection()
        for o in selected:
            if o in self.components:
                self.component.setCurrentIndex(self.components.index(o))
            if o in self.sources:
                self.source.setCurrentIndex(self.sources.index(o))
        self.component.currentIndexChanged.connect(self._jointsChanged)
        self.source.currentIndexChanged.connect(self._sourceChanged)
        self._jointsChanged()
        self._sourceChanged()

    def _sourceChanged(self, *args):
        explicit = self.source.currentIndex() == 0
        self.parameters.setEnabled(explicit)
        self.owner.setEnabled(not explicit)

    def _jointsChanged(self, *args):
        self.joints.clear()
        self.availableJoints = []
        if self.component.currentIndex() < 0:
            return
        component = self.components[self.component.currentIndex()]
        for joint in UtilsAssembly.getJointGroup(self.assembly).Group:
            if hasattr(joint, "Reference1") and component in (
                UtilsAssembly.getMovingPart(joint.Reference1),
                UtilsAssembly.getMovingPart(joint.Reference2),
            ):
                self.availableJoints.append(joint)
                item = QtWidgets.QListWidgetItem(joint.Label, self.joints)
                item.setFlags(item.flags() | QtCore.Qt.ItemIsUserCheckable)
                item.setCheckState(QtCore.Qt.Unchecked)

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel

    def accept(self):
        if self._pending:
            return False
        doc = self.assembly.Document
        if self.component.currentIndex() < 0:
            self.error.setText(translate("Assembly", "Choose a component link"))
            return False
        doc.openTransaction(translate("Assembly", "Pattern component"))
        try:
            vectors = {}
            if self.source.currentIndex() == 0:
                for prop, widget in (
                    ("center", self.center),
                    ("axis", self.axis),
                    ("direction", self.direction),
                ):
                    try:
                        values = [float(v.strip()) for v in widget.text().split(",")]
                    except ValueError:
                        values = []
                    if len(values) != 3:
                        raise ValueError(
                            translate(
                                "Assembly",
                                "Vectors require three comma-separated numbers",
                            )
                        )
                    vectors[prop] = App.Vector(*values)
            self.pattern = createComponentPattern(
                self.assembly,
                self.components[self.component.currentIndex()],
                self.sources[self.source.currentIndex()],
                patternComponent=(
                    self.components[self.owner.currentIndex() - 1]
                    if self.owner.currentIndex()
                    else None
                ),
                joints=[
                    j
                    for i, j in enumerate(self.availableJoints)
                    if self.joints.item(i).checkState() == QtCore.Qt.Checked
                ],
                count=self.count.value(),
                angle=self.angle.value(),
                length=self.length.value(),
                patternType=self.kind.currentData(),
                recompute=False,
                **vectors,
            )
            self.recomputeHandle = doc.recomputeAsync()
            self._pending = True
            self.form.setEnabled(False)
            self._timer.start()
        except Exception as exc:
            doc.abortTransaction()
            self.error.setText(str(exc))
            return False
        # TaskView closes the panel when accept returns True. Keep it alive until
        # the timer observes completion and the complete transaction is committed.
        return False

    def _recomputed(self):
        global activeTask
        if not self._pending or not self.recomputeHandle.done():
            return
        self._timer.stop()
        self._pending = False
        self.form.setEnabled(True)
        doc = self.assembly.Document
        status = self.recomputeHandle.status()
        if (
            self._rejecting
            or status["state"] != "completed"
            or "Invalid" in self.pattern.State
        ):
            doc.abortTransaction()
            self.error.setText(
                translate("Assembly", "Component pattern could not be recomputed")
                + ": "
                + status.get("diagnostic", "")
            )
            if self._rejecting:
                activeTask = None
                Gui.Control.closeDialog()
            return
        doc.commitTransaction()
        activeTask = None
        Gui.Control.closeDialog()

    def reject(self):
        global activeTask
        if self._pending:
            self._rejecting = True
            self.recomputeHandle.cancel()
            return False
        activeTask = None
        Gui.Control.closeDialog()
        return True


Gui.addCommand("Assembly_ComponentPattern", CommandComponentPattern())
