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

    class ReviewNoteAtAwarePlainTextEdit(QtWidgets.QPlainTextEdit):
        """Plain-text editor with @ref autocomplete (modeless-safe)."""

        def __init__(self, parent=None, popup_width=280):
            super().__init__(parent)
            self._completer = None
            self._model = None
            self._popup_width = int(popup_width)
            self._last_cursor_pos = 0
            self.cursorPositionChanged.connect(self._remember_cursor_pos)

        def _remember_cursor_pos(self):
            self._last_cursor_pos = self.textCursor().position()

        def focusOutEvent(self, event):
            self._remember_cursor_pos()
            super().focusOutEvent(event)

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
            self._remember_cursor_pos()

        def insert_at_reference(self, path):
            """Insert ``@path`` at the last remembered caret (3D-pick friendly).

            Replaces an in-progress ``@`` token only when it is incomplete (does not
            already end in FaceN/EdgeN/VertexN). Otherwise inserts a new reference
            so successive 3D picks append rather than overwrite.
            """
            path = str(path or "").strip()
            if not path:
                return False
            text = self.toPlainText()
            pos = max(0, min(int(self._last_cursor_pos), len(text)))
            cursor = self.textCursor()
            at_idx, prefix = at_token_at_cursor(text, pos)
            replace = False
            if at_idx is not None:
                parts = str(prefix or "").split(".")
                last = parts[-1] if parts else ""
                if not _SHAPE_ELEMENT_NAME_RE.match(last):
                    replace = True
            cursor.beginEditBlock()
            if replace:
                cursor.setPosition(at_idx)
                cursor.setPosition(pos, QtGui.QTextCursor.KeepAnchor)
                cursor.insertText("@" + path)
            else:
                insert = "@" + path
                if pos > 0 and text[pos - 1] not in " \n\t":
                    insert = " " + insert
                cursor.setPosition(pos)
                cursor.insertText(insert)
            cursor.endEditBlock()
            self.setTextCursor(cursor)
            self._remember_cursor_pos()
            return True

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
            suggestions = collect_review_note_at_suggestions(App.ActiveDocument, prefix)
            if not suggestions:
                self._completer.popup().hide()
                return
            fill_review_note_at_suggestion_model(self._model, suggestions)
            cr = self.cursorRect()
            popup = self._completer.popup()
            cr.setWidth(self._popup_width)
            popup.setFixedWidth(self._popup_width)
            self._completer.complete(cr)

else:
    ReviewNoteAtSuggestionDelegate = None
    ReviewNoteAtAwarePlainTextEdit = None

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


def _linked_shape(obj):
    """Return the Shape used for @ref Face/Edge/Vertex suggestions, or None."""
    try:
        shape_obj = obj
        if hasattr(obj, "getLinkedObject"):
            linked = obj.getLinkedObject(True)
            if linked is not None:
                shape_obj = linked
        shape = getattr(shape_obj, "Shape", None)
        if shape is None or shape.isNull():
            return None
        return shape
    except Exception:
        return None


def _shape_element_suggestions(obj, limit=60):
    """Preview Face/Edge/Vertex names from an object's shape (capped for performance)."""
    names = []
    shape = _linked_shape(obj)
    if shape is None:
        return names
    try:
        for i in range(1, min(len(shape.Faces), limit) + 1):
            names.append("Face{}".format(i))
        for i in range(1, min(len(shape.Edges), max(0, limit - len(names))) + 1):
            names.append("Edge{}".format(i))
        for i in range(1, min(len(shape.Vertexes), max(0, limit - len(names))) + 1):
            names.append("Vertex{}".format(i))
    except Exception:
        pass
    return names


_EXACT_SHAPE_ELEMENT_RE = re.compile(r"^(Face|Edge|Vertex)(\d+)$", re.IGNORECASE)


