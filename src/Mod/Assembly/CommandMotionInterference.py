# SPDX-License-Identifier: LGPL-2.1-or-later

"""Task-panel entry point for bounded, sampled joint interference sweeps."""

from concurrent.futures import ThreadPoolExecutor

import FreeCAD as App
import FreeCADGui as Gui
import Assembly
import AssemblyApp
import AssemblyGui
import UtilsAssembly
from PySide import QtCore, QtWidgets
from PySide.QtCore import QT_TRANSLATE_NOOP

translate = App.Qt.translate


def _value(number):
    text = f"{number:.6f}".rstrip("0").rstrip(".")
    return text if "." in text else text + ".0"


def _joints(assembly):
    return [joint for joint in UtilsAssembly.getJointsOfType(assembly, ["Revolute", "Slider"])
            if not getattr(joint, "Suppressed", False)]


class CommandMotionInterference:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_CheckInterference",
            "CmdType": "ForEdit",
            "MenuText": QT_TRANSLATE_NOOP("Assembly", "Check Interference through Motion"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly", "Sample two components through a bounded revolute or slider movement"),
        }

    def IsActive(self):
        assembly = UtilsAssembly.activeAssembly()
        return bool(assembly and not Gui.Control.activeDialog() and _joints(assembly))

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly or Gui.Control.activeDialog():
            return
        self.panel = MotionInterferenceTask(assembly)
        dialog = Gui.Control.showDialog(self.panel)
        if dialog is not None:
            dialog.setDocumentName(assembly.Document.Name)
            dialog.setAutoCloseOnDeletedDocument(True)


