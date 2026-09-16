# SPDX-License-Identifier: LGPL-2.1-or-later

"""Shared Sketcher GUI test helper.

Env-gated traces are default-off so CI behaviour is unchanged.

Trace APIs:
- gui_trace_enabled()
- note_gui_trace(**fields) / trace_before_command(name)
- move / click / right_click / wait_until (unchanged default timing)
"""

from __future__ import annotations

import json
import math
import os
import re
import time
import unittest

import FreeCAD

try:
    import FreeCADGui

    GUI_MODULE_AVAILABLE = True
except ImportError:
    FreeCADGui = None
    GUI_MODULE_AVAILABLE = False

try:
    from PySide import QtCore, QtGui

    QT_MODULE_AVAILABLE = True
except ImportError:
    QtCore = None
    QtGui = None
    QT_MODULE_AVAILABLE = False

GUI_TRACE_ENV = "FREECAD_SKETCHER_GUI_TRACE"
GUI_TRACE_DIR_ENV = "FREECAD_SKETCHER_GUI_TRACE_DIR"


def gui_available():
    if not GUI_MODULE_AVAILABLE:
        return False

    try:
        return FreeCADGui.getMainWindow() is not None
    except (AttributeError, RuntimeError):
        return False


def gui_trace_enabled():
    value = os.environ.get(GUI_TRACE_ENV, "").strip().lower()
    return value not in ("", "0", "false", "no", "off")


def _gui_trace_dir():
    explicit = os.environ.get(GUI_TRACE_DIR_ENV, "").strip()
    if explicit:
        os.makedirs(explicit, exist_ok=True)
        return explicit
    path = os.path.join(os.path.realpath(os.environ.get("TMPDIR", "/tmp")), "sketcher-gui-trace")
    os.makedirs(path, exist_ok=True)
    return path


def _jsonable(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        if isinstance(value, float) and not math.isfinite(value):
            return str(value)
        return value
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in value.items()}
    if hasattr(value, "name") and hasattr(value, "value") and not hasattr(value, "x"):
        try:
            return str(value)
        except Exception:
            return repr(value)
    if hasattr(value, "x") and hasattr(value, "y"):
        try:
            x = value.x() if callable(value.x) else value.x
            y = value.y() if callable(value.y) else value.y
            if hasattr(value, "z"):
                z = value.z() if callable(value.z) else value.z
                return [float(x), float(y), float(z)]
            return [int(x) if isinstance(x, int) else float(x), int(y) if isinstance(y, int) else float(y)]
        except Exception:
            pass
    try:
        return str(value)
    except Exception as exc:
        return {"repr_error": str(exc)}


