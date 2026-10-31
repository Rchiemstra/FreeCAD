# SPDX-License-Identifier: LGPL-2.1-or-later
"""Extract a local assembly component, preserving references and one-step Undo.

The saved component file is an independent artifact: Undo restores the source
model but deliberately leaves that file on disk. Redo uses the same document.

Copy, property changes, deletion, and save run on each document owner.
The destination recompute is that commit's eager native recompute. The source
recompute runs after the rewrite commit, because an eager recompute inside it
refuses the new cross-document link. View providers of objects about to be
deleted are recorded on the GUI thread before those objects enter the undo
transaction, so Undo does not leak a second provider. Dialogs, selection, and
messages stay on the GUI thread.
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
    """A preflight refusal. ``leftover`` identifies the unsupported references."""

    def __init__(self, message, leftover=()):
        self.leftover = tuple(leftover)
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


def _inspectPythonState(value, watched, seen):
    """Detect opaque Python state that the native link protocol cannot rewrite.

    Returns true when ``value`` holds a watched object or its state cannot be
    inspected. Slots are read from every class in the MRO, including private
    names mangled as ``_Class__slot``.
    """
    if any(value is obj for obj in watched):
        return True
    if value is None or type(value) in (str, bytes, int, float, bool, complex):
        return False
    # Native geometry values store numbers, never document references.
    if type(value) in (App.Placement, App.Vector, App.Rotation, App.Matrix):
        return False
    if isinstance(value, App.DocumentObject):
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
            (cls, type.__getattribute__(cls, "__dict__"))
            for cls in type.__getattribute__(type(value), "__mro__")
            if cls is not object
        ]
        saw_instance_dict = False
        for cls, attributes in classes:
            # A C base can hold pointers that are not in slots or an instance dict.
            if attributes.get("__module__") == "builtins" or any(
                isinstance(
                    member,
                    (
                        types.BuiltinFunctionType,
                        types.MethodDescriptorType,
                        types.WrapperDescriptorType,
                        types.ClassMethodDescriptorType,
                    ),
                )
                for member in attributes.values()
            ):
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
            slots = attributes.get("__slots__")
            if slots is None:
                continue
            inspected = True
            if isinstance(slots, str):
                slots = (slots,)
            for name in slots:
                if name in ("__dict__", "__weakref__"):
                    if name == "__dict__":
                        saw_instance_dict = True
                    continue
                if name.startswith("__") and not name.endswith("__"):
                    name = "_" + type.__getattribute__(cls, "__name__").lstrip("_") + name
                descriptor = attributes.get(name)
                if not isinstance(descriptor, types.MemberDescriptorType):
                    return True
                try:
                    children.append(descriptor.__get__(value, type(value)))
                except AttributeError:
                    # An unassigned slot has no payload.
                    pass
        # A normal Python instance stores its attributes in object's __dict__.
        # Read that descriptor directly so a fake instance property is not called.
        if not saw_instance_dict:
            descriptor = type.__getattribute__(object, "__dict__").get("__dict__")
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
    return any(_inspectPythonState(child, watched, seen) for child in children)


def _containsOwned(value, watched, seen=None):
    """Refuse references and Python payloads whose state cannot be inspected."""
    try:
        return _inspectPythonState(value, watched, set() if seen is None else seen)
    except Exception:
        return True


def _python_state_blocks(value, watched):
    """True when live or serialized Python state cannot be kept after a move."""
    if _containsOwned(value, watched):
        return True
    try:
        if hasattr(value, "dumps"):
            state = value.dumps()
        elif hasattr(getattr(value, "__getstate__", None), "__func__"):
            state = value.__getstate__()
        else:
            state = getattr(value, "__dict__", value)
        if state is not value and _containsOwned(state, watched):
            return True
        json.dumps(state)
    except Exception:
        return True
    return False


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
    # Known limitation, shared with Insert Link, Insert New Part, and tree
    # copy-move: this check sees inbound links that already exist. Another
    # document can still gain an inbound link after preflight and before the
    # source objects are deleted. That link is outside this document's Undo.
    # Closing the window needs cross-document reservation, a separate base
    # project; this command does not add one.
    for doc in App.listDocuments().values():
        if doc != source:
            for obj in doc.Objects:
                for prop in obj.PropertiesList:
                    if obj.getTypeIdOfProperty(prop) == "App::PropertyPythonObject":
                        if _python_state_blocks(
                            getattr(obj, prop), [item for item in owned if item is not obj]
                        ):
                            leftovers.append((obj.FullName, prop))
            refs_elsewhere = AssemblyApp._extractionReferences(
                doc.Objects, [(obj, obj, obj, obj, obj == component) for obj in owned]
            )
            leftovers.extend(
                (obj.FullName, prop)
                for obj, prop in refs_elsewhere["redirected"]
                + refs_elsewhere["leftover"]
            )
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


_record_removed_view_providers = None


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


def _service_until(pump, done, timeout):
    """Keep owner-to-GUI tasks moving without entering the scene graph.

    ``processEvents`` re-enters Coin while a commit replays new objects and
    does not return. An empty hop to an idle document's owner waits in
    ``serviceMarshalledTasks``, the same drain ``dispatchToOwner`` uses,
    without recomputing. Recomputing that document races the commit's
    read-only postcondition.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if done():
            return
        try:
            AssemblyApp._serviceDocumentOwner(pump)
        except Exception:
            time.sleep(0.02)
        if done():
            return
    raise ExtractionError(translate("Assembly", "Timed out while extracting the component."))


