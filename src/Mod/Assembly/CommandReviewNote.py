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

import re

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

import UtilsAssembly

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtGui, QtWidgets

    class ReviewNoteAtSuggestionDelegate(QtWidgets.QStyledItemDelegate):
        """Paint middle-ellipsized @ref paths; tooltips keep the full path."""

        def paint(self, painter, option, index):
            full = index.data(QtCore.Qt.DisplayRole)
            opt = QtWidgets.QStyleOptionViewItem(option)
            self.initStyleOption(opt, index)
            if full:
                opt.text = ellipsis_review_note_at_path_for_width(
                    str(full), option.fontMetrics, option.rect.width()
                )
            widget = opt.widget
            style = widget.style() if widget is not None else QtWidgets.QApplication.style()
            style.drawControl(QtWidgets.QStyle.CE_ItemViewItem, opt, painter, widget)

        def helpEvent(self, event, view, option, index):
            if event is not None and event.type() == QtCore.QEvent.ToolTip:
                full = index.data(QtCore.Qt.ToolTipRole) or index.data(QtCore.Qt.UserRole)
                if full:
                    QtWidgets.QToolTip.showText(event.globalPos(), str(full), view)
                    return True
            return super().helpEvent(event, view, option, index)

    def fill_review_note_at_suggestion_model(model, suggestions):
        """Populate a QStandardItemModel with full paths (display shortened by delegate)."""
        model.clear()
        for full in suggestions or []:
            path = str(full)
            item = QtGui.QStandardItem(path)
            item.setEditable(False)
            item.setData(path, QtCore.Qt.EditRole)
            item.setData(path, QtCore.Qt.UserRole)
            item.setData(path, QtCore.Qt.ToolTipRole)
            model.appendRow(item)

else:
    ReviewNoteAtSuggestionDelegate = None

    def fill_review_note_at_suggestion_model(model, suggestions):
        raise RuntimeError("Review Note @ suggestions require a GUI session")

__title__ = "Assembly Command Review Note"
__author__ = "The FreeCAD Project Association AISBL"
__url__ = "https://www.freecad.org"

translate = App.Qt.translate

JOINT_SIDE_NONE = "None"
JOINT_SIDE_REF1 = "Reference1"
JOINT_SIDE_REF2 = "Reference2"

_SUPPORTED_SUB_PREFIXES = ("Face", "Edge", "Vertex")

# @Object or @Object.Child.Face12 — same-document object + optional subpath.
_REVIEW_NOTE_REF_RE = re.compile(r"@([A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*)")


def parse_review_note_references(lines):
    """Parse @Object[.Sub...] spans from LabelText lines.

    Returns a list of dicts with keys:
      full, obj_name, sub_name, line, start, end, text
    """
    refs = []
    for line_index, line in enumerate(lines or []):
        text = str(line)
        for match in _REVIEW_NOTE_REF_RE.finditer(text):
            full = match.group(1)
            parts = [p for p in full.split(".") if p]
            if not parts:
                continue
            refs.append(
                {
                    "full": full,
                    "obj_name": parts[0],
                    "sub_name": ".".join(parts[1:]),
                    "line": line_index,
                    "start": match.start(),
                    "end": match.end(),
                    "text": match.group(0),
                }
            )
    return refs


def resolve_review_note_reference(doc, obj_name, sub_name=""):
    """Resolve a same-document @ref to (object, sub_name) or (None, sub_name)."""
    if doc is None or not obj_name:
        return None, sub_name or ""
    obj = doc.getObject(obj_name)
    if obj is None:
        return None, sub_name or ""
    return obj, sub_name or ""


def select_review_note_reference(doc, obj_name, sub_name=""):
    """Clear selection and select the resolved @ref target. Returns True on success."""
    if not App.GuiUp:
        return False
    obj, sub = resolve_review_note_reference(doc, obj_name, sub_name)
    if obj is None:
        return False
    Gui.Selection.clearSelection()
    if sub:
        # Prefer object-rooted selection so the target (not a parent Part) is primary.
        Gui.Selection.addSelection(obj, sub)
    else:
        Gui.Selection.addSelection(obj)
    return True


