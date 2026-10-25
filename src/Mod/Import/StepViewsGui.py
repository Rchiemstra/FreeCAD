# SPDX-License-Identifier: LGPL-2.1-or-later
"""Transient, explicit AP242 saved-view presentation in an existing 3D view.

activate(record, targetView) and clear(targetView) return True on success, raise
ValueError for unavailable data/targets, and propagate presentation failures
(after rollback). No document properties, preferences or active views are written.
Clear restores the camera preceding the first activation. Replace keeps that
baseline. Native records/links remain authoritative; RawMetadata is never replayed.
"""
import math
import re

import FreeCAD as App
import FreeCADGui as Gui
from pivy import coin

_states = {}


def _document(record):
    try:
        doc = record.Document
        if App.listDocuments().get(doc.Name) is doc and doc.getObject(record.Name) is record:
            return doc
    except (AttributeError, ReferenceError, RuntimeError):
        pass
    raise ValueError("Saved-view record is no longer in a live document")


def _view_document(view):
    # Enumerate native live views BEFORE calling any method on the supplied wrapper.
    # A closed view's Python wrapper may outlive the native view.
    for name, doc in App.listDocuments().items():
        gui = Gui.getDocument(name)
        if gui and any(v is view for v in gui.mdiViewsOfType("Gui::View3DInventor")):
            return doc
    raise ValueError("An existing live 3D target view is required")


def _number(value):
    value = float(value)
    # Coin fields use floats; avoid silently converting a finite double to infinity.
    if not math.isfinite(value) or abs(value) > 1e30:
        raise ValueError("Invalid saved-view number")
    return value


def _vector(value, size=3):
    if len(value) != size:
        raise ValueError("Invalid saved-view vector dimensions")
    return tuple(_number(x) for x in value)


def _dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def _close(a, b):
    return all(math.isclose(x, y, rel_tol=1e-7, abs_tol=1e-7) for x, y in zip(a, b))


def _unit(value):
    value = _vector(value)
    if not math.isclose(_dot(value, value), 1, rel_tol=1e-7, abs_tol=1e-7):
        raise ValueError("A unit direction is required")
    return value


def _available(record, kind):
    capping = record.Capping if kind == "ClippingPlane" else getattr(record, "Capping", False)
    if (record.RecordType != kind or record.Units != "mm"
            or _number(record.sourceLengthFactor) <= 0
            or record.PresentationStatus != "data available" or record.UnavailableReason
            or not isinstance(capping, bool) or capping):
        raise ValueError("Saved-view presentation is unavailable: " + record.UnavailableReason)


def _operands(expression):
    """Parse only the owned intersection grammar; no evaluation or name lookup."""
    expression = re.sub(r"\s+", "", expression)
    if not expression:
        return set()
    if len(expression) > 8192:
        raise ValueError("Clipping expression is too large")
    tokens = re.findall(r"#[1-9][0-9]*|[()*]", expression)
    if "".join(tokens) != expression:
        raise ValueError("Only complete intersection clipping is supported")
    index, found = 0, set()

    def term(depth):
        nonlocal index
        if depth > 64 or index >= len(tokens):
            raise ValueError("Invalid clipping expression")
        token = tokens[index]
        index += 1
        if token.startswith("#"):
            found.add(token)
        elif token == "(":
            term(depth + 1)
            while index < len(tokens) and tokens[index] == "*":
                index += 1
                term(depth + 1)
            if index >= len(tokens) or tokens[index] != ")":
                raise ValueError("Incomplete clipping expression")
            index += 1
        else:
            raise ValueError("Invalid clipping operand")

    term(0)
    if index != len(tokens):
        raise ValueError("Invalid clipping expression")
    return found


def _clip(group, normal, distance):
    clip = coin.SoClipPlane()
    clip.plane.setValue(coin.SbPlane(coin.SbVec3f(*normal), _number(distance)))
    clip.on = True
    group.addChild(clip)


