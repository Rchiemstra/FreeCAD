# SPDX-License-Identifier: LGPL-2.1-or-later
"""Extract a local assembly component, preserving references and one-step Undo.

The saved component file is an independent artifact: Undo restores the source
model but deliberately leaves that file on disk. Redo uses the same document.

Copy, property changes, deletion, and save run on each document owner.
The destination recompute is that commit's eager native recompute. The source
recompute runs after the rewrite commit, because an eager recompute inside it
refuses the new cross-document link. The Assembly view-provider hook was
removed. Base Gui::Document ordering (src/Gui/Document.cpp:1360,
addObjectNewBefore) records the provider before the object, so Undo does not
leak a second provider. Dialogs, selection, and messages stay on the GUI thread.
"""

from dataclasses import dataclass
import json
import os
import tempfile
import threading
import time
import types

import FreeCAD as App
import AssemblyApp

translate = App.Qt.translate


class ExtractionError(ValueError):
    """A preflight or commit refusal. ``leftover`` identifies unsupported references.

    ``commit_status`` is the native status when a commit returned one, or None
    when ``wait`` raised or this error is not a commit result.
    """

    def __init__(self, message, leftover=(), commit_status=None):
        self.leftover = tuple(leftover)
        self.commit_status = commit_status
        super().__init__(message)


@dataclass
class ExtractionResult:
    document: object
    component: object
    link: object
    redirected: tuple
    leftover: tuple = ()
    forwarding_links: tuple = ()
    geometry_binders: tuple = ()
    shape_proxy: object = None


def _linkedObjects(value):
    if hasattr(value, "Document") and hasattr(value, "Name"):
        yield value
    elif isinstance(value, (tuple, list)):
        for item in value:
            yield from _linkedObjects(item)


def _owned(component):
    """Structural ownership, never an indiscriminate recursive dependency copy."""
    result = []
    root_parents = [
        p for p in component.InList if hasattr(p, "Group") and component in p.Group
    ]

    def visit(obj):
        if obj is None or obj in result or obj.Document != component.Document:
            return
        result.append(obj)
        if obj.isDerivedFrom("App::Link"):
            return
        for child in getattr(obj, "Group", ()):
            visit(child)
        origin = getattr(obj, "Origin", None)
        if origin:
            visit(origin)
            for child in origin.OriginFeatures:
                visit(child)
        # Features can own construction history in their containing assembly.
        for name in ("Profile", "Base", "Tool", "Shapes", "Originals", "BaseFeature"):
            value = getattr(obj, name, None)
            for dependency in _linkedObjects(value):
                if (
                    not hasattr(dependency, "Document")
                    or dependency.Document != component.Document
                ):
                    continue
                parents = [
                    p
                    for p in dependency.InList
                    if hasattr(p, "Group") and dependency in p.Group
                ]
                if not parents or all(
                    p in result or p in root_parents for p in parents
                ):
                    visit(dependency)

    visit(component)
    return result


def _assembly_link_message():
    return translate(
        "Assembly",
        "Extracting an AssemblyLink is not supported; its flexible assembly behavior cannot be preserved.",
    )


def _refuse_assembly_links(objects):
    """AssemblyLink derives from App::Part, so the App::Link exclusion misses it."""
    for obj in objects:
        if obj is not None and obj.isDerivedFrom("Assembly::AssemblyLink"):
            raise ExtractionError(_assembly_link_message())


def _recompute_closure(component, owned):
    """Owned objects plus same-document dependents whose recompute would run."""
    closure = []
    seen = set()
    pending = list(owned)
    while pending:
        obj = pending.pop()
        if obj is None or obj in seen or obj.Document != component.Document:
            continue
        seen.add(obj)
        closure.append(obj)
        pending.extend(getattr(obj, "InListRecursive", ()) or ())
    return closure


def _refuse_skipped_recompute(document, objects):
    """Refuse a source whose recompute would be skipped, before any mutation."""
    if document.RecomputesFrozen:
        raise ExtractionError(
            translate(
                "Assembly",
                "Unfreeze recomputes in the assembly document before extracting a component.",
            )
        )
    skipped = [
        obj.Name
        for obj in objects
        if obj.getStatusString() == "Freezed"
        or (hasattr(obj, "isFreezed") and obj.isFreezed())
    ]
    if skipped:
        raise ExtractionError(
            translate("Assembly", "Unfreeze these objects before extracting a component:")
            + " "
            + ", ".join(skipped)
        )


def _parent(assembly, component):
    if (
        assembly is None
        or not assembly.isDerivedFrom("Assembly::AssemblyObject")
        or component is None
        or component == assembly
        or component.Document != assembly.Document
        or component.isDerivedFrom("App::Link")
        or component.isDerivedFrom("Assembly::AssemblyLink")
        or not (
            component.isDerivedFrom("App::Part")
            or component.isDerivedFrom("Part::Feature")
        )
    ):
        if component is not None and component.isDerivedFrom("Assembly::AssemblyLink"):
            raise ExtractionError(_assembly_link_message())
        raise ExtractionError(
            translate("Assembly", "Select a local component of the active assembly.")
        )

    def find(container):
        for obj in container.Group:
            if obj == component:
                return container
            if obj.isDerivedFrom("App::Part"):
                found = find(obj)
                if found:
                    return found
        return None

    parent = find(assembly)
    if parent is None:
        raise ExtractionError(
            translate("Assembly", "The component is not in the active assembly.")
        )
    return parent