def _is_at_suggestion_object(obj):
    """Objects that may appear in @ref autocomplete."""
    if obj is None:
        return False
    tid = getattr(obj, "TypeId", "")
    if tid in (
        "Assembly::ReviewNote",
        "Assembly::ReviewNoteGroup",
        "Assembly::JointGroup",
        "Assembly::BomGroup",
        "App::Origin",
        "App::Line",
        "App::Plane",
        "App::OriginFeature",
    ):
        return False
    if obj.isDerivedFrom("App::DocumentObjectGroup") and not obj.isDerivedFrom("App::Part"):
        return False
    if obj.isDerivedFrom("App::LocalCoordinateSystem"):
        return False
    if obj.isDerivedFrom("App::DatumElement"):
        return False
    return True


def _shape_element_suggestions(obj, limit=60):
    """Face/Edge/Vertex names from an object's shape (or linked shape)."""
    names = []
    try:
        shape_obj = obj
        if hasattr(obj, "getLinkedObject"):
            linked = obj.getLinkedObject(True)
            if linked is not None:
                shape_obj = linked
        shape = getattr(shape_obj, "Shape", None)
        if shape is None or shape.isNull():
            return names
        for i in range(1, min(len(shape.Faces), limit) + 1):
            names.append("Face{}".format(i))
        for i in range(1, min(len(shape.Edges), max(0, limit - len(names))) + 1):
            names.append("Edge{}".format(i))
        for i in range(1, min(len(shape.Vertexes), max(0, limit - len(names))) + 1):
            names.append("Vertex{}".format(i))
    except Exception:
        pass
    return names


def _child_name_suggestions(obj):
    """Direct child Names under Part/Link groups for nested @paths."""
    children = []
    try:
        for child in list(getattr(obj, "Group", []) or []):
            if _is_at_suggestion_object(child):
                children.append(child.Name)
        if getattr(obj, "TypeId", "") == "App::Link":
            linked = obj.getLinkedObject(True) if hasattr(obj, "getLinkedObject") else None
            if linked is not None and linked is not obj:
                for child in list(getattr(linked, "Group", []) or []):
                    if _is_at_suggestion_object(child) and child.Name not in children:
                        children.append(child.Name)
    except Exception:
        pass
    return children


def collect_review_note_at_suggestions(doc, prefix=""):
    """Return @ref completion candidates for the typed prefix (without leading @).

    Examples:
      "" / "Bo" -> object Names
      "Box." / "Box.Fa" -> Box.FaceN / children
    """
    if doc is None:
        return []
    prefix = str(prefix or "")
    max_items = 80

    if "." not in prefix:
        prefix_l = prefix.lower()
        names = []
        for obj in doc.Objects:
            if not _is_at_suggestion_object(obj):
                continue
            name = obj.Name
            if prefix_l:
                label = getattr(obj, "Label", "") or ""
                if not name.lower().startswith(prefix_l) and not label.lower().startswith(prefix_l):
                    continue
            names.append(name)
        names.sort(key=lambda n: n.lower())
        return names[:max_items]

    parts = prefix.split(".")
    root = parts[0]
    obj = doc.getObject(root)
    if obj is None or not _is_at_suggestion_object(obj):
        return []

    cursor_obj = obj
    built = [root]
    for seg in parts[1:-1]:
        nxt = doc.getObject(seg)
        if nxt is None or not _is_at_suggestion_object(nxt):
            child_names = _child_name_suggestions(cursor_obj)
            if seg not in child_names:
                return []
            nxt = doc.getObject(seg)
            if nxt is None:
                return []
        cursor_obj = nxt
        built.append(seg)

    partial = parts[-1]
    path_prefix = ".".join(built)
    partial_l = partial.lower()
    suggestions = []
    for child_name in _child_name_suggestions(cursor_obj):
        if not partial_l or child_name.lower().startswith(partial_l):
            suggestions.append(path_prefix + "." + child_name)
    for elem in _shape_element_suggestions(cursor_obj):
        if not partial_l or elem.lower().startswith(partial_l):
            suggestions.append(path_prefix + "." + elem)

    seen = set()
    out = []
    for s in suggestions:
        if s in seen:
            continue
        seen.add(s)
        out.append(s)
        if len(out) >= max_items:
            break
    return out