def _exact_shape_element_if_valid(obj, partial):
    """If ``partial`` is FaceN/EdgeN/VertexN and that element exists, return canonical name.

    Used so typed numbers beyond the preview limit (e.g. Face126) still autocomplete.
    """
    m = _EXACT_SHAPE_ELEMENT_RE.match(str(partial or ""))
    if not m:
        return None
    kind = {"face": "Face", "edge": "Edge", "vertex": "Vertex"}[m.group(1).lower()]
    try:
        index = int(m.group(2))
    except ValueError:
        return None
    if index < 1:
        return None
    shape = _linked_shape(obj)
    if shape is None:
        return None
    try:
        if kind == "Face" and index <= len(shape.Faces):
            return "Face{}".format(index)
        if kind == "Edge" and index <= len(shape.Edges):
            return "Edge{}".format(index)
        if kind == "Vertex" and index <= len(shape.Vertexes):
            return "Vertex{}".format(index)
    except Exception:
        return None
    return None


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
      "Box." / "Box.Fa" -> Box.FaceN / children (preview-capped)
      "Box.Face126" -> exact Face126 when that subelement exists (beyond preview)
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

    # Typed FaceN/EdgeN/VertexN: validate that exact element even past the preview cap.
    exact = _exact_shape_element_if_valid(cursor_obj, partial)
    exact_path = (path_prefix + "." + exact) if exact else None

    seen = set()
    out = []
    if exact_path:
        out.append(exact_path)
        seen.add(exact_path)
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
        return App.Vector(w, h - d, 0.0)
    if d <= bottom:
        return App.Vector(w - (d - right), -h, 0.0)
    if d <= left:
        return App.Vector(-w, -h + (d - bottom), 0.0)
    return App.Vector(-w + (d - left), h, 0.0)


def review_note_auto_boundary_endpoint(text_pos, half_w, half_h):
    """Nearest point on the label rectangle border along the line toward the base."""
    w = max(1e-6, float(half_w))
    h = max(1e-6, float(half_h))
    text = App.Vector(text_pos)
    if text.Length < 1e-9:
        return App.Vector(-w, 0.0, 0.0)
    toward_base = text * (-1.0)
    dir_xy = App.Vector(toward_base.x, toward_base.y, 0.0)
    if dir_xy.Length < 1e-9:
        return text + App.Vector(-w, 0.0, 0.0)
    sx = (w / abs(dir_xy.x)) if abs(dir_xy.x) > 1e-12 else 1e12
    sy = (h / abs(dir_xy.y)) if abs(dir_xy.y) > 1e-12 else 1e12
    t = min(sx, sy)
    return text + dir_xy * t


def review_note_leader_glue_status(text_pos, leader_end, half_extent, max_half=40.0):
    """Check whether a leader endpoint is still glued to the text box.

    Returns ``(ok, reason)``. ``ok`` is True only when the endpoint leaves the
    text center, stays within the billboard half-diagonal, and half-extents are
    not inflated like the old FontSize×bitmap fallback.

    Negative cases (stuck/detached logs) must return ``ok=False`` — used by tests
    so regressions are not only covered by happy-path asserts.
    """
    import math

    text = App.Vector(text_pos)
    end = App.Vector(leader_end)
    half = App.Vector(half_extent)
    dist = (end - text).Length
    max_h = max(abs(half.x), abs(half.y), 0.0)
    if dist <= 1e-6:
        return False, "collapsed: LeaderEnd equals text center"
    limit = math.sqrt(half.x * half.x + half.y * half.y) * 1.25 + 1e-3
    if dist > limit:
        return False, "detached: dist={:.3f} exceeds half-diagonal limit={:.3f}".format(
            dist, limit
        )
    if max_h >= float(max_half):
        return False, "inflated: half-extent {:.3f} >= max_half {:.3f}".format(
            max_h, float(max_half)
        )
    return True, "glued"


def review_note_leader_is_stuck_after_move(old_end, new_text, new_end, min_move=5.0):
    """True when TextPosition moved but LeaderEnd stayed on the previous endpoint.

    Matches the drag-finish race in review_note_drag_camera_*.jsonl where
    LeaderEnd remained at the pre-move attachment after TextPosition committed.
    """
    old = App.Vector(old_end)
    text = App.Vector(new_text)
    end = App.Vector(new_end)
    if (text - old).Length < float(min_move):
        return False
    # Stuck: new end still equals the old end while text has moved away.
    return end.isEqual(old, 1e-2)


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


_SHAPE_ELEMENT_NAME_RE = re.compile(r"^(Face|Edge|Vertex)(\d+)$", re.IGNORECASE)