# Nodes visited, not bytes. Larger graphs fail closed as state that cannot
# be inspected, including flat containers with no document object inside.
_MAX_PYTHON_STATE_NODES = 8192
_UNSAFE_SERIALIZER = object()
# Class layout is read through type's own descriptors. type.__getattribute__
# honours a metaclass data descriptor and can hide the real dict or MRO.
_TYPE_MRO = type.__dict__["__mro__"]
_TYPE_DICT = type.__dict__["__dict__"]
# object.__getattribute__ is the default lookup. Builtins such as list, dict,
# int and tuple carry their own C slot wrapper (WrapperDescriptorType); native
# save calls that wrapper. A function, staticmethod, or callable instance
# stored as __getattribute__ is refused without being called.
# object.__getstate__ is a method descriptor with no __func__ (added in
# 3.11). Native save treats it as "no state function" and reads __dict__
# (PropertyPythonObject.cpp:255-268). On 3.10 the dict has no entry, so
# .get returns None. The only comparison is `raw is _OBJECT_GETSTATE` in
# _state_without_dumps, next to `raw is None`. None matches only that
# branch: it is not a function and it is not _UNSAFE_SERIALIZER.
_OBJECT_DICT = _TYPE_DICT.__get__(object)
_OBJECT_GETATTRIBUTE = _OBJECT_DICT["__getattribute__"]
_OBJECT_GETSTATE = _OBJECT_DICT.get("__getstate__")


def _foreign_c_storage(attributes):
    """True when a class dict holds C storage this walker cannot read.

    Assigning ``object``'s own wrapper under the same name (for example
    ``__getattribute__ = object.__getattribute__``) is ordinary lookup, not a
    C base.
    """
    if attributes.get("__module__") == "builtins":
        return True
    for name, member in attributes.items():
        if not isinstance(
            member,
            (
                types.BuiltinFunctionType,
                types.MethodDescriptorType,
                types.WrapperDescriptorType,
                types.ClassMethodDescriptorType,
            ),
        ):
            continue
        if _OBJECT_DICT.get(name) is member:
            continue
        return True
    return False


def _inspectPythonState(value, watched, seen, budget):
    """Detect opaque Python state that the native link protocol cannot rewrite.

    Returns true when ``value`` holds a watched object or its state cannot be
    inspected. Slot values are the member descriptors in each class dict, not
    a replay of ``__slots__``. ``budget`` counts nodes visited.
    """
    budget[0] += 1
    if budget[0] > _MAX_PYTHON_STATE_NODES:
        return True
    if any(value is obj for obj in watched):
        return True
    if value is None or type(value) in (str, bytes, int, float, bool, complex):
        return False
    # Native geometry values store numbers, never document references.
    if type(value) in (App.Placement, App.Vector, App.Rotation, App.Matrix):
        return False
    # type() is the instance layout. isinstance() follows a custom __class__
    # and would treat a wrapper as a document object, hiding its payload.
    if issubclass(type(value), App.DocumentObject):
        return False
    if id(value) in seen:
        return False
    seen.add(id(value))
    if type(value) is dict:
        children = list(value.keys()) + list(value.values())
    elif type(value) in (tuple, list, set, frozenset):
        children = value
    else:
        # A missing __dict__ is not empty state. Slots may live on any base,
        # and native or callable payloads can hide references from both.
        if callable(value):
            return True
        children = []
        inspected = False
        classes = [
            (cls, _class_dict(cls))
            for cls in _class_mro(type(value))
            if cls is not object
        ]
        saw_instance_dict = False
        for _cls, attributes in classes:
            # A C base can hold pointers that are not in slots or an instance dict.
            if _foreign_c_storage(attributes):
                return True
            dictionary = attributes.get("__dict__")
            if dictionary is not None:
                if not isinstance(dictionary, types.GetSetDescriptorType):
                    return True
                state = dictionary.__get__(value, type(value))
                if type(state) is not dict:
                    return True
                children.extend(state.keys())
                children.extend(state.values())
                inspected = True
                saw_instance_dict = True
            # __weakref__ from a slots declaration is a get-set descriptor.
            # A different object under that name is not the storage CPython made.
            weak = attributes.get("__weakref__")
            if weak is not None and not isinstance(weak, types.GetSetDescriptorType):
                return True
            # The key alone counts: an iterator stored as __slots__ was already
            # consumed when the class was created, and replaying it would run
            # user code native save never runs.
            if "__slots__" in attributes:
                inspected = True
            for descriptor in attributes.values():
                if not isinstance(descriptor, types.MemberDescriptorType):
                    continue
                if getattr(descriptor, "__name__", None) in ("__dict__", "__weakref__"):
                    continue
                try:
                    children.append(descriptor.__get__(value, type(value)))
                except AttributeError:
                    # An unassigned slot has no payload.
                    pass
        # A normal Python instance stores its attributes in object's __dict__.
        # Read that descriptor directly so a fake instance property is not called.
        if not saw_instance_dict:
            descriptor = _class_dict(object).get("__dict__")
            if isinstance(descriptor, types.GetSetDescriptorType):
                try:
                    state = descriptor.__get__(value, type(value))
                except (AttributeError, TypeError):
                    state = None
                if type(state) is dict:
                    children.extend(state.keys())
                    children.extend(state.values())
                    inspected = True
        if not inspected:
            return True
    return any(
        _inspectPythonState(child, watched, seen, budget) for child in children
    )


def _containsOwned(value, watched, seen=None):
    """Refuse references and Python payloads whose state cannot be inspected."""
    try:
        return _inspectPythonState(
            value,
            watched,
            set() if seen is None else seen,
            [0],
        )
    except Exception:
        return True


# The walker follows Python's lookup rules for ordinary code and fails
# closed where native save could call code it does not model. It does not
# try to defeat code written to deceive it, for example a spoofed __class__
# or a __slots__ object mutated after class creation.
def _class_mro(cls):
    """Real MRO via ``type``'s descriptor, not a metaclass override."""
    return _TYPE_MRO.__get__(cls)


