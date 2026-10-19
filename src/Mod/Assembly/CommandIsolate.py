# SPDX-License-Identifier: LGPL-2.1-or-later
"""Assembly selection isolation. All presentation changes belong to the view provider."""

import FreeCAD as App
from PySide import QtCore
from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui

import AssemblyPreferences
import UtilsAssembly


class _IsolationSession(QtCore.QObject):
    refresh_requested = QtCore.Signal(str)
    clear_requested = QtCore.Signal()

    def __init__(self):
        super().__init__()
        self.assembly = None
        self.document_name = None
        self.components = ()
        self.mode = 2
        self.refresh_timer = QtCore.QTimer(self)
        self.refresh_timer.setSingleShot(True)
        self.refresh_timer.timeout.connect(self.refresh)
        self.refresh_requested.connect(self.schedule_refresh)
        self.clear_requested.connect(self.clear)

    @QtCore.Slot()
    def clear(self):
        self.refresh_timer.stop()
        assembly, self.assembly = self.assembly, None
        self.document_name = None
        self.components = ()
        if assembly is not None:
            try:
                view = assembly.ViewObject
                if view is not None:
                    view.clearIsolate(True)
            except (ReferenceError, NameError):
                pass  # Its document/view provider has already been destroyed.

    @QtCore.Slot(str)
    def schedule_refresh(self, document_name):
        if self.assembly is not None and document_name == self.document_name:
            # Coalesce model notifications and wait until undo/group scene rebuilds finish.
            # QObject signals also marshal worker notifications to the GUI thread.
            self.refresh_timer.start(0)

    @QtCore.Slot()
    def refresh(self):
        assembly = self.assembly
        if assembly is None:
            return
        try:
            doc = assembly.Document
            components = [doc.getObject(name) for name in self.components]
            components = [obj for obj in components if obj is not None]
            if components:
                assembly.ViewObject.isolateComponents(components, self.mode, True)
            else:
                self.clear()
        except (ReferenceError, NameError):
            self.clear()

    def slotChangedObject(self, obj, prop):
        if prop == "Group" and obj.Document is not None:
            self.refresh_requested.emit(obj.Document.Name)

    def slotCommitTransaction(self, doc):
        self.refresh_requested.emit(doc.Name)

    def slotAbortTransaction(self, doc):
        self.refresh_requested.emit(doc.Name)

    def slotActivateDocument(self, doc):
        if self.document_name is not None and doc.Name != self.document_name:
            self.clear_requested.emit()

    def slotDeletedDocument(self, doc):
        if doc.Name == self.document_name:
            self.clear_requested.emit()

    def slotDeletedObject(self, obj):
        if obj == self.assembly:
            self.clear_requested.emit()


_session = _IsolationSession()
if App.GuiUp:
    App.addDocumentObserver(_session)


def clear_isolation():
    """Exit the current user isolation without a transaction."""
    _session.clear()


def is_isolated(assembly=None):
    return _session.assembly is not None and (assembly is None or _session.assembly == assembly)


def selected_components(assembly, selection):
    """Resolve object paths, stopping at a Body or Link occurrence.

    A face/edge/vertex selects its owning component. Parts and subassemblies
    remain traversable so a deeply nested selection does not isolate its siblings.
    Linked source objects are never changed through an occurrence selection.
    """
    if not selection:
        return []
    members = {}
    pending = [(obj, None) for obj in assembly.Group]
    while pending:
        obj, owner = pending.pop()
        if obj in members:
            continue
        members[obj] = owner or obj
        if obj.isDerivedFrom("App::Link"):
            continue
        if obj.isDerivedFrom("PartDesign::Body"):
            owner = obj
        pending.extend((child, owner) for child in getattr(obj, "Group", ()))

    result = set()
    for sel in selection:
        for sub in sel.SubElementNames or ("",):
            obj = sel.Object
            # Selecting the assembly itself is useful too: retain its whole tree.
            component = assembly if obj == assembly and not sub else members.get(obj)
            for index, token in enumerate(sub.split(".")):
                if (
                    obj is None
                    or obj.isDerivedFrom("App::Link")
                    or obj.isDerivedFrom("PartDesign::Body")
                ):
                    break
                if not token:
                    break
                obj = sel.Object.getSubObject(
                    ".".join(sub.split(".")[: index + 1]) + ".", retType=1
                )
                if obj == assembly:
                    component = assembly
                elif obj in members:
                    component = members[obj]
            if component is not None:
                result.add(component)
    return list(result)


def isolate_selection(assembly=None, selection=None):
    """Replace isolation; an empty selection restores the original scene."""
    if assembly is None:
        assembly = UtilsAssembly.activeAssembly()
    if assembly is None:
        return
    if selection is None:
        selection = Gui.Selection.getSelectionEx("*", 0)
    if not selection:
        clear_isolation()
        return
    components = selected_components(assembly, selection)
    if not components:
        return
    clear_isolation()
    transparent = AssemblyPreferences.preferences().GetBool("IsolateSelectionTransparent", False)
    assembly.ViewObject.isolateComponents(components, 0 if transparent else 2, True)
    _session.document_name = assembly.Document.Name
    _session.components = tuple(obj.Name for obj in components)
    _session.mode = 0 if transparent else 2
    _session.assembly = assembly


class CommandIsolateSelection:
    def GetResources(self):
        return {
            "CmdType": "Alter3DView|NoTransaction|ForEdit",
            "Pixmap": "Assembly_IsolateSelection",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_IsolateSelection", "Isolate Selection"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_IsolateSelection",
                "Temporarily isolates the selected components",
            ),
        }

    def IsActive(self):
        assembly = UtilsAssembly.activeAssembly()
        return assembly is not None and bool(
            selected_components(assembly, Gui.Selection.getSelectionEx("*", 0))
        )

    def Activated(self):
        isolate_selection()


class CommandExitIsolation:
    def GetResources(self):
        return {
            "CmdType": "Alter3DView|NoTransaction|ForEdit",
            "Pixmap": "Assembly_ExitIsolation",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_ExitIsolation", "Exit Isolation"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_ExitIsolation", "Restores the view before selection isolation"
            ),
        }

    def IsActive(self):
        assembly = UtilsAssembly.activeAssembly()
        return assembly is not None and is_isolated(assembly)

    def Activated(self):
        clear_isolation()


if App.GuiUp:
    Gui.addCommand("Assembly_IsolateSelection", CommandIsolateSelection())
    Gui.addCommand("Assembly_ExitIsolation", CommandExitIsolation())
