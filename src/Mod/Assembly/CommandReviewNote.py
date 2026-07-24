# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 The FreeCAD Project Association AISBL              *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""Assembly review-note commands and target normalization helpers."""

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

import UtilsAssembly

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtWidgets

__title__ = "Assembly Command Review Note"
__author__ = "The FreeCAD Project Association AISBL"
__url__ = "https://www.freecad.org"

translate = App.Qt.translate

JOINT_SIDE_NONE = "None"
JOINT_SIDE_REF1 = "Reference1"
JOINT_SIDE_REF2 = "Reference2"

_SUPPORTED_SUB_PREFIXES = ("Face", "Edge", "Vertex")


def _is_joint_object(obj):
    if obj is None:
        return False
    if hasattr(obj, "ObjectToGround"):
        return False
    return hasattr(obj, "JointType") and hasattr(obj, "Reference1") and hasattr(obj, "Reference2")


def _is_unsupported_target_object(obj):
    """True for containers, origins, LCS/datums, groups — not note-worthy components."""
    if obj is None:
        return True
    if obj.isDerivedFrom("Assembly::AssemblyObject"):
        return True
    if obj.isDerivedFrom("App::Origin"):
        return True
    if obj.isDerivedFrom("App::LocalCoordinateSystem"):
        return True
    if obj.isDerivedFrom("App::DatumElement"):
        return True
    if obj.isDerivedFrom("Part::Datum"):
        return True
    if obj.isDerivedFrom("App::DocumentObjectGroup"):
        return True
    if obj.TypeId in ("App::Line", "App::Plane", "App::OriginFeature"):
        return True
    return False


def _is_supported_geometry_sub(sub_name):
    """Accept empty (whole component) or a single Face/Edge/Vertex element name."""
    if not sub_name:
        return True
    # Reject multi-segment or dotted element paths (e.g. Face6.Extra).
    parts = [p for p in str(sub_name).split(".") if p]
    if len(parts) != 1:
        return False
    element = parts[0]
    return any(
        element.startswith(prefix) and element[len(prefix) :].isdigit()
        for prefix in _SUPPORTED_SUB_PREFIXES
    )


def _is_supported_component(obj):
    if obj is None or _is_unsupported_target_object(obj):
        return False
    if obj.isDerivedFrom("App::GeoFeature"):
        return True
    # App::Link is a common assembly component but is not a GeoFeature.
    if obj.isDerivedFrom("App::Link"):
        return True
    return hasattr(obj, "Placement") and (
        hasattr(obj, "LinkedObject") or hasattr(obj, "Shape")
    )


def _same_document(assembly, obj):
    return (
        assembly is not None
        and obj is not None
        and getattr(assembly, "Document", None) is not None
        and obj.Document == assembly.Document
    )


def _target_in_assembly(assembly, obj):
    if assembly is None or obj is None:
        return False
    if obj == assembly:
        return True
    try:
        return bool(assembly.hasObject(obj, True))
    except Exception:
        return False


def _selection_entries(selection=None):
    """Flatten SelectionObject list into (Object, SubElementName, PickedPoint|None) entries."""
    if selection is None:
        if not App.GuiUp:
            return []
        selection = Gui.Selection.getSelectionEx("*", 0)

    entries = []
    for sel in selection or []:
        obj = sel.Object
        subs = list(sel.SubElementNames) if sel.SubElementNames else [""]
        picks = list(sel.PickedPoints) if getattr(sel, "PickedPoints", None) else []
        for i, sub in enumerate(subs):
            pick = picks[i] if i < len(picks) else (picks[0] if picks else None)
            entries.append((obj, sub, pick))
    return entries


