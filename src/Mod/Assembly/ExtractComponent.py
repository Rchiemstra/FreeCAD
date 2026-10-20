# SPDX-License-Identifier: LGPL-2.1-or-later
"""Extract a local assembly component, preserving references and one-step Undo.

The saved component file is an independent artifact: Undo restores the source
model but deliberately leaves that file on disk. Redo uses the same document.
"""

from dataclasses import dataclass
import json
import os
import tempfile

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


def _parent(assembly, component):
    if (
        assembly is None
        or not assembly.isDerivedFrom("Assembly::AssemblyObject")
        or component is None
        or component == assembly
        or component.Document != assembly.Document
        or component.isDerivedFrom("App::Link")
        or not (
            component.isDerivedFrom("App::Part")
            or component.isDerivedFrom("Part::Feature")
        )
    ):
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


def _containsOwned(value, owned, seen=None):
    """Detect opaque Python state that the native link protocol cannot rewrite."""
    if any(value is obj for obj in owned):
        return True
    if value is None or isinstance(value, (str, bytes, int, float, bool)):
        return False
    if hasattr(value, "Document") and hasattr(value, "Name"):
        return False
    seen = set() if seen is None else seen
    if id(value) in seen:
        return False
    seen.add(id(value))
    if isinstance(value, dict):
        children = list(value.keys()) + list(value.values())
    elif isinstance(value, (tuple, list, set)):
        children = value
    else:
        children = getattr(value, "__dict__", {}).values()
    return any(_containsOwned(child, owned, seen) for child in children)


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
    if os.path.exists(path) or not os.path.isdir(os.path.dirname(path)):
        raise ExtractionError(
            translate("Assembly", "Choose a new file in an existing folder.")
        )
    if any(
        d.FileName and os.path.realpath(d.FileName) == os.path.realpath(path)
        for d in App.listDocuments().values()
    ):
        raise ExtractionError(
            translate("Assembly", "The destination document is already open.")
        )
    owned = _owned(component)
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
            value = getattr(obj, prop)
            # Match PropertyPythonObject::toString(), which serializes JSON.
            # Native copy logs serialization failures but can discard the data.
            try:
                if hasattr(value, "dumps"):
                    state = value.dumps()
                elif hasattr(getattr(value, "__getstate__", None), "__func__"):
                    state = value.__getstate__()
                else:
                    state = getattr(value, "__dict__", value)
                if _containsOwned(state, all_objects):
                    leftovers.append((obj.Name, prop))
                else:
                    json.dumps(state)
            except Exception:
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
                if _containsOwned(getattr(obj, prop), owned):
                    leftovers.append((obj.Name, prop))
    refs = AssemblyApp._extractionReferences(
        owners, [(obj, obj, obj, obj, obj == component) for obj in owned]
    )
    leftovers.extend((obj.Name, prop) for obj, prop in refs["leftover"])
    # Other documents would require a separate Undo. Refuse rather than silently
    # leaving a second document changed after Undo in the assembly.
    for doc in App.listDocuments().values():
        if doc != source:
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


def _geometryProperties(owner):
    properties = []
    if owner.TypeId == "PartDesign::ShapeBinder":
        properties.append(("Support", _TRACED if owner.TraceSupport else _LOCAL))
    if owner.isDerivedFrom("Sketcher::SketchObject"):
        properties.append(("ExternalGeometry", _LOCAL))
    if "AttachmentSupport" in owner.PropertiesList:
        properties.append(("AttachmentSupport", _ATTACHED))
    return properties


def extract(assembly, component, path):
    """extract(assembly, component, path) -> ExtractionResult (no GUI required).

    The caller must save the assembly first. Existing files are never overwritten.
    Unsupported ownership or references raise ExtractionError before any changes.
    One source-document transaction owns all replacement and deletion operations.
    """
    parent, owned, owners, referenced, shape_targets, path = preflight(
        assembly, component, path
    )
    source = assembly.Document
    original_names = tuple(obj.Name for obj in owned)
    previous_active = App.ActiveDocument
    destination = None
    transaction = False
    written = False
    staging = None
    try:
        destination = App.newDocument("ExtractedComponent")
        destination.Label = component.Label
        # XLink uses QFileInfo.canonicalPath(), which requires an existing file.
        # Keep a private placeholder until import has resolved its relative paths.
        fd, staging = tempfile.mkstemp(suffix=".FCStd", dir=os.path.dirname(path))
        os.close(fd)
        destination.FileName = staging
        if not source.UndoMode:
            source.UndoMode = 1
        source.openTransaction(translate("Assembly", "Extract component to file"))
        transaction = True
        copies = destination.copyObject(owned, False)
        if not isinstance(copies, (tuple, list)):
            copies = [copies]
        mapping = dict(zip(owned, copies))
        copied = mapping[component]
        destination.recompute(None, True)
        invalid = [obj.Name for obj in destination.Objects if "Invalid" in obj.State]
        if invalid:
            raise ExtractionError(
                translate(
                    "Assembly", "The extracted component could not be recomputed:"
                )
                + " "
                + ", ".join(invalid)
            )
        # Serialize before replacing source objects. Export UUID metadata is
        # included in the source transaction too. Publish with an
        # exclusive hard link so a racing writer can never be overwritten.
        os.unlink(staging)
        destination.saveAs(staging)
        os.link(staging, path)
        written = True
        destination.FileName = path
        os.unlink(staging)
        staging = None

        replacements = {}
        forwarding = []
        labels = {}
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

            shape_proxy = ExtractionShape.create(source, copied, link)
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
                        source, copied, mapping[original], link, parent
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
        for name in original_names:
            if source.getObject(name) is not None:
                source.removeObject(name)
        for replacement, label in labels.items():
            replacement.Label = label
        source.recompute()
        invalid = [
            obj.Name
            for obj in owners + forwarding + helpers + frames
            if "Invalid" in obj.State
        ]
        if invalid:
            raise ExtractionError(
                translate("Assembly", "References could not be recomputed:")
                + " "
                + ", ".join(invalid)
            )
        source.commitTransaction()
        transaction = False
        return ExtractionResult(
            destination,
            copied,
            link,
            tuple(refs["redirected"]),
            forwarding_links=tuple(forwarding),
            geometry_binders=tuple(helpers),
            shape_proxy=shape_proxy,
        )
    except Exception:
        if transaction:
            source.abortTransaction()
        if destination:
            App.closeDocument(destination.Name)
        if written:
            os.unlink(path)
        raise
    finally:
        if staging and os.path.exists(staging):
            os.unlink(staging)
        if previous_active and previous_active.Name in App.listDocuments():
            App.setActiveDocument(previous_active.Name)
