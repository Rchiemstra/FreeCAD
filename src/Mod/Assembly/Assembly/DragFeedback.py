# SPDX-License-Identifier: LGPL-2.1-or-later

"""Transient observations of displayed poses; no document writes or queued step history."""

from concurrent.futures import ThreadPoolExecutor
import math
import time

import FreeCAD as App
import FreeCADGui as Gui
import AssemblyApp
import AssemblyGui
from PySide import QtCore
from pivy import coin

from .api import _require_assembly
from .MotionInterference import _number

_sessions = {}


def _key(assembly):
    return assembly.Document.Name, assembly.Name


def _dispatch(assembly, ended):
    feedback = _sessions.get(_key(assembly))
    if feedback is not None:
        feedback.step(ended)


def setDragFeedback(assembly, pairs=None, enabled=True, *, clearance=None):
    """Opt into contact/clearance feedback at this pose only (off by default).

    pairs is a sequence of (first, second) occurrence handles accepted by
    sweepJoint. Clearance defaults to the assembly's current value; spreadsheet
    rules and exclusions are honored. Returns a transient controller, or None
    when disabled. Requires the GUI. No settings are stored in the document.
    """
    assembly = _require_assembly(assembly)
    key = _key(assembly)
    previous = _sessions.get(key)
    if previous is not None:
        previous.close()
    if not enabled:
        return None
    pairs = tuple(tuple(pair) for pair in (pairs or ()))
    if not pairs or any(len(pair) != 2 for pair in pairs):
        raise ValueError("Choose at least one pair of distinct component occurrences")
    if clearance is not None:
        clearance = _number(clearance, "mm", "clearance")
        if clearance < 0:
            raise ValueError("clearance must be nonnegative")
    feedback = _Feedback(assembly, pairs, clearance)
    _sessions[key] = feedback
    AssemblyGui._setDragFeedbackCallback(_dispatch)
    return feedback


