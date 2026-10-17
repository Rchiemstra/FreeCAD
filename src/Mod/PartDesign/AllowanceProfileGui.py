# SPDX-License-Identifier: LGPL-2.1-or-later
"""PartDesign command and task panel for a nominal sketch's allowance profile."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui

from PartDesign.AllowanceProfile import makeAllowanceProfile


def _translate(text):
    return App.Qt.translate("PartDesign_AllowanceProfile", text)


class AllowanceMenu:
    """Extend the native workbench through its public Python menu hook."""

    def modifyMenuBar(self):
        return [{"insert": "PartDesign_AllowanceProfile", "menuItem": "PartDesign_ShapeBinder"}]


class CommandAllowanceProfile:
    def GetResources(self):
        return {
            "Pixmap": "PartDesign_ShapeBinder",
            "MenuText": QtCore.QT_TRANSLATE_NOOP(
                "PartDesign_AllowanceProfile", "Allowance Profile"
            ),
            "ToolTip": QtCore.QT_TRANSLATE_NOOP(
                "PartDesign_AllowanceProfile",
                "Creates a manufacturing offset of the selected sketch",
            ),
        }

    def _source(self):
        if App.ActiveDocument is None or Gui.Control.activeDialog():
            return None
        body = Gui.activeDocument().activeView().getActiveObject("pdbody")
        selected = Gui.Selection.getSelection()
        if body is None or len(selected) != 1:
            return None
        source = selected[0]
        if (
            source.isDerivedFrom("Sketcher::SketchObject")
            and source.Document == App.ActiveDocument
            and source.getParentGeoFeatureGroup() == body
        ):
            return source
        return None

    def IsActive(self):
        return self._source() is not None

    def Activated(self):
        source = self._source()
        if source is None:
            return
        doc = source.Document
        doc.openTransaction(_translate("Create allowance profile"))
        try:
            obj = makeAllowanceProfile(source)
            obj.Proxy.execute(obj)
            source.Visibility = False
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(obj)
            Gui.activeDocument().setEdit(obj.Name)
        except Exception:
            doc.abortTransaction()
            raise


class ViewProviderAllowanceProfile:
    def __init__(self, view):
        view.Proxy = self

    def getIcon(self):
        return ":/icons/PartDesign_ShapeBinder.svg"

    def setEdit(self, view, mode):
        if mode != 0:
            return False
        doc = view.Object.Document
        if not doc.HasPendingTransaction:
            doc.openTransaction(_translate("Edit allowance profile"))
        self.panel = AllowanceTaskPanel(view.Object)
        Gui.Control.showDialog(self.panel)
        return True

    def unsetEdit(self, view, mode):
        Gui.Control.closeDialog()
        self.panel = None
        return True

    def doubleClicked(self, view):
        return Gui.activeDocument().setEdit(view.Object.Name)

    def dumps(self):
        return None

    def loads(self, state):
        pass


class AllowanceTaskPanel:
    def __init__(self, obj):
        self.obj = obj
        self.form = QtGui.QWidget()
        self.form.setWindowTitle(_translate("Allowance profile"))
        layout = QtGui.QFormLayout(self.form)
        self.input = Gui.UiLoader().createWidget("Gui::InputField")
        self.input.setText(obj.Allowance.UserString)
        self.input.setToolTip(obj.getDocumentationOfProperty("Allowance"))
        layout.addRow(_translate("Allowance"), self.input)
        self.binding = Gui.ExpressionBinding(self.input)
        self.binding.bind(obj, "Allowance")
        self.error = QtGui.QLabel()
        self.error.setWordWrap(True)
        layout.addRow(self.error)

    def accept(self):
        try:
            if self.binding.hasExpression():
                self.binding.apply("Allowance")
                expression = dict(self.obj.ExpressionEngine)["Allowance"]
                self.obj.Allowance = self.obj.evalExpression(expression)
            else:
                self.obj.Allowance = self.input.text()
            self.obj.Proxy.execute(self.obj)
        except Exception as exc:
            self.error.setText(str(exc))
            return False
        doc = self.obj.Document
        Gui.activeDocument().resetEdit()
        doc.commitTransaction()
        # The standard command uses the host's recompute policy (asynchronous
        # in this fork), while remaining compatible with upstream FreeCAD.
        Gui.runCommand("Std_Refresh")
        return True

    def reject(self):
        doc = self.obj.Document
        gui_doc = Gui.activeDocument()
        # resetEdit commits the active transaction, so roll back first.
        doc.abortTransaction()
        gui_doc.resetEdit()
        return True