def review_note_at_path_from_selection(root_obj, sub_name=""):
    """Build a full @ref path (without leading @) from a 3D selection.

    Examples:
      Box + ``Face6`` → ``Box.Face6``
      Assembly + ``Box.Face6`` → ``Assembly.Box.Face6``

    Returns ``None`` when the selection is not a Face/Edge/Vertex subelement.
    """
    names = _selection_path_names(root_obj, sub_name)
    if len(names) < 2:
        return None
    last = names[-1]
    m = _SHAPE_ELEMENT_NAME_RE.match(last)
    if not m:
        return None
    kind = {"face": "Face", "edge": "Edge", "vertex": "Vertex"}[m.group(1).lower()]
    names[-1] = "{}{}".format(kind, int(m.group(2)))
    return ".".join(names)


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


def is_add_review_note_task_eligible(selection=None):
    """True when the Assembly Tasks panel should offer Add Review Note.

    Uses the same eligibility as the command/context menu: exactly one supported
    component, Face/Edge/Vertex, or joint under a resolvable owner Part/Assembly.
    The owning Assembly does not need to be active yet.

    When ``selection`` is omitted, reads the live GUI selection (requires GuiUp).
    """
    if selection is None:
        if not App.GuiUp:
            return False
        selection = Gui.Selection.getSelectionEx("*", 0)
    if not selection:
        return False
    owner = find_review_note_owner(selection=selection)
    return is_add_review_note_eligible(owner, selection)


def _snapshot_selection(selection=None):
    """Capture selection as (doc, obj, sub, optional pick) tuples for restore."""
    if not App.GuiUp:
        return []
    if selection is None:
        selection = Gui.Selection.getSelectionEx("*", 0)
    snap = []
    for sel in selection or []:
        obj = getattr(sel, "Object", None)
        if obj is None:
            continue
        doc_name = obj.Document.Name
        obj_name = obj.Name
        subs = list(sel.SubElementNames) if sel.SubElementNames else [""]
        picks = []
        if hasattr(sel, "PickedPoints") and sel.PickedPoints:
            picks = [App.Vector(p) for p in sel.PickedPoints]
        snap.append((doc_name, obj_name, subs, picks))
    return snap


def _restore_selection(snap):
    """Restore a selection snapshot produced by ``_snapshot_selection``."""
    if not App.GuiUp:
        return
    Gui.Selection.clearSelection()
    for doc_name, obj_name, subs, picks in snap or []:
        for i, sub in enumerate(subs):
            try:
                if picks and i < len(picks):
                    p = picks[i]
                    Gui.Selection.addSelection(
                        doc_name, obj_name, sub or "", p.x, p.y, p.z
                    )
                else:
                    Gui.Selection.addSelection(doc_name, obj_name, sub or "")
            except Exception:
                try:
                    Gui.Selection.addSelection(doc_name, obj_name, sub or "")
                except Exception:
                    pass


def ensure_review_note_owner_active(owner, selection=None):
    """Activate ``owner`` if it is an inactive AssemblyObject; preserve selection.

    Returns the owner (unchanged for non-Assembly Parts). Selection is snapshotted
    before ``setEdit`` and restored afterward because assembly activation can clear
    or reshape the current selection.
    """
    if owner is None or not App.GuiUp:
        return owner
    if not owner.isDerivedFrom("Assembly::AssemblyObject"):
        return owner

    active = None
    try:
        active = UtilsAssembly.activeAssembly()
    except Exception:
        active = None
    if active is owner:
        return owner

    snap = _snapshot_selection(selection)
    try:
        Gui.ActiveDocument.setEdit(owner)
    except Exception:
        try:
            if owner.ViewObject:
                Gui.ActiveDocument.setEdit(owner.ViewObject)
        except Exception:
            pass

    try:
        Gui.updateGui()
    except Exception:
        pass

    _restore_selection(snap)
    try:
        Gui.updateGui()
    except Exception:
        pass
    return owner


def _normalize_label_lines(text):
    """Split editor text into LabelText lines, or None if empty."""
    lines = [line.rstrip() for line in str(text or "").splitlines()]
    if not any(line.strip() for line in lines):
        return None
    while lines and not lines[-1].strip():
        lines.pop()
    return lines


_active_review_note_task = None


def get_active_review_note_task():
    """Return the open Review Note task panel, if any."""
    return _active_review_note_task


def _close_active_task_dialog():
    if not App.GuiUp:
        return
    if not Gui.Control.activeDialog():
        return
    try:
        task = Gui.Control.activeTaskDialog()
        if task is not None and hasattr(task, "reject"):
            task.reject()
            return
    except Exception:
        pass
    try:
        Gui.Control.closeDialog()
    except Exception:
        pass