def _wait_for_owner(pump, result):
    """Block until an async owner commit finishes without stalling the GUI."""
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
    _service_until(pump, lambda: not thread.is_alive(), 180.0)
    thread.join()
    if "error" in box:
        raise box["error"]
    return box.get("result")


def _commit_on_owner(document, callback, pump=None, **kwargs):
    """Run ``callback`` on ``document``'s owner thread and require a commit."""
    result = _wait_for_owner(
        pump if pump is not None else document,
        document.commitCompatibilityMutationAsync(callback, **kwargs),
    )
    if not isinstance(result, dict) or result.get("status") != "Committed":
        message = result.get("message") if isinstance(result, dict) else str(result)
        raise ExtractionError(
            message or translate("Assembly", "The component could not be extracted.")
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
            name = obj.Name
            status = obj.getStatusString()
            state = set(obj.State)
            valid = obj.isValid()
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


def extract(assembly, component, path):
    """extract(assembly, component, path) -> ExtractionResult (no GUI required).

    The caller must save the assembly first. Existing files are never overwritten.
    Unsupported ownership or references raise ExtractionError before any changes.
    One source-document owner commit owns replacement and deletion. A failed
    commit rolls that transaction back and leaves undo/redo history and
    ``UndoMode`` as they were.
    """
    parent, owned, owners, referenced, shape_targets, path = preflight(
        assembly, component, path
    )
    source = assembly.Document
    original_names = tuple(obj.Name for obj in owned)
    previous_active = App.ActiveDocument
    saved_undo_mode = source.UndoMode
    destination = None
    written = False
    staging = None
    source_committed = False
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
        outcome = destination.saveAsWithOutcome(path, False)
        if not outcome["success"]:
            raise ExtractionError(outcome["message"])
        written = True
        os.unlink(staging)
        staging = None

        result = {}

        def replace_source():
            # Record undo for this commit when the document had it disabled.
            # The setter is restored below if the commit does not stick.
            if not source.UndoMode:
                source.UndoMode = 1
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
            # Copying preserves original names inside the new document; component-
            # rooted sub-element paths therefore retain their topology names.
            # The commit defers transaction-remove until after the document
            # object is already in the undo transaction. The GUI registers a
            # recorder that puts view providers into that transaction first,
            # so Undo does not leak the provider signalNewObject creates.
            if _record_removed_view_providers is not None:
                _record_removed_view_providers(owned)
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
            result["checked"] = owners + forwarding + helpers + frames

        # Eager recompute inside this commit refuses cross-document links
        # ("recompute the document directly, outside this collaborative
        # commit"). The rewrite is one undo transaction; the direct recompute
        # below is what has to settle it. A failure there undoes that
        # transaction.
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
        # The link view provider is created when the commit replays and can
        # mark the link shown. Restore the visibility captured from the
        # source. The extraction stays one undo step.
        link = result["value"].link
        if bool(link.Visibility) != result["visible"]:
            link.Visibility = result["visible"]
        return result["value"]
    except Exception as exc:
        if source_committed:
            try:
                source.undo()
            except Exception:
                pass
        if source.UndoMode != saved_undo_mode:
            source.UndoMode = saved_undo_mode
        if destination is not None and destination.Name in App.listDocuments():
            App.closeDocument(destination.Name)
        if written:
            os.unlink(path)
        raise
    finally:
        if staging and os.path.exists(staging):
            os.unlink(staging)
        if previous_active and previous_active.Name in App.listDocuments():
            App.setActiveDocument(previous_active.Name)