def normalize_review_note_target(assembly, root_obj, sub_name="", picked_point=None):
    """Normalize a selection into review-note target data.

    Returns a dict:
      target_obj, sub_list, local_anchor, joint_side
    or None if unsupported.

    Selection may report the Assembly (or other container) as Object with a
    child subpath such as ``Box.Face6``; resolve that before rejecting containers.
    """
    if assembly is None or root_obj is None:
        return None
    if not _same_document(assembly, root_obj):
        return None

    # Joint selected from the tree (no subelement) or 3D (synthetic Main).
    if _is_joint_object(root_obj):
        if not _target_in_assembly(assembly, root_obj):
            return None
        joint = root_obj
        element = (sub_name or "").split(".")[-1] if sub_name else ""
        if element and element not in ("", "Main"):
            return None

        side = JOINT_SIDE_REF1
        if element == "Main" and picked_point is not None:
            side = _nearest_joint_side(joint, picked_point)
        jcs_plc = joint.Placement1 if side == JOINT_SIDE_REF1 else joint.Placement2
        return {
            "target_obj": joint,
            "sub_list": ["Main"] if element == "Main" else [],
            "local_anchor": App.Vector(0, 0, 0),
            "joint_side": side,
            "jcs_placement": jcs_plc,
        }

    # Resolve container + subpath (e.g. Assembly + "Box.Face6") to a component.
    comp, rel_sub = UtilsAssembly.getComponentReference(assembly, root_obj, sub_name or "")
    if not comp:
        # Whole-component selection where root_obj is already the component.
        if (
            root_obj != assembly
            and _target_in_assembly(assembly, root_obj)
            and _is_supported_component(root_obj)
            and not _is_unsupported_target_object(root_obj)
        ):
            comp = root_obj
            rel_sub = sub_name or ""
        else:
            return None

    if not _target_in_assembly(assembly, comp):
        return None
    # Never anchor to the owning Assembly, LCS/origin/datum/group, etc.
    if comp == assembly or _is_unsupported_target_object(comp):
        return None

    # Reject joints mistakenly resolved as geometry.
    if _is_joint_object(comp):
        return normalize_review_note_target(assembly, comp, sub_name, picked_point)

    if not _is_supported_geometry_sub(rel_sub):
        return None

    sub_list = [rel_sub] if rel_sub else []
    local_anchor = _local_anchor_for_component(assembly, comp, rel_sub, picked_point)
    return {
        "target_obj": comp,
        "sub_list": sub_list,
        "local_anchor": local_anchor,
        "joint_side": JOINT_SIDE_NONE,
        "jcs_placement": None,
    }


def _nearest_joint_side(joint, picked_point):
    try:
        plc1 = UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)
        plc2 = UtilsAssembly.getJcsGlobalPlc(joint.Placement2, joint.Reference2)
    except Exception:
        return JOINT_SIDE_REF1

    d1 = (plc1.Base - picked_point).Length
    d2 = (plc2.Base - picked_point).Length
    return JOINT_SIDE_REF1 if d1 <= d2 else JOINT_SIDE_REF2


def _component_placement_in_assembly(assembly, component):
    """Return component Placement in Assembly-local coordinates (exclude Assembly Placement)."""
    parents = component.Parents
    if parents:
        for parent, sub in parents:
            if parent == assembly:
                # getPlacementOf includes the assembly's own Placement; strip it.
                full = assembly.getPlacementOf(sub, component)
                asm_plc = assembly.Placement if hasattr(assembly, "Placement") else App.Placement()
                return asm_plc.inverse() * full
    if hasattr(component, "Placement"):
        return component.Placement
    return App.Placement()


def _assembly_global_placement(assembly):
    if assembly is None:
        return App.Placement()
    try:
        if hasattr(assembly, "getGlobalPlacement"):
            return assembly.getGlobalPlacement()
    except Exception:
        pass
    return assembly.Placement if hasattr(assembly, "Placement") else App.Placement()


def _local_anchor_for_component(assembly, component, rel_sub, picked_point):
    plc = _component_placement_in_assembly(assembly, component)

    if picked_point is not None:
        # Convert world/document pick into assembly-local then component-local.
        # Use global placement so nested assemblies under a parent Placement work.
        asm_global = _assembly_global_placement(assembly)
        assembly_local = asm_global.inverse().multVec(picked_point)
        return plc.inverse().multVec(assembly_local)

    # Fallback: component bounding-box center in component-local coordinates.
    center = _component_bbox_center_local(component)
    return center


def _component_bbox_center_local(component):
    try:
        if hasattr(component, "Shape") and component.Shape is not None and not component.Shape.isNull():
            bbox = component.Shape.BoundBox
            if getattr(bbox, "isValid", lambda: True)():
                return App.Vector(bbox.Center)
    except Exception:
        pass

    # Deterministic fallback for simple Part primitives before Shape is ready.
    try:
        if hasattr(component, "Length") and hasattr(component, "Width") and hasattr(
            component, "Height"
        ):
            return App.Vector(
                float(component.Length) / 2.0,
                float(component.Width) / 2.0,
                float(component.Height) / 2.0,
            )
    except Exception:
        pass

    if App.GuiUp and hasattr(component, "ViewObject") and component.ViewObject:
        try:
            bbox = component.ViewObject.getBoundingBox()
            if bbox and bbox.isValid():
                if hasattr(component, "Placement"):
                    return component.Placement.inverse().multVec(bbox.Center)
                return App.Vector(bbox.Center)
        except Exception:
            pass

    return App.Vector(0, 0, 0)


