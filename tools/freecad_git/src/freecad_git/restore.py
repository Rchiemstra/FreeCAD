"""Restore Assembly ReviewNote / ReviewNoteGroup objects from a freecad-git sidecar.

The sidecar remains a review artifact; this module is an explicit import path that
recreates native FreeCAD objects and links from exported semantic fields.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


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


def _vector_from_json(value: Any):
    import FreeCAD as App

    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return App.Vector(float(value[0]), float(value[1]), float(value[2]))
    raise ValueError(f"expected vector [x,y,z], got {value!r}")


def _set_enumeration(prop, value: Any) -> None:
    if isinstance(value, dict) and "value" in value:
        value = value["value"]
    if isinstance(value, str) and value.isdigit():
        prop.setValue(int(value))
    else:
        prop.setValue(value)


def _set_color(prop, value: Any) -> None:
    if isinstance(value, dict) and value.get("type") == "color":
        packed = int(value.get("packed", "0"))
        prop.setValue(packed)
        return
    prop.setValue(value)


def _apply_view_properties(obj, view_props: dict[str, Any]) -> None:
    vo = getattr(obj, "ViewObject", None)
    if vo is None or not view_props:
        return
    for name in _NOTE_VIEW_PROPERTIES:
        if name not in view_props or not hasattr(vo, name):
            continue
        value = view_props[name]
        prop = getattr(vo, name)
        if name in ("BackgroundColor", "TextColor"):
            _set_color(vo.getPropertyByName(name), value)
        elif name in ("FontSize",) and not isinstance(value, (int, float)):
            setattr(vo, name, float(value))
        elif name in ("Justification", "DisplayMode") and isinstance(value, dict):
            _set_enumeration(vo.getPropertyByName(name), value)
        elif name == "Frame":
            setattr(vo, name, bool(value))
        else:
            setattr(vo, name, value)


def _ensure_group(doc, owner, group_name: str, entry: dict[str, Any]):
    existing = doc.getObject(group_name)
    if existing is not None:
        if existing.TypeId != REVIEW_NOTE_GROUP_TYPE:
            raise TypeError(
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
            raise ValueError(f"ReviewNote Target object not found: {obj_name!r}")
        note.Target = (target_obj, [sub] if sub else [])
        return
    if target_value.get("type") == "link":
        obj_name = target_value.get("target", "")
        target_obj = doc.getObject(obj_name) if obj_name else None
        if target_obj is None:
            raise ValueError(f"ReviewNote Target object not found: {obj_name!r}")
        note.Target = (target_obj, [])


def _ensure_note(doc, group, note_name: str, entry: dict[str, Any]):
    existing = doc.getObject(note_name)
    if existing is not None:
        if existing.TypeId != REVIEW_NOTE_TYPE:
            raise TypeError(
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
    props = entry.get("properties", {})

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
        _set_enumeration(note.getPropertyByName("JointSide"), props["JointSide"])

    if "Target" in props:
        _apply_target(note, props["Target"], doc)

    if "visibility" in entry and note.ViewObject:
        note.ViewObject.Visibility = bool(entry["visibility"])
    _apply_view_properties(note, entry.get("view", {}))
    return note


def restore_review_notes(
    doc,
    sidecar: dict[str, Any],
    *,
    replace_existing: bool = False,
) -> dict[str, Any]:
    """Recreate ReviewNoteGroup / ReviewNote objects from a sidecar dict.

    Parameters
    ----------
    doc:
        An open ``App.Document``.
    sidecar:
        Parsed `.FCStd.git.json` content.
    replace_existing:
        When True, remove existing ReviewNote/ReviewNoteGroup objects named in the
        sidecar before recreating them.

    Returns
    -------
    dict with keys ``groups``, ``notes``, and ``owners``.
    """
    objects = sidecar.get("objects", {})
    groups_out: list[str] = []
    notes_out: list[str] = []
    owners: dict[str, str | None] = {}

    group_entries = [
        (name, entry)
        for name, entry in objects.items()
        if entry.get("type") == REVIEW_NOTE_GROUP_TYPE
    ]
    note_entries = [
        (name, entry)
        for name, entry in objects.items()
        if entry.get("type") == REVIEW_NOTE_TYPE
    ]

    if replace_existing:
        for name, _ in note_entries + group_entries:
            obj = doc.getObject(name)
            if obj is not None:
                doc.removeObject(name)

    # Create groups first so notes can join membership.
    group_objs: dict[str, Any] = {}
    for name, entry in sorted(group_entries, key=lambda item: item[0]):
        owner_name = find_group_owner(objects, name)
        owner = doc.getObject(owner_name) if owner_name else None
        group = _ensure_group(doc, owner, name, entry)
        group_objs[name] = group
        groups_out.append(name)
        owners[name] = owner_name

        # Restore declared membership order after notes exist.
        members = entry.get("membership", {}).get("group", [])
        # Stash for second pass.
        entry["_pending_members"] = list(members) if isinstance(members, list) else []

    for name, entry in sorted(note_entries, key=lambda item: item[0]):
        # Prefer the group that lists this note.
        group = None
        for gname, gentry in group_entries:
            pending = gentry.get("_pending_members") or gentry.get("membership", {}).get(
                "group", []
            )
            if name in pending:
                group = group_objs.get(gname)
                break
        if group is None and group_objs:
            # Fallback: sole group in document.
            group = next(iter(group_objs.values()))
        note = _ensure_note(doc, group, name, entry)
        notes_out.append(name)

    # Apply group membership in sidecar order.
    for name, entry in group_entries:
        group = group_objs.get(name)
        members = entry.pop("_pending_members", None)
        if group is None or members is None:
            continue
        resolved = []
        for member_name in members:
            obj = doc.getObject(member_name)
            if obj is not None:
                resolved.append(obj)
        group.Group = resolved

    doc.recompute()
    return {"groups": groups_out, "notes": notes_out, "owners": owners}


def restore_review_notes_from_file(doc, sidecar_path: Path | str, **kwargs) -> dict[str, Any]:
    """Load a sidecar file and restore Review Notes into ``doc``."""
    return restore_review_notes(doc, load_sidecar(sidecar_path), **kwargs)
