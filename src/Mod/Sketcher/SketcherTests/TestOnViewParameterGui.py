# SPDX-License-Identifier: LGPL-2.1-or-later

import json
import os
import time

import FreeCAD
from PySide import QtCore, QtGui
from SketcherTests.GuiTestCase import FreeCADGui, SketcherGuiTestCase


class TestOnViewParameterGui(SketcherGuiTestCase):
    KEYS = {
        "0": QtCore.Qt.Key_0,
        "1": QtCore.Qt.Key_1,
        "2": QtCore.Qt.Key_2,
        "3": QtCore.Qt.Key_3,
        "4": QtCore.Qt.Key_4,
        "5": QtCore.Qt.Key_5,
        "6": QtCore.Qt.Key_6,
        "7": QtCore.Qt.Key_7,
        "8": QtCore.Qt.Key_8,
        "9": QtCore.Qt.Key_9,
        ".": QtCore.Qt.Key_Period,
        "-": QtCore.Qt.Key_Minus,
        " ": QtCore.Qt.Key_Space,
    }

    def setUp(self):
        super().setUp()

        self.sketcher_tool_params = FreeCAD.ParamGet(
            "User parameter:BaseApp/Preferences/Mod/Sketcher/Tools"
        )
        self.had_ovp_visibility = "OnViewParameterVisibility" in self.sketcher_tool_params.GetInts()
        self.old_ovp_visibility = self.sketcher_tool_params.GetInt("OnViewParameterVisibility", 1)
        self.sketcher_tool_params.SetInt("OnViewParameterVisibility", 1)

        FreeCADGui.activateWorkbench("SketcherWorkbench")
        self.doc = FreeCAD.newDocument("TestOnViewParameterGui")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        self.doc.recompute()
        self._origin_trace_seq = 0

    def tearDown(self):
        try:
            self.save_origin_trace("before_cleanup")
        finally:
            super().tearDown()

    def tearDown(self):
        try:
            if self.had_ovp_visibility:
                self.sketcher_tool_params.SetInt(
                    "OnViewParameterVisibility", self.old_ovp_visibility
                )
            else:
                self.sketcher_tool_params.RemInt("OnViewParameterVisibility")
        finally:
            super().tearDown()

    def pack_color(self, color):
        r, g, b, a = color
        return (
            int(r * 255.0 + 0.5) << 24
            | int(g * 255.0 + 0.5) << 16
            | int(b * 255.0 + 0.5) << 8
            | int(a * 255.0 + 0.5)
        )

    def key_text(self, widget, text):
        for ch in text:
            self.key_click(widget, self.KEYS[ch], ch)

    def active_spinbox(self):
        widget = QtGui.QApplication.focusWidget()
        if isinstance(widget, QtGui.QAbstractSpinBox):
            return widget
        if isinstance(widget, QtGui.QLineEdit):
            parent = widget.parent()
            if isinstance(parent, QtGui.QAbstractSpinBox):
                return parent
        return None

    def visible_spinboxes(self):
        main_window = FreeCADGui.getMainWindow()
        return [
            spinbox
            for spinbox in main_window.findChildren(QtGui.QAbstractSpinBox)
            if spinbox.isVisible()
        ]

    def ovp_spinboxes(self):
        spinboxes = self.visible_spinboxes()
        self.assertEqual(len(spinboxes), 2, "Expected exactly two visible rectangle OVPs")
        return spinboxes

    def focus_ovp_spinbox(self, spinbox):
        main_window = FreeCADGui.getMainWindow()
        main_window.raise_()
        main_window.activateWindow()
        spinbox.setFocus(QtCore.Qt.OtherFocusReason)
        self.flush_gui()

    def ovp_lock_icon(self, spinbox):
        return spinbox.findChild(QtGui.QLabel, "onViewParameterLockIcon")

    def active_task_dialog(self):
        return FreeCADGui.Control.activeTaskDialog()

    def assert_sketch_edit_active(self):
        edit = FreeCADGui.ActiveDocument.getInEdit()
        self.assertIsNotNone(edit, "Expected sketch edit mode to remain active")
        self.assertTrue(
            edit.isDerivedFrom("SketcherGui::ViewProviderSketch"),
            "Expected a Sketcher view provider to remain in edit mode",
        )

    def assert_sketch_edit_inactive(self):
        self.assertIsNone(
            FreeCADGui.ActiveDocument.getInEdit(),
            "Expected sketch edit mode to be inactive",
        )

    def begin_sketch_edit_with_task_dialog(self):
        FreeCADGui.getMainWindow().show()
        FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)
        self.pump(200)
        self.assert_sketch_edit_active()
        self.assertIsNotNone(self.active_task_dialog(), "Expected the Sketcher task dialog to open")

    def begin_rectangle_with_visible_ovp(self):
        self.begin_sketch_edit_with_task_dialog()

        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        self.pump(100)

        viewport = view.graphicsView().viewport()
        origin = FreeCAD.Vector(0, 0, 0)

        def wait_for_origin_point():
            origin_point = None

            def origin_is_framed():
                nonlocal origin_point

                width, height = view.getSize()
                if width <= 0 or height <= 0:
                    return False

                view.fitAll()
                point = self.viewport_to_qpoint(view, viewport, view.getPointOnScreen(origin))
                interior_rect = viewport.rect().adjusted(20, 20, -20, -20)
                if not interior_rect.contains(point):
                    return False

                origin_point = point
                return True

            self.assertTrue(
                self.wait_until(origin_is_framed, timeout_ms=5000, step_ms=100),
                "Expected the sketch origin to project to an interior viewport point",
            )
            self.assertIsNotNone(origin_point)
            return origin_point

        FreeCADGui.runCommand("Sketcher_CreateRectangle")
        self.pump(250)

        origin_point = wait_for_origin_point()
        first_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(origin_point.x() + 80, origin_point.y() - 60),
        )

        move_target = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(first_point.x() + 100, first_point.y() + 80),
        )

        self.move(viewport, first_point)
        self.click(viewport, first_point)
        self.move(viewport, move_target)

        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 2, timeout_ms=1000),
            "Expected the rectangle OVPs to become visible after the first click",
        )
        self.ovp_spinboxes()

        return viewport, first_point

    def origin_marker_index(self):
        from pivy import coin

        view = FreeCADGui.ActiveDocument.ActiveView
        root = view.getViewer().getSoRenderManager().getSceneGraph()
        search = coin.SoSearchAction()
        search.setName("OriginPointSet")
        search.setSearchingAll(True)
        search.apply(root)
        path = search.getPath()
        self.assertIsNotNone(path, "Expected the visible origin marker in the scene graph")
        return path.getTail().markerIndex.getValues()[0]

    def origin_marker_indices(self, marker_name):
        # MarkerSize is converted to a device-pixel size by the C++ view provider. Querying all
        # supported sizes keeps this semantic check independent of the display scale factor.
        return {
            FreeCADGui.getMarkerIndex(marker_name, size)
            for size in (5, 7, 9, 11, 13, 15, 20, 25, 30)
        }

    def origin_marker_is(self, marker_name):
        return self.origin_marker_index() in self.origin_marker_indices(marker_name)

    def origin_trace_enabled(self):
        value = os.environ.get("FREECAD_SKETCHER_ORIGIN_TRACE", "").strip().lower()
        return value not in ("", "0", "false", "no", "off")

    def origin_trace_dir(self):
        configured = os.environ.get("FREECAD_SKETCHER_ORIGIN_TRACE_DIR", "").strip()
        if configured:
            os.makedirs(configured, exist_ok=True)
            return configured
        return None

    def _coin_node_id(self, node):
        this = getattr(node, "this", None)
        if this is not None:
            return str(this)
        try:
            return hex(id(node))
        except Exception:
            return repr(node)

    def _path_names(self, path):
        names = []
        try:
            for index in range(path.getLength()):
                node = path.getNode(index)
                name = str(node.getName()) if hasattr(node, "getName") else ""
                type_name = ""
                try:
                    type_name = str(node.getTypeId().getName())
                except Exception:
                    type_name = type(node).__name__
                names.append(
                    {
                        "index": index,
                        "name": name,
                        "type": type_name,
                        "id": self._coin_node_id(node),
                    }
                )
        except Exception as exc:
            names.append({"error": str(exc)})
        return names

    def _classify_marker_index(self, marker_index):
        if marker_index in self.origin_marker_indices("CIRCLE_FILLED"):
            return "CIRCLE_FILLED"
        if marker_index in self.origin_marker_indices("CIRCLE_LINE"):
            return "CIRCLE_LINE"
        return f"other:{marker_index}"

    def _walk_named_nodes(self, root, node_name, max_nodes=32, max_depth=48):
        """Find named Coin nodes without SoSearchAction.ALL (empty-path crash)."""

        found = []

        def visit(node, names, depth):
            if node is None or len(found) >= max_nodes or depth > max_depth:
                return
            name = ""
            try:
                name = str(node.getName())
            except Exception:
                name = ""
            path_names = names + ([name] if name else [])
            if name == node_name:
                found.append((node, path_names))
            try:
                child_count = int(node.getNumChildren())
            except Exception:
                return
            for index in range(child_count):
                try:
                    visit(node.getChild(index), path_names, depth + 1)
                except Exception:
                    continue

        visit(root, [], 0)
        return found

    def _marker_record_from_node(self, node, path_names, source):
        values = []
        try:
            values = [int(value) for value in node.markerIndex.getValues()]
        except Exception:
            values = []
        marker_index = values[0] if values else None
        return {
            "source": source,
            "node_id": self._coin_node_id(node),
            "marker_index": marker_index,
            "marker_kind": None if marker_index is None else self._classify_marker_index(marker_index),
            "under_sketch_edit_root": "Sketch_EditRoot" in path_names,
            "path_names": path_names,
        }

    def _marker_record_from_path(self, path, source):
        try:
            if path is None or path.getLength() == 0:
                return None
            tail = path.getTail()
        except Exception as exc:
            return {"source": source, "error": str(exc)}
        ancestor_names = [entry["name"] for entry in self._path_names(path) if entry.get("name")]
        record = self._marker_record_from_node(tail, ancestor_names, source)
        record["path"] = self._path_names(path)
        return record

    def origin_marker_lookup_report(self):
        """Compare first-scene-graph lookup vs current edit-root / in-edit RootNode."""

        report = {
            "first_search": None,
            "all_origin_point_sets": [],
            "edit_root_origin_point_sets": [],
            "in_edit_rootnode_origin_point_sets": [],
            "edit_roots": [],
            "lookup_reads_current_edit_root": None,
        }
        try:
            from pivy import coin

            view = FreeCADGui.ActiveDocument.ActiveView
            scene = view.getViewer().getSoRenderManager().getSceneGraph()

            first_search = coin.SoSearchAction()
            first_search.setName("OriginPointSet")
            first_search.setSearchingAll(True)
            first_search.apply(scene)
            first_path = first_search.getPath()
            if first_path is not None:
                try:
                    copied = first_path.copy()
                except Exception:
                    copied = first_path
                report["first_search"] = self._marker_record_from_path(copied, "first_sosearch")

            for node, path_names in self._walk_named_nodes(scene, "OriginPointSet"):
                report["all_origin_point_sets"].append(
                    self._marker_record_from_node(node, path_names, "scene_walk")
                )

            for node, path_names in self._walk_named_nodes(scene, "Sketch_EditRoot"):
                report["edit_roots"].append(
                    {
                        "node_id": self._coin_node_id(node),
                        "path_names": path_names,
                    }
                )
                for origin_node, origin_names in self._walk_named_nodes(node, "OriginPointSet"):
                    report["edit_root_origin_point_sets"].append(
                        self._marker_record_from_node(
                            origin_node, path_names + origin_names, "sketch_edit_root"
                        )
                    )

            edit = (
                FreeCADGui.ActiveDocument.getInEdit()
                if FreeCADGui.ActiveDocument is not None
                else None
            )
            if edit is not None:
                try:
                    vp_root = edit.RootNode
                    for origin_node, origin_names in self._walk_named_nodes(vp_root, "OriginPointSet"):
                        report["in_edit_rootnode_origin_point_sets"].append(
                            self._marker_record_from_node(
                                origin_node, origin_names, "in_edit_rootnode"
                            )
                        )
                except Exception as exc:
                    report["in_edit_rootnode_error"] = str(exc)

            first = report["first_search"] or {}
            walk_under_edit = [
                record
                for record in report["all_origin_point_sets"]
                if record.get("under_sketch_edit_root")
            ]
            # VP RootNode does not contain OriginPointSet; identity is scene Sketch_EditRoot.
            report["lookup_reads_current_edit_root"] = bool(
                first.get("under_sketch_edit_root")
            ) and (len(walk_under_edit) <= 1)
        except Exception as exc:
            report["error"] = str(exc)
        return report

    def drawing_session_report(self, viewport=None, qt_point=None):
        report = {
            "document": None,
            "sketch": None,
            "in_edit": None,
            "task_dialog": False,
            "visible_spinboxes": 0,
            "geometry_count": None,
            "constraint_count": None,
            "create_line_is_active": None,
            "sketcher_tool_widgets": [],
            "focus_widget": None,
            "widget_at_click": None,
            "camera": None,
            "selection": [],
        }
        try:
            report["document"] = getattr(self.doc, "Name", None) if getattr(self, "doc", None) else None
            report["sketch"] = getattr(self.sketch, "Name", None) if getattr(self, "sketch", None) else None
            if getattr(self, "sketch", None) is not None:
                report["geometry_count"] = int(self.sketch.GeometryCount)
                report["constraint_count"] = int(self.sketch.ConstraintCount)

            gui_doc = FreeCADGui.ActiveDocument
            if gui_doc is not None:
                edit = gui_doc.getInEdit()
                if edit is not None:
                    report["in_edit"] = {
                        "name": getattr(edit, "Name", None) or str(edit),
                        "type": edit.TypeId if hasattr(edit, "TypeId") else None,
                    }

            report["task_dialog"] = self.active_task_dialog() is not None
            report["visible_spinboxes"] = len(self.visible_spinboxes())

            try:
                command = FreeCADGui.Command.get("Sketcher_CreateLine")
                report["create_line_is_active"] = bool(command.isActive()) if command else None
            except Exception as exc:
                report["create_line_is_active"] = f"error:{exc}"

            main_window = FreeCADGui.getMainWindow()
            if main_window is not None:
                for widget in main_window.findChildren(QtGui.QWidget):
                    try:
                        class_name = widget.metaObject().className()
                    except Exception:
                        continue
                    if "SketcherTool" not in class_name:
                        continue
                    report["sketcher_tool_widgets"].append(
                        {
                            "class": class_name,
                            "object_name": str(widget.objectName()),
                            "visible": bool(widget.isVisible()),
                        }
                    )

                focus = QtGui.QApplication.focusWidget()
                if focus is not None:
                    report["focus_widget"] = {
                        "class": focus.metaObject().className(),
                        "object_name": str(focus.objectName()),
                    }

            if viewport is not None and qt_point is not None:
                try:
                    global_pos = viewport.mapToGlobal(qt_point)
                    widget_at = QtGui.QApplication.widgetAt(global_pos)
                    report["widget_at_click"] = {
                        "qt_local": [int(qt_point.x()), int(qt_point.y())],
                        "qt_global": [int(global_pos.x()), int(global_pos.y())],
                        "inside_viewport": bool(viewport.rect().contains(qt_point)),
                        "class": widget_at.metaObject().className() if widget_at else None,
                        "object_name": str(widget_at.objectName()) if widget_at else None,
                    }
                except Exception as exc:
                    report["widget_at_click"] = {"error": str(exc)}

            try:
                view = FreeCADGui.ActiveDocument.ActiveView
                camera = view.getCameraNode()
                camera_info = {
                    "type": str(camera.getTypeId().getName()) if camera is not None else None
                }
                if camera is not None:
                    if hasattr(camera, "height"):
                        camera_info["height"] = float(camera.height.getValue())
                    if hasattr(camera, "nearDistance"):
                        camera_info["near"] = float(camera.nearDistance.getValue())
                    if hasattr(camera, "farDistance"):
                        camera_info["far"] = float(camera.farDistance.getValue())
                    position = camera.position.getValue()
                    camera_info["position"] = [float(position[0]), float(position[1]), float(position[2])]
                    size = view.getSize()
                    camera_info["view_size"] = [int(size[0]), int(size[1])]
                report["camera"] = camera_info
            except Exception as exc:
                report["camera"] = {"error": str(exc)}

            try:
                report["selection"] = list(FreeCADGui.Selection.getSelectionEx())
                report["selection"] = [
                    {
                        "object": sel.ObjectName,
                        "subnames": list(sel.SubElementNames),
                    }
                    for sel in FreeCADGui.Selection.getSelectionEx()
                ]
            except Exception as exc:
                report["selection"] = {"error": str(exc)}
        except Exception as exc:
            report["error"] = str(exc)
        return report

    def origin_decision_fields(self, before=None, after=None):
        after = after or {}
        lookup = after.get("origin_lookup") or {}
        session = after.get("drawing_session") or {}
        first = lookup.get("first_search") or {}
        current = lookup.get("in_edit_rootnode_origin_point_sets") or lookup.get(
            "edit_root_origin_point_sets"
        ) or []
        current_kinds = [record.get("marker_kind") for record in current]
        widgets_before = ((before or {}).get("drawing_session") or {}).get("sketcher_tool_widgets") or []
        widgets_after = session.get("sketcher_tool_widgets") or []
        return {
            "first_search_kind": first.get("marker_kind"),
            "current_edit_marker_kinds": current_kinds,
            "lookup_reads_current_edit_root": lookup.get("lookup_reads_current_edit_root"),
            "origin_point_set_count": len(lookup.get("all_origin_point_sets") or []),
            "edit_root_count": len(lookup.get("edit_roots") or []),
            "geometry_count": session.get("geometry_count"),
            "sketcher_tool_widget_visible_before": any(
                widget.get("visible") for widget in widgets_before
            ),
            "sketcher_tool_widget_visible_after": any(
                widget.get("visible") for widget in widgets_after
            ),
            "create_line_is_active": session.get("create_line_is_active"),
        }

    def save_origin_trace(self, label, extra=None, viewport=None, qt_point=None):
        if not self.origin_trace_enabled():
            return None
        trace_dir = self.origin_trace_dir()
        if not trace_dir:
            return None

        self._origin_trace_seq += 1
        stamp = time.strftime("%Y%m%dT%H%M%S")
        test_id = "test"
        if hasattr(self, "id"):
            test_id = str(self.id()).split(".")[-1]
        prefix = f"{self._origin_trace_seq:02d}_{test_id}_{label}"
        payload = {
            "label": label,
            "seq": self._origin_trace_seq,
            "test": self.id() if hasattr(self, "id") else None,
            "time": stamp,
            "origin_lookup": self.origin_marker_lookup_report(),
            "drawing_session": self.drawing_session_report(viewport=viewport, qt_point=qt_point),
        }
        if extra:
            payload["extra"] = extra
        payload["decision"] = self.origin_decision_fields(after=payload)

        json_path = os.path.join(trace_dir, f"{prefix}.json")
        with open(json_path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, default=str)

        screenshot_errors = []
        try:
            view = FreeCADGui.ActiveDocument.ActiveView
            view.saveImage(os.path.join(trace_dir, f"{prefix}_view.png"), 800, 600, "Current")
        except Exception as exc:
            screenshot_errors.append(f"view:{exc}")
        try:
            main_window = FreeCADGui.getMainWindow()
            if main_window is not None:
                main_window.grab().save(os.path.join(trace_dir, f"{prefix}_main.png"))
        except Exception as exc:
            screenshot_errors.append(f"main:{exc}")
        if viewport is not None:
            try:
                viewport.grab().save(os.path.join(trace_dir, f"{prefix}_viewport.png"))
            except Exception as exc:
                screenshot_errors.append(f"viewport:{exc}")
        if screenshot_errors:
            payload["screenshot_errors"] = screenshot_errors
            with open(json_path, "w", encoding="utf-8") as handle:
                json.dump(payload, handle, indent=2, default=str)
        return payload

    def origin_material_transparency(self, name):
        from pivy import coin

        view = FreeCADGui.ActiveDocument.ActiveView
        root = view.getViewer().getSoRenderManager().getSceneGraph()
        search = coin.SoSearchAction()
        search.setName(name)
        search.setSearchingAll(True)
        search.apply(root)
        path = search.getPath()
        self.assertIsNotNone(path, f"Expected {name} in the scene graph")
        transparency = path.getTail().transparency
        return transparency.getValues()[0], transparency.isDefault()

    def datum_labels(self, view):
        from pivy import coin

        root = view.getViewer().getSoRenderManager().getSceneGraph()
        datum_type = coin.SoType.fromName("SoDatumLabel")
        labels = []

        def visit(node):
            if node.isOfType(datum_type):
                labels.append(node)
            if node.isOfType(coin.SoGroup.getClassTypeId()):
                for index in range(node.getNumChildren()):
                    visit(node.getChild(index))

        visit(root)
        return labels

    def datum_label_points(self, label):
        return tuple(
            tuple(float(label.pnts[i][axis]) for axis in range(3))
            for i in range(label.pnts.getNum())
        )

    def record_datum_label_point_updates(self, labels):
        from pivy import coin

        updates = []
        completed_updates = []
        sensors = []

        for label_index, label in enumerate(labels):
            point_count = [label.pnts.getNum()]

            def record_update(
                data,
                sensor,
                label=label,
                label_index=label_index,
                point_count=point_count,
            ):
                points = self.datum_label_points(label)
                updates.append((label_index, points))

                # SoDatumLabel::setPoints() grows pnts before it edits both values. Coin
                # notifies the priority-0 sensor for that allocation as well as for the
                # completed edit. Ignore only the notification that changes the point
                # count; every same-sized notification is a completed assignment and
                # must be retained, including a zero-to-nonzero transition.
                current_point_count = label.pnts.getNum()
                if current_point_count == point_count[0]:
                    completed_updates.append((label_index, points))
                else:
                    point_count[0] = current_point_count

            sensor = coin.SoFieldSensor(record_update, None)
            sensor.setPriority(0)
            sensor.attach(label.pnts)
            sensors.append(sensor)

        return sensors, updates, completed_updates

    def test_origin_marker_tracks_drawing_tool_state(self):
        """The origin marker tracks drawing state during off-origin interaction."""

        self.begin_sketch_edit_with_task_dialog()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        self.pump(150)
        viewport = view.graphicsView().viewport()
        origin_point = self.viewport_to_qpoint(
            view,
            viewport,
            view.getPointOnScreen(FreeCAD.Vector(0, 0, 0)),
        )
        drawing_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(origin_point.x() + 80, origin_point.y() - 60),
        )
        filled_marker = self.origin_marker_index()
        self.assertTrue(
            self.origin_marker_is("CIRCLE_FILLED"),
            "Expected the idle origin marker to use the filled circle marker",
        )

        transparency, is_default = self.origin_material_transparency("OriginPointMaterial")
        self.assertEqual(transparency, 0.0)
        self.assertFalse(is_default, "Expected the origin transparency to be explicitly set")

        FreeCADGui.runCommand("Sketcher_CreateLine")
        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_LINE"),
                timeout_ms=3000,
            ),
            "Expected the drawing tool to switch the origin marker appearance",
        )
        hollow_marker = self.origin_marker_index()

        self.assertNotEqual(
            hollow_marker,
            filled_marker,
            "Expected an active drawing tool to switch the origin marker appearance",
        )

        view_params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        old_marker_size = view_params.GetInt("MarkerSize", 9)
        new_marker_size = 13 if old_marker_size != 13 else 15
        try:
            view_params.SetInt("MarkerSize", new_marker_size)
            self.assertTrue(
                self.wait_until(
                    lambda: self.origin_marker_is("CIRCLE_LINE")
                    and self.origin_marker_index() != hollow_marker,
                    timeout_ms=3000,
                ),
                "Expected the active origin marker to update after changing its size",
            )
            resized_hollow_marker = self.origin_marker_index()
            self.assertNotEqual(
                resized_hollow_marker,
                hollow_marker,
                "Expected an active hollow marker to be regenerated when its size changes",
            )
        finally:
            view_params.SetInt("MarkerSize", old_marker_size)
        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_LINE")
                and self.origin_marker_index() != resized_hollow_marker,
                timeout_ms=3000,
            ),
            "Expected restoring the marker size to update the active marker",
        )

        before_cancel = self.save_origin_trace(
            "before_first_right_click",
            extra={
                "drawing_point": [int(drawing_point.x()), int(drawing_point.y())],
                "origin_point": [int(origin_point.x()), int(origin_point.y())],
            },
            viewport=viewport,
            qt_point=drawing_point,
        )
        self.move(viewport, drawing_point)
        self.save_origin_trace(
            "after_move_before_first_right_click",
            viewport=viewport,
            qt_point=drawing_point,
        )
        self.right_click(viewport, drawing_point)
        self.save_origin_trace(
            "after_first_right_click",
            extra={"before_cancel_decision": (before_cancel or {}).get("decision")},
            viewport=viewport,
            qt_point=drawing_point,
        )
        restored_filled = self.wait_until(
            lambda: self.origin_marker_is("CIRCLE_FILLED"),
            timeout_ms=3000,
        )
        self.save_origin_trace(
            "after_wait_filled_following_first_right_click",
            extra={
                "wait_restored_filled": restored_filled,
                "timeout_ms": 3000,
            },
            viewport=viewport,
            qt_point=drawing_point,
        )
        self.assertTrue(
            restored_filled,
            "Expected leaving the drawing tool to restore the filled origin marker",
        )

        second_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(drawing_point.x() + 100, drawing_point.y() + 80),
        )
        FreeCADGui.runCommand("Sketcher_CreateLine")
        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_LINE"),
                timeout_ms=3000,
            ),
            "Expected the second line tool activation to switch the origin marker appearance",
        )
        self.move(viewport, drawing_point)
        self.click(viewport, drawing_point)
        self.move(viewport, second_point)
        self.click(viewport, second_point)
        self.assertGreater(
            self.sketch.GeometryCount,
            0,
            "Expected geometry away from the origin before cancelling the tool",
        )
        self.move(viewport, second_point)
        self.right_click(viewport, second_point)
        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_FILLED"),
                timeout_ms=3000,
            ),
            "Expected cancelling the drawing tool to leave a filled origin marker",
        )
        transparency, is_default = self.origin_material_transparency("OriginPointMaterial")
        self.assertEqual(transparency, 0.0)
        self.assertFalse(is_default, "Expected the origin transparency to remain explicit")

        for command in ("Sketcher_CreateRectangle", "Sketcher_CreateCircle"):
            FreeCADGui.runCommand(command)
            self.assertTrue(
                self.wait_until(
                    lambda: self.origin_marker_is("CIRCLE_LINE"),
                    timeout_ms=3000,
                ),
                f"Expected {command} to activate the hollow origin marker",
            )
            self.move(viewport, drawing_point)
            self.right_click(viewport, drawing_point)
            self.assertTrue(
                self.wait_until(
                    lambda: self.origin_marker_is("CIRCLE_FILLED"),
                    timeout_ms=3000,
                ),
                f"Expected {command} cancellation to restore the filled origin marker",
            )

        FreeCADGui.ActiveDocument.resetEdit()
        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected leaving sketch edit mode to close the task dialog",
        )
        FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)
        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_FILLED"),
                timeout_ms=3000,
            ),
            "Expected the origin marker to be filled after re-entering the sketch",
        )
        transparency, is_default = self.origin_material_transparency("OriginPointMaterial")
        self.assertEqual(transparency, 0.0)
        self.assertFalse(is_default, "Expected the re-entered origin transparency to be explicit")

    def test_origin_marker_resets_between_edit_sessions(self):
        """A hollow drawing marker must not leak into a later edit session."""

        self.begin_sketch_edit_with_task_dialog()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        self.pump(150)
        self.assertTrue(self.origin_marker_is("CIRCLE_FILLED"))

        FreeCADGui.runCommand("Sketcher_CreateLine")
        self.assertTrue(
            self.wait_until(lambda: self.origin_marker_is("CIRCLE_LINE"), timeout_ms=3000),
            "Expected the drawing tool to switch the origin marker appearance",
        )

        FreeCADGui.ActiveDocument.resetEdit()
        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected leaving sketch edit mode to close the task dialog",
        )
        FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)

        self.assertTrue(
            self.wait_until(
                lambda: self.origin_marker_is("CIRCLE_FILLED"),
                timeout_ms=3000,
            ),
            "Expected a new edit session to start with a filled origin marker",
        )
        transparency, is_default = self.origin_material_transparency("OriginPointMaterial")
        self.assertEqual(transparency, 0.0)
        self.assertFalse(is_default, "Expected the origin transparency to be explicit")

    def test_rectangle_ovp_transition_never_resets_to_origin(self):
        """OVP points must not be updated to the sketch origin during tool transitions."""

        self.begin_sketch_edit_with_task_dialog()
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        view.fitAll()
        self.pump(150)
        viewport = view.graphicsView().viewport()

        for command, needs_radius in (
            ("Sketcher_CreateRectangle", False),
            ("Sketcher_CreateRectangle_Center", False),
            ("Sketcher_CreateOblong", True),
        ):
            initial_geometry_count = self.sketch.GeometryCount
            FreeCADGui.runCommand(command)
            self.assertTrue(
                self.wait_until(lambda: self.origin_marker_is("CIRCLE_LINE"), timeout_ms=3000),
                f"Expected {command} to activate its drawing handler",
            )
            view.fitAll()
            origin = self.viewport_to_qpoint(
                view,
                viewport,
                view.getPointOnScreen(FreeCAD.Vector(0, 0, 0)),
            )
            first_point = self.clamp_to_widget(
                viewport,
                QtCore.QPoint(origin.x() + 80, origin.y() - 60),
            )
            second_point = self.clamp_to_widget(
                viewport,
                QtCore.QPoint(first_point.x() + 100, first_point.y() + 80),
            )
            radius_point = self.clamp_to_widget(
                viewport,
                QtCore.QPoint(second_point.x() + 25, second_point.y() + 20),
            )
            labels = self.datum_labels(view)
            self.assertGreaterEqual(
                len(labels),
                4,
                f"Expected {command} to create its on-view datum labels",
            )
            sensors = []
            observations = []

            def observe_current_labels():
                current_labels = self.datum_labels(view)
                self.assertGreaterEqual(
                    len(current_labels),
                    4,
                    f"Expected {command} to retain its on-view datum labels",
                )
                # Observe every label. The scene-graph order is not a stable part of the
                # handler API, and the centered rectangle does not necessarily expose its
                # dimension labels at the same positions as the diagonal rectangle. The
                # regression is specifically a complete two-point assignment at the origin,
                # so it is safe and more reliable to inspect all labels owned by this tool.
                current_sensors, current_updates, current_completed_updates = (
                    self.record_datum_label_point_updates(current_labels)
                )
                sensors.extend(current_sensors)
                observations.append((current_updates, current_completed_updates))

            self.move(viewport, first_point)
            self.flush_gui()
            observe_current_labels()
            self.click(viewport, first_point)
            self.assertTrue(
                self.wait_until(lambda: len(self.visible_spinboxes()) == 2, timeout_ms=1000),
                f"Expected {command} to activate its width and height OVPs",
            )
            self.move(viewport, second_point)
            self.flush_gui()
            observe_current_labels()
            self.click(viewport, second_point)
            if needs_radius:
                self.move(viewport, radius_point)
                self.flush_gui()
                observe_current_labels()
                self.click(viewport, radius_point)

            for sensor in sensors:
                sensor.detach()

            updates = [update for batch, _ in observations for update in batch]
            completed_updates = [update for _, batch in observations for update in batch]

            self.assertTrue(
                updates, f"Expected {command} to update OVP points during the transition"
            )
            self.assertTrue(
                completed_updates,
                f"Expected {command} to complete OVP point assignments during the transition: {updates}",
            )

            origin_updates = [
                points
                for _, points in completed_updates
                if len(points) >= 2
                and all(all(abs(coordinate) < 1e-9 for coordinate in point) for point in points[:2])
            ]
            self.assertFalse(
                origin_updates,
                f"{command} left both OVP points at (0, 0, 0): {origin_updates}",
            )
            self.assertTrue(
                self.wait_until(
                    lambda: self.sketch.GeometryCount > initial_geometry_count,
                    timeout_ms=3000,
                ),
                f"Expected {command} to create geometry",
            )

            self.right_click(viewport, second_point)
            self.assert_sketch_edit_active()

    def test_reset_edit_closes_sketch_task_dialog(self):
        self.begin_sketch_edit_with_task_dialog()

        FreeCADGui.ActiveDocument.resetEdit()

        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected resetEdit() to close the Sketcher task dialog",
        )
        self.assert_sketch_edit_inactive()

    def test_task_dialog_reject_exits_sketch_edit(self):
        self.begin_sketch_edit_with_task_dialog()

        self.active_task_dialog().reject()

        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected reject() to close the Sketcher task dialog",
        )
        self.assert_sketch_edit_inactive()

    def test_task_dialog_accept_exits_sketch_edit(self):
        self.begin_sketch_edit_with_task_dialog()

        self.active_task_dialog().accept()

        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected accept() to close the Sketcher task dialog",
        )
        self.assert_sketch_edit_inactive()

    def test_rectangle_ovp_enter_finishes_without_crash(self):
        """
        Reproduce the rectangle OVP acceptance flow from PR #29201 review:
        click first point, type width, Tab, type height, Enter.

        If the process survives, the rectangle should be created in the sketch.
        """

        self.begin_rectangle_with_visible_ovp()

        first_spinbox = self.ovp_spinboxes()[0]
        self.focus_ovp_spinbox(first_spinbox)
        self.key_text(first_spinbox, "10")
        self.key_click(first_spinbox, QtCore.Qt.Key_Tab, "\t")

        second_spinbox = self.ovp_spinboxes()[1]
        self.assertTrue(
            self.wait_until(lambda: second_spinbox.hasFocus(), timeout_ms=1000),
            "Expected Tab to move focus to the second rectangle OVP",
        )
        self.key_text(second_spinbox, "20")
        self.key_click(second_spinbox, QtCore.Qt.Key_Return, "\r")

        self.pump(500)

        self.assertGreaterEqual(
            self.sketch.GeometryCount,
            4,
            "Expected the rectangle to be created after accepting both OVPs",
        )

    def test_clearing_committed_rectangle_ovp_releases_its_lock(self):
        """A cleared OVP must leave its committed state and resume live geometry input."""

        self.begin_rectangle_with_visible_ovp()

        first_spinbox = self.active_spinbox()
        self.assertIsNotNone(first_spinbox, "Expected the first rectangle OVP to have focus")
        self.key_text(first_spinbox, "10")
        self.key_click(first_spinbox, QtCore.Qt.Key_Tab, "\t")

        lock_icon = self.ovp_lock_icon(first_spinbox)
        self.assertIsNotNone(lock_icon)
        self.assertTrue(
            self.wait_until(lock_icon.isVisible, timeout_ms=1000),
            "Expected Tab to lock the committed first OVP",
        )

        first_edit = first_spinbox.findChild(QtGui.QLineEdit)
        self.assertIsNotNone(first_edit)
        first_spinbox.setFocus(QtCore.Qt.OtherFocusReason)
        self.pump(100)
        self.assertIs(self.active_spinbox(), first_spinbox)
        first_edit.clear()
        self.pump(60)

        self.assertTrue(
            self.wait_until(lambda: not lock_icon.isVisible(), timeout_ms=1000),
            "Expected clearing the first OVP to release its lock",
        )

        first_edit.setText("15")
        self.pump(60)
        self.key_click(first_spinbox, QtCore.Qt.Key_Tab, "\t")

        self.assertTrue(
            self.wait_until(
                lambda: self.active_spinbox() is not None
                and self.active_spinbox() is not first_spinbox,
                timeout_ms=1000,
            ),
            "Expected a replacement value to recommit the first OVP",
        )
        self.assertTrue(
            self.wait_until(lock_icon.isVisible, timeout_ms=1000),
            "Expected the replacement value to lock the first OVP again",
        )

    def test_rectangle_ovp_escape_resets_tool_without_exiting_sketch(self):
        viewport, first_point = self.begin_rectangle_with_visible_ovp()

        first_spinbox = self.ovp_spinboxes()[0]
        self.focus_ovp_spinbox(first_spinbox)
        self.key_click(first_spinbox, QtCore.Qt.Key_Escape)

        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 0, timeout_ms=1000),
            "Expected Esc to close the rectangle OVPs",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)
        self.assert_sketch_edit_active()

        restart_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(first_point.x() + 120, first_point.y() + 50),
        )
        restart_move = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(restart_point.x() + 70, restart_point.y() - 40),
        )

        self.move(viewport, restart_point)
        self.click(viewport, restart_point)
        self.move(viewport, restart_move)

        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 2, timeout_ms=1000),
            "Expected Esc to reset the rectangle tool back to its first stage",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)
        self.focus_ovp_spinbox(self.ovp_spinboxes()[0])

    def test_rectangle_ovp_escape_then_right_click_exits_tool(self):
        viewport, first_point = self.begin_rectangle_with_visible_ovp()

        first_spinbox = self.ovp_spinboxes()[0]
        self.focus_ovp_spinbox(first_spinbox)
        self.key_click(first_spinbox, QtCore.Qt.Key_Escape)

        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 0, timeout_ms=1000),
            "Expected Esc to close the rectangle OVPs",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)
        self.assert_sketch_edit_active()

        cancel_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(first_point.x() + 120, first_point.y() + 50),
        )
        retry_move = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(cancel_point.x() + 70, cancel_point.y() - 40),
        )

        self.right_click(viewport, cancel_point)
        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 0, timeout_ms=400),
            "Expected right click to keep the rectangle OVPs closed after canceling",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)
        self.assert_sketch_edit_active()

        self.move(viewport, cancel_point)
        self.click(viewport, cancel_point)
        self.move(viewport, retry_move)

        self.assertFalse(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 2, timeout_ms=500),
            "Expected right click to exit the rectangle tool after OVP Esc",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)

    def test_rectangle_ovp_escape_then_escape_then_escape_exits_sketch(self):
        viewport, first_point = self.begin_rectangle_with_visible_ovp()

        first_spinbox = self.ovp_spinboxes()[0]
        self.focus_ovp_spinbox(first_spinbox)
        self.key_click(first_spinbox, QtCore.Qt.Key_Escape)

        self.assertTrue(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 0, timeout_ms=1000),
            "Expected the first Esc to close the rectangle OVPs",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)
        self.assert_sketch_edit_active()

        view = FreeCADGui.ActiveDocument.ActiveView
        graphics_view = view.graphicsView()
        graphics_view.setFocus(QtCore.Qt.OtherFocusReason)
        self.pump(100)

        self.key_click(graphics_view, QtCore.Qt.Key_Escape)
        self.assert_sketch_edit_active()

        cancel_point = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(first_point.x() + 120, first_point.y() + 50),
        )
        retry_move = self.clamp_to_widget(
            viewport,
            QtCore.QPoint(cancel_point.x() + 70, cancel_point.y() - 40),
        )

        self.move(viewport, cancel_point)
        self.click(viewport, cancel_point)
        self.move(viewport, retry_move)

        self.assertFalse(
            self.wait_until(lambda: len(self.visible_spinboxes()) == 2, timeout_ms=500),
            "Expected the second Esc to exit the rectangle tool",
        )
        self.assertEqual(self.sketch.GeometryCount, 0)

        graphics_view.setFocus(QtCore.Qt.OtherFocusReason)
        self.pump(100)
        self.key_click(graphics_view, QtCore.Qt.Key_Escape)

        self.assertTrue(
            self.wait_until(lambda: self.active_task_dialog() is None, timeout_ms=1000),
            "Expected the third Esc to close the Sketcher task dialog",
        )
        self.assert_sketch_edit_inactive()

    def test_auto_color_restores_line_color_from_preferences(self):
        view_params = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        color_key = "SketchEdgeColor"
        had_color = color_key in view_params.GetUnsigneds()
        old_color = view_params.GetUnsigned(color_key, 0)

        try:
            manual_color = 0x112233FF
            preference_color = 0x44AA88FF

            view = self.sketch.ViewObject
            view.AutoColor = False
            view.LineColor = manual_color
            self.assertEqual(self.pack_color(view.LineColor), manual_color)

            view_params.SetUnsigned(color_key, preference_color)
            self.pump(100)
            self.assertEqual(self.pack_color(view.LineColor), manual_color)

            view.AutoColor = True
            self.pump(100)

            self.assertEqual(self.pack_color(view.LineColor), preference_color)
        finally:
            if had_color:
                view_params.SetUnsigned(color_key, old_color)
            else:
                view_params.RemUnsigned(color_key)
