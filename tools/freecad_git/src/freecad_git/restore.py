"""Restore Assembly ReviewNote / ReviewNoteGroup objects from a freecad-git sidecar.

The sidecar remains a review artifact; this module is an explicit import path that
recreates native FreeCAD objects and links from exported semantic fields.

Restore is atomic: the sidecar schema and Review Note structures are validated
before any document mutation, and all mutations run inside one FreeCAD
transaction that is aborted on any failure.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

from .errors import InvalidSchemaError, MalformedSidecarError
from .schema_validate import validate_sidecar_dict


REVIEW_NOTE_TYPE = "Assembly::ReviewNote"
REVIEW_NOTE_GROUP_TYPE = "Assembly::ReviewNoteGroup"

# App properties restored onto Assembly::ReviewNote.
_NOTE_APP_PROPERTIES = (
    "LabelText",
    "LocalAnchor",
    "TextPosition",
    "BasePosition",
    "Resolved",
    "LeaderPort",
    "JointSide",
    "AttachmentBroken",
)

# View-provider display properties restored when Gui is available.
_NOTE_VIEW_PROPERTIES = (
    "BackgroundColor",
    "TextColor",
    "FontName",
    "FontSize",
    "Frame",
    "Justification",
    "ShowInTree",
    "DisplayMode",
)


class RestoreError(ValueError):
    """Raised when Review Note restore cannot proceed or must roll back."""


def load_sidecar(path: Path | str) -> dict[str, Any]:
    """Load a `.FCStd.git.json` sidecar as a dict."""
    data = Path(path).read_text(encoding="utf-8")
    return json.loads(data)


def iter_review_note_objects(sidecar: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """Return `(name, object_entry)` pairs for ReviewNote and ReviewNoteGroup."""
    objects = sidecar.get("objects", {})
    result: list[tuple[str, dict[str, Any]]] = []
    for name in sorted(objects.keys()):
        entry = objects[name]
        if entry.get("type") in (REVIEW_NOTE_TYPE, REVIEW_NOTE_GROUP_TYPE):
            result.append((name, entry))
    return result


def find_group_owner(objects: dict[str, Any], group_name: str) -> str | None:
    """Find the Part/Assembly whose membership.group contains ``group_name``."""
    for name, entry in objects.items():
        members = entry.get("membership", {}).get("group", [])
        if isinstance(members, list) and group_name in members:
            return name
    return None


def find_note_group(group_entries: list[tuple[str, dict[str, Any]]], note_name: str) -> str | None:
    """Return the ReviewNoteGroup that lists ``note_name`` in membership order."""
    for gname, gentry in group_entries:
        members = gentry.get("membership", {}).get("group", [])
        if isinstance(members, list) and note_name in members:
            return gname
    return None


def _vector_from_json(value: Any):
    import FreeCAD as App

    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return App.Vector(float(value[0]), float(value[1]), float(value[2]))
    raise RestoreError(f"expected vector [x,y,z], got {value!r}")


def _set_enumeration(obj_or_prop, name_or_value: Any, value: Any = None) -> None:
    """Set an enumeration on a property object or via ``setattr`` fallback."""
    if value is None:
        # Legacy call shape: _set_enumeration(prop, value)
        prop = obj_or_prop
        value = name_or_value
        if isinstance(value, dict) and "value" in value:
            value = value["value"]
        if isinstance(value, str) and value.isdigit():
            value = int(value)
        if hasattr(prop, "setValue"):
            prop.setValue(value)
        else:
            raise RestoreError(f"enumeration target has no setValue: {prop!r}")
        return

    # Preferred: _set_enumeration(obj, name, value)
    if isinstance(value, dict) and "value" in value:
        value = value["value"]
    if isinstance(value, str) and value.isdigit():
        value = int(value)
    prop = None
    if hasattr(obj_or_prop, "getPropertyByName"):
        try:
            prop = obj_or_prop.getPropertyByName(name_or_value)
        except Exception:
            prop = None
    if prop is not None and hasattr(prop, "setValue"):
        prop.setValue(value)
    else:
        setattr(obj_or_prop, name_or_value, value)


def _set_color(obj, name: str, value: Any) -> None:
    if isinstance(value, dict) and value.get("type") == "color":
        packed = int(value.get("packed", "0"))
        prop = None
        if hasattr(obj, "getPropertyByName"):
            try:
                prop = obj.getPropertyByName(name)
            except Exception:
                prop = None
        if prop is not None and hasattr(prop, "setValue"):
            prop.setValue(packed)
            return
        setattr(obj, name, packed)
        return
    setattr(obj, name, value)


def _apply_view_properties(obj, view_props: dict[str, Any]) -> None:
    vo = getattr(obj, "ViewObject", None)
    if vo is None or not view_props:
        return
    for name in _NOTE_VIEW_PROPERTIES:
        if name not in view_props or not hasattr(vo, name):
            continue
        value = view_props[name]
        try:
            if name in ("BackgroundColor", "TextColor"):
                _set_color(vo, name, value)
            elif name in ("FontSize",) and not isinstance(value, (int, float)):
                setattr(vo, name, float(value))
            elif name in ("Justification", "DisplayMode") and isinstance(value, dict):
                _set_enumeration(vo, name, value)
            elif name == "Frame":
                setattr(vo, name, bool(value))
            else:
                setattr(vo, name, value)
        except Exception as exc:
            raise RestoreError(f"failed to set view property {name}: {exc}") from exc


def _validate_vector_shape(name: str, value: Any) -> None:
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        raise RestoreError(f"{name} must be a 3-element vector, got {value!r}")
    try:
        float(value[0])
        float(value[1])
        float(value[2])
    except (TypeError, ValueError) as exc:
        raise RestoreError(f"{name} has non-numeric components: {value!r}") from exc


def _validate_target_shape(note_name: str, target_value: Any) -> str:
    if not isinstance(target_value, dict):
        raise RestoreError(f"{note_name}.Target must be an object, got {target_value!r}")
    ttype = target_value.get("type")
    if ttype == "link_sub":
        obj_name = target_value.get("object", "")
        if not isinstance(obj_name, str) or not obj_name:
            raise RestoreError(f"{note_name}.Target link_sub missing object name")
        return obj_name
    if ttype == "link":
        obj_name = target_value.get("target", "")
        if not isinstance(obj_name, str) or not obj_name:
            raise RestoreError(f"{note_name}.Target link missing target name")
        return obj_name
    raise RestoreError(f"{note_name}.Target has unsupported type {ttype!r}")


def _validate_note_entry(note_name: str, entry: dict[str, Any]) -> str | None:
    """Validate ReviewNote-specific structures; return Target object name if any."""
    if entry.get("type") != REVIEW_NOTE_TYPE:
        raise RestoreError(f"{note_name} is not a {REVIEW_NOTE_TYPE}")
    props = entry.get("properties", {})
    if props is None:
        props = {}
    if not isinstance(props, dict):
        raise RestoreError(f"{note_name}.properties must be an object")

    if "LabelText" in props:
        label = props["LabelText"]
        if not isinstance(label, (list, str)):
            raise RestoreError(f"{note_name}.LabelText must be a string or list")

    for key in ("LocalAnchor", "TextPosition", "BasePosition"):
        if key in props:
            _validate_vector_shape(f"{note_name}.{key}", props[key])

    for key in ("Resolved", "AttachmentBroken"):
        if key in props and not isinstance(props[key], bool):
            raise RestoreError(f"{note_name}.{key} must be a boolean")

    if "LeaderPort" in props:
        try:
            float(props["LeaderPort"])
        except (TypeError, ValueError) as exc:
            raise RestoreError(f"{note_name}.LeaderPort must be numeric") from exc

    if "Target" in props:
        return _validate_target_shape(note_name, props["Target"])
    return None


def _validate_group_entry(group_name: str, entry: dict[str, Any], note_names: set[str]) -> list[str]:
    if entry.get("type") != REVIEW_NOTE_GROUP_TYPE:
        raise RestoreError(f"{group_name} is not a {REVIEW_NOTE_GROUP_TYPE}")
    members = entry.get("membership", {}).get("group", [])
    if members is None:
        return []
    if not isinstance(members, list):
        raise RestoreError(f"{group_name}.membership.group must be a list")
    resolved: list[str] = []
    for member in members:
        if not isinstance(member, str) or not member:
            raise RestoreError(f"{group_name} membership contains invalid name {member!r}")
        if member not in note_names:
            raise RestoreError(
                f"{group_name} membership references unknown ReviewNote {member!r}"
            )
        resolved.append(member)
    return resolved


def _preflight_restore(
    doc,
    objects: dict[str, Any],
    group_entries: list[tuple[str, dict[str, Any]]],
    note_entries: list[tuple[str, dict[str, Any]]],
    *,
    replace_existing: bool,
) -> None:
    """Validate everything needed for restore before any document mutation."""
    note_names = {name for name, _ in note_entries}
    group_names = {name for name, _ in group_entries}

    # Name collisions between notes and groups in the sidecar itself.
    overlap = note_names & group_names
    if overlap:
        raise RestoreError(f"sidecar names used as both note and group: {sorted(overlap)}")

    for name, entry in group_entries:
        _validate_group_entry(name, entry, note_names)
        owner_name = find_group_owner(objects, name)
        if owner_name is not None and doc.getObject(owner_name) is None:
            raise RestoreError(f"ReviewNoteGroup owner not found in document: {owner_name!r}")

    for name, entry in note_entries:
        target_name = _validate_note_entry(name, entry)
        if target_name is not None and doc.getObject(target_name) is None:
            raise RestoreError(f"ReviewNote Target object not found: {target_name!r}")
        # Membership must resolve to at most one owning group; never pick arbitrarily.
        owners = [
            gname
            for gname, gentry in group_entries
            if name in (gentry.get("membership", {}).get("group") or [])
        ]
        if len(owners) > 1:
            raise RestoreError(
                f"ReviewNote {name!r} is listed in multiple groups: {owners}"
            )

    # Type-safe name collisions: fail before mutation; never remove wrong types.
    for name, expected in (
        *[(n, REVIEW_NOTE_TYPE) for n, _ in note_entries],
        *[(n, REVIEW_NOTE_GROUP_TYPE) for n, _ in group_entries],
    ):
        existing = doc.getObject(name)
        if existing is None:
            continue
        if existing.TypeId != expected:
            raise RestoreError(
                f"object {name!r} exists with type {existing.TypeId}, expected {expected}"
            )
        if not replace_existing and existing.TypeId == expected:
            # In-place update of matching Review Note objects is allowed.
            continue


def _ensure_group(doc, owner, group_name: str, entry: dict[str, Any]):
    existing = doc.getObject(group_name)
    if existing is not None:
        if existing.TypeId != REVIEW_NOTE_GROUP_TYPE:
            raise RestoreError(
                f"object {group_name!r} exists with type {existing.TypeId}, "
                f"expected {REVIEW_NOTE_GROUP_TYPE}"
            )
        group = existing
    else:
        if owner is not None and hasattr(owner, "newObject"):
            group = owner.newObject(REVIEW_NOTE_GROUP_TYPE, group_name)
        else:
            group = doc.addObject(REVIEW_NOTE_GROUP_TYPE, group_name)
            if owner is not None and hasattr(owner, "addObject"):
                owner.addObject(group)
    if "label" in entry:
        group.Label = entry["label"]
    if "visibility" in entry and group.ViewObject:
        group.ViewObject.Visibility = bool(entry["visibility"])
    _apply_view_properties(group, entry.get("view", {}))
    return group


def _apply_target(note, target_value: Any, doc) -> None:
    if not isinstance(target_value, dict):
        return
    if target_value.get("type") == "link_sub":
        obj_name = target_value.get("object", "")
        sub = target_value.get("subelement") or (
            (target_value.get("subelements") or [""])[0]
        )
        target_obj = doc.getObject(obj_name) if obj_name else None
        if target_obj is None:
            raise RestoreError(f"ReviewNote Target object not found: {obj_name!r}")
        note.Target = (target_obj, [sub] if sub else [])
        return
    if target_value.get("type") == "link":
        obj_name = target_value.get("target", "")
        target_obj = doc.getObject(obj_name) if obj_name else None
        if target_obj is None:
            raise RestoreError(f"ReviewNote Target object not found: {obj_name!r}")
        note.Target = (target_obj, [])


def _ensure_note(doc, group, note_name: str, entry: dict[str, Any]):
    existing = doc.getObject(note_name)
    if existing is not None:
        if existing.TypeId != REVIEW_NOTE_TYPE:
            raise RestoreError(
                f"object {note_name!r} exists with type {existing.TypeId}, "
                f"expected {REVIEW_NOTE_TYPE}"
            )
        note = existing
    else:
        note = doc.addObject(REVIEW_NOTE_TYPE, note_name)
    if group is not None and note not in list(group.Group):
        group.addObject(note)

    if "label" in entry:
        note.Label = entry["label"]
    props = entry.get("properties", {}) or {}

    if "LabelText" in props:
        lines = props["LabelText"]
        if isinstance(lines, list):
            note.LabelText = [str(line) for line in lines]
        else:
            note.LabelText = [str(lines)]

    for key in ("LocalAnchor", "TextPosition", "BasePosition"):
        if key in props:
            setattr(note, key, _vector_from_json(props[key]))

    if "Resolved" in props:
        note.Resolved = bool(props["Resolved"])
    if "AttachmentBroken" in props:
        note.AttachmentBroken = bool(props["AttachmentBroken"])
    if "LeaderPort" in props:
        note.LeaderPort = float(props["LeaderPort"])
    if "JointSide" in props:
        _set_enumeration(note, "JointSide", props["JointSide"])

    if "Target" in props:
        _apply_target(note, props["Target"], doc)

    if "visibility" in entry and note.ViewObject:
        note.ViewObject.Visibility = bool(entry["visibility"])
    _apply_view_properties(note, entry.get("view", {}))
    return note


def _remove_replaceable(doc, name: str, expected_type: str) -> None:
    existing = doc.getObject(name)
    if existing is None:
        return
    if existing.TypeId != expected_type:
        # Should have been caught in preflight; refuse rather than delete.
        raise RestoreError(
            f"refusing to remove {name!r}: type {existing.TypeId} != {expected_type}"
        )
    doc.removeObject(name)


def restore_review_notes(
    doc,
    sidecar: dict[str, Any],
    *,
    replace_existing: bool = False,
    validate_schema: bool = True,
) -> dict[str, Any]:
    """Recreate ReviewNoteGroup / ReviewNote objects from a sidecar dict.

    Parameters
    ----------
    doc:
        An open ``App.Document``.
    sidecar:
        Parsed `.FCStd.git.json` content. Never mutated.
    replace_existing:
        When True, remove existing objects **only** when their TypeId matches
        ``Assembly::ReviewNote`` / ``Assembly::ReviewNoteGroup`` for the sidecar
        names, then recreate them. Wrong-type name collisions fail before any
        mutation and leave the unrelated object untouched. On any failure after
        removals begin, the FreeCAD transaction is aborted so originals return.
    validate_schema:
        When True (default), validate the sidecar against the freecad-git schema
        before mutation.

    Returns
    -------
    dict with keys ``groups``, ``notes``, and ``owners``.
    """
    if validate_schema:
        try:
            validate_sidecar_dict(sidecar)
        except (InvalidSchemaError, MalformedSidecarError):
            raise
        except Exception as exc:  # pragma: no cover - defensive
            raise MalformedSidecarError(f"sidecar validation failed: {exc}") from exc

    # Work on a deep copy so callers never observe temporary restore fields.
    objects = copy.deepcopy(sidecar.get("objects", {}))
    if not isinstance(objects, dict):
        raise RestoreError("sidecar objects must be a mapping")

    groups_out: list[str] = []
    notes_out: list[str] = []
    owners: dict[str, str | None] = {}

    group_entries = [
        (name, entry)
        for name, entry in objects.items()
        if isinstance(entry, dict) and entry.get("type") == REVIEW_NOTE_GROUP_TYPE
    ]
    note_entries = [
        (name, entry)
        for name, entry in objects.items()
        if isinstance(entry, dict) and entry.get("type") == REVIEW_NOTE_TYPE
    ]

    # Local membership plan — never write `_pending_members` into sidecar entries.
    pending_members: dict[str, list[str]] = {}
    for name, entry in group_entries:
        members = entry.get("membership", {}).get("group", [])
        pending_members[name] = list(members) if isinstance(members, list) else []

    _preflight_restore(
        doc,
        objects,
        group_entries,
        note_entries,
        replace_existing=replace_existing,
    )

    opened_txn = False
    try:
        if hasattr(doc, "openTransaction"):
            doc.openTransaction("Restore Review Notes")
            opened_txn = True

        if replace_existing:
            # Remove notes before groups so group membership clears cleanly.
            for name, _ in note_entries:
                _remove_replaceable(doc, name, REVIEW_NOTE_TYPE)
            for name, _ in group_entries:
                _remove_replaceable(doc, name, REVIEW_NOTE_GROUP_TYPE)

        group_objs: dict[str, Any] = {}
        for name, entry in sorted(group_entries, key=lambda item: item[0]):
            owner_name = find_group_owner(objects, name)
            owner = doc.getObject(owner_name) if owner_name else None
            group = _ensure_group(doc, owner, name, entry)
            group_objs[name] = group
            groups_out.append(name)
            owners[name] = owner_name

        for name, entry in sorted(note_entries, key=lambda item: item[0]):
            gname = find_note_group(group_entries, name)
            group = group_objs.get(gname) if gname else None
            # No fallback to an arbitrary first group when membership is absent.
            note = _ensure_note(doc, group, name, entry)
            notes_out.append(name)

        for name, members in pending_members.items():
            group = group_objs.get(name)
            if group is None:
                continue
            resolved = []
            for member_name in members:
                obj = doc.getObject(member_name)
                if obj is None:
                    raise RestoreError(
                        f"group {name!r} member missing after create: {member_name!r}"
                    )
                resolved.append(obj)
            group.Group = resolved

        doc.recompute()

        if opened_txn and hasattr(doc, "commitTransaction"):
            doc.commitTransaction()
            opened_txn = False
    except Exception:
        if opened_txn and hasattr(doc, "abortTransaction"):
            doc.abortTransaction()
        raise

    return {"groups": groups_out, "notes": notes_out, "owners": owners}


def restore_review_notes_from_file(doc, sidecar_path: Path | str, **kwargs) -> dict[str, Any]:
    """Load a sidecar file and restore Review Notes into ``doc``."""
    return restore_review_notes(doc, load_sidecar(sidecar_path), **kwargs)


def restore_review_notes_into_fcstd(
    sidecar_path: Path | str,
    fcstd_path: Path | str,
    *,
    replace_existing: bool = False,
    save: bool = True,
    close: bool = True,
) -> dict[str, Any]:
    """Open ``fcstd_path`` in the *current* FreeCAD process, restore, optionally save.

    This is the supported CLI / FreeCADCmd entry point. A console process cannot
    see documents open in another FreeCAD GUI process; pass an explicit `.FCStd`
    path instead.

    Overwrite / in-place behavior
    -----------------------------
    When ``save`` is True (default), the opened document is saved back to
    ``fcstd_path`` (in-place overwrite of that file). The sidecar JSON is never
    modified. Set ``save=False`` to restore into the open document only.
    """
    import FreeCAD as App

    fcstd_path = Path(fcstd_path).resolve()
    sidecar_path = Path(sidecar_path).resolve()
    if not fcstd_path.is_file():
        raise RestoreError(f"destination FCStd not found: {fcstd_path}")
    if not sidecar_path.is_file():
        raise RestoreError(f"sidecar not found: {sidecar_path}")

    doc = App.openDocument(str(fcstd_path))
    try:
        result = restore_review_notes_from_file(
            doc, sidecar_path, replace_existing=replace_existing
        )
        if save:
            doc.save()
        result["document"] = doc.Name
        result["fcstd"] = str(fcstd_path)
        return result
    finally:
        if close:
            try:
                App.closeDocument(doc.Name)
            except Exception:
                pass