def at_token_at_cursor(text, pos):
    """If caret is inside a typed @ref, return (at_index, prefix_before_caret).

    ``prefix`` is the text after ``@`` up to the caret (empty right after ``@``).
    Returns ``(None, None)`` when the caret is not in an @ token.
    """
    text = str(text or "")
    if pos < 0 or pos > len(text):
        return None, None
    i = pos - 1
    while i >= 0:
        ch = text[i]
        if ch == "@":
            prefix = text[i + 1 : pos]
            if prefix == "" or re.match(r"^[A-Za-z_][\w.]*$", prefix):
                return i, prefix
            return None, None
        if ch.isalnum() or ch in "_.":
            i -= 1
            continue
        return None, None
    return None, None


_AT_PATH_ELLIPSIS = "…"


def _char_middle_ellipsis(text, max_chars):
    """Character-level middle ellipsis (fallback when path segments cannot shrink)."""
    text = str(text or "")
    if max_chars <= 0:
        return ""
    if len(text) <= max_chars:
        return text
    ell = _AT_PATH_ELLIPSIS
    if max_chars <= len(ell):
        return ell[:max_chars]
    inner = max_chars - len(ell)
    left = (inner + 1) // 2
    right = inner - left
    if right <= 0:
        return text[:inner] + ell
    return text[:left] + ell + text[-right:]


def ellipsis_review_note_at_path(path, max_chars):
    """Shorten a dotted @ref path for display with a middle ellipsis.

    Keeps the leading object and the trailing object/subelement when possible,
    e.g. ``AssemblyCase.…RightPocket.Face12``. Does not alter the real path used
    for search or insertion.
    """
    path = str(path or "")
    if max_chars <= 0:
        return ""
    if len(path) <= max_chars:
        return path

    parts = [p for p in path.split(".") if p]
    if len(parts) < 2:
        return _char_middle_ellipsis(path, max_chars)

    head = parts[0]
    # Prefer keeping the final two segments (object + Face/Edge/Vertex), then one.
    for tail_count in (2, 1):
        if len(parts) <= tail_count:
            continue
        # Need at least one omitted middle segment.
        if len(parts) < tail_count + 2:
            continue
        tail = ".".join(parts[-tail_count:])
        candidate = "{}.{}{}".format(head, _AT_PATH_ELLIPSIS, tail)
        if len(candidate) <= max_chars:
            return candidate

    # Shrink the head if the preferred tails still overflow.
    for tail_count in (2, 1):
        if len(parts) < tail_count + 2:
            continue
        tail = ".".join(parts[-tail_count:])
        # "{head}.…{tail}" — budget for head after fixed suffix.
        suffix = ".{}{}".format(_AT_PATH_ELLIPSIS, tail)
        if len(suffix) >= max_chars:
            continue
        head_budget = max_chars - len(suffix)
        if head_budget <= 0:
            continue
        trimmed_head = head if len(head) <= head_budget else head[:head_budget]
        if not trimmed_head:
            continue
        return trimmed_head + suffix

    return _char_middle_ellipsis(path, max_chars)


def ellipsis_review_note_at_path_for_width(path, font_metrics, width_px, padding=8):
    """Width-aware display label for a full @ref path (pixels via QFontMetrics)."""
    path = str(path or "")
    avail = max(0, int(width_px) - int(padding))
    if avail <= 0:
        return _AT_PATH_ELLIPSIS

    def _advance(text):
        if hasattr(font_metrics, "horizontalAdvance"):
            return int(font_metrics.horizontalAdvance(text))
        return int(font_metrics.width(text))

    if _advance(path) <= avail:
        return path

    lo, hi = 1, len(path)
    best = _AT_PATH_ELLIPSIS
    while lo <= hi:
        mid = (lo + hi) // 2
        label = ellipsis_review_note_at_path(path, mid)
        if _advance(label) <= avail:
            best = label
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def apply_review_note_at_completion(text, cursor_pos, completion):
    """Replace the @token at cursor with ``@`` + full completion path."""
    text = str(text or "")
    completion = str(completion or "")
    at_idx, _prefix = at_token_at_cursor(text, cursor_pos)
    if at_idx is None:
        return text
    return text[:at_idx] + "@" + completion + text[cursor_pos:]