def _class_dict(cls):
    """Real class dict via ``type``'s descriptor, not a metaclass override."""
    return _TYPE_DICT.__get__(cls)


def _mro_dict_entry(cls, name):
    """First class-dict entry for ``name`` along the MRO. Never the metaclass."""
    for klass in _class_mro(cls):
        attributes = _class_dict(klass)
        if name in attributes:
            return attributes[name]
    raise AttributeError(name)


def _class_dict_defines(cls, name):
    """True when ``name`` is defined on ``cls`` or a base other than ``object``."""
    for klass in _class_mro(cls):
        if klass is object:
            continue
        if name in _class_dict(klass):
            return True
    return False


def _instance_dict_contains(value, name):
    """True when ``name`` is in the instance dict. Does not call user lookup.

    On Python 3.13 the ``__dict__`` descriptor lives on the user class, not on
    ``object``. A non-descriptor ``__dict__`` cannot be read safely. A dict
    subclass is legal storage, but native lookup reads it directly and does
    not use its ``__contains__``. Both are treated as present so the caller
    fails closed.
    """
    cls = type(value)
    for klass in _class_mro(cls):
        attrs = _class_dict(klass)
        if "__dict__" not in attrs:
            continue
        descriptor = attrs["__dict__"]
        if not isinstance(descriptor, types.GetSetDescriptorType):
            return True
        try:
            state = descriptor.__get__(value, cls)
        except (AttributeError, TypeError):
            return False
        if type(state) is not dict:
            return True
        return name in state
    return False


def _dumps_function(value):
    """Plain class ``dumps`` function, None if absent, or ``_UNSAFE_SERIALIZER``.

    ``PropertyPythonObject::toString`` uses ``hasAttr("dumps")`` and
    ``getAttr("dumps")()`` on the instance (``PyObject_HasAttrString`` /
    ``PyObject_GetAttrString``). Only a plain function in the class dicts is
    called. A metaclass ``dumps`` is invisible to that lookup. A
    ``__getattribute__`` that is not ``object``'s or a C slot wrapper, an
    instance ``dumps`` (including one stored in a dict subclass), a
    non-function class ``dumps``, or a ``__getattr__`` that could answer
    ``dumps`` is refused and not called.
    """
    cls = type(value)
    try:
        getter = _mro_dict_entry(cls, "__getattribute__")
    except AttributeError:
        return _UNSAFE_SERIALIZER
    if (
        getter is not _OBJECT_GETATTRIBUTE
        and type(getter) is not types.WrapperDescriptorType
    ):
        return _UNSAFE_SERIALIZER
    if _instance_dict_contains(value, "dumps"):
        return _UNSAFE_SERIALIZER
    try:
        raw = _mro_dict_entry(cls, "dumps")
    except AttributeError:
        if _class_dict_defines(cls, "__getattr__"):
            return _UNSAFE_SERIALIZER
        return None
    if type(raw) is not types.FunctionType:
        return _UNSAFE_SERIALIZER
    return raw


def _state_without_dumps(value):
    """State native save serializes when ``dumps`` is absent.

    ``PropertyPythonObject::toString`` looks ``__getstate__`` up three times
    (hasAttr, then ``__func__``, then the call; PropertyPythonObject.cpp:255-262).
    Only a plain function in the class dicts is called, as ``raw(value)``.
    ``object.__getstate__`` means there is no state function. On Python 3.10
    that entry is absent and ``_OBJECT_GETSTATE`` is None, which hits this
    same branch. An instance-dict entry, property, classmethod, staticmethod,
    wrapper, or other callable is refused and not called.
    """
    if _instance_dict_contains(value, "__getstate__"):
        return _UNSAFE_SERIALIZER
    try:
        raw = _mro_dict_entry(type(value), "__getstate__")
    except AttributeError:
        raw = None
    if raw is None or raw is _OBJECT_GETSTATE:
        return getattr(value, "__dict__", value)
    if type(raw) is types.FunctionType:
        return raw(value)
    return _UNSAFE_SERIALIZER


def _python_state_blocks(value, watched):
    """True when live or serialized Python state cannot be kept after a move."""
    if _containsOwned(value, watched):
        return True
    try:
        dumps = _dumps_function(value)
        if dumps is _UNSAFE_SERIALIZER:
            return True
        if dumps is not None:
            # A plain dumps() is this feature's own serializer, which save calls too.
            state = dumps(value)
        else:
            state = _state_without_dumps(value)
            if state is _UNSAFE_SERIALIZER:
                return True
        if state is not value and _containsOwned(state, watched):
            return True
        json.dumps(state)
    except Exception:
        return True
    return False


def _foreign_inbound_leftovers(source, owned, component):
    """References in other documents that this command must not rewrite."""
    leftovers = []
    for doc in App.listDocuments().values():
        if doc == source:
            continue
        for obj in doc.Objects:
            for prop in obj.PropertiesList:
                if obj.getTypeIdOfProperty(prop) != "App::PropertyPythonObject":
                    continue
                if _python_state_blocks(
                    getattr(obj, prop),
                    [item for item in owned if item is not obj],
                ):
                    leftovers.append((obj.FullName, prop))
        refs_elsewhere = AssemblyApp._extractionReferences(
            doc.Objects,
            [(obj, obj, obj, obj, obj == component) for obj in owned],
        )
        leftovers.extend(
            (obj.FullName, prop)
            for obj, prop in refs_elsewhere["redirected"] + refs_elsewhere["leftover"]
        )
    return leftovers


