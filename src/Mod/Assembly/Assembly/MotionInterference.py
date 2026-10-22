# SPDX-License-Identifier: LGPL-2.1-or-later

"""Bounded, sampled single-joint interference checks on the document thread.

No continuous-motion guarantee is implied. A complete result describes only the
sampled + refined poses, with no global-minimum claim. Event scans own their
world-space geometry after state restoration.
"""

import bisect
import math
import threading

import FreeCAD as App
import AssemblyApp
from PySide import QtCore

from .api import _require_assembly


class SweepCancellation:
    """Cooperative cancellation, including a static scan running on another thread."""

    def __init__(self):
        self._event = threading.Event()
        self._token = AssemblyApp.newInterferenceScanCancellation()

    def cancel(self):
        self._event.set()
        AssemblyApp.cancelInterferenceScan(self._token)

    @property
    def cancelled(self):
        return self._event.is_set()


def _number(value, unit, field):
    if isinstance(value, (str, App.Units.Quantity)):
        value = App.Units.Quantity(value).getValueAs(unit).Value
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"{field} must be finite")
    return value


def _interval(joint, interval, step):
    angular = joint.JointType == "Revolute"
    unit, prefix = ("deg", "Angle") if angular else ("mm", "Length")
    endpoints = (None, None) if interval is None else tuple(interval)
    if len(endpoints) != 2:
        raise ValueError("interval must contain two endpoints (each may be None)")
    limits = []
    for suffix in ("Min", "Max"):
        enabled = getattr(joint, "Enable" + prefix + suffix)
        limits.append(_number(getattr(joint, prefix + suffix), unit, "limit")
                      if enabled else None)
    if all(v is not None for v in limits) and limits[0] > limits[1]:
        raise ValueError("Enabled minimum limit exceeds maximum limit")
    values = []
    for endpoint, limit in zip(endpoints, limits):
        if endpoint is None and limit is None:
            raise ValueError("Missing bound: supply an explicit interval endpoint")
        values.append(limit if endpoint is None else _number(endpoint, unit, "endpoint"))
    descending = values[1] < values[0]
    low, high = sorted(values)
    if limits[0] is not None:
        low = max(low, limits[0])
    if limits[1] is not None:
        high = min(high, limits[1])
    if low > high:
        raise ValueError("Requested interval does not intersect enabled limits")
    start, end = (high, low) if descending else (low, high)
    step = 1.0 if step is None else _number(step, unit, "step")
    if step <= 0:
        raise ValueError("step must be positive")
    # Adjacent rotations must be unambiguous when unwrapping the measured angle.
    spacing = min(step, 90.0) if angular else step
    count = max(1, math.ceil(abs(end - start) / spacing))
    if count > 10000:
        raise ValueError("Sweep exceeds 10001 sampled poses; increase step or narrow interval")
    return start, end, count, unit