def collect_review_note_targets(assembly, selection=None):
    """Return normalized targets for the current selection.

    Eligible when exactly one supported target is present.
    """
    entries = _selection_entries(selection)
    if not entries:
        return []

    targets = []
    seen = set()
    for root_obj, sub_name, pick in entries:
        # Joints living under the assembly joint group.
        if _is_joint_object(root_obj):
            key = ("joint", root_obj.Name, sub_name or "")
            if key in seen:
                continue
            seen.add(key)
            data = normalize_review_note_target(assembly, root_obj, sub_name, pick)
            if data:
                targets.append(data)
            continue

        data = normalize_review_note_target(assembly, root_obj, sub_name, pick)
        if not data:
            continue
        key = (
            data["target_obj"].Name,
            tuple(data["sub_list"]),
            data["joint_side"],
        )
        if key in seen:
            continue
        seen.add(key)
        targets.append(data)

    return targets


def is_add_review_note_eligible(assembly, selection=None):
    if assembly is None:
        return False
    targets = collect_review_note_targets(assembly, selection)
    return len(targets) == 1


def _prompt_multiline_text(existing_lines=None):
    if not App.GuiUp:
        return existing_lines or ["Review note"]

    dialog = QtWidgets.QInputDialog()
    dialog.setWindowTitle(translate("Assembly", "Review Note"))
    dialog.setLabelText(translate("Assembly", "Enter review note text:"))
    dialog.setInputMode(QtWidgets.QInputDialog.TextInput)
    dialog.setOption(QtWidgets.QInputDialog.UsePlainTextEditForTextInput, True)
    if existing_lines:
        dialog.setTextValue("\n".join(existing_lines))
    ok = dialog.exec_()
    if not ok:
        return None
    text = dialog.textValue()
    lines = [line.rstrip() for line in text.splitlines()]
    # Require at least one non-empty line.
    if not any(line.strip() for line in lines):
        return None
    # Preserve internal blank lines but strip trailing empties.
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


def _fallback_text_offset():
    """Exactly 20 mm along a stable unit direction."""
    offset = App.Vector(1, 1, 1)
    offset.normalize()
    return offset * 20.0


def _initial_text_offset(assembly, target_data, camera_direction=None, camera_up=None):
    """Relative TextPosition toward camera upper-right, clamped to 10–100 mm."""
    fallback = _fallback_text_offset()
    diagonal = 0.0

    target = target_data["target_obj"]
    try:
        if hasattr(target, "Shape") and target.Shape is not None:
            diagonal = float(target.Shape.BoundBox.DiagonalLength)
    except Exception:
        diagonal = 0.0

    if diagonal <= 0 and App.GuiUp and hasattr(target, "ViewObject") and target.ViewObject:
        try:
            bbox = target.ViewObject.getBoundingBox()
            if bbox and bbox.isValid():
                diagonal = float(bbox.DiagonalLength)
        except Exception:
            pass

    if diagonal <= 0:
        length = 20.0
    else:
        length = max(10.0, min(100.0, 0.15 * diagonal))

    # Build a camera-relative upper-right offset in assembly-local space.
    if camera_direction is None or camera_up is None:
        if App.GuiUp:
            try:
                view = Gui.ActiveDocument.ActiveView
                camera_direction = view.getViewDirection()
                camera_up = view.getUpDirection()
            except Exception:
                return fallback
        else:
            return fallback

    view_dir = App.Vector(camera_direction)
    up = App.Vector(camera_up)

    # Camera axes are world-space; TextPosition is Assembly-local.
    asm_global = _assembly_global_placement(assembly)
    inv_rot = asm_global.Rotation.inverted()
    view_dir = inv_rot.multVec(view_dir)
    up = inv_rot.multVec(up)

    if view_dir.Length < 1e-9:
        return fallback
    view_dir.normalize()

    # Remove view-direction component from up.
    up = up - view_dir * up.dot(view_dir)
    if up.Length < 1e-9:
        up = App.Vector(0, 1, 0)
    up.normalize()

    right = view_dir.cross(up)
    if right.Length < 1e-9:
        right = App.Vector(1, 0, 0)
    right.normalize()

    # Toward camera (opposite view direction), upper-right.
    offset = (right + up - view_dir)
    if offset.Length < 1e-9:
        return fallback
    offset.normalize()
    return offset * length