def _native_inbound_outside(source, owned):
    """InList owners that live outside ``source``.

    A cross-document ``App::Link`` or ``PropertyXLink`` records the linking
    object on the target (``PropertyXLink::setValue`` calls ``_addBackLink``).
    Only that source-local list is read: no ``App.listDocuments()``, no other
    document's ``Objects``, and no Python state outside ``source``. The
    destination document is not exempt.
    """
    found = []
    seen = set()
    for obj in owned:
        for owner in obj.InList:
            document = owner.Document
            if document == source:
                continue
            marker = (owner.FullName, obj.Name)
            if marker in seen:
                continue
            seen.add(marker)
            found.append(marker)
    return found


def _unredirectable(entries):
    return ExtractionError(
        translate("Assembly", "These references cannot be redirected:")
        + "\n"
        + "\n".join(f"{obj}.{prop}" for obj, prop in entries),
        entries,
    )


def preflight(assembly, component, path):
    """Read-only validation; no document, transaction or file is created here."""
    parent = _parent(assembly, component)
    source = assembly.Document
    if source.HasPendingTransaction or source.getBookedTransactionID():
        raise ExtractionError(
            translate(
                "Assembly",
                "Finish the current operation before extracting a component.",
            )
        )
    if not source.FileName or not os.path.isfile(source.FileName):
        raise ExtractionError(
            translate(
                "Assembly", "Save the assembly document before extracting a component."
            )
        )
    path = os.path.abspath(os.fspath(path))
    if not path.lower().endswith(".fcstd"):
        raise ExtractionError(
            translate("Assembly", "Choose a FreeCAD document (.FCStd).")
        )
    directory = os.path.dirname(path)
    if os.path.exists(path) or not os.path.isdir(directory):
        raise ExtractionError(
            translate("Assembly", "Choose a new file in an existing folder.")
        )
    # Advisory only: mode bits can be stale. saveAsWithOutcome remains the
    # authoritative write failure after the destination document exists.
    if not os.access(directory, os.W_OK | os.X_OK):
        raise ExtractionError(
            translate("Assembly", "The destination folder is not writable.")
        )
    if any(
        d.FileName and os.path.realpath(d.FileName) == os.path.realpath(path)
        for d in App.listDocuments().values()
    ):
        raise ExtractionError(
            translate("Assembly", "The destination document is already open.")
        )
    owned = _owned(component)
    _refuse_assembly_links(owned)
    _refuse_skipped_recompute(source, _recompute_closure(component, owned))
    all_objects = source.Objects
    leftovers = []
    # A standalone file cannot retain local outbound dependencies on its former
    # assembly. Report these before changing the source or writing a file.
    for obj in owned:
        for target in obj.OutList:
            if target.Document == source and target not in owned:
                leftovers.append((obj.Name, target.Name))
            elif target.Document != source and (
                not target.Document.FileName
                or not os.path.isfile(target.Document.FileName)
            ):
                leftovers.append((obj.Name, target.FullName))
        for owner in obj.InList:
            if owner not in owned and hasattr(owner, "Group") and obj in owner.Group:
                if owner != parent:
                    leftovers.append((owner.Name, "Group"))
        for prop in obj.PropertiesList:
            if obj.getTypeIdOfProperty(prop) != "App::PropertyPythonObject":
                continue
            # Match PropertyPythonObject::toString(), which serializes JSON.
            # Native copy logs serialization failures but can discard the data.
            if _python_state_blocks(
                getattr(obj, prop), [item for item in all_objects if item is not obj]
            ):
                leftovers.append((obj.Name, prop))
    owners = [obj for obj in all_objects if obj not in owned]
    invalid = [obj.Name for obj in owned + owners if "Invalid" in obj.State]
    if invalid:
        raise ExtractionError(
            translate("Assembly", "Fix invalid objects before extracting a component:")
            + " "
            + ", ".join(invalid)
        )
    for obj in owners:
        for prop in obj.PropertiesList:
            if obj.getTypeIdOfProperty(prop) == "App::PropertyPythonObject":
                if _python_state_blocks(getattr(obj, prop), owned):
                    leftovers.append((obj.Name, prop))
    refs = AssemblyApp._extractionReferences(
        owners, [(obj, obj, obj, obj, obj == component) for obj in owned]
    )
    leftovers.extend((obj.Name, prop) for obj, prop in refs["leftover"])
    # Other documents would require a separate Undo. Refuse rather than silently
    # leaving a second document changed after Undo in the assembly.
    #
    # This full check runs twice on the GUI thread: here, and again immediately
    # before the source commit. A native link gained after that second check is
    # caught on the source owner, from InList, just before deletion. Opaque
    # Python references gained after the second GUI check are not covered.
    leftovers.extend(_foreign_inbound_leftovers(source, owned, component))
    if leftovers:
        raise ExtractionError(
            translate("Assembly", "These references cannot be redirected:")
            + "\n"
            + "\n".join(f"{obj}.{prop}" for obj, prop in leftovers),
            leftovers,
        )
    referenced = set(refs["referenced"])
    referenced.add(component)
    return parent, owned, owners, referenced, set(refs["shapes"]), path


_LOCAL, _TRACED, _ATTACHED = range(3)


def _geometryProperties(owner):
    properties = []
    if owner.TypeId == "PartDesign::ShapeBinder":
        properties.append(("Support", _TRACED if owner.TraceSupport else _LOCAL))
    if owner.isDerivedFrom("Sketcher::SketchObject"):
        properties.append(("ExternalGeometry", _LOCAL))
    if "AttachmentSupport" in owner.PropertiesList:
        properties.append(("AttachmentSupport", _ATTACHED))
    return properties


def _gui_thread():
    return bool(AssemblyApp._isGuiThread())


