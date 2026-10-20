# SPDX-License-Identifier: LGPL-2.1-or-later
"""Assembly GUI for the headless component extraction API."""

import os

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets

import UtilsAssembly
from ExtractComponent import ExtractionError, _parent, extract, preflight

translate = App.Qt.translate


def _saveAssemblyThenExtract(assembly, component):
    """Keep the GUI responsive and resume only after the lane's save completes."""
    doc = assembly.Document
    path, _ = QtWidgets.QFileDialog.getSaveFileName(
        Gui.getMainWindow(),
        translate("Assembly", "Save Document"),
        doc.Label + ".FCStd",
        translate("Assembly", "FreeCAD document (*.FCStd)"),
    )
    if not path:
        return
    if not path.lower().endswith(".fcstd"):
        path += ".FCStd"
    try:
        handle = doc.saveAsync(path)
    except Exception as exc:
        QtWidgets.QMessageBox.warning(
            Gui.getMainWindow(), translate("Assembly", "Cannot save assembly"), str(exc)
        )
        return
    timer = QtCore.QTimer(Gui.getMainWindow())

    def completed():
        if not handle.done():
            return
        timer.stop()
        timer.deleteLater()
        status = handle.status()
        if status["state"] != "Completed":
            QtWidgets.QMessageBox.warning(
                Gui.getMainWindow(),
                translate("Assembly", "Cannot save assembly"),
                status["diagnostic"],
            )
            return
        # The user may close the document while the save is pending.
        try:
            _parent(assembly, component)
        except (ExtractionError, AttributeError, ReferenceError, RuntimeError):
            return
        extractWithDialog(assembly, component)

    timer.timeout.connect(completed)
    timer.start(20)


def selectedComponent():
    assembly = UtilsAssembly.activeAssembly()
    selection = Gui.Selection.getSelectionEx("*", 0)
    if assembly is None or len(selection) != 1:
        return None
    selected = selection[0]
    component = selected.Object
    try:
        if selected.SubElementNames:
            sub = selected.Object.resolveSubElement(selected.SubElementNames[0], True)[
                2
            ]
            names = sub.rstrip(".").split(".")
            geometry = names[-1].startswith(("Face", "Edge", "Vertex"))
            if geometry:
                root, _ = UtilsAssembly.getComponentReference(
                    assembly, selected.Object, sub
                )
                if root is not None:
                    component = root
                    _parent(assembly, component)
                    return component
                names.pop()
            if names:
                component = selected.Object.getSubObject(".".join(names) + ".", 1)
            # A feature selected through a Body belongs to that Body component.
            if geometry and component is not None:
                for owner in component.InList:
                    if (
                        owner.isDerivedFrom("PartDesign::Body")
                        and component in owner.Group
                    ):
                        component = owner
                        break
        _parent(assembly, component)
    except (ExtractionError, AttributeError, ReferenceError, RuntimeError):
        return None
    return component


def extractWithDialog(assembly, component):
    doc = assembly.Document
    if not doc.FileName or not os.path.isfile(doc.FileName):
        answer = QtWidgets.QMessageBox.warning(
            Gui.getMainWindow(),
            translate("Assembly", "Save Document"),
            translate(
                "Assembly",
                "The assembly document must be saved before extracting a component.",
            ),
            QtWidgets.QMessageBox.Save | QtWidgets.QMessageBox.Cancel,
        )
        if answer == QtWidgets.QMessageBox.Save:
            _saveAssemblyThenExtract(assembly, component)
        return
    filename = component.Label.replace("/", "_").replace("\\", "_")
    path, _ = QtWidgets.QFileDialog.getSaveFileName(
        Gui.getMainWindow(),
        translate("Assembly", "Extract component to file"),
        os.path.join(os.path.dirname(doc.FileName), filename + ".FCStd"),
        translate("Assembly", "FreeCAD document (*.FCStd)"),
    )
    if not path:
        return
    if not path.lower().endswith(".fcstd"):
        path += ".FCStd"
    try:
        # Report all unsupported references before entering the operation.
        preflight(assembly, component, path)
        result = extract(assembly, component, path)
    except Exception as exc:
        QtWidgets.QMessageBox.warning(
            Gui.getMainWindow(),
            translate("Assembly", "Cannot extract component"),
            str(exc),
        )
        return
    Gui.Selection.clearSelection()
    Gui.Selection.addSelection(result.link)
    App.Console.PrintMessage(
        translate(
            "Assembly",
            "Component extracted. Undo restores the assembly; the written file is kept.\n",
        )
    )
    return result


def offer(component):
    """GUI-triggered Std_Part continuation, after its transaction has committed."""
    import AssemblyPreferences

    if not AssemblyPreferences.preferences().GetBool("DetachStdPart", False):
        return
    assembly = UtilsAssembly.activeAssembly()
    try:
        parent = _parent(assembly, component)
        if not parent.isDerivedFrom("Assembly::AssemblyObject"):
            return
    except (ExtractionError, AttributeError, ReferenceError):
        return
    extractWithDialog(assembly, component)


class CommandExtractComponent:
    def GetResources(self):
        return {
            "Pixmap": "document-save-as",
            "MenuText": QtCore.QT_TRANSLATE_NOOP(
                "Assembly", "Extract component to file"
            ),
            "ToolTip": QtCore.QT_TRANSLATE_NOOP(
                "Assembly",
                "Move a local component into a new file and replace it with a link",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return not Gui.Control.activeDialog() and selectedComponent() is not None

    def Activated(self):
        component = selectedComponent()
        if component is not None:
            extractWithDialog(UtilsAssembly.activeAssembly(), component)


Gui.addCommand("Assembly_ExtractComponent", CommandExtractComponent())