class _Feedback:
    def __init__(self, assembly, pairs, clearance):
        self.assembly, self.pairs, self.clearance = assembly, pairs, clearance
        self.key = _key(assembly)
        self.enabled = True
        self.onFailure = None
        self._generation = 0
        self._lastStart = -math.inf
        self._pending = None
        self._placing = set()
        self._geometry = [AssemblyApp._prepareSelectedPairScan(
            assembly, first, second, clearance=clearance) for first, second in pairs]
        self._future = None
        self._executor = ThreadPoolExecutor(max_workers=1)
        self._token = AssemblyApp.newInterferenceScanCancellation()
        self._root = self._scene = None
        self.findings = []
        self.message = ""
        self._timer = QtCore.QTimer()
        self._timer.setInterval(10)
        self._timer.timeout.connect(self._poll)
        App.addDocumentObserver(self)

    def _clear(self):
        if self._root is not None:
            self._scene.removeChild(self._root)
            self._root = self._scene = None
        if self.message:
            Gui.getMainWindow().statusBar().clearMessage()
        self.findings = []
        self.message = ""

    def step(self, ended=False):
        if not self.enabled:
            return
        try:
            # A result belongs only to the exact displayed step that captured it.
            self._generation += 1
            self._clear()
            self._pending = None
            if ended:
                AssemblyApp.cancelInterferenceScan(self._token)
                self._lastStart = -math.inf
                return
            # Retain only the latest generation, never a queue of drag steps.
            self._pending = self._generation
            self._timer.start()
            self._startPending()
        except Exception as error:
            self._fail(error)

    def _startPending(self):
        if (self._pending is None or self._future is not None
                or time.monotonic() - self._lastStart < 0.1):
            return
        generation, self._pending = self._pending, None
        if generation != self._generation:
            return
        self._lastStart = time.monotonic()
        self._token = AssemblyApp.newInterferenceScanCancellation()
        prepared = [AssemblyApp._prepareSelectedPairScan(
            self.assembly, first, second, measureDistances=True,
            clearance=self.clearance, cancel=self._token,
            geometry=None if self._geometry is None else self._geometry[index])
            for index, (first, second) in enumerate(self.pairs)]
        if self._geometry is None:
            self._geometry = prepared
        # Geometry is owned by the session, independent of the document.
        self._future = self._executor.submit(
            lambda: (generation, [AssemblyApp._runPreparedInterferenceScan(p) for p in prepared]))

    def _poll(self):
        try:
            self._startPending()
            if self._future is None:
                if self._pending is None:
                    self._timer.stop()
                return
            if not self._future.done():
                return
            future, self._future = self._future, None
            generation, scans = future.result()
            if not self.enabled or generation != self._generation:
                self._startPending()
                return
            for scan in scans:
                if scan["cancelled"]:
                    continue
                if not scan["complete"] or not scan["pairs"]:
                    raise RuntimeError("Static contact scan incomplete")
                for pair in scan["pairs"]:
                    if not pair["distanceMeasured"]:
                        raise RuntimeError("Static contact scan did not measure the pair")
                    distance = pair["solidDetection"]["minimumDistance"]
                    if not math.isfinite(distance) or distance < 0:
                        raise RuntimeError("Static contact scan returned an invalid distance")
                    if pair["excluded"]:
                        continue
                    hits = [h for h in pair["faceHits"] if not h["suppressedByExclusion"]]
                    contact = pair["solidDetection"]["kind"] in ("Contact", "Penetration") or any(
                        h["kind"] in ("Contact", "Penetration") for h in hits)
                    violation = contact or pair["detection"]["kind"] == "ClearanceViolation" or any(
                        h["kind"] == "ClearanceViolation" for h in hits)
                    if violation:
                        leaves = [scan["leaves"][pair[key]] for key in ("leafIndexA", "leafIndexB")]
                        self.findings.append((leaves, distance, contact))
            if self.findings:
                self._present()
        except Exception as error:
            self._fail(error)

    def _present(self):
        view = Gui.getDocument(self.assembly.Document.Name).activeView()
        self._scene = view.getSceneGraph()
        self._root = coin.SoType.fromName("SoSkipBoundingGroup").createInstance()
        self._root.mode = 1  # EXCLUDE_BBOX: overlays must not affect cursor projection.
        content = coin.SoSeparator()
        self._root.addChild(content)
        pick = coin.SoPickStyle()
        pick.style = coin.SoPickStyle.UNPICKABLE
        content.addChild(pick)
        material = coin.SoMaterial()
        material.diffuseColor = (1.0, 0.2, 0.05)
        material.transparency = 0.35
        content.addChild(material)
        for leaves, _, _ in self.findings:
            for leaf in leaves:
                # Meshing must not populate triangulations on a document shape.
                vertices, faces = leaf["worldShape"].copy().tessellate(0.1)
                coords = coin.SoCoordinate3()
                coords.point.setValues(0, len(vertices), [tuple(v) for v in vertices])
                triangles = coin.SoIndexedFaceSet()
                indices = [index for face in faces for index in (*face, -1)]
                triangles.coordIndex.setValues(0, len(indices), indices)
                content.addChild(coords)
                content.addChild(triangles)
        self._scene.addChild(self._root)
        leaves, distance, contact = min(self.findings, key=lambda finding: finding[1])
        kind = App.Qt.translate("Assembly", "Contact" if contact else "Clearance violation")
        self.message = App.Qt.translate("Assembly", "{} at this pose: {} / {} — {:.6g} mm").format(
            kind, leaves[0]["displayPath"], leaves[1]["displayPath"], distance)
        Gui.getMainWindow().statusBar().showMessage(self.message)

    def _fail(self, error):
        self.close()
        App.Console.PrintWarning(f"Contact feedback disabled: {error}\n")
        if self.onFailure is not None:
            self.onFailure()

    def close(self):
        if not self.enabled:
            return
        self.enabled = False
        self._pending = None
        self._geometry = None
        self._generation += 1
        self._timer.stop()
        AssemblyApp.cancelInterferenceScan(self._token)
        self._executor.shutdown(wait=False, cancel_futures=True)
        self._clear()
        App.removeDocumentObserver(self)
        _sessions.pop(self.key, None)
        if not _sessions:
            AssemblyGui._setDragFeedbackCallback(None)

    def slotDeletedDocument(self, doc):
        if doc.Name == self.key[0]:
            self.close()

    def slotBeforeChangeObject(self, obj, prop):
        if prop in ("Placement", "LinkPlacement", "PlacementList"):
            self._placing.add(obj.FullName)

    def slotChangedObject(self, obj, prop):
        # Link providers touch this synthetic property while refreshing the
        # display. Actual definition changes arrive through their own signals.
        if prop == "_LinkTouched":
            return
        # Part::Feature updates Shape inside its Placement notification. That
        # nested write changes only the location, so retain the owned geometry.
        placement = prop in ("Placement", "LinkPlacement", "PlacementList")
        if not placement and not (prop == "Shape" and obj.FullName in self._placing):
            self._geometry = None
        if placement:
            self._placing.discard(obj.FullName)
        self._generation += 1
        self._pending = None
        self._clear()

    def slotDeletedObject(self, obj):
        if obj == self.assembly:
            self.close()