def _service_owner(pump):
    """Keep owner-to-GUI tasks moving without entering the scene graph.

    ``processEvents`` re-enters Coin while a commit replays new objects and
    does not return. An empty hop to an idle document's owner waits in
    ``serviceMarshalledTasks``, the same drain ``dispatchToOwner`` uses,
    without recomputing. Recomputing that document races the commit's
    read-only postcondition.
    """
    try:
        AssemblyApp._serviceDocumentOwner(pump)
    except Exception:
        time.sleep(0.02)


def _service_until_done(pump, done):
    while not done():
        _service_owner(pump)


def _wait_for_owner(pump, result):
    """Block until an async owner commit finishes without stalling the GUI.

    Cleanup must see a terminal result. ``commitCompatibilityMutationAsync``
    returns a dict with ``wait`` and no cancel handle. On the GUI thread,
    service owner tasks until that wait ends.
    """
    if not isinstance(result, dict) or not callable(result.get("wait")):
        return result
    if not _gui_thread():
        return result["wait"]()
    box = {}

    def wait():
        try:
            box["result"] = result["wait"]()
        except BaseException as exc:  # noqa: BLE001 - re-raised on the GUI thread
            box["error"] = exc

    thread = threading.Thread(target=wait, name="extract-owner-wait", daemon=True)
    thread.start()
    _service_until_done(pump, lambda: not thread.is_alive())
    thread.join()
    if "error" in box:
        raise box["error"]
    return box.get("result")


# Statuses of one source commitCompatibilityMutation for which the native
# transaction was rolled back, or never opened. The destination is discarded
# only for these, and only when _source_restored is also true. RollbackFailed,
# PublicationFailed, Cancelled, any other status, and a wait() that raises
# keep the destination and the file.
#
# ApplyFailed, RecomputeFailed, PostconditionFailed, and Conflict come back
# from abortAndRestore / finishFailedRecompute only while rollback.restored
# is true. A failed restore is replaced with RollbackFailed
# (DocumentCommitCoordinator.cpp:1167-1180, 1260-1334, 1463-1468, 1494-1537,
# 1650-1653). ApplyFailed before the transaction (1107-1116) has nothing to
# undo (Document.cpp:3525-3527 returns restored). Conflict before the
# transaction (1009-1013) never runs the callback.
# StaleDocument, InvalidPreparedEdit, Unsupported, and Busy are admission
# refusals before openNativeCommitTransaction succeeds
# (DocumentCollaborationService.cpp:1581-1863;
# DocumentCommitCoordinator.cpp:886-934, 949-1214). Busy at 1208-1214 is
# "refused to open the native transaction".
# PublicationFailed is not in this set: it is also returned after the native
# transaction has already committed (1730-1745). RollbackFailed means the
# restore failed, a persistent recompute probe failed, the operation escaped
# its transaction, or the commit was already poisoned (1170-1180, 1261-1274,
# 1324-1330, 1376-1384, 912-916). Cancelled is not produced on this path.
_SOURCE_ROLLBACK_STATUSES = frozenset(
    {
        "ApplyFailed",
        "RecomputeFailed",
        "PostconditionFailed",
        "Conflict",
        "StaleDocument",
        "InvalidPreparedEdit",
        "Unsupported",
        "Busy",
    }
)


def _commit_on_owner(document, callback, pump=None, **kwargs):
    """Run ``callback`` on ``document``'s owner thread and require a commit.

    A result dict that is not ``Committed`` raises ``ExtractionError`` with
    ``commit_status`` set. An exception from ``wait`` propagates unchanged,
    with no status.
    """
    result = _wait_for_owner(
        pump if pump is not None else document,
        document.commitCompatibilityMutationAsync(callback, **kwargs),
    )
    if not isinstance(result, dict) or result.get("status") != "Committed":
        message = result.get("message") if isinstance(result, dict) else str(result)
        status = result.get("status") if isinstance(result, dict) else None
        raise ExtractionError(
            message or translate("Assembly", "The component could not be extracted."),
            commit_status=status,
        )
    return result


def _require_settled(document, objects, message):
    """Native settled postcondition: valid, not touched, and nothing left to run.

    ``Committed`` already means the owner recompute reported no object error and
    ``mustExecute()`` was clear. A skipped recompute leaves Touched/Freezed
    objects that are not Invalid, so that status alone is not success.
    """
    if document.RecomputesFrozen:
        raise ExtractionError(message)
    pending = []
    for obj in objects:
        if obj is None:
            continue
        try:
            name, status, state, valid = _settled_observation(obj)
        except (AttributeError, ReferenceError, RuntimeError):
            continue
        if (
            not valid
            or status != "Valid"
            or "Invalid" in state
            or "Touched" in state
            or "Recompute" in state
        ):
            pending.append(name)
    if document.mustExecute() or pending:
        detail = ", ".join(pending)
        raise ExtractionError(message + (" " + detail if detail else ""))
    return True


def _settled_observation(obj):
    return obj.Name, obj.getStatusString(), set(obj.State), bool(obj.isValid())


def _save_destination(destination, path):
    return destination.saveAsWithOutcome(path, False)


def _object_names(document):
    return [obj.Name for obj in document.Objects]


def _quote_document_label(label):
    """Escape ``label`` the way ``App::quote`` writes a non-Python label.

    Mirrors the switch in ObjectIdentifier.cpp:54-92 with ``toPython`` false:
    tab, newline, carriage return, backslash, both quotes, and ``>``.
    """
    escaped = []
    for char in label:
        if char == "\t":
            escaped.append("\\t")
        elif char == "\n":
            escaped.append("\\n")
        elif char == "\r":
            escaped.append("\\r")
        elif char == "\\":
            escaped.append("\\\\")
        elif char == "'":
            escaped.append("\\'")
        elif char == '"':
            escaped.append('\\"')
        elif char == ">":
            escaped.append("\\>")
        else:
            escaped.append(char)
    return "".join(escaped)


