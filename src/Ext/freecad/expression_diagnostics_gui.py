# SPDX-License-Identifier: LGPL-2.1-or-later
"""Global read-only expression diagnostics command (loaded by GUI startup)."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets
from freecad import expression_diagnostics_property_editor  # install the Data-tab observer

translate = App.Qt.translate
_dialogs = []


class DiagnosticsDialog(QtWidgets.QDialog):
    def __init__(self, document):
        super().__init__(Gui.getMainWindow())
        from freecad.expression_diagnostics import diagnose

        self.setObjectName("ExpressionDiagnostics")
        self.setWindowTitle(translate("ExpressionDiagnostics", "Expression diagnostics") + " — " + document.Label)
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.resize(1000, 450)
        layout = QtWidgets.QVBoxLayout(self)
        rows = diagnose(document)
        label = QtWidgets.QLabel(translate("ExpressionDiagnostics", "Current stored values; no recompute performed."))
        layout.addWidget(label)
        self.table = QtWidgets.QTableWidget(len(rows), 7, self)
        self.table.setHorizontalHeaderLabels([
            translate("ExpressionDiagnostics", text) for text in
            ("Object", "Property", "Expression", "Role", "Kind", "Reason", "Dependency chain")])
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QtWidgets.QAbstractItemView.SingleSelection)
        for index, row in enumerate(rows):
            chain = " → ".join(step["object"] + ("." + step["property"] if step["property"] else "")
                               for step in row["chain"])
            values = [row[k] for k in ("object", "property", "expression", "role", "kind", "message")] + [chain]
            for column, value in enumerate(values):
                item = QtWidgets.QTableWidgetItem(value)
                item.setToolTip(value)
                self.table.setItem(index, column, item)
        self.table.resizeColumnsToContents()
        self.table.setColumnWidth(5, 300)
        self.table.itemSelectionChanged.connect(self.select_object)
        layout.addWidget(self.table)
        if not rows:
            layout.addWidget(QtWidgets.QLabel(translate("ExpressionDiagnostics", "No expression problems found.")))
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Close)
        buttons.rejected.connect(self.close)
        layout.addWidget(buttons)

    def select_object(self):
        index = self.table.currentRow()
        if index < 0:
            return
        full_name = self.table.item(index, 0).text()
        doc_name, obj_name = full_name.split("#", 1)
        document = App.listDocuments().get(doc_name)
        if document is not None and document.getObject(obj_name) is not None:
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(doc_name, obj_name)


class ExpressionDiagnosticsCommand:
    def GetResources(self):
        return {
            "MenuText": translate("ExpressionDiagnostics", "Expression diagnostics…"),
            "ToolTip": translate("ExpressionDiagnostics", "Inspect expression failures and downstream impact without recomputing"),
            "Pixmap": "bound-expression",
            "CmdType": "NoTransaction",
        }

    def IsActive(self):
        return App.activeDocument() is not None

    def Activated(self):
        document = App.activeDocument()
        if document is None:
            return
        dialog = DiagnosticsDialog(document)
        _dialogs.append(dialog)
        dialog.destroyed.connect(lambda: _dialogs.remove(dialog) if dialog in _dialogs else None)
        dialog.show()


class DiagnosticsMenu:
    def modifyMenuBar(self):
        return [{"insert": "Std_ExpressionDiagnostics", "menuItem": "Std_DlgParameter"}]


Gui.addCommand("Std_ExpressionDiagnostics", ExpressionDiagnosticsCommand())
_menu = DiagnosticsMenu()
Gui.addWorkbenchManipulator(_menu)