def review_note_label_half_extents(image_width, image_height, font_size=10.0):
    """Approximate annotation-box half-extents in local units (matches Gui VP)."""
    mm_per_px = max(0.05, float(font_size) * 0.03)
    half_w = max(0.5, 0.5 * float(image_width) * mm_per_px)
    half_h = max(0.5, 0.5 * float(image_height) * mm_per_px)
    return half_w, half_h


def review_note_perimeter_offset(port, half_w, half_h):
    """Map LeaderPort in [0,1] to an offset from the text/image center."""
    import math

    w = max(1e-6, float(half_w))
    h = max(1e-6, float(half_h))
    peri = 2.0 * (2.0 * w + 2.0 * h)
    d = math.fmod(float(port), 1.0)
    if d < 0.0:
        d += 1.0
    d *= peri
    right = 2.0 * h
    bottom = right + 2.0 * w
    left = bottom + 2.0 * h
    if d <= right:
        offset = App.Vector(w, h - d, 0.0)
    elif d <= bottom:
        offset = App.Vector(w - (d - right), -h, 0.0)
    elif d <= left:
        offset = App.Vector(-w, -h + (d - bottom), 0.0)
    else:
        offset = App.Vector(-w + (d - left), h, 0.0)
    # Never leave the leader on a corner vertex — snap to the nearer side midpoint.
    eps = 1e-4 * min(w, h)
    if abs(abs(offset.x) - w) < eps and abs(abs(offset.y) - h) < eps:
        if abs(offset.x) >= abs(offset.y):
            offset = App.Vector(w if offset.x >= 0.0 else -w, 0.0, 0.0)
        else:
            offset = App.Vector(0.0, h if offset.y >= 0.0 else -h, 0.0)
    return offset


def review_note_auto_boundary_endpoint(text_pos, half_w, half_h):
    """Leader end at the midpoint of the box side facing the base (not a corner)."""
    w = max(1e-6, float(half_w))
    h = max(1e-6, float(half_h))
    text = App.Vector(text_pos)
    if text.Length < 1e-9:
        return App.Vector(-w, 0.0, 0.0)
    toward_base = text * (-1.0)
    ax = abs(toward_base.x) / w
    ay = abs(toward_base.y) / h
    if ax >= ay:
        return text + App.Vector(w if toward_base.x >= 0.0 else -w, 0.0, 0.0)
    return text + App.Vector(0.0, h if toward_base.y >= 0.0 else -h, 0.0)


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
    """Accept empty (whole component) or a Face/Edge/Vertex, optionally under a relative path."""
    if not sub_name:
        return True
    parts = [p for p in str(sub_name).split(".") if p]
    if not parts:
        return False
    # Occurrence-relative paths like Spool_GearTipRelief.Face182 are allowed;
    # only the final segment must be a geometry element name.
    element = parts[-1]
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
        if bool(assembly.hasObject(obj, True)):
            return True
    except Exception:
        pass
    # hasObject does not detect LinkElements (FreeCAD#16113).
    if getattr(obj, "TypeId", None) == "App::LinkElement":
        link_group = None
        getter = getattr(obj, "getLinkGroup", None)
        if callable(getter):
            try:
                link_group = getter()
            except Exception:
                link_group = None
        if link_group is None:
            for parent in getattr(obj, "InList", []) or []:
                if (
                    getattr(parent, "TypeId", None) == "App::Link"
                    and obj in list(getattr(parent, "ElementList", []) or [])
                ):
                    link_group = parent
                    break
        if link_group is not None:
            try:
                return bool(assembly.hasObject(link_group, True))
            except Exception:
                return False
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


def _selection_path_names(root_obj, sub_name):
    """Return document object names from a selection path, preserving occurrences."""
    names = []
    if root_obj is not None and getattr(root_obj, "Name", None):
        names.append(root_obj.Name)
    for part in str(sub_name or "").split("."):
        if not part or ";" in part or part.startswith(":"):
            continue
        names.append(part)
    return names


def _component_in_selection_path(root_obj, sub_name, component):
    """True when component.Name appears in the raw selection path (occurrence-preserving)."""
    if component is None:
        return False
    return component.Name in _selection_path_names(root_obj, sub_name)