def _expression_names_document(text, destination):
    """True when an expression string names ``destination``.

    Extraction writes ``<document>#<object>.Placement``. The label form is
    ``<<quoted-label>>#``, with the label escaped by ``App::quote``
    (ObjectIdentifier.cpp:54, non-Python). A value that is not text is
    treated as a reference so cleanup does not delete the file.
    """
    if not isinstance(text, str):
        return True
    if f"{destination.Name}#" in text:
        return True
    label = getattr(destination, "Label", None)
    if not isinstance(label, str) or not label:
        return False
    return f"<<{_quote_document_label(label)}>>#" in text


def _link_value_in_document(value, dest_name, depth=0):
    """True when a link-property value holds an object in document ``dest_name``.

    ``PropertyLink`` is a document object or None. ``PropertyLinkSub`` and
    ``PropertyXLinkSub``, including the hidden subclasses, are
    ``(object, subnames)``. Lists hold objects or those pairs. Dict keys and
    values are walked the same way. ``type()`` is the real layout, so a
    custom ``__class__`` is not treated as a container. Reading the object's
    document is fail-closed. Nesting past a few levels is a hit so a cyclic
    value cannot recurse without bound.
    """
    if depth > 8:
        return True
    if value is None or type(value) in (str, bytes, int, float, bool):
        return False
    if type(value) in (tuple, list):
        return any(
            _link_value_in_document(item, dest_name, depth + 1) for item in value
        )
    if type(value) is dict:
        nested = list(value.keys()) + list(value.values())
        return any(
            _link_value_in_document(item, dest_name, depth + 1) for item in nested
        )
    if not issubclass(type(value), App.DocumentObject):
        return False
    try:
        return getattr(value.Document, "Name", None) == dest_name
    except Exception:
        return True


def _points_at_document(obj, destination):
    """True when ``obj`` still has an outgoing reference to ``destination``.

    Python ``OutList`` is ``getOutList()`` with option 0
    (DocumentObject.cpp:579-586, 596-614). That includes every
    ``PropertyLinkBase`` whose scope is not hidden, cross-document
    ``PropertyXLink`` included, because ``OutListNoXLinked`` is not set
    (DocumentObject.h:514, DocumentObject.cpp:616-625). ``getLinks`` is
    called with ``all`` false, so ``LinkScope::Hidden`` links are omitted
    (PropertyLinks.h:190-198, PropertyLinks.cpp:901-910). Resolved
    expression dependencies are in that list too (DocumentObject.cpp:612-614).

    Hidden links are therefore read from the properties themselves. Every
    property whose type id starts with ``App::PropertyLink`` or
    ``App::PropertyXLink`` is included: ``PropertyLinkHidden``,
    ``PropertyLinkSubHidden``, ``PropertyLinkListHidden``, and
    ``PropertyXLinkSubHidden`` (Assembly joints store ``Reference1`` and
    ``Reference2`` in the last of these). A read that raises counts as a
    reference. An unresolved XLink whose target object is missing has no
    document object in the value (``PropertyXLinkSub::getPyObject`` returns
    None) and is not detected.

    ``ExpressionEngine`` text is checked because a ``Name#`` or
    ``<<quoted-label>>#`` reference can remain when it does not resolve into
    ``OutList``. The label spelling is ``App::quote`` (ObjectIdentifier.cpp:54).
    Spreadsheet formulas live on ``Spreadsheet::Sheet`` cells
    (``PropertySheet``), not in ``ExpressionEngine``, so ``getContents`` of
    every ``getUsedCells()`` address is checked the same way when the text
    starts with ``=``. ``Cell::getStringContent`` (Cell.cpp:248) prefixes a
    string literal with ``'`` and returns a bare number for a number; those
    are not expressions. A cell read that raises, or a non-string cell value,
    counts as a reference. That catches
    ``=hiddenref(<<Ext\\>Part>>#Pad.Length)``.
    """
    if destination is None:
        return False
    try:
        dest_name = destination.Name
    except (AttributeError, ReferenceError, RuntimeError):
        return False
    try:
        linked = obj.OutList
    except (AttributeError, ReferenceError, RuntimeError):
        return True
    for item in linked:
        try:
            document = item.Document
        except (AttributeError, ReferenceError, RuntimeError):
            return True
        if getattr(document, "Name", None) == dest_name:
            return True
    try:
        engine = obj.ExpressionEngine
    except (AttributeError, ReferenceError, RuntimeError):
        return True
    for entry in engine or ():
        text = entry[1] if isinstance(entry, (tuple, list)) and len(entry) > 1 else entry
        if _expression_names_document(text, destination):
            return True
    try:
        prop_names = obj.PropertiesList
    except Exception:
        return True
    for name in prop_names:
        try:
            type_id = obj.getTypeIdOfProperty(name)
            if not (
                type_id.startswith("App::PropertyLink")
                or type_id.startswith("App::PropertyXLink")
            ):
                continue
            value = getattr(obj, name)
        except Exception:
            return True
        if _link_value_in_document(value, dest_name):
            return True
    try:
        is_sheet = obj.isDerivedFrom("Spreadsheet::Sheet")
    except Exception:
        return True
    if not is_sheet:
        return False
    try:
        cells = obj.getUsedCells()
    except Exception:
        return True
    for address in cells or ():
        try:
            contents = obj.getContents(address)
        except Exception:
            return True
        # Cell.cpp:248: a string literal is "'...", a number is bare text.
        # Only a leading "=" is an expression. Non-strings stay fail-closed.
        if isinstance(contents, str) and not contents.startswith("="):
            continue
        if _expression_names_document(contents, destination):
            return True
    return False