class SketcherGuiTestCase(unittest.TestCase):
    def setUp(self):
        super().setUp()
        if not gui_available():
            self.skipTest("GUI not available")
        self._gui_trace_seq = 0
        self._gui_trace_ctx = {}
        self._gui_trace_coin_events = []
        self._gui_trace_coin_cb = None
        self._gui_trace_coin_view = None
        self._gui_trace_records = []
        if gui_trace_enabled():
            self._init_gui_trace()

    def tearDown(self):
        try:
            self.cleanup_gui_document(getattr(self, "doc", None))
        finally:
            self._finalize_gui_trace()
            super().tearDown()

    def pump(self, timeout_ms=50):
        if not QT_MODULE_AVAILABLE:
            return

        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(timeout_ms, loop.quit)
        loop.exec_()

    def flush_gui(self, timeout_ms=0):
        if not gui_available():
            return

        if QT_MODULE_AVAILABLE:
            QtGui.QApplication.processEvents()

        FreeCADGui.updateGui()

        if timeout_ms:
            self.pump(timeout_ms)

    def cleanup_gui_document(self, doc, timeout_ms=80):
        if not gui_available():
            return

        if gui_trace_enabled():
            self._save_gui_trace_screenshot("pre_cleanup")
            self._write_gui_trace(
                "pre_cleanup",
                extra={"note": "screenshot and state captured before resetEdit/closeDocument"},
            )

        gui_doc = FreeCADGui.ActiveDocument
        if gui_doc is not None:
            gui_doc.resetEdit()
            self.flush_gui(timeout_ms)

        if FreeCADGui.Control.activeDialog() is not None:
            FreeCADGui.Control.closeDialog()
            self.flush_gui(timeout_ms)

        if doc is not None and doc.Name in FreeCAD.listDocuments():
            FreeCAD.closeDocument(doc.Name)
            self.flush_gui(timeout_ms)

    def wait_until(self, predicate, timeout_ms=1000, step_ms=50):
        remaining = timeout_ms
        while remaining > 0:
            if predicate():
                return True
            self.flush_gui(step_ms)
            remaining -= step_ms
        return predicate()

    def send_mouse(self, widget, event_type, pos, button, buttons):
        if gui_trace_enabled():
            self._send_mouse_traced(widget, event_type, pos, button, buttons)
            return

        global_pos = widget.mapToGlobal(pos)
        event = QtGui.QMouseEvent(
            event_type,
            pos,
            global_pos,
            button,
            buttons,
            QtCore.Qt.NoModifier,
        )
        QtGui.QApplication.sendEvent(widget, event)

    def click(self, widget, pos):
        self._trace_interaction_phase("before_move_click", widget, pos, action="click")
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonPress,
            pos,
            QtCore.Qt.LeftButton,
            QtCore.Qt.LeftButton,
        )
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonRelease,
            pos,
            QtCore.Qt.LeftButton,
            QtCore.Qt.NoButton,
        )
        self.pump(120)
        self._trace_interaction_phase("after_handling", widget, pos, action="click")

    def right_click(self, widget, pos):
        self._trace_interaction_phase("before_move_click", widget, pos, action="right_click")
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonPress,
            pos,
            QtCore.Qt.RightButton,
            QtCore.Qt.RightButton,
        )
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonRelease,
            pos,
            QtCore.Qt.RightButton,
            QtCore.Qt.NoButton,
        )
        self.pump(120)
        self._trace_interaction_phase("after_handling", widget, pos, action="right_click")

    def move(self, widget, pos):
        self._trace_interaction_phase("before_move_click", widget, pos, action="move")
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseMove,
            pos,
            QtCore.Qt.NoButton,
            QtCore.Qt.NoButton,
        )
        self.pump(80)
        self._trace_interaction_phase("after_handling", widget, pos, action="move")

    def key_click(self, widget, key, text=""):
        press = QtGui.QKeyEvent(QtCore.QEvent.KeyPress, key, QtCore.Qt.NoModifier, text)
        release = QtGui.QKeyEvent(QtCore.QEvent.KeyRelease, key, QtCore.Qt.NoModifier, text)
        QtGui.QApplication.sendEvent(widget, press)
        QtGui.QApplication.sendEvent(widget, release)
        self.pump(60)

    def clamp_to_widget(self, widget, pos, margin=10):
        rect = widget.rect()
        return QtCore.QPoint(
            max(margin, min(pos.x(), rect.right() - margin)),
            max(margin, min(pos.y(), rect.bottom() - margin)),
        )

    def device_pixel_ratio(self, widget):
        return widget.devicePixelRatioF()

    def viewport_to_qpoint(self, view, viewport, point):
        _, height = view.getSize()
        scale = self.device_pixel_ratio(viewport)
        x = int(round(point[0] / scale))
        y = int(round((height - point[1] - 1) / scale))
        return QtCore.QPoint(x, y)

    def note_gui_trace(self, **fields):
        """Attach fields (world_point, intended_subelements, coin_point) to later events."""
        if not gui_trace_enabled():
            return
        self._gui_trace_ctx.update(fields)

    def trace_before_command(self, command_name):
        """Snapshot document/edit/handler/selection/prefs before Gui.runCommand."""
        if not gui_trace_enabled():
            return
        self.note_gui_trace(command_name=command_name)
        self._write_gui_trace(
            "before_command",
            extra={"command_name": command_name, "action": "before_command"},
        )

    def _init_gui_trace(self):
        root = _gui_trace_dir()
        slug = re.sub(r"[^A-Za-z0-9._=+-]+", "_", self.id())[:180]
        self._gui_trace_case_dir = os.path.join(root, slug)
        os.makedirs(self._gui_trace_case_dir, exist_ok=True)
        meta = {
            "test_id": self.id(),
            "pid": os.getpid(),
            "started": time.time(),
            "env": GUI_TRACE_ENV,
            "dir": self._gui_trace_case_dir,
        }
        with open(os.path.join(self._gui_trace_case_dir, "meta.json"), "w", encoding="utf-8") as handle:
            json.dump(_jsonable(meta), handle, indent=2)

    def _finalize_gui_trace(self):
        self._remove_coin_tap()
        if not gui_trace_enabled() or not getattr(self, "_gui_trace_case_dir", None):
            return
        summary_path = os.path.join(self._gui_trace_case_dir, "summary.json")
        try:
            with open(summary_path, "w", encoding="utf-8") as handle:
                json.dump(_jsonable(self._gui_trace_records), handle, indent=2)
        except Exception:
            pass

    def _active_view(self):
        view = getattr(self, "view", None)
        if view is not None:
            return view
        try:
            doc = FreeCADGui.ActiveDocument
            if doc is not None:
                return doc.activeView()
        except Exception:
            return None
        return None

    def _ensure_coin_tap(self, view):
        if view is None or view is self._gui_trace_coin_view:
            return
        self._remove_coin_tap()
        self._gui_trace_coin_events = []

        def _on_coin(event):
            self._gui_trace_coin_events.append(self._coin_event_record(event))

        try:
            view.addEventCallback("SoEvent", _on_coin)
            self._gui_trace_coin_cb = _on_coin
            self._gui_trace_coin_view = view
        except Exception as exc:
            self._gui_trace_ctx["coin_tap_error"] = str(exc)

    def _remove_coin_tap(self):
        view = self._gui_trace_coin_view
        callback = self._gui_trace_coin_cb
        self._gui_trace_coin_view = None
        self._gui_trace_coin_cb = None
        if view is None or callback is None:
            return
        try:
            view.removeEventCallback("SoEvent", callback)
        except Exception:
            pass

    def _coin_event_record(self, event):
        try:
            return {
                "type": event.get("Type") if hasattr(event, "get") else str(event),
                "position": _jsonable(event.get("Position") if hasattr(event, "get") else None),
                "state": event.get("State") if hasattr(event, "get") else None,
                "button": event.get("Button") if hasattr(event, "get") else None,
            }
        except Exception as exc:
            return {"error": str(exc), "raw": str(event)}

    def _camera_info(self, view):
        info = {
            "type": None,
            "height": None,
            "finite": None,
            "position": None,
            "near": None,
            "far": None,
            "aspect": None,
        }
        if view is None:
            return info
        try:
            info["type"] = view.getCameraType()
        except Exception as exc:
            info["type_error"] = str(exc)
        try:
            raw = view.getCamera()
            info["raw"] = raw
            match = re.search(r"\bheight\s+([^\s}]+)", raw or "")
            if match:
                info["height"] = float(match.group(1))
        except Exception as exc:
            info["raw_error"] = str(exc)
        try:
            camera = view.getCameraNode()
            if camera is not None:
                position = camera.position.getValue()
                info["position"] = [float(position[0]), float(position[1]), float(position[2])]
                info["near"] = float(camera.nearDistance.getValue())
                info["far"] = float(camera.farDistance.getValue())
                info["aspect"] = float(camera.aspectRatio.getValue())
                if hasattr(camera, "height"):
                    info["height"] = float(camera.height.getValue())
                elif hasattr(camera, "heightAngle"):
                    info["heightAngle"] = float(camera.heightAngle.getValue())
        except Exception as exc:
            info["node_error"] = str(exc)

        values = [info.get("height"), info.get("near"), info.get("far")]
        info["finite"] = all(
            value is None or (isinstance(value, (int, float)) and math.isfinite(float(value)))
            for value in values
        )
        height = info.get("height")
        try:
            info["usable"] = bool(
                info["finite"]
                and height is not None
                and 1e-6 < abs(float(height)) < 1e6
            )
        except (TypeError, ValueError):
            info["usable"] = False
        return info

    def _selection_info(self):
        selected = []
        try:
            for item in FreeCADGui.Selection.getSelectionEx():
                selected.append(
                    {
                        "document": item.DocumentName,
                        "object": item.ObjectName,
                        "subs": list(item.SubElementNames),
                    }
                )
        except Exception as exc:
            selected = [{"error": str(exc)}]
        preselection = None
        try:
            pre = FreeCADGui.Selection.getPreselection()
            if pre is not None and getattr(pre, "Object", None) is not None:
                preselection = {
                    "document": getattr(pre, "DocName", None) or getattr(pre, "DocumentName", None),
                    "object": getattr(pre, "ObjectName", None) or getattr(pre.Object, "Name", None),
                    "subs": list(getattr(pre, "SubElementNames", []) or getattr(pre, "SubNames", []) or []),
                }
        except Exception as exc:
            preselection = {"error": str(exc)}
        return {"selection": selected, "gui_preselection": preselection}

    def _preselection_at_coin(self, coin_point):
        if coin_point is None:
            return None
        try:
            import SketcherGui

            point = (int(coin_point[0]), int(coin_point[1]))
            return SketcherGui.getActiveSketchPreselection(point)
        except Exception as exc:
            return {"error": str(exc)}

    def _handler_info(self, widget):
        info = {
            "in_edit": None,
            "edit_name": None,
            "active_dialog": None,
            "cursor_shape": None,
            "cursor_name": None,
        }
        try:
            gui_doc = FreeCADGui.ActiveDocument
            edit = gui_doc.getInEdit() if gui_doc is not None else None
            info["in_edit"] = edit is not None
            if edit is not None:
                obj = getattr(edit, "Object", None)
                info["edit_name"] = getattr(obj, "Name", None) or str(edit)
        except Exception as exc:
            info["in_edit_error"] = str(exc)
        try:
            info["active_dialog"] = FreeCADGui.Control.activeDialog() is not None
        except Exception as exc:
            info["dialog_error"] = str(exc)
        try:
            if widget is not None:
                shape = widget.cursor().shape()
                info["cursor_name"] = str(shape)
                value = getattr(shape, "value", None)
                info["cursor_shape"] = int(value) if isinstance(value, int) else None
        except Exception as exc:
            info["cursor_error"] = str(exc)
        return info

    def _pref_info(self):
        try:
            params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher")
            return {
                "ContinuousConstraintMode": params.GetBool("ContinuousConstraintMode", True),
                "ShowDialogOnDistanceConstraint": params.GetBool(
                    "ShowDialogOnDistanceConstraint", True
                ),
            }
        except Exception as exc:
            return {"error": str(exc)}

    def _document_info(self):
        doc = getattr(self, "doc", None) or FreeCAD.ActiveDocument
        sketch = getattr(self, "sketch", None)
        info = {
            "active_document": getattr(FreeCAD.ActiveDocument, "Name", None),
            "test_document": getattr(doc, "Name", None),
            "documents": list(FreeCAD.listDocuments()),
            "sketch": getattr(sketch, "Name", None),
            "constraint_count": getattr(sketch, "ConstraintCount", None),
        }
        if sketch is not None and getattr(sketch, "ConstraintCount", 0):
            try:
                info["constraints"] = [
                    {"type": constraint.Type, "value": constraint.Value}
                    for constraint in sketch.Constraints
                ]
            except Exception as exc:
                info["constraints_error"] = str(exc)
        return info

    def _geometry_snapshot(self, view, widget, pos):
        snapshot = {
            "qt_local": _jsonable(pos),
            "qt_global": None,
            "qt_window": None,
            "inside_viewport": None,
            "widget": None,
            "view_size": None,
            "widget_size": None,
            "device_pixel_ratio": None,
            "coin_from_ctx": _jsonable(self._gui_trace_ctx.get("coin_point")),
            "world_point": _jsonable(self._gui_trace_ctx.get("world_point")),
            "intended_subelements": _jsonable(self._gui_trace_ctx.get("intended_subelements")),
        }
        if widget is not None and pos is not None:
            try:
                snapshot["qt_global"] = _jsonable(widget.mapToGlobal(pos))
                window = widget.window()
                snapshot["qt_window"] = _jsonable(widget.mapTo(window, pos) if window else None)
                snapshot["inside_viewport"] = bool(widget.rect().contains(pos))
                snapshot["widget"] = {
                    "class": type(widget).__name__,
                    "object_name": widget.objectName(),
                    "rect": [widget.rect().x(), widget.rect().y(), widget.rect().width(), widget.rect().height()],
                }
                snapshot["widget_size"] = [widget.width(), widget.height()]
                snapshot["device_pixel_ratio"] = float(self.device_pixel_ratio(widget))
            except Exception as exc:
                snapshot["widget_error"] = str(exc)
        if view is not None:
            try:
                snapshot["view_size"] = list(view.getSize())
            except Exception as exc:
                snapshot["view_size_error"] = str(exc)
            coin = self._gui_trace_ctx.get("coin_point")
            world = self._gui_trace_ctx.get("world_point")
            if world is not None:
                try:
                    snapshot["coin_getPointOnScreen"] = list(view.getPointOnScreen(world))
                except Exception as exc:
                    snapshot["coin_getPointOnScreen_error"] = str(exc)
                try:
                    snapshot["coin_getPointOnViewport"] = list(view.getPointOnViewport(world))
                except Exception as exc:
                    snapshot["coin_getPointOnViewport_error"] = str(exc)
            pick_coin = (
                coin
                or snapshot.get("coin_getPointOnViewport")
                or snapshot.get("coin_getPointOnScreen")
            )
            if pick_coin is not None:
                try:
                    reverse = view.getPointOnFocalPlane(int(pick_coin[0]), int(pick_coin[1]))
                    snapshot["reproject_focal"] = _jsonable(reverse)
                    if world is not None:
                        dx = float(reverse.x) - float(world.x)
                        dy = float(reverse.y) - float(world.y)
                        snapshot["reproject_xy_error"] = [dx, dy]
                        snapshot["reproject_ok"] = (dx * dx + dy * dy) ** 0.5 < 2.0
                except Exception as exc:
                    snapshot["reproject_error"] = str(exc)
                snapshot["preselection_at_coin"] = self._preselection_at_coin(pick_coin)
                try:
                    snapshot["object_info"] = view.getObjectInfo(
                        (int(pick_coin[0]), int(pick_coin[1]))
                    )
                except Exception as exc:
                    snapshot["object_info_error"] = str(exc)
            else:
                snapshot["preselection_at_coin"] = None
        snapshot["camera"] = self._camera_info(view)
        return snapshot

    def _classify(self, record):
        geometry = record.get("geometry") or {}
        handler = record.get("handler") or {}
        coin_events = record.get("coin_events") or []
        intended = geometry.get("intended_subelements") or []
        preselection = geometry.get("preselection_at_coin") or {}
        names = []
        if isinstance(preselection, dict):
            names = list(preselection.get("SubElementNames") or [])
        reasons = []
        coin = geometry.get("coin_getPointOnViewport") or geometry.get("coin_from_ctx")
        if coin in ((0, 0), [0, 0], (0, 0.0), [0, 0.0]) or geometry.get("inside_viewport") is False:
            reasons.append("wrong_projection")
        if geometry.get("reproject_ok") is False:
            reasons.append("wrong_projection")
        camera = geometry.get("camera") or {}
        if not camera.get("finite", True) or camera.get("usable") is False:
            reasons.append("wrong_projection")
        context = record.get("context") or {}
        delivered = coin_events or context.get("last_coin_events") or []
        button_events = [
            event
            for event in delivered
            if "MouseButton" in str(event.get("type", ""))
        ]
        if record.get("action") in ("click", "right_click") and record.get("phase") == "after_handling":
            qt_accepted = record.get("qt_accepted")
            if qt_accepted is None:
                qt_accepted = context.get("last_qt_accepted")
            if not button_events and not delivered:
                reasons.append("wrong_delivery")
            elif qt_accepted is False:
                reasons.append("wrong_delivery")
        intended_hit = bool(intended) and any(name in names for name in intended)
        if record.get("phase") == "after_handling" and record.get("action") == "click":
            if intended and not intended_hit:
                reasons.append("pick_miss")
            elif not names and geometry.get("world_point") is not None:
                reasons.append("pick_miss")
            if intended_hit:
                after_count = (record.get("document") or {}).get("constraint_count")
                if after_count == 0:
                    reasons.append("command_received_pick_and_failed")
        if "wrong_projection" in reasons:
            primary = "wrong_projection"
        elif "wrong_delivery" in reasons:
            primary = "wrong_delivery"
        elif "pick_miss" in reasons:
            primary = "pick_miss"
        elif "command_received_pick_and_failed" in reasons:
            primary = "command_received_pick_and_failed"
        else:
            primary = "ok_or_indeterminate"
        return {"primary": primary, "reasons": reasons, "preselection_names": names}

    def _snapshot_state(self, widget=None, pos=None):
        view = self._active_view()
        if widget is None:
            widget = getattr(self, "viewport", None)
        return {
            "document": self._document_info(),
            "handler": self._handler_info(widget),
            "selection": self._selection_info(),
            "prefs": self._pref_info(),
            "geometry": self._geometry_snapshot(view, widget, pos),
            "context": _jsonable(self._gui_trace_ctx),
        }

    def _write_gui_trace(self, phase, widget=None, pos=None, extra=None):
        if not gui_trace_enabled() or not getattr(self, "_gui_trace_case_dir", None):
            return
        try:
            self._gui_trace_seq += 1
            record = {
                "seq": self._gui_trace_seq,
                "phase": phase,
                "time": time.time(),
                "test_id": self.id(),
            }
            record.update(self._snapshot_state(widget=widget, pos=pos))
            if extra:
                record.update(extra)
            if phase in ("after_handling", "during_delivery", "pre_cleanup"):
                record["classification"] = self._classify(record)
            self._gui_trace_records.append(record)
            name = f"{self._gui_trace_seq:04d}_{phase}.json"
            path = os.path.join(self._gui_trace_case_dir, name)
            with open(path, "w", encoding="utf-8") as handle:
                json.dump(_jsonable(record), handle, indent=2)
            index_path = os.path.join(self._gui_trace_case_dir, "events.jsonl")
            with open(index_path, "a", encoding="utf-8") as handle:
                handle.write(
                    json.dumps(
                        _jsonable(
                            {
                                "seq": record["seq"],
                                "phase": phase,
                                "classification": record.get("classification"),
                            }
                        )
                    )
                    + "\n"
                )
        except Exception as exc:
            try:
                path = os.path.join(self._gui_trace_case_dir, "trace_write_error.txt")
                with open(path, "a", encoding="utf-8") as handle:
                    handle.write(f"{phase}: {exc}\n")
            except Exception:
                pass

    def _trace_interaction_phase(self, phase, widget, pos, action):
        if not gui_trace_enabled():
            return
        if phase == "after_handling" and action in ("click", "right_click"):
            self._save_gui_trace_screenshot(f"{self._gui_trace_seq + 1:04d}_{action}")
        self._write_gui_trace(phase, widget=widget, pos=pos, extra={"action": action})

    def _send_mouse_traced(self, widget, event_type, pos, button, buttons):
        try:
            self._ensure_coin_tap(self._active_view())
        except Exception:
            pass
        before_len = len(getattr(self, "_gui_trace_coin_events", []))
        global_pos = widget.mapToGlobal(pos)
        event = QtGui.QMouseEvent(
            event_type,
            pos,
            global_pos,
            button,
            buttons,
            QtCore.Qt.NoModifier,
        )
        accepted = bool(QtGui.QApplication.sendEvent(widget, event))
        try:
            new_events = self._gui_trace_coin_events[before_len:]
            self._gui_trace_ctx["last_coin_events"] = new_events
            self._gui_trace_ctx["last_qt_accepted"] = accepted
            self._gui_trace_ctx["last_qt_event"] = str(event_type)
            self._write_gui_trace(
                "during_delivery",
                widget=widget,
                pos=pos,
                extra={
                    "action": str(event_type),
                    "qt_button": _jsonable(button),
                    "qt_buttons": _jsonable(buttons),
                    "qt_accepted": accepted,
                    "qt_spontaneous": bool(event.spontaneous())
                    if hasattr(event, "spontaneous")
                    else None,
                    "receiver": {
                        "class": type(widget).__name__,
                        "object_name": widget.objectName() if widget is not None else None,
                    },
                    "coin_events": new_events,
                    "coin_event_count": len(new_events),
                    "handler_received_coin": bool(new_events)
                    and any(
                        "MouseButton" in str(item.get("type", ""))
                        or "Location2" in str(item.get("type", ""))
                        for item in new_events
                    ),
                },
            )
        except Exception as exc:
            self._gui_trace_ctx["trace_error"] = str(exc)

    def _save_gui_trace_screenshot(self, name):
        if not getattr(self, "_gui_trace_case_dir", None):
            return
        path = os.path.join(self._gui_trace_case_dir, f"{name}.png")
        errors = []
        view = self._active_view()
        if view is not None:
            try:
                view.saveImage(path)
            except Exception as exc:
                errors.append(f"saveImage: {exc}")
        if (not os.path.isfile(path) or os.path.getsize(path) == 0) and QT_MODULE_AVAILABLE:
            widget = getattr(self, "viewport", None)
            if widget is None:
                try:
                    widget = FreeCADGui.getMainWindow()
                except Exception as exc:
                    errors.append(f"main_window: {exc}")
                    widget = None
            if widget is not None:
                try:
                    widget.grab().save(path)
                except Exception as exc:
                    errors.append(f"grab: {exc}")
        if errors and (not os.path.isfile(path) or os.path.getsize(path) == 0):
            try:
                with open(
                    os.path.join(self._gui_trace_case_dir, f"{name}.error.txt"),
                    "w",
                    encoding="utf-8",
                ) as handle:
                    handle.write("\n".join(errors))
            except Exception:
                pass
