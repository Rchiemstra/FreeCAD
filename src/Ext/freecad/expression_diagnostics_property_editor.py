# SPDX-License-Identifier: LGPL-2.1-or-later
"""Debounced Data-tab presentation of the shared expression diagnostics.

Observers only enqueue invalidation (they may run on a recompute worker). The
Qt timer diagnoses quiescent documents once per batch, never from a model role
request. Only strings and a QColor cross into the C++ property-item paint path.
"""

import re

import FreeCAD as App
from freecad.expression_diagnostics_text import reason_text
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets


class PropertyDiagnostics(QtCore.QObject):
    invalidated = QtCore.Signal()
    color_changed = QtCore.Signal()

    def __init__(self):
        super().__init__(Gui.getMainWindow())
        self.stopped = False
        self.timer = QtCore.QTimer(self)
        self.timer.setSingleShot(True)
        self.timer.setInterval(50)
        self.timer.timeout.connect(self.refresh)
        self.invalidated.connect(self.schedule, QtCore.Qt.QueuedConnection)
        self.color_changed.connect(self.publish_color, QtCore.Qt.QueuedConnection)
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/PropertyView")
        self.params.Attach(self)
        self.reports = {}
        self.published = None
        self.dirty = False
        self.queued = False
        self.pending = set()
        App.addDocumentObserver(self)
        QtWidgets.QApplication.instance().aboutToQuit.connect(self.stop)
        self.parent().destroyed.connect(self.stop)
        self.publish_color()
        self.schedule()

    @QtCore.Slot()
    def stop(self):
        if self.stopped:
            return
        self.stopped = True
        self.timer.stop()
        App.removeDocumentObserver(self)
        self.params.Detach(self)

    @QtCore.Slot()
    def schedule(self):
        if self.stopped:
            return
        self.queued = False
        self.dirty = True
        if not self.timer.isActive():
            self.timer.start()

    def onChange(self, group, name):
        if name == "ExpressionErrorColor" and not self.stopped:
            self.color_changed.emit()

    @QtCore.Slot()
    def publish_color(self):
        if self.stopped:
            return
        rgba = self.params.GetUnsigned("ExpressionErrorColor", 0xD35400FF)
        self.parent().setProperty("expressionDiagnosticsErrorColor", QtGui.QColor(
            (rgba >> 24) & 255, (rgba >> 16) & 255, (rgba >> 8) & 255, rgba & 255))
        self.repaint()

    def invalidate(self):
        if self.stopped:
            return
        # Observer calls hold Python's GIL. Queue one Qt event for a burst of
        # property notifications, rather than one event for every property.
        if not self.queued:
            self.queued = True
            self.invalidated.emit()

    # Do not dereference observer arguments: change/recompute can be delivered
    # on the document owner thread. Queued invalidation also coalesces deletes,
    # label rewrites, dynamic property edits, undo/redo and document restoration.
    def slotChangedObject(self, obj, prop):
        self.invalidate()

    slotAppendDynamicProperty = slotChangedObject
    slotRemoveDynamicProperty = slotChangedObject
    slotChangedDocument = slotChangedObject

    def slotCreatedObject(self, obj):
        self.invalidate()

    def slotDeletedObject(self, obj):
        self.invalidate()

    def slotRecomputedDocument(self, doc):
        self.invalidate()

    def slotDeletedDocument(self, doc):
        self.invalidate()

    slotUndoDocument = slotRecomputedDocument
    slotRedoDocument = slotRecomputedDocument
    slotRelabelDocument = slotRecomputedDocument

    @QtCore.Slot()
    def refresh(self):
        if self.stopped:
            return
        from freecad import expression_diagnostics

        admit = getattr(Gui, "tryRunPresentationCallback", None)
        updated = False

        def discover():
            nonlocal updated
            # Discovery itself reads the application/document registry. Keep it
            # in the same reservation as report pruning and per-document reads.
            documents = App.listDocuments()
            names = set(documents)
            updated = bool(set(self.reports) - names)
            self.reports = {name: rows for name, rows in self.reports.items() if name in names}
            if self.dirty:
                self.pending = names
                self.dirty = False
            else:
                self.pending.intersection_update(names)
            for name in sorted(self.pending):
                document = documents[name]

                def observe():
                    nonlocal updated
                    self.reports[name] = expression_diagnostics.diagnose(document)
                    updated = True

                try:
                    if admit is None or admit(observe, document):
                        if admit is None:
                            observe()
                        self.pending.discard(name)
                except ReferenceError:
                    updated = self.reports.pop(name, None) is not None or updated
                    self.pending.discard(name)
            if not self.pending:
                self.populate_rows()

        # Upstream is synchronous and has no presentation reservation API.
        if admit is None:
            discover()
        elif not admit(discover):
            self.timer.start()
            return
        if self.pending:
            self.timer.start()
        if not updated:
            return
        reasons = {}
        for rows in self.reports.values():
            for row in rows:
                # The table also reports exposure to failed inputs. Preserve
                # the normal colour of bindings that still evaluate correctly.
                if (not row["property"] or row["role"] != "root"
                        or row["kind"] in ("stale_value", "not_evaluated")):
                    continue
                reason = (row["property"] + " = " + row["expression"] + "\n"
                          + reason_text(row))
                # Also flag a collapsed compound property with a failing leaf.
                path = row["property"]
                paths = [path] + [path[:match.start()] for match in re.finditer(r"[.\[]", path)]
                for path in paths:
                    key = row["object"] + "." + path
                    messages = reasons.setdefault(key, [])
                    if reason not in messages:
                        messages.append(reason)
        reasons = {key: "\n\n".join(messages) for key, messages in reasons.items()}
        if reasons != self.published:
            self.published = reasons
            self.parent().setProperty("expressionDiagnosticsReasons", reasons)
            self.repaint()

    def populate_rows(self):
        # C++ snapshots all row identities and legacy presentation roles here,
        # under the discovery reservation. QModelIndex/Qt strings are the only
        # things used by subsequent paint and tooltip requests.
        for view in self.parent().findChildren(QtWidgets.QTreeView, "propertyEditorData"):
            model = view.model()
            for row in range(model.rowCount()):
                model.index(row, 0).data(QtCore.Qt.UserRole + 26496)

    def repaint(self):
        # These are presentation roles. dataChanged would reset live editors
        # through the delegate, replacing text the user has not committed yet.
        for view in self.parent().findChildren(QtWidgets.QTreeView, "propertyEditorData"):
            view.viewport().update()


def _install():
    global _cache
    if Gui.getMainWindow() is None:
        QtCore.QTimer.singleShot(50, _install)
        return
    _cache = PropertyDiagnostics()


# GUI startup imports this before workbench activation. Wait for its main window.
QtCore.QTimer.singleShot(0, _install)