def _validate_mechanism(assembly, joint):
    """Require a grounded tree of rigid components with exactly one moving joint."""
    import UtilsAssembly

    group = next((o for o in assembly.Group if o.TypeId == "Assembly::JointGroup"), None)
    joints = list(group.Group) if group else []
    if joint not in joints or getattr(joint, "Suppressed", True):
        raise ValueError("Choose an active joint in this assembly")
    if joint.JointType not in ("Revolute", "Slider"):
        raise ValueError("Only a single revolute or slider joint is supported")
    components = [o for o in assembly.Group if "Placement" in o.PropertiesList]
    for obj in assembly.Document.Objects:
        if obj != assembly and assembly.hasObject(obj, True):
            if obj.isDerivedFrom("Assembly::AssemblyObject") or obj.TypeId == "Assembly::AssemblyLink":
                raise ValueError("Nested or flexible assemblies are not supported")
            if getattr(obj, "ElementCount", 0):
                raise ValueError("Array occurrences are not supported as moving components")
    parent = {o: o for o in components}
    ground = object()
    parent[ground] = ground

    def root(obj):
        if obj not in parent:
            raise ValueError("Joint references must identify direct rigid assembly components")
        while parent[obj] != obj:
            obj = parent[obj]
        return obj

    def connect(a, b):
        a, b = root(a), root(b)
        if a == b:
            raise ValueError("Closed loops or redundant grounding are not supported")
        parent[a] = b

    edges = []
    for item in joints:
        if getattr(item, "Suppressed", False):
            continue
        if hasattr(item, "ObjectToGround"):
            if item.ObjectToGround is None:
                raise ValueError("Grounded joint has no component")
            connect(ground, item.ObjectToGround)
        elif hasattr(item, "ObjectsToRigidGroup"):
            members = list(item.ObjectsToRigidGroup)
            for member in members[1:]:
                connect(members[0], member)
        elif hasattr(item, "JointType"):
            if item != joint and item.JointType != "Fixed":
                raise ValueError("Coupled or additional moving joints are not supported")
            refs = (UtilsAssembly.getMovingPart(item.Reference1),
                    UtilsAssembly.getMovingPart(item.Reference2))
            if None in refs:
                raise ValueError("Joint has a missing component reference")
            edges.append((item, refs))
        else:
            raise ValueError("Unsupported object in the joint group")
    for item, (a, b) in edges:
        if item != joint:
            connect(a, b)
    refs = next(refs for item, refs in edges if item == joint)
    if (root(refs[0]) == root(ground)) == (root(refs[1]) == root(ground)):
        raise ValueError("Joint must connect grounded and moving rigid components; closed loops unsupported")
    connect(*refs)
    if any(root(obj) != root(ground) for obj in components):
        raise ValueError("Additional ungrounded moving components are not supported")


def _coordinate(joint, unit):
    import UtilsAssembly

    if unit == "deg":
        return math.degrees(UtilsAssembly.getJointXYAngle(joint))
    return UtilsAssembly.getJointDistance(joint)


def _driver(doc, joint, start, end, count, unit, created):
    # Plain motion data, with no import of the GUI simulation task module.
    sim = doc.addObject("App::FeaturePython", "MotionInterferenceSimulation")
    created.append(sim.Name)
    sim.addProperty("App::PropertyLinkList", "Group")
    for name, value in (("aTimeStart", 0), ("bTimeEnd", 1),
                        ("cTimeStepOutput", 1 / count), ("fGlobalErrorTolerance", 1e-8)):
        sim.addProperty("App::PropertyFloat", name)
        setattr(sim, name, value)
    motion = doc.addObject("App::FeaturePython", "MotionInterferenceDriver")
    created.append(motion.Name)
    motion.addProperty("App::PropertyXLinkSubHidden", "Joint")
    motion.Joint = (joint, [""])
    motion.addProperty("App::PropertyEnumeration", "MotionType")
    motion.MotionType = ["Angular", "Linear"]
    motion.MotionType = "Angular" if unit == "deg" else "Linear"
    motion.addProperty("App::PropertyString", "Formula")
    scale = math.pi / 180 if unit == "deg" else 1
    motion.Formula = f"({start * scale:.17g})+({(end - start) * scale:.17g})*time"
    sim.Group = [motion]
    return sim