def _plan(record, doc):
    """Validate all live values/links and construct detached presentation nodes."""
    try:
        _available(record, "SavedView")
        if any(not isinstance(getattr(record, flag), bool)
               for flag in ("FrontClipping", "BackClipping", "SidesClipping")):
            raise ValueError("Incomplete view-volume flags")
        origin = _vector(record.Origin)
        right, up, axis = (_unit(record.RightDirection), _unit(record.UpDirection),
                           _unit(record.ViewDirection))
        cross = (right[1]*up[2]-right[2]*up[1], right[2]*up[0]-right[0]*up[2],
                 right[0]*up[1]-right[1]*up[0])
        if not _close(cross, axis) or abs(_dot(right, up)) > 1e-7:
            raise ValueError("Invalid camera basis")
        width, height = _vector(record.WindowSize, 2)
        distance = _number(record.ViewPlaneDistance)
        if width <= 0 or height <= 0 or distance <= 0:
            raise ValueError("Positive window dimensions and view distance are required")
        if (not _close(_vector(record.WindowRight), (1, 0, 0))
                or not _close(_vector(record.WindowUp), (0, 1, 0))
                or not _close(_vector(record.WindowOrigin), (-width/2, -height/2, distance))):
            raise ValueError("Only centered axis-aligned windows on the view plane are supported")
        projection = _vector(record.ProjectionPoint)
        if not _close(projection[:2], (0, 0)):
            raise ValueError("Oblique or off-axis projection is unavailable")
        position = _vector(record.CameraPosition)
        expected = tuple(origin[i] + right[i]*projection[0] + up[i]*projection[1]
                         + axis[i]*projection[2] for i in range(3))
        if not _close(position, expected):
            raise ValueError("Camera position disagrees with its owned basis/projection point")
        signed_distance = distance - projection[2]
        focal = abs(signed_distance)
        if focal <= 1e-7:
            raise ValueError("Projection point lies on the view plane")
        sign = 1 if signed_distance > 0 else -1
        forward = tuple(sign*x for x in axis)
        backward = tuple(-x for x in forward)
        camera_right = tuple(-sign*x for x in right)
        rotation = App.Rotation(App.Vector(*camera_right), App.Vector(*up),
                                App.Vector(*backward), "ZXY")
        if record.Projection == "parallel":
            camera = coin.SoOrthographicCamera()
            camera.height = height
            if camera.height.getValue() <= 0:
                raise ValueError("Window height cannot be represented by the camera")
        elif record.Projection == "perspective":
            camera = coin.SoPerspectiveCamera()
            angle = 2*math.atan(height/(2*focal))
            if not 1e-7 < angle < math.pi-1e-7:
                raise ValueError("Perspective angle is outside the supported range")
            camera.heightAngle = angle
        else:
            raise ValueError("Unsupported saved-view projection")
        camera.position = position
        camera.orientation = rotation.Q
        camera.focalDistance = _number(focal)
        camera.aspectRatio = _number(width/height)
        if not 1e-7 < width/height < 1e7:
            raise ValueError("Window aspect is outside the supported range")
        # Letterbox the saved window, preserving dimensions and aspect at any viewport size.
        camera.viewportMapping = coin.SoCamera.CROP_VIEWPORT_NO_FRAME
        # Render-manager depth adjustment remains enabled. Exact STEP front/back
        # clipping uses owned world planes, independent of automatic near/far updates.
        camera.nearDistance = _number(focal/10000)
        camera.farDistance = _number(focal*10000)
        front, back = _number(record.FrontDistance), _number(record.BackDistance)
        if front < 0 or back < 0:
            raise ValueError("Invalid clipping distances")
        if record.FrontClipping and sign*(front-projection[2]) < 0:
            raise ValueError("Front clipping plane is behind the eye")
        if record.BackClipping and sign*(back-projection[2]) <= 0:
            raise ValueError("Back clipping plane is behind the eye")
        if record.FrontClipping and record.BackClipping and sign*(back-front) <= 0:
            raise ValueError("Front/back clipping planes are out of order")
        ids = _operands(record.ClippingExpression)
        planes = list(record.Planes)
        owned = set()
        groups = [o for o in record.InList if getattr(o, "MetadataKind", "") == "SavedViews"]
        nodes = coin.SoGroup()  # State must propagate to following geometry, unlike SoSeparator.
        nodes.setName("StepViewsActivation")
        for plane in planes:
            if _document(plane) is not doc:
                raise ValueError("Clipping plane belongs to a different document")
            _available(plane, "ClippingPlane")
            if (plane.StepEntityId in owned or plane.StepEntityId not in ids
                    or plane.SourceId != plane.StepEntityId
                    or plane.sourceLengthFactor != record.sourceLengthFactor
                    or any(plane not in group.Group for group in groups)):
                raise ValueError("Clipping expression/plane ownership is incomplete or ambiguous")
            owned.add(plane.StepEntityId)
            normal = _unit(plane.Normal)
            offset = _number(plane.Offset)
            if (plane.RetainedHalfspace != "positive"
                    or not math.isclose(_dot(normal, _vector(plane.Origin)), -offset,
                                        rel_tol=1e-7, abs_tol=1e-7)):
                raise ValueError("Invalid clipping half-space or offset")
            # STEP n.x + offset >= 0; Coin n.x >= distance.
            _clip(nodes, normal, -offset)
        if owned != ids:
            raise ValueError("Clipping operands require native plane links")
        for enabled, coordinate, normal in ((record.FrontClipping, front, forward),
                                            (record.BackClipping, back, backward)):
            if enabled:
                point = tuple(origin[i] + axis[i]*coordinate for i in range(3))
                _clip(nodes, normal, _dot(normal, point))
        if nodes.getNumChildren() > 6:
            raise ValueError("Too many clipping planes")
        return camera, nodes
    except (AttributeError, TypeError, ReferenceError, OverflowError) as error:
        raise ValueError("Incomplete or invalid saved-view data") from error


def _write_camera(view, camera):
    kind = "Perspective" if camera.isOfType(coin.SoPerspectiveCamera.getClassTypeId()) else "Orthographic"
    if view.getCameraType() != kind:
        view.setCameraType(kind)
    # Copy all fields, including perspective angle/aspect omitted by setCamera().
    view.getCameraNode().copyFieldValues(camera, False)