def create_review_note(assembly, target_data, text_lines, text_offset=None, open_transaction=True):
    """Create Review Notes group (if needed) and a note in one optional transaction."""
    if assembly is None or not target_data or not text_lines:
        return None
    if not any(line.strip() for line in text_lines):
        return None
    target_obj = target_data.get("target_obj")
    if not _same_document(assembly, target_obj) or not _target_in_assembly(assembly, target_obj):
        return None

    doc = assembly.Document
    if open_transaction:
        doc.openTransaction(translate("Assembly", "Add Review Note"))

    try:
        group = UtilsAssembly.getReviewNoteGroup(assembly)
        note = group.newObject("Assembly::ReviewNote", "ReviewNote")
        note.LabelText = list(text_lines)
        note.Target = (target_data["target_obj"], list(target_data["sub_list"]))
        note.LocalAnchor = App.Vector(target_data["local_anchor"])
        note.JointSide = target_data["joint_side"]
        note.Resolved = False

        # Ensure attachment tracking is live and BasePosition is current.
        note.refreshBasePosition()

        if text_offset is None:
            text_offset = _initial_text_offset(assembly, target_data)
        note.TextPosition = App.Vector(text_offset)

        if open_transaction:
            doc.commitTransaction()
        return note
    except Exception:
        if open_transaction:
            doc.abortTransaction()
        raise


def edit_review_note(note, new_lines=None):
    if note is None or not note.isDerivedFrom("Assembly::ReviewNote"):
        return False

    if new_lines is None:
        existing = list(note.LabelText) if note.LabelText else []
        new_lines = _prompt_multiline_text(existing)
        if new_lines is None:
            return False

    if not any(line.strip() for line in new_lines):
        return False

    doc = note.Document
    doc.openTransaction(translate("Assembly", "Edit Review Note"))
    try:
        note.LabelText = list(new_lines)
        doc.commitTransaction()
        return True
    except Exception:
        doc.abortTransaction()
        raise


def toggle_resolve_review_note(note):
    if note is None or not note.isDerivedFrom("Assembly::ReviewNote"):
        return False

    doc = note.Document
    new_value = not bool(note.Resolved)
    label = (
        translate("Assembly", "Resolve Review Note")
        if new_value
        else translate("Assembly", "Reopen Review Note")
    )
    doc.openTransaction(label)
    try:
        note.Resolved = new_value
        doc.commitTransaction()
        return True
    except Exception:
        doc.abortTransaction()
        raise


class CommandAddReviewNote:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_ReviewNote",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_AddReviewNote", "Add Review Note"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_AddReviewNote",
                "Adds a review note anchored to the selected assembly component, "
                "geometry element, or joint.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        assembly = UtilsAssembly.activeAssembly()
        if assembly is None:
            return False
        return is_add_review_note_eligible(assembly)

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if assembly is None:
            return

        targets = collect_review_note_targets(assembly)
        if len(targets) != 1:
            return

        lines = _prompt_multiline_text()
        if lines is None:
            return

        create_review_note(assembly, targets[0], lines)


class CommandEditReviewNote:
    def GetResources(self):
        return {
            "Pixmap": "Assembly_ReviewNote",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_EditReviewNote", "Edit Review Note"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_EditReviewNote",
                "Edits the text of the selected review note.",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return _selected_review_note() is not None

    def Activated(self):
        note = _selected_review_note()
        if note:
            edit_review_note(note)


class CommandToggleResolveReviewNote:
    def GetResources(self):
        note = _selected_review_note()
        if note is not None and note.Resolved:
            menu = QT_TRANSLATE_NOOP("Assembly_ToggleResolveReviewNote", "Reopen Review Note")
            tip = QT_TRANSLATE_NOOP(
                "Assembly_ToggleResolveReviewNote",
                "Marks the selected review note as open again.",
            )
        else:
            menu = QT_TRANSLATE_NOOP("Assembly_ToggleResolveReviewNote", "Resolve Review Note")
            tip = QT_TRANSLATE_NOOP(
                "Assembly_ToggleResolveReviewNote",
                "Marks the selected review note as resolved.",
            )
        return {
            "Pixmap": "Assembly_ReviewNoteResolved",
            "MenuText": menu,
            "ToolTip": tip,
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return _selected_review_note() is not None

    def Activated(self):
        note = _selected_review_note()
        if note:
            toggle_resolve_review_note(note)


def _selected_review_note():
    if not App.GuiUp:
        return None
    selection = Gui.Selection.getSelection()
    if len(selection) != 1:
        return None
    obj = selection[0]
    if obj.isDerivedFrom("Assembly::ReviewNote"):
        return obj
    return None


if App.GuiUp:
    Gui.addCommand("Assembly_AddReviewNote", CommandAddReviewNote())
    Gui.addCommand("Assembly_EditReviewNote", CommandEditReviewNote())
    Gui.addCommand("Assembly_ToggleResolveReviewNote", CommandToggleResolveReviewNote())