def sweepJoint(assembly, joint, first, second, *, interval=None, step=None,
               clearance=None, maximumGap=None, refinementTolerance=None,
               refinementPoseBudget=20, cancel=None, progress=None, maxFacePairCandidates=10000,
               _scan=None):
    """Scan two occurrences through one bounded joint sweep, synchronously.

    Numeric endpoints/step are signed, unwrapped degrees for revolutes or mm for
    sliders; quantities and unit-bearing strings are accepted. None endpoints use
    enabled limits. Explicit intervals are intersected with those limits. Step is
    a maximum spacing (default 1 degree/mm); both endpoints are sampled.

    After a complete coarse sweep, bisect detected event transitions and refine
    the smallest measured whole-solid gap locally around neighbouring samples.
    Every evaluated pose contributes to events and the minimum; newly discovered
    event brackets are also bisected within their remaining refinement budget.
    refinementTolerance uses the joint coordinate unit (default step/100, with
    step defaulting to 1); refinementPoseBudget limits scans per refinement
    (default 20, zero disables extra poses). Brackets are sorted coordinates.
    This is a local sampled + refined search, never a global-minimum guarantee.
    maximumGap optionally bounds the pair's shortest separation in mm.

    Return a dict with samples, firstContact, firstClearanceViolation,
    firstGapViolation, minimumClearance, sampledMinimumClearance, completeness
    and reason. Refined records include bracket, converged, refinementPoses and
    sampledCoordinate; minima also retain sampledValue. Events retain scan
    witnesses and placements, previousClearSample and resolution. Exclusions
    suppress contact/clearance events, but retain raw distance measurements (also
    used for maximumGap). Contact is included in a clearance violation. Validation raises ValueError; runtime failures return incomplete.

    Use SweepCancellation for cooperative cancellation. progress(sample) runs
    after each scan (phase is "sampling" or "refinement") and may cancel or raise;
    it must not edit the document. Call on the document thread with no active
    transaction, simulation task or drag.
    """
    assembly = _require_assembly(assembly)
    _validate_mechanism(assembly, joint)
    start, end, count, unit = _interval(joint, interval, step)
    requirement = _number(assembly.InterferenceClearance if clearance is None else clearance,
                          "mm", "clearance")
    if requirement < 0:
        raise ValueError("clearance must be nonnegative")
    refinementTolerance = _number(
        (1.0 if step is None else _number(step, unit, "step")) / 100
        if refinementTolerance is None else refinementTolerance, unit, "refinementTolerance")
    if refinementTolerance <= 0:
        raise ValueError("refinementTolerance must be positive")
    if (not isinstance(refinementPoseBudget, int) or isinstance(refinementPoseBudget, bool)
            or refinementPoseBudget < 0):
        raise ValueError("refinementPoseBudget must be a nonnegative integer")
    if maximumGap is not None:
        maximumGap = _number(maximumGap, "mm", "maximumGap")
        if maximumGap < 0:
            raise ValueError("maximumGap must be nonnegative")
    if cancel is None:
        cancel = SweepCancellation()
    if not isinstance(cancel, SweepCancellation):
        raise TypeError("cancel must be a SweepCancellation")
    scan_pair = AssemblyApp.scanSelectedPair if _scan is None else _scan
    # Validate selected occurrence handles before any motion or temporary writes.
    AssemblyApp._prepareSelectedPairScan(
        assembly, first, second, cancel=cancel._token,
        maxFacePairCandidates=maxFacePairCandidates)
    result = dict(unit=unit, interval=(start, end), clearance=requirement, samples=[],
                  maximumGap=maximumGap, refinementTolerance=refinementTolerance,
                  refinementPoseBudget=refinementPoseBudget, sampledMinimumClearance=None,
                  firstGapViolation=None, firstContact=None, firstClearanceViolation=None,
                  minimumClearance=None,
                  completeness="incomplete", reason="Sweep did not finish",
                  message="no finding at the sampled + refined poses")
    tolerance = 1e-6

    def write(callback):
        # Permission is limited to synchronous controller-owned writes. Event
        # delivery, scans and user progress callbacks run with admission closed.
        return AssemblyApp._withMotionSweepMutation(assembly, callback)

    def run():
        created = []
        original_clearance = assembly.InterferenceClearance.Value
        event_flags = (("firstContact", "contact"),
                       ("firstClearanceViolation", "clearanceViolation"),
                       ("firstGapViolation", "gapViolation"))
        clear_poses = {flag: [] for _, flag in event_flags}

        def sweep_order(record):
            return record["coordinate"] * (1 if end >= start else -1)

        def register(record):
            # Every solved scan contributes, regardless of which refinement
            # requested it. Clear poses are ordered along the sweep, not time.
            sample = {k: v for k, v in record.items() if k not in ("scan", "placements")}
            for key, flag in event_flags:
                event = result[key]
                if not record[flag]:
                    bisect.insort(clear_poses[flag], sample, key=sweep_order)
                elif event is None:
                    event = result[key] = dict(record, sampledCoordinate=record["coordinate"],
                                               refinementPoses=0)
                elif sweep_order(record) < sweep_order(event):
                    event.update(record)
                if event is not None:
                    index = bisect.bisect_left(clear_poses[flag], sweep_order(event), key=sweep_order)
                    prior = clear_poses[flag][index - 1] if index else None
                    coordinate = event["coordinate"]
                    clear = prior["coordinate"] if prior else coordinate
                    event.update(previousClearSample=prior,
                                 resolution=abs(coordinate - clear) if prior else None,
                                 bracket=tuple(sorted((clear, coordinate))),
                                 converged=abs(coordinate - clear) <= refinementTolerance)
            minimum = result["minimumClearance"]
            if minimum is None:
                minimum = result["minimumClearance"] = dict(
                    record, value=record["minimumClearance"], sampledValue=record["minimumClearance"],
                    sampledCoordinate=record["coordinate"], converged=False,
                    bracket=None, refinementPoses=0)
            elif record["minimumClearance"] < minimum["value"]:
                minimum.update(record, value=record["minimumClearance"])
                if minimum["bracket"] is not None:
                    low, high = minimum["bracket"]
                    minimum["bracket"] = (min(low, record["coordinate"]),
                                          max(high, record["coordinate"]))
                    minimum["converged"] = (
                        minimum["bracket"][1] - minimum["bracket"][0] <= refinementTolerance)
            if record["phase"] == "sampling":
                minimum.update(sampledValue=minimum["value"], sampledCoordinate=minimum["coordinate"])
                result["sampledMinimumClearance"] = dict(minimum)

        def remove_drivers():
            for name in reversed(created):
                assembly.Document.removeObject(name)
            created.clear()

        def generate(low, high, segments):
            try:
                sim = _driver(assembly.Document, joint, low, high, segments, unit, created)
                code = assembly.generateSimulation(sim)
                if code != 0:
                    raise RuntimeError(f"Simulation generation failed ({code})")
                frames = assembly.numberOfFrames()
                if frames < 2:
                    raise RuntimeError("Simulation did not cover the requested end endpoint")
                if frames > segments + 3:
                    raise RuntimeError("Simulation returned an unexpected frame count")
                return frames
            finally:
                remove_drivers()

        def poses(low, high, segments):
            # Keep the native solver call short. All solving and document access
            # stays on this thread; no complete trajectory precedes the first scan.
            for offset in range(0, segments, 8):
                QtCore.QCoreApplication.processEvents()
                if cancel.cancelled:
                    return
                size = min(8, segments - offset)
                first = low + (high - low) * offset / segments
                last = low + (high - low) * (offset + size) / segments
                frames = write(lambda: generate(first, last, size))
                # Frame 0 is unsolved INPUT, frame 1 is the solved start.
                for frame in range(1, frames):
                    QtCore.QCoreApplication.processEvents()
                    if cancel.cancelled:
                        return
                    write(lambda: assembly.updateForFrame(frame))
                    yield frame

        class Cancelled(Exception):
            pass

        def evaluate(coordinate, frame, phase, index):
            if cancel.cancelled:
                raise Cancelled()
            sample = dict(coordinate=coordinate, frame=frame, phase=phase, index=index)
            scan = scan_pair(
                assembly, first, second, measureDistances=True,
                maxFacePairCandidates=maxFacePairCandidates, cancel=cancel._token)
            if scan["cancelled"] or cancel.cancelled:
                raise Cancelled()
            if not scan["complete"]:
                diagnostics = [i["diagnostic"] for i in scan["componentIssues"]]
                diagnostics += [p["faceEnumerationDiagnostic"] for p in scan["pairs"]]
                raise RuntimeError("Static scan incomplete: " + "; ".join(filter(None, diagnostics)))
            pairs = scan["pairs"]
            if not pairs or any(not p["distanceMeasured"] for p in pairs):
                raise RuntimeError("Static scan did not measure the whole selected pair")
            distance = min(p["solidDetection"]["minimumDistance"] for p in pairs)
            if not math.isfinite(distance) or distance < 0:
                raise RuntimeError("Static scan returned an invalid distance")
            active = [p for p in pairs if not p["excluded"]]
            contact = any(p["solidDetection"]["kind"] in ("Contact", "Penetration")
                          or any(h["kind"] in ("Contact", "Penetration")
                                 and not h["suppressedByExclusion"] for h in p["faceHits"])
                          for p in active)
            violation = contact or any(
                p["detection"]["kind"] == "ClearanceViolation" or any(
                    h["kind"] == "ClearanceViolation" and not h["suppressedByExclusion"]
                    for h in p["faceHits"]) for p in active)
            sample.update(minimumClearance=distance, contact=contact, clearanceViolation=violation)
            sample["gapViolation"] = maximumGap is not None and distance > maximumGap
            record = dict(sample, scan=scan,
                          placements={o.Name: App.Placement(o.Placement)
                                      for o in assembly.Document.Objects
                                      if "Placement" in o.PropertiesList})
            register(record)
            return record

        def notify(record):
            if progress:
                progress({k: v for k, v in record.items() if k not in ("scan", "placements")})
            if cancel.cancelled:
                raise Cancelled()

        def extra_pose(coordinate, index):
            QtCore.QCoreApplication.processEvents()
            if cancel.cancelled:
                raise Cancelled()
            current = _coordinate(joint, unit)
            if unit == "deg":
                current += 360 * round((coordinate - current) / 360)
            segments = max(1, math.ceil(abs(coordinate - current) / 30)) if unit == "deg" else 1
            for frame in poses(current, coordinate, segments):
                pass
            if cancel.cancelled:
                raise Cancelled()
            actual = _coordinate(joint, unit)
            if unit == "deg":
                actual += 360 * round((coordinate - actual) / 360)
            if not math.isfinite(actual) or abs(actual - coordinate) > tolerance:
                raise RuntimeError("Simulation did not solve the refinement coordinate")
            return evaluate(actual, frame, "refinement", index)

        def refine_minimum():
            sampled = result["sampledMinimumClearance"]
            index = sampled["index"]
            samples = result["samples"]
            low, high = sorted((samples[max(0, index - 1)]["coordinate"],
                                samples[min(len(samples) - 1, index + 1)]["coordinate"]))
            best = dict(result["minimumClearance"])
            low, high = min(low, best["coordinate"]), max(high, best["coordinate"])
            best.update(sampledCoordinate=sampled["coordinate"], sampledValue=sampled["value"],
                        bracket=(low, high), converged=False, refinementPoses=0)
            minimum = result["minimumClearance"]
            minimum.update(best)
            # Golden-section interval reduction, preserving the best solved pose
            # inside the bracket, including endpoint and flat minima.
            for used in range(refinementPoseBudget):
                if high - low <= refinementTolerance:
                    break
                x = best["coordinate"]
                edge = low if x - low > high - x else high
                probe = extra_pose(x + (edge - x) * (3 - math.sqrt(5)) / 2, index)
                y = probe["coordinate"]
                if probe["minimumClearance"] < best["value"]:
                    if y < x:
                        high = x
                    else:
                        low = x
                    best.update(probe, value=probe["minimumClearance"])
                elif y < x:
                    low = y
                else:
                    high = y
                best.update(bracket=(low, high), refinementPoses=used + 1)
                minimum.update(bracket=(low, high), refinementPoses=used + 1)
                notify(probe)
            minimum["converged"] = high - low <= refinementTolerance

        def refine_event(key):
            event = result[key]
            if event is None:
                return
            for used in range(event["refinementPoses"], refinementPoseBudget):
                previous = event["previousClearSample"]
                clear = previous["coordinate"] if previous else event["coordinate"]
                hit = event["coordinate"]
                if abs(hit - clear) <= refinementTolerance:
                    break
                probe = extra_pose((clear + hit) / 2, event["index"])
                event["refinementPoses"] = used + 1
                notify(probe)

        def refine_events():
            # A later event's bisection can discover an earlier event type.
            # Revisit those brackets while preserving each event's budget.
            while any(result[key] is not None and not result[key]["converged"]
                      and result[key]["refinementPoses"] < refinementPoseBudget
                      for key, _ in event_flags):
                for key, _ in event_flags:
                    refine_event(key)

        try:
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled before simulation")
                return
            write(lambda: setattr(assembly, "InterferenceClearance", requirement))
            if unit == "deg":
                current = _coordinate(joint, unit)
                delta = math.remainder(start - current, 360)
                if abs(delta) > 45:
                    # Initialize the rotational driver's direction-cosine branch
                    # in small steps. These seed poses are outside sweep coverage.
                    for _ in poses(current, current + delta, math.ceil(abs(delta) / 30)):
                        pass
                    if not cancel.cancelled and abs(
                            math.remainder(_coordinate(joint, unit) - start, 360)) > tolerance:
                        raise RuntimeError("Simulation could not solve the angular start branch")
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled during initialization")
                return
            for frame in poses(start, end, count):
                coordinate = _coordinate(joint, unit)
                previous = result["samples"][-1] if result["samples"] else None
                if unit == "deg":
                    anchor = previous["coordinate"] if previous else start
                    coordinate += 360 * round((anchor - coordinate) / 360)
                if not math.isfinite(coordinate):
                    raise RuntimeError("Solver returned a nonfinite joint coordinate")
                if previous:
                    delta = coordinate - previous["coordinate"]
                    if abs(delta) <= tolerance:
                        continue  # Native zero-start simulations repeat their initial frame.
                    if delta * (end - start) < 0 or abs(delta) > abs(end - start) / count + tolerance:
                        raise RuntimeError("Unexpected solver motion or missing sample")
                elif abs(coordinate - start) > tolerance:
                    raise RuntimeError(f"Simulation solved start {coordinate:g} {unit}; "
                                       f"requested {start:g} {unit}")
                if coordinate < min(start, end) - tolerance or coordinate > max(start, end) + tolerance:
                    raise RuntimeError("Solved pose is outside the permitted interval")
                record = evaluate(coordinate, frame, "sampling", len(result["samples"]))
                sample = {k: v for k, v in record.items() if k not in ("scan", "placements")}
                result["samples"].append(sample)
                notify(record)
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled during sweep")
            elif not result["samples"] or abs(result["samples"][-1]["coordinate"] - end) > tolerance:
                raise RuntimeError("Simulation did not cover the requested end endpoint")
            else:
                refine_events()
                refine_minimum()
                refine_events()
                result.update(completeness="complete", reason="")
        except Cancelled:
            result.update(completeness="cancelled", reason="Cancelled during sweep or refinement")
        finally:
            def cleanup():
                try:
                    remove_drivers()
                finally:
                    assembly.InterferenceClearance = original_clearance

            write(cleanup)

    try:
        AssemblyApp._withMotionSweepState(assembly, run)
    except Exception as error:
        result.update(completeness="incomplete", reason=str(error))
    if any(result[key] for key in ("firstContact", "firstClearanceViolation", "firstGapViolation")):
        result["message"] = "Finding at the sampled + refined poses; see event coordinate and resolution"
    return result