def _apply_camera(view, camera):
    _write_camera(view, camera)


def _install_nodes(root, nodes):
    root.insertChild(nodes, 0)


def _drop(key, restore=False):
    state = _states.get(key)
    if state is None:
        return
    if restore:
        _write_camera(state["view"], state["baseline"])
    _states.pop(key)
    root, nodes = state["root"], state["nodes"]
    if root.findChild(nodes) >= 0:
        root.removeChild(nodes)
    try:
        state["widget"].destroyed.disconnect(state["closed"])
    except (RuntimeError, TypeError):
        pass  # The target widget may already be in its destroyed callback.


def activate(record, targetView):
    """Apply a complete owned record to this existing view only; refuse unchanged."""
    doc = _document(record)
    if _view_document(targetView) is not doc:
        raise ValueError("Record and target view must belong to the same document")
    camera, nodes = _plan(record, doc)
    root = targetView.getSceneGraph()
    key = id(targetView)
    previous = _states.get(key)
    old_index = root.findChild(previous["nodes"]) if previous else -1
    if previous and old_index < 0:
        raise ValueError("The owned presentation nodes have been removed")
    search = coin.SoSearchAction()
    search.setType(coin.SoClipPlane.getClassTypeId())
    search.setInterest(coin.SoSearchAction.ALL)
    search.apply(root)
    existing = sum(bool(p.getTail().on.getValue()) for p in search.getPaths())
    if previous:
        existing -= previous["nodes"].getNumChildren()
    if existing + nodes.getNumChildren() > 6:
        raise ValueError("Existing clipping leaves insufficient plane capacity")
    snapshot = targetView.getCameraNode().copy()
    try:
        _apply_camera(targetView, camera)
        _install_nodes(root, nodes)
        if previous:
            root.removeChild(previous["nodes"])
            state = dict(previous, record=record, planes=tuple(record.Planes), nodes=nodes)
        else:
            widget = targetView.graphicsView()
            closed = lambda *_: _drop(key)
            widget.destroyed.connect(closed)
            state = dict(view=targetView, document=doc, record=record, root=root, nodes=nodes,
                         baseline=snapshot, planes=tuple(record.Planes), widget=widget, closed=closed)
        _states[key] = state
    except Exception:
        if root.findChild(nodes) >= 0:
            root.removeChild(nodes)
        if previous and root.findChild(previous["nodes"]) < 0:
            root.insertChild(previous["nodes"], old_index)
        _write_camera(targetView, snapshot)
        raise
    return True


def clear(targetView):
    """Remove this view's owned nodes and restore its pre-activation camera."""
    _view_document(targetView)
    _drop(id(targetView), restore=True)
    return True


class _Observer:
    def slotDeletedObject(self, obj):
        for key, state in list(_states.items()):
            if state["record"] is obj or any(plane is obj for plane in state["planes"]):
                _drop(key, restore=True)

    def slotDeletedDocument(self, doc):
        for key, state in list(_states.items()):
            if state["document"] is doc:
                _drop(key)


_observer = _Observer()
App.addDocumentObserver(_observer)


def _selection():
    selected = Gui.Selection.getSelection()
    if len(selected) != 1 or getattr(selected[0], "RecordType", "") != "SavedView":
        return None
    gui = Gui.activeDocument()
    if gui and gui.Document is selected[0].Document:
        view = gui.activeView()
        if view and any(v is view for v in gui.mdiViewsOfType("Gui::View3DInventor")):
            return selected[0], view
    return None


class _ActivateCommand:
    def GetResources(self):
        return {"MenuText": "Activate STEP saved view", "ToolTip":
                "Apply this saved camera and intersection clipping to the current 3D view"}

    def IsActive(self):
        return _selection() is not None

    def Activated(self):
        selection = _selection()
        if selection:
            try:
                activate(*selection)
            except ValueError as error:
                App.Console.PrintWarning(str(error) + "\n")


class _ClearCommand:
    def GetResources(self):
        return {"MenuText": "Clear STEP saved view", "ToolTip":
                "Remove saved-view clipping and restore the preceding camera in this view"}

    def IsActive(self):
        gui = Gui.activeDocument()
        return bool(gui and id(gui.activeView()) in _states)

    def Activated(self):
        if self.IsActive():
            clear(Gui.activeDocument().activeView())


class _Menu:
    def modifyContextMenu(self, recipient):
        if recipient != "Tree":
            return []
        commands = []
        if activate_command.IsActive():
            commands.append("StepViews_Activate")
        if clear_command.IsActive():
            commands.append("StepViews_Clear")
        return [{"insert": command, "menuItem": "Std_Delete"} for command in commands]


activate_command, clear_command, menu = _ActivateCommand(), _ClearCommand(), _Menu()
Gui.addCommand("StepViews_Activate", activate_command)
Gui.addCommand("StepViews_Clear", clear_command)
Gui.addWorkbenchManipulator(menu)