class TaskAssemblyReviewNote:
    """Modeless Task panel for creating/editing Review Note text.

    View, tree, and selection stay interactive so users can rotate/zoom and pick
    geometry for @mentions. Clicking a Face/Edge/Vertex inserts ``@ObjectPath…``
    at the last text-caret position. The note's original anchor is frozen when
    the panel opens and is never updated from later selection changes.
    """

    def __init__(self, note=None, assembly=None, target_data=None):
        if not App.GuiUp:
            raise RuntimeError("TaskAssemblyReviewNote requires a GUI session")

        self.note = note
        self.assembly = assembly
        self.target_data = target_data
        self._closed = False
        self._anchor_snapshot = None
        self._selection_observer = False
        self._inserting_selection = False

        if note is not None:
            self.assembly = note.getOwnerPart() or assembly
            self._anchor_snapshot = {
                "Target": note.Target,
                "LocalAnchor": App.Vector(note.LocalAnchor),
                "BasePosition": App.Vector(note.BasePosition),
                "JointSide": note.JointSide,
            }
            existing = list(note.LabelText) if note.LabelText else []
            command_name = translate("Assembly", "Edit Review Note")
        else:
            if assembly is None or target_data is None:
                raise ValueError("Create mode requires assembly and target_data")
            # Freeze the create-time anchor — later selection must not retarget.
            self.target_data = {
                "target_obj": target_data["target_obj"],
                "sub_list": list(target_data.get("sub_list") or []),
                "local_anchor": App.Vector(target_data["local_anchor"]),
                "joint_side": target_data.get("joint_side", JOINT_SIDE_NONE),
            }
            existing = []
            command_name = translate("Assembly", "Add Review Note")

        self.form = QtWidgets.QWidget()
        self.form.setWindowTitle(translate("Assembly", "Review Note"))
        layout = QtWidgets.QVBoxLayout(self.form)

        hint = QtWidgets.QLabel(
            translate(
                "Assembly",
                "Edit the note text. You can rotate/zoom the view and select "
                "geometry for @mentions. The note stays anchored to the original target.",
            )
        )
        hint.setWordWrap(True)
        layout.addWidget(hint)

        self.edit = ReviewNoteAtAwarePlainTextEdit(self.form, popup_width=280)
        self.edit.setPlaceholderText(translate("Assembly", "Review note"))
        self.edit.setMinimumHeight(160)
        if existing:
            self.edit.setPlainText("\n".join(existing))
            cursor = self.edit.textCursor()
            cursor.movePosition(QtGui.QTextCursor.End)
            self.edit.setTextCursor(cursor)
        self.edit._remember_cursor_pos()
        layout.addWidget(self.edit)

        model = QtGui.QStandardItemModel(self.form)
        completer = QtWidgets.QCompleter(model, self.form)
        self.edit.set_at_completer(completer, model)
        self.edit.cursorPositionChanged.connect(self.edit.refresh_at_completer)

        Gui.ActiveDocument.openCommand(command_name)
        try:
            Gui.Selection.addObserver(self, Gui.Selection.ResolveMode.NoResolve)
            self._selection_observer = True
        except Exception:
            self._selection_observer = False
        self.edit.setFocus()

    def getStandardButtons(self):
        return (
            QtWidgets.QDialogButtonBox.Ok
            | QtWidgets.QDialogButtonBox.Apply
            | QtWidgets.QDialogButtonBox.Cancel
        )

    def isAllowedAlterSelection(self):
        return True

    def isAllowedAlterView(self):
        return True

    def isAllowedAlterDocument(self):
        return True

    def needsFullSpace(self):
        return False

    def open(self):
        pass

    def addSelection(self, doc_name, obj_name, sub_name, mouse_pos=None):
        """Insert @ObjectPath.FaceN at the last caret; never retarget the note."""
        if self._closed or self._inserting_selection:
            return
        path = self._at_path_from_pick(doc_name, obj_name, sub_name)
        if not path:
            return
        self._inserting_selection = True
        try:
            self.edit.insert_at_reference(path)
        finally:
            self._inserting_selection = False

    def setPreselection(self, doc_name, obj_name, sub_name):
        pass

    def removeSelection(self, doc_name, obj_name, sub_name, mouse_pos=None):
        pass

    def clearSelection(self, doc_name):
        pass

    def _at_path_from_pick(self, doc_name, obj_name, sub_name):
        doc = App.getDocument(doc_name) if doc_name else App.ActiveDocument
        if doc is None:
            return None
        obj = doc.getObject(obj_name)
        if obj is None:
            return None
        tid = getattr(obj, "TypeId", "")
        if tid in ("Assembly::ReviewNote", "Assembly::ReviewNoteGroup"):
            return None
        return review_note_at_path_from_selection(obj, sub_name)

    def _current_lines(self):
        return _normalize_label_lines(self.edit.toPlainText())

    def _anchor_still_frozen(self):
        """True when Target/LocalAnchor were not changed by selection while open."""
        if self.note is None:
            return True
        if self._anchor_snapshot is None:
            return True
        snap = self._anchor_snapshot
        try:
            cur_target, cur_subs = self.note.Target
            snap_target, snap_subs = snap["Target"]
            if cur_target != snap_target or list(cur_subs) != list(snap_subs):
                return False
            if not self.note.LocalAnchor.isEqual(snap["LocalAnchor"], 1e-9):
                return False
            if str(self.note.JointSide) != str(snap["JointSide"]):
                return False
        except Exception:
            return False
        return True

    def apply_text(self):
        """Commit current editor text to the note without closing the panel."""
        lines = self._current_lines()
        if lines is None:
            return False

        if self.note is None:
            note = create_review_note(
                self.assembly,
                self.target_data,
                lines,
                open_transaction=False,
            )
            if note is None:
                return False
            self.note = note
            self._anchor_snapshot = {
                "Target": note.Target,
                "LocalAnchor": App.Vector(note.LocalAnchor),
                "BasePosition": App.Vector(note.BasePosition),
                "JointSide": note.JointSide,
            }
        else:
            self.note.LabelText = list(lines)

        return self._anchor_still_frozen()

    def clicked(self, button):
        if button == QtWidgets.QDialogButtonBox.Apply:
            self.apply_text()

    def accept(self):
        if not self.apply_text():
            return False
        try:
            Gui.ActiveDocument.commitCommand()
        except Exception:
            pass
        self._cleanup(close_dialog=True)
        return True

    def reject(self):
        try:
            Gui.ActiveDocument.abortCommand()
        except Exception:
            pass
        self._cleanup(close_dialog=True)
        return True

    def _cleanup(self, close_dialog=False):
        global _active_review_note_task
        if self._selection_observer:
            try:
                Gui.Selection.removeObserver(self)
            except Exception:
                pass
            self._selection_observer = False
        if self._closed:
            if close_dialog and Gui.Control.activeDialog():
                try:
                    Gui.Control.closeDialog()
                except Exception:
                    pass
            return
        self._closed = True
        if _active_review_note_task is self:
            _active_review_note_task = None
        if close_dialog and Gui.Control.activeDialog():
            try:
                Gui.Control.closeDialog()
            except Exception:
                pass