def _source_restored(source, destination, original_names, names_before):
    """True when the original objects are back and nothing still points at the file.

    A group named ``ExtractionReferences`` left by an earlier extraction is
    already in ``names_before``. The name-set comparison catches a new one.
    """
    if any(source.getObject(name) is None for name in original_names):
        return False
    if set(_object_names(source)) != set(names_before):
        return False
    return not any(_points_at_document(obj, destination) for obj in source.Objects)


def _discard_unreferenced_destination(destination, path, written):
    if destination is not None and getattr(destination, "Name", None) in App.listDocuments():
        App.closeDocument(destination.Name)
    if written and path and os.path.isfile(path):
        os.unlink(path)


def _restore_link_visibility(link, visible):
    """Restore the visibility captured before the source objects were replaced.

    The link view provider is created when the commit replays and can mark the
    link shown. This stays outside the commit so the extraction is one undo step.
    """
    if bool(link.Visibility) != bool(visible):
        link.Visibility = visible


def _finish_failed_message(path, detail):
    message = (
        translate("Assembly", "The component was extracted to the file at")
        + f" {path}, "
        + translate(
            "Assembly",
            "but the assembly did not finish updating. Undo restores the "
            "assembly while the extracted file stays on disk.",
        )
    )
    detail = (detail or "").strip()
    if detail and detail not in message:
        message = message + "\n" + detail
    return message


def _unverified_rollback_message(path, detail):
    message = (
        translate(
            "Assembly",
            "The assembly could not be verified as restored.",
        )
        + " "
        + translate("Assembly", "The new document and the file at")
        + f" {path} "
        + translate("Assembly", "were kept.")
    )
    detail = (detail or "").strip()
    if detail and detail not in message:
        message = message + "\n" + detail
    return message