def find_review_note_owner(root_obj=None, sub_name="", selection=None):
    """Nearest owning App::Part for a selection (AssemblyObject counts as App::Part).

    Prefers an active AssemblyObject when the selection lies under it; otherwise uses
    the selected Part root or the nearest Part parent of the selected object.
    """
    if selection is None and root_obj is None:
        if App.GuiUp:
            selection = Gui.Selection.getSelectionEx("*", 0)
        else:
            selection = []

    if root_obj is None:
        entries = _selection_entries(selection)
        if not entries:
            # Fall back to active assembly when nothing is selected.
            return UtilsAssembly.activeAssembly() if App.GuiUp else None
        root_obj, sub_name, _pick = entries[0]

    if root_obj is None:
        return None

    # Selection rooted at a Part/Assembly with an inside subpath → that Part owns the note.
    if root_obj.isDerivedFrom("App::Part") and (sub_name or ""):
        return root_obj

    active = None
    if App.GuiUp:
        try:
            active = UtilsAssembly.activeAssembly()
        except Exception:
            active = None
    if active is not None and _target_in_assembly(active, root_obj):
        return active

    if root_obj.isDerivedFrom("App::Part"):
        return root_obj

    # Climb parents for the nearest App::Part.
    seen = set()
    stack = list(getattr(root_obj, "InList", []) or [])
    while stack:
        parent = stack.pop(0)
        if parent is None or id(parent) in seen:
            continue
        seen.add(id(parent))
        if parent.isDerivedFrom("App::Part"):
            return parent
        stack.extend(list(getattr(parent, "InList", []) or []))
    return active