class MotionInterferenceTask:
    def __init__(self, assembly):
        self.assembly = assembly
        self.document = assembly.Document
        self.joints = _joints(assembly)
        self.components = [obj for obj in assembly.Group
                           if "Placement" in obj.PropertiesList
                           and (hasattr(obj, "Shape") or obj.isDerivedFrom("App::Part")
                                or obj.isDerivedFrom("App::Link"))]
        self.running = self.replaying = self._owned = self._closePending = False
        self._closed = self._stale = False
        self._replayLoop = None
        self._restorePending = False
        self.result = None
        self.events = []
        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Interference through Motion"))
        layout = QtWidgets.QVBoxLayout(self.form)
        explanation = QtWidgets.QLabel(translate(
            "Assembly", "Choose one revolute or slider joint and two components. "
            "Results use sampled + refined poses; narrow collisions between samples can be missed."))
        explanation.setWordWrap(True)
        layout.addWidget(explanation)
        self.inputs = QtWidgets.QWidget()
        fields = QtWidgets.QFormLayout(self.inputs)
        fields.setRowWrapPolicy(QtWidgets.QFormLayout.WrapLongRows)
        self.jointPicker = QtWidgets.QComboBox()
        self.firstPicker = QtWidgets.QComboBox()
        self.secondPicker = QtWidgets.QComboBox()
        for joint in self.joints:
            self.jointPicker.addItem(joint.Label)
        for component in self.components:
            self.firstPicker.addItem(component.Label)
            self.secondPicker.addItem(component.Label)
        self.secondPicker.setCurrentIndex(min(1, len(self.components) - 1))
        fields.addRow(translate("Assembly", "Joint"), self.jointPicker)
        fields.addRow(translate("Assembly", "First component"), self.firstPicker)
        fields.addRow(translate("Assembly", "Second component"), self.secondPicker)
        self.startField = QtWidgets.QLineEdit()
        self.endField = QtWidgets.QLineEdit()
        self.stepField = QtWidgets.QLineEdit("1")
        self.startLabel = QtWidgets.QLabel()
        self.endLabel = QtWidgets.QLabel()
        self.stepLabel = QtWidgets.QLabel()
        fields.addRow(self.startLabel, self.startField)
        fields.addRow(self.endLabel, self.endField)
        fields.addRow(self.stepLabel, self.stepField)
        self.clearanceField = QtWidgets.QLineEdit()
        self.clearanceField.setPlaceholderText(translate("Assembly", "Use assembly clearance"))
        fields.addRow(translate("Assembly", "Required clearance (mm, optional)"), self.clearanceField)
        self.maximumGapField = QtWidgets.QLineEdit()
        self.maximumGapField.setPlaceholderText(translate("Assembly", "Off"))
        fields.addRow(translate("Assembly", "Maximum gap (mm, optional)"), self.maximumGapField)
        self.maximumGapField.setToolTip(translate(
            "Assembly", "Upper bound on the components' shortest whole-solid separation"))
        layout.addWidget(self.inputs)
        buttons = QtWidgets.QHBoxLayout()
        self.runButton = QtWidgets.QPushButton(translate("Assembly", "Run"))
        self.cancelButton = QtWidgets.QPushButton(translate("Assembly", "Cancel"))
        self.cancelButton.setEnabled(False)
        buttons.addWidget(self.runButton)
        buttons.addWidget(self.cancelButton)
        layout.addLayout(buttons)
        self.progress = QtWidgets.QProgressBar()
        self.progress.setValue(0)
        layout.addWidget(self.progress)
        self.resultText = QtWidgets.QLabel(translate("Assembly", "Ready. Enter any missing bounds."))
        self.resultText.setWordWrap(True)
        self.resultText.setTextFormat(QtCore.Qt.PlainText)
        self.resultText.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        layout.addWidget(self.resultText)
        self.eventPicker = QtWidgets.QComboBox()
        self.eventPicker.setSizeAdjustPolicy(
            QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.eventPicker.setMinimumContentsLength(18)
        layout.addWidget(self.eventPicker)
        replay = QtWidgets.QHBoxLayout()
        self.showButton = QtWidgets.QPushButton(translate("Assembly", "Show"))
        self.restoreButton = QtWidgets.QPushButton(translate("Assembly", "Restore pose"))
        self.showButton.setEnabled(False)
        self.restoreButton.setEnabled(False)
        replay.addWidget(self.showButton)
        replay.addWidget(self.restoreButton)
        layout.addLayout(replay)
        self.jointPicker.currentIndexChanged.connect(self._jointChanged)
        self.runButton.clicked.connect(self.start)
        self.cancelButton.clicked.connect(self.cancel)
        self.showButton.clicked.connect(self.show)
        self.restoreButton.clicked.connect(self.restore)
        self._prefillSelection()
        self._jointChanged()
        for picker in (self.jointPicker, self.firstPicker, self.secondPicker):
            picker.currentIndexChanged.connect(self._settingsChanged)
        for field in (self.startField, self.endField, self.stepField, self.clearanceField,
                      self.maximumGapField):
            field.textChanged.connect(self._settingsChanged)
        App.addDocumentObserver(self)

    def _prefillSelection(self):
        selected = []
        for item in Gui.Selection.getSelectionEx("*", 0):
            for sub in item.SubElementNames or [""]:
                names = [item.Object.Name] + sub.split(".")
                for index, joint in enumerate(self.joints):
                    if joint.Name in names:
                        self.jointPicker.setCurrentIndex(index)
                for index, component in enumerate(self.components):
                    if component.Name in names and index not in selected:
                        selected.append(index)
                        break
        if selected:
            self.firstPicker.setCurrentIndex(selected[0])
        if len(selected) > 1:
            self.secondPicker.setCurrentIndex(selected[1])

    def _jointChanged(self, *args):
        if self.jointPicker.currentIndex() < 0:
            self.runButton.setEnabled(False)
            return
        joint = self.joints[self.jointPicker.currentIndex()]
        unit, prefix = ("deg", "Angle") if joint.JointType == "Revolute" else ("mm", "Length")
        self.unit = unit
        for field, label, suffix, text in (
                (self.startField, self.startLabel, "Min", "Start ({})"),
                (self.endField, self.endLabel, "Max", "End ({})")):
            label.setText(translate("Assembly", text).format(unit))
            enabled = getattr(joint, "Enable" + prefix + suffix)
            field.setText(str(getattr(joint, prefix + suffix).Value) if enabled else "")
            field.setPlaceholderText(translate("Assembly", "Explicit bound required"))
        self.stepLabel.setText(translate("Assembly", "Maximum step ({})").format(unit))
        self.stepField.setText("1")

    def _settingsChanged(self, *args):
        if self.result is not None:
            self._stale = True
            self.showButton.setEnabled(False)
            self.resultText.setText(translate("Assembly", "Sweep settings changed. Run again."))

    def getStandardButtons(self):
        return QtWidgets.QDialogButtonBox.Close

    def isAllowedAlterDocument(self):
        return False

    def isAllowedAlterSelection(self):
        return False

    def isAllowedAlterView(self):
        return True

    def start(self):
        if self.running or self.replaying or self._closed:
            return
        self.result = None
        self.events = []
        self.eventPicker.clear()
        self.showButton.setEnabled(False)
        self._stale = False
        self.token = Assembly.SweepCancellation()
        self.running = True
        self.inputs.setEnabled(False)
        self.runButton.setEnabled(False)
        self.cancelButton.setEnabled(True)
        self.progress.setRange(0, 0)
        self.resultText.setText(translate("Assembly", "Preparing movement…"))
        QtCore.QTimer.singleShot(0, self._run)

    def _scan(self, *args, **kwargs):
        # Document access stops here. The worker receives only owned geometry,
        # rules and a cancellation flag, just like the static checker worker.
        prepared = AssemblyApp._prepareSelectedPairScan(*args, **kwargs)
        future = self._executor.submit(AssemblyApp._runPreparedInterferenceScan, prepared)
        loop = QtCore.QEventLoop()
        timer = QtCore.QTimer()
        timer.setInterval(20)
        timer.timeout.connect(lambda: loop.quit() if future.done() else None)
        timer.start()
        self._owned = False
        try:
            if not future.done():
                loop.exec_()
            return future.result()
        finally:
            self._owned = True
            timer.stop()

    def _progress(self, sample):
        if sample["phase"] == "refinement":
            self.progress.setRange(0, 0)
        else:
            self.progress.setRange(0, self._sampleCount)
        self.progress.setValue(sample["index"] + 1)
        self.resultText.setText(translate("Assembly", "Checked {0} pose {1} {2}.").format(
            sample["phase"], _value(sample["coordinate"]), self.unit))
        self._owned = False
        try:
            QtWidgets.QApplication.processEvents()
        finally:
            self._owned = True

    def _run(self):
        from Assembly.MotionInterference import _interval

        self._owned = True
        try:
            joint = self.joints[self.jointPicker.currentIndex()]
            first = self.components[self.firstPicker.currentIndex()]
            second = self.components[self.secondPicker.currentIndex()]
            interval = tuple(field.text().strip() or None
                             for field in (self.startField, self.endField))
            # Bare numbers use the units shown beside the fields.
            def quantity(text):
                try:
                    return float(text)
                except ValueError:
                    return App.Units.Quantity(text)

            interval = tuple(quantity(value) if value is not None else None
                             for value in interval)
            step = quantity(self.stepField.text())
            _, _, count, self.unit = _interval(joint, interval, step)
            self._sampleCount = count + 1
            clearance = self.clearanceField.text().strip()
            clearance = quantity(clearance) if clearance else None
            maximum_gap = self.maximumGapField.text().strip()
            maximum_gap = quantity(maximum_gap) if maximum_gap else None
            self._labels = (first.Label, second.Label)
            with ThreadPoolExecutor(max_workers=1) as self._executor:
                self.result = Assembly.sweepJoint(
                    self.assembly, joint, first, second, interval=interval, step=step,
                    clearance=clearance, maximumGap=maximum_gap,
                    cancel=self.token, progress=self._progress,
                    _scan=self._scan)
            self._displayResult()
        except Exception as error:
            self.resultText.setText(translate("Assembly", "Cannot run sweep: {}. Original pose restored.")
                                    .format(error))
        finally:
            self._owned = self.running = False
            self.inputs.setEnabled(True)
            self.runButton.setEnabled(True)
            self.cancelButton.setEnabled(False)
            if self.progress.maximum() == 0:
                self.progress.setRange(0, 1)
                self.progress.setValue(0)
            if self._stale:
                self._invalidate()
            if self._closePending:
                self._close()

    def _displayResult(self):
        result = self.result
        sentences = []
        if result["completeness"] != "complete":
            sentences.append(translate("Assembly", "Sweep {0}: {1}. Original pose restored.").format(
                result["completeness"], result["reason"]))
        def refinement(record):
            bracket = record["bracket"]
            if bracket is None:
                return translate("Assembly", "refinement not completed")
            width = _value(bracket[1] - bracket[0])
            if record["converged"]:
                return translate("Assembly", "refined to {0} {1}").format(width, result["unit"])
            return translate("Assembly", "refinement not converged; bracket {0} {1}").format(
                width, result["unit"])

        for key, physical in (("firstContact", True), ("firstClearanceViolation", False),
                              ("firstGapViolation", True)):
            event = result[key]
            if event is None:
                continue
            if key == "firstGapViolation":
                action = translate("Assembly", "exceeds the maximum gap from")
            elif physical:
                penetrates = any(pair["solidDetection"]["kind"] == "Penetration"
                                 and not pair["excluded"] for pair in event["scan"]["pairs"])
                action = (translate("Assembly", "intersects") if penetrates
                          else translate("Assembly", "touches"))
            else:
                action = translate("Assembly", "violates the required clearance from")
            sentence = translate("Assembly", "{0} {1} {2} at {3} {4}").format(
                self._labels[0], action, self._labels[1],
                _value(event["coordinate"]), result["unit"])
            previous = event["previousClearSample"]
            if previous is not None:
                sentence += translate("Assembly", " ({0} {1} was clear, resolution {2} {1})").format(
                    _value(previous["coordinate"]), result["unit"], _value(event["resolution"]))
            else:
                sentence += translate("Assembly", " (first evaluated pose)")
            sentence += " (" + refinement(event) + "; sampled + refined)"
            sentences.append(sentence + ".")
            self.events.append((event, physical, sentence))
        minimum = result["minimumClearance"]
        if not self.events and result["completeness"] == "complete":
            sentences.append(translate("Assembly", "No finding at the sampled + refined poses."))
        if minimum is not None:
            sentence = translate("Assembly", "Smallest clearance {0} mm at {1} {2}").format(
                _value(minimum["value"]), _value(minimum["coordinate"]), result["unit"])
            sentence += " (" + refinement(minimum) + "; sampled + refined)"
            sentences.append(sentence + ".")
            self.events.append((minimum, True, sentence))
        self.resultText.setText("\n".join(sentences))
        for _, _, sentence in self.events:
            self.eventPicker.addItem(sentence)
        self.showButton.setEnabled(bool(self.events) and not self._stale)

    def show(self):
        if self.running or self.replaying or self._stale or not self.events:
            return
        self.replaying = True
        self._restorePending = False
        self.showButton.setEnabled(False)
        self.runButton.setEnabled(False)
        self.inputs.setEnabled(False)
        self.restoreButton.setEnabled(True)
        QtCore.QTimer.singleShot(0, self._show)

    def _show(self):
        event, raw, sentence = self.events[self.eventPicker.currentIndex()]
        preview = None

        def apply_pose():
            for name, placement in event["placements"].items():
                obj = self.document.getObject(name)
                if obj is not None and obj.Placement.toMatrix().A != placement.toMatrix().A:
                    obj.Placement = placement
            for joint in self.assembly.Joints:
                if hasattr(joint.Proxy, "redrawJointPlacements"):
                    joint.Proxy.redrawJointPlacements(joint)

        def replay():
            nonlocal preview
            self._owned = True
            try:
                AssemblyApp._withMotionSweepMutation(self.assembly, apply_pose)
                pairs = event["scan"]["pairs"]
                if "value" in event or (raw and event.get("gapViolation")):
                    index = min(range(len(pairs)), key=lambda i:
                                pairs[i]["solidDetection"]["minimumDistance"])
                elif raw:
                    index = next(i for i, pair in enumerate(pairs) if not pair["excluded"] and (
                        pair["solidDetection"]["kind"] in ("Contact", "Penetration") or any(
                            hit["kind"] in ("Contact", "Penetration") and not hit["suppressedByExclusion"]
                            for hit in pair["faceHits"])))
                else:
                    index = next(i for i, pair in enumerate(pairs) if not pair["excluded"] and (
                        pair["detection"]["kind"] in ("ClearanceViolation", "Contact", "Penetration") or any(
                            hit["kind"] == "ClearanceViolation" and not hit["suppressedByExclusion"]
                            for hit in pair["faceHits"])))
                preview = AssemblyGui._showMotionInterferenceResult(
                    self.assembly, event["scan"]["_snapshot"], index, raw, sentence)
                self._replayLoop = QtCore.QEventLoop()
                self._owned = False
                if not (self._restorePending or self._closePending or self._stale):
                    self._replayLoop.exec_()
            finally:
                self._owned = True
                self._replayLoop = None
                if preview is not None:
                    AssemblyGui._clearMotionInterferenceResult(preview)

        try:
            AssemblyApp._withMotionSweepState(self.assembly, replay)
        except Exception as error:
            self.resultText.setText(translate("Assembly", "Cannot show pose: {}").format(error))
        finally:
            self._owned = self.replaying = False
            self.inputs.setEnabled(True)
            self.runButton.setEnabled(True)
            self.showButton.setEnabled(not self._stale)
            self.restoreButton.setEnabled(False)
            if self._closePending:
                self._close()

    def restore(self):
        self._restorePending = True
        if self._replayLoop is not None:
            self._replayLoop.quit()

    def cancel(self):
        if self.running:
            self.token.cancel()
            self.cancelButton.setEnabled(False)
            self.resultText.setText(translate("Assembly", "Cancelling; waiting for the current scan…"))

    def reject(self):
        if self._closed:
            return True
        if self.running or self.replaying:
            self._closePending = True
            self.cancel()
            self.restore()
            return False
        self._closed = True
        App.removeDocumentObserver(self)
        return True

    def _close(self):
        self.reject()
        Gui.Control.closeDialog()

    def _invalidate(self):
        self._stale = True
        self.showButton.setEnabled(False)
        self.resultText.setText(translate("Assembly", "Assembly changed. Run again to update the results."))
        self.cancel()
        self.restore()

    def slotDeletedDocument(self, doc):
        if doc == self.document and not self._closed:
            self._closed = True
            App.removeDocumentObserver(self)

    def slotChangedObject(self, obj, prop):
        if not self._closed and not self._owned and obj.Document == self.document:
            self._invalidate()

    def slotDeletedObject(self, obj):
        if not self._closed and not self._owned and obj.Document == self.document:
            self._invalidate()


Gui.addCommand("Assembly_MotionInterference", CommandMotionInterference())
