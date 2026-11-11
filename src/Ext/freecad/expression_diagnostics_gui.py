# SPDX-License-Identifier: LGPL-2.1-or-later
"""Global read-only expression diagnostics command (loaded by GUI startup)."""

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets
from freecad import expression_diagnostics_property_editor  # install the Data-tab observer

translate = App.Qt.translate
_dialogs = []


class DiagnosticsDialog(QtWidgets.QDialog):
    invalidated = QtCore.Signal()

    def __init__(self, document):
        super().__init__(Gui.getMainWindow())
        self.document = document
        self.stopped = False
        self.queued = False
        self.rows = []
        self.pending_selection = None
        self.timer = QtCore.QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(50)
        self.timer.timeout.connect(self.refresh)
        self.invalidated.connect(self.schedule, QtCore.Qt.QueuedConnection)
        self.setObjectName("ExpressionDiagnostics")
        self.setWindowTitle(translate("ExpressionDiagnostics", "Expression diagnostics"))
        self.setAttribute(QtCore.Qt.WA_DeleteOnClose)
        self.resize(1000, 450)
        layout = QtWidgets.QVBoxLayout(self)
        self.status = QtWidgets.QLabel(self)
        layout.addWidget(self.status)
        self.table = QtWidgets.QTableWidget(0, 7, self)
        # Literal calls are required by lupdate's Python parser.
        headers = (
            translate("ExpressionDiagnostics", "Object"),
            translate("ExpressionDiagnostics", "Property"),
            translate("ExpressionDiagnostics", "Expression"),
            translate("ExpressionDiagnostics", "Role"),
            translate("ExpressionDiagnostics", "Kind"),
            translate("ExpressionDiagnostics", "Reason"),
            translate("ExpressionDiagnostics", "Dependency chain"))
        self.table.setHorizontalHeaderLabels(headers)
        self.table.setEditTriggers(QtWidgets.QAbstractItemView.NoEditTriggers)
        self.table.setSelectionBehavior(QtWidgets.QAbstractItemView.SelectRows)
        self.table.setSelectionMode(QtWidgets.QAbstractItemView.SingleSelection)
        self.table.itemSelectionChanged.connect(self.select_object)
        layout.addWidget(self.table)
        buttons = QtWidgets.QDialogButtonBox(QtWidgets.QDialogButtonBox.Close)
        buttons.rejected.connect(self.close)
        layout.addWidget(buttons)
        App.addDocumentObserver(self)
        QtWidgets.QApplication.instance().aboutToQuit.connect(self.stop)
        self.refresh()

    def schedule(self):
        self.queued = False
        if not self.stopped:
            if not self.timer.isActive():
                self.timer.start()

    def invalidate(self, *args):
        # Notifications can originate on the owner thread; never inspect args.
        if not self.stopped and not self.queued:
            self.queued = True
            self.invalidated.emit()

    slotChangedObject = invalidate
    slotAppendDynamicProperty = invalidate
    slotRemoveDynamicProperty = invalidate
    slotChangedDocument = invalidate
    slotCreatedObject = invalidate
    slotDeletedObject = invalidate
    slotRecomputedDocument = invalidate
    slotDeletedDocument = invalidate
    slotCreatedDocument = invalidate
    slotUndoDocument = invalidate
    slotRedoDocument = invalidate
    slotRelabelDocument = invalidate

    def stop(self):
        if not self.stopped:
            self.stopped = True
            self.timer.stop()
            App.removeDocumentObserver(self)

    def closeEvent(self, event):
        self.stop()
        super().closeEvent(event)

    def observe(self, callback):
        """Reserve discovery and every possible referenced document together."""
        admit = getattr(Gui, "tryRunPresentationCallback", None)
        complete = False
        def discover():
            nonlocal complete
            documents = App.listDocuments()
            if self.document not in documents.values():
                self.close()
                complete = True
                return
            def reserve(remaining):
                nonlocal complete
                if not remaining:
                    callback(documents)
                    complete = True
                elif admit is None:
                    reserve(remaining[1:])
                else:
                    admit(lambda: reserve(remaining[1:]), remaining[0])
            reserve(list(documents.values()))
        try:
            if admit is None:
                discover()
            else:
                admit(discover)
        except ReferenceError:
            # A close can invalidate a Python document wrapper between retries.
            self.schedule()
        if not complete and not self.stopped:
            self.table.setEnabled(False)
            self.status.setText(translate("ExpressionDiagnostics", "Waiting for document work to finish…"))
            self.schedule()
        return complete

    def refresh(self):
        if self.stopped:
            return
        from freecad.expression_diagnostics import diagnose
        from freecad.expression_diagnostics_text import role_text, kind_text, reason_text
        def capture(documents):
            self.setWindowTitle(translate("ExpressionDiagnostics", "Expression diagnostics")
                                + " — " + self.document.Label)
            self.rows = diagnose(self.document)
            self.table.blockSignals(True)
            self.table.setRowCount(len(self.rows))
            for index, row in enumerate(self.rows):
                chain = " → ".join(step["object"] + ("." + step["property"] if step["property"] else "")
                                   for step in row["chain"])
                values = [row["object"], row["property"], row["expression"],
                          role_text(row["role"]), kind_text(row["kind"]), reason_text(row), chain]
                for column, value in enumerate(values):
                    item = QtWidgets.QTableWidgetItem(value)
                    item.setToolTip(value)
                    self.table.setItem(index, column, item)
            self.table.blockSignals(False)
            self.table.setEnabled(True)
            self.table.resizeColumnsToContents()
            self.table.setColumnWidth(5, 300)
            self.status.setText(translate("ExpressionDiagnostics", "Current stored values; no recompute performed.")
                                if self.rows else translate("ExpressionDiagnostics", "No expression problems found."))
            self.apply_selection(documents)
        self.observe(capture)

    def select_object(self):
        index = self.table.currentRow()
        if 0 <= index < len(self.rows):
            row = self.rows[index]
            self.pending_selection = (row["object"], row["property"])
            self.observe(self.apply_selection)

    def apply_selection(self, documents):
        if self.pending_selection is None:
            return
        full_name, path = self.pending_selection
        if not any((row["object"], row["property"]) == self.pending_selection for row in self.rows):
            self.pending_selection = None
            return
        doc_name, obj_name = full_name.split("#", 1)
        document = documents.get(doc_name)
        obj = document.getObject(obj_name) if document is not None else None
        if obj is None:
            self.pending_selection = None
            return
        if Gui.Selection.getSelection() != [obj]:
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(doc_name, obj_name)
        # Rows expose canonical, cached identities. Labels may be translated,
        # split into words, or differ from expression subpaths (e.g. Placement).
        key = full_name + "." + path
        for view in Gui.getMainWindow().findChildren(QtWidgets.QTreeView, "propertyEditorData"):
            model = view.model()
            for row in range(model.rowCount()):
                model.index(row, 0).data(QtCore.Qt.UserRole + 26496)
            def select(parent=QtCore.QModelIndex()):
                for row in range(model.rowCount(parent)):
                    index = model.index(row, 0, parent)
                    if key in (index.data(QtCore.Qt.UserRole + 26497) or []):
                        ancestor = index.parent()
                        while ancestor.isValid():
                            view.expand(ancestor)
                            ancestor = ancestor.parent()
                        tabs = Gui.getMainWindow().findChild(QtWidgets.QTabWidget, "propertyTab")
                        if tabs is not None and tabs.indexOf(view) >= 0:
                            tabs.setCurrentWidget(view)
                        view.setCurrentIndex(index)
                        view.scrollTo(index)
                        return True
                    if select(index):
                        return True
                return False
            if select() or not path:
                self.pending_selection = None
        if self.pending_selection is not None:
            self.schedule()


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