def normalize_review_note_target(assembly, root_obj, sub_name="", picked_point=None):
    """Normalize a selection into review-note target data.

    Returns a dict:
      target_obj, sub_list, local_anchor, joint_side
    or None if unsupported.

    Selection may report a Part/Assembly as Object with a child subpath such as
    ``AssemblySpool.Spool_GearTipRelief.Face182``. The occurrence name from the
    selection path is preserved (not remapped through link resolution).
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

    # Resolve container + subpath to a component, preferring the occurrence named
    # in the selection path so Links are not collapsed to their linked object.
    comp, rel_sub = UtilsAssembly.getComponentReference(assembly, root_obj, sub_name or "")
    if comp and not _component_in_selection_path(root_obj, sub_name, comp):
        # getComponentReference remapped through a link; fall back to path walk.
        comp = None
        rel_sub = ""
        path = _selection_path_names(root_obj, sub_name)
        try:
            owner_idx = path.index(assembly.Name)
            candidates = path[owner_idx + 1 :]
        except ValueError:
            candidates = path[1:] if path and path[0] == root_obj.Name else path
        doc = assembly.Document
        for i, name in enumerate(candidates):
            obj = doc.getObject(name)
            if not obj:
                continue
            if obj.isDerivedFrom("App::DocumentObjectGroup"):
                continue
            if UtilsAssembly.isLinkGroup(obj):
                continue
            if UtilsAssembly.isLink(obj) or obj.isDerivedFrom("App::GeoFeature"):
                if _is_supported_component(obj) and not _is_unsupported_target_object(obj):
                    comp = obj
                    rel_sub = ".".join(candidates[i + 1 :])
                    break

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
    # Never anchor to the owning Part/Assembly, LCS/origin/datum/group, etc.
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

    doc = App.ActiveDocument
    # Keep the popup readable on long nested paths; delegate ellipsizes to this width.
    popup_width = 280

    class _AtAwarePlainTextEdit(QtWidgets.QPlainTextEdit):
        def __init__(self, parent=None):
            super().__init__(parent)
            self._completer = None
            self._model = None

        def set_at_completer(self, completer, model):
            self._completer = completer
            self._model = model
            completer.setWidget(self)
            completer.setCompletionMode(QtWidgets.QCompleter.UnfilteredPopupCompletion)
            completer.setCaseSensitivity(QtCore.Qt.CaseInsensitive)
            completer.setCompletionRole(QtCore.Qt.EditRole)
            completer.setMaxVisibleItems(12)
            popup = completer.popup()
            popup.setItemDelegate(ReviewNoteAtSuggestionDelegate(popup))
            popup.setUniformItemSizes(True)
            popup.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
            completer.activated.connect(self._insert_completion)

        def _resolve_completion(self, completion):
            """Prefer the full path stored on the current popup row."""
            popup = self._completer.popup() if self._completer else None
            if popup is not None:
                idx = popup.currentIndex()
                if idx.isValid():
                    full = idx.data(QtCore.Qt.UserRole) or idx.data(QtCore.Qt.EditRole)
                    if full:
                        return str(full)
            return str(completion)

        def _insert_completion(self, completion):
            full = self._resolve_completion(completion)
            cursor = self.textCursor()
            end_pos = cursor.position()
            text = self.toPlainText()
            at_idx, _prefix = at_token_at_cursor(text, end_pos)
            if at_idx is None:
                return
            cursor.beginEditBlock()
            cursor.setPosition(at_idx)
            cursor.setPosition(end_pos, QtGui.QTextCursor.KeepAnchor)
            cursor.insertText("@" + full)
            cursor.endEditBlock()
            self.setTextCursor(cursor)

        def keyPressEvent(self, event):
            popup = self._completer.popup() if self._completer else None
            if popup is not None and popup.isVisible():
                if event.key() in (
                    QtCore.Qt.Key_Enter,
                    QtCore.Qt.Key_Return,
                    QtCore.Qt.Key_Escape,
                    QtCore.Qt.Key_Tab,
                    QtCore.Qt.Key_Backtab,
                    QtCore.Qt.Key_Up,
                    QtCore.Qt.Key_Down,
                ):
                    event.ignore()
                    return
            super().keyPressEvent(event)
            self.refresh_at_completer()

        def refresh_at_completer(self):
            if self._completer is None or self._model is None:
                return
            cursor = self.textCursor()
            text = self.toPlainText()
            at_idx, prefix = at_token_at_cursor(text, cursor.position())
            if at_idx is None:
                self._completer.popup().hide()
                return
            # Filter/search always uses complete paths (never the ellipsized labels).
            suggestions = collect_review_note_at_suggestions(doc, prefix)
            if not suggestions:
                self._completer.popup().hide()
                return
            fill_review_note_at_suggestion_model(self._model, suggestions)
            cr = self.cursorRect()
            popup = self._completer.popup()
            cr.setWidth(popup_width)
            popup.setFixedWidth(popup_width)
            self._completer.complete(cr)

    dialog = QtWidgets.QDialog()
    dialog.setWindowTitle(translate("Assembly", "Review Note"))
    dialog.setMinimumWidth(420)
    dialog.setMinimumHeight(260)

    layout = QtWidgets.QVBoxLayout(dialog)
    hint = QtWidgets.QLabel(
        translate(
            "Assembly",
            "Enter review note text. Type @ to mention an object (e.g. @Box.Face1).",
        )
    )
    hint.setWordWrap(True)
    layout.addWidget(hint)

    edit = _AtAwarePlainTextEdit(dialog)
    edit.setPlaceholderText(translate("Assembly", "Review note"))
    if existing_lines:
        edit.setPlainText("\n".join(existing_lines))
        cursor = edit.textCursor()
        cursor.movePosition(QtGui.QTextCursor.End)
        edit.setTextCursor(cursor)
    layout.addWidget(edit)

    model = QtGui.QStandardItemModel(dialog)
    completer = QtWidgets.QCompleter(model, dialog)
    edit.set_at_completer(completer, model)
    edit.cursorPositionChanged.connect(edit.refresh_at_completer)

    buttons = QtWidgets.QDialogButtonBox(
        QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel
    )
    buttons.accepted.connect(dialog.accept)
    buttons.rejected.connect(dialog.reject)
    layout.addWidget(buttons)

    edit.setFocus()
    if dialog.exec_() != QtWidgets.QDialog.Accepted:
        return None

    text = edit.toPlainText()
    lines = [line.rstrip() for line in text.splitlines()]
    if not any(line.strip() for line in lines):
        return None
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
        owner = find_review_note_owner()
        if owner is None:
            return False
        return is_add_review_note_eligible(owner)

    def Activated(self):
        owner = find_review_note_owner()
        if owner is None:
            return

        targets = collect_review_note_targets(owner)
        if len(targets) != 1:
            return

        lines = _prompt_multiline_text()
        if lines is None:
            return

        create_review_note(owner, targets[0], lines)


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