def open_review_note_text_task(note=None, assembly=None, target_data=None):
    """Show the modeless Review Note text Task panel and return it."""
    global _active_review_note_task
    if not App.GuiUp:
        raise RuntimeError("open_review_note_text_task requires a GUI session")

    _close_active_task_dialog()
    panel = TaskAssemblyReviewNote(
        note=note, assembly=assembly, target_data=target_data
    )
    _active_review_note_task = panel
    Gui.Control.showDialog(panel)
    return panel


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
        if not App.GuiUp:
            return False
        open_review_note_text_task(note=note)
        return True

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
        return is_add_review_note_task_eligible()

    def Activated(self):
        selection = Gui.Selection.getSelectionEx("*", 0)
        owner = find_review_note_owner(selection=selection)
        if owner is None:
            return

        # Tasks / context may offer the action while the owning Assembly is
        # inactive — activate it first and keep the user's selection.
        owner = ensure_review_note_owner_active(owner, selection=selection)
        selection = Gui.Selection.getSelectionEx("*", 0)
        owner = find_review_note_owner(selection=selection) or owner

        if not is_add_review_note_eligible(owner, selection):
            return

        targets = collect_review_note_targets(owner, selection)
        if len(targets) != 1:
            return

        open_review_note_text_task(assembly=owner, target_data=targets[0])


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