def extract(assembly, component, path):
    """extract(assembly, component, path) -> ExtractionResult (no GUI required).

    The caller must save the assembly first. Existing files are never overwritten.
    Unsupported ownership or references raise ExtractionError before any changes.
    One source-document owner commit owns replacement and deletion. A failed
    commit rolls that transaction back and leaves undo/redo history as it was.
    The new file is removed only when that source commit's status is one that
    guarantees rollback and the assembly is verified restored. RollbackFailed,
    any other status, a wait() that raised, or a failed verification keeps
    the new document and file. A failed destination commit leaves the source
    untouched and still uses the restoration check alone.
    If the commit succeeds but the assembly does not finish updating, the
    extraction and its file stay in place. Undo restores the assembly while
    the extracted file stays on disk.
    """
    parent, owned, owners, referenced, shape_targets, path = preflight(
        assembly, component, path
    )
    source = assembly.Document
    original_names = tuple(obj.Name for obj in owned)
    names_before = _object_names(source)
    previous_active = App.ActiveDocument
    destination = None
    written = False
    staging = None
    source_committed = False
    source_commit = False
    try:
        destination = App.newDocument("ExtractedComponent")
        # XLink uses QFileInfo.canonicalPath(), which requires an existing file.
        # Keep a private placeholder until import has resolved its relative paths.
        fd, staging = tempfile.mkstemp(suffix=".FCStd", dir=os.path.dirname(path))
        os.close(fd)
        copied = {}

        def copy_component():
            destination.Label = component.Label
            destination.FileName = staging
            copies = destination.copyObject(owned, False)
            if not isinstance(copies, (tuple, list)):
                copies = [copies]
            copied["objects"] = list(copies)

        def destination_settled():
            return _require_settled(
                destination,
                destination.Objects,
                translate("Assembly", "The extracted component could not be recomputed:"),
            )

        _commit_on_owner(
            destination,
            copy_component,
            pump=source,
            structural=True,
            recompute=True,
            postcondition=destination_settled,
        )
        copies = copied["objects"]
        mapping = dict(zip(owned, copies))
        copied_component = mapping[component]
        # The save API publishes exclusively and adopts the final identity and
        # canonical savepoint together. Assigning FileName after a staging save
        # leaves the savepoint bound to a temporary path. Saving is refused
        # while a commit is open, so this stays after the copy commit and still
        # runs on the destination owner inside saveAsWithOutcome.
        outcome = _save_destination(destination, path)
        if not outcome["success"]:
            raise ExtractionError(outcome["message"])
        written = True
        os.unlink(staging)
        staging = None

        result = {}

        def replace_source():
            replacements = {}
            forwarding = []
            labels = {}
            component_visible = bool(component.Visibility)
            for original in owned:
                if original != component and original not in referenced:
                    continue
                link = source.addObject("App::Link", original.Name + "Link")
                link.setLink(mapping[original])
                # Keep labels unique while both originals and replacements exist,
                # so label-based expressions bind to the replacement unambiguously.
                labels[link] = original.Label
                link.Label2 = original.Label2
                temporary_label = link.Name
                used_labels = {obj.Label for obj in source.Objects if obj != link}
                while temporary_label in used_labels:
                    temporary_label += "_"
                link.Label = temporary_label
                link.LinkTransform = False
                if hasattr(original, "Placement"):
                    link.Placement = original.Placement
                    if original != component:
                        link.setExpression(
                            "Placement",
                            f"{destination.Name}#{mapping[original].Name}.Placement",
                        )
                link.Visibility = original.Visibility if original == component else False
                replacements[original] = link
                if original != component:
                    forwarding.append(link)
            link = replacements[component]
            shape_proxy = None
            if component in shape_targets:
                import ExtractionShape

                shape_proxy = ExtractionShape.create(source, copied_component, link)
            # Native binders keep local shape coordinates for legacy sketch/shape
            # references and assembly coordinates for traced supports. Attachment
            # links retain hierarchy for supports such as Body.Pad.Face3.
            targets = {kind: set() for kind in (_LOCAL, _TRACED, _ATTACHED)}
            for owner in owners:
                for prop, kind in _geometryProperties(owner):
                    targets[kind].update(
                        target
                        for target in _linkedObjects(getattr(owner, prop))
                        if target in replacements
                    )
            geometry = {kind: {} for kind in targets}
            frames = []
            for kind in targets:
                for original in targets[kind]:
                    datum = original.TypeId in ("App::Plane", "App::Line", "App::Point")
                    type_id = (
                        original.TypeId
                        if datum
                        else (
                            "App::Link"
                            if kind == _ATTACHED
                            else "PartDesign::SubShapeBinder"
                        )
                    )
                    binder = source.addObject(
                        type_id,
                        original.Name + "Geometry",
                    )
                    if type_id == "App::Link":
                        binder.setLink(mapping[original])
                        binder.LinkTransform = False
                    elif not datum:
                        binder.Relative = False
                        binder.Support = [(mapping[original], [""])]
                    if kind != _LOCAL:
                        import ExtractionShape

                        frame = ExtractionShape.createFrame(
                            source, copied_component, mapping[original], link, parent
                        )
                        frames.append(frame)
                        binder.setExpression("Placement", f"{frame.Name}.Result")
                    elif hasattr(original, "Placement"):
                        binder.setExpression(
                            "Placement",
                            f"{destination.Name}#{mapping[original].Name}.Placement",
                        )
                    binder.Visibility = False
                    geometry[kind][original] = binder
            helpers = [obj for objects in geometry.values() for obj in objects.values()]
            if forwarding or helpers or shape_proxy:
                group = source.addObject("App::DocumentObjectGroup", "ExtractionReferences")
                group.Label = translate("Assembly", "Extracted component references")
                group.Group = (
                    forwarding + helpers + frames + ([shape_proxy] if shape_proxy else [])
                )
            refs = AssemblyApp._extractionReferences(
                owners,
                [
                    (
                        old,
                        new,
                        geometry[_LOCAL].get(old, new),
                        mapping[old],
                        old == component,
                        shape_proxy if old == component and shape_proxy else new,
                        geometry[_TRACED].get(old, new),
                        geometry[_ATTACHED].get(old, new),
                    )
                    for old, new in replacements.items()
                ],
                True,
            )
            if refs["leftover"]:
                raise ExtractionError(
                    translate("Assembly", "A reference could not be redirected."),
                    refs["leftover"],
                )
            # Native links gained after the second GUI-thread check. InList is
            # local to the owned objects; this does not walk other documents.
            # Raising rolls this commit back before any removal. Opaque Python
            # references gained after that GUI check are not covered.
            late = _native_inbound_outside(source, owned)
            if late:
                raise _unredirectable(late)
            # Copying preserves original names inside the new document; component-
            # rooted sub-element paths therefore retain their topology names.
            for name in original_names:
                if source.getObject(name) is not None:
                    source.removeObject(name)
            replacements[component].Visibility = component_visible
            result["visible"] = component_visible
            for replacement, label in labels.items():
                replacement.Label = label
            result["value"] = ExtractionResult(
                destination,
                copied_component,
                link,
                tuple(refs["redirected"]),
                forwarding_links=tuple(forwarding),
                geometry_binders=tuple(helpers),
                shape_proxy=shape_proxy,
            )
            result["checked"] = owners + forwarding + helpers + frames + [link]

        # The full other-document check, on this thread, after copying and
        # saving and before the source commit. Nothing in the source is
        # committed yet, so a refusal uses the non-committed cleanup.
        late = _foreign_inbound_leftovers(source, owned, component)
        if late:
            raise _unredirectable(late)
        source_commit = True
        # Eager recompute inside this commit refuses cross-document links
        # ("recompute the document directly, outside this collaborative
        # commit"). The rewrite is one undo transaction; the direct recompute
        # and visibility restore below are what finish the update. A failure
        # after the commit keeps the commit and the file: undo() would park
        # the extraction on the redo stack, and no Python API drops only that
        # redo entry. Undo of the kept commit restores the assembly; the
        # extracted file stays on disk.
        _commit_on_owner(
            source,
            replace_source,
            pump=destination,
            structural=True,
            recompute=False,
            label=translate("Assembly", "Extract component to file"),
        )
        source_committed = True
        source.recompute()
        _require_settled(
            source,
            result["checked"],
            translate("Assembly", "References could not be recomputed:"),
        )
        _restore_link_visibility(result["value"].link, result["visible"])
        return result["value"]
    except Exception as exc:
        if source_committed:
            # clearUndos() erases earlier history as well as redo. abortTransaction()
            # only cancels an open transaction. Keep the file while the source
            # references it, and leave the redo stack untouched.
            raise ExtractionError(_finish_failed_message(path, str(exc))) from exc
        # Destination failures never mutate the source. A source failure is
        # discarded only when the status proves the transaction was rolled back.
        proven = (
            isinstance(exc, ExtractionError)
            and exc.commit_status in _SOURCE_ROLLBACK_STATUSES
        )
        if (not source_commit or proven) and _source_restored(
            source, destination, original_names, names_before
        ):
            _discard_unreferenced_destination(destination, path, written)
            raise
        leftover = exc.leftover if isinstance(exc, ExtractionError) else ()
        raise ExtractionError(
            _unverified_rollback_message(path, str(exc)), leftover
        ) from exc
    finally:
        if staging and os.path.exists(staging):
            os.unlink(staging)
        if previous_active and previous_active.Name in App.listDocuments():
            App.setActiveDocument(previous_active.Name)
