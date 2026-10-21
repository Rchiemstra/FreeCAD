# SPDX-License-Identifier: LGPL-2.1-or-later

"""Bounded, sampled single-joint interference checks on the document thread.

No continuous-motion guarantee is implied. A complete result describes only the
sampled poses. Event scans own their world-space geometry after state restoration.
"""

import math
import threading

import FreeCAD as App
import AssemblyApp

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
               clearance=None, cancel=None, progress=None, maxFacePairCandidates=10000,
               _scan=None):
    """Scan two occurrences through one bounded joint sweep, synchronously.

    Numeric endpoints/step are signed, unwrapped degrees for revolutes or mm for
    sliders; quantities and unit-bearing strings are accepted. None endpoints use
    enabled limits. Explicit intervals are intersected with those limits. Step is
    a maximum spacing (default 1 degree/mm); both endpoints are sampled.

    Return a dict with samples, firstContact, firstClearanceViolation,
    minimumClearance, completeness and reason. Events retain scan witnesses and
    placements, previousClearSample and resolution. Exclusions suppress events,
    but retain raw distance measurements. Contact is included in a clearance
    violation. Validation raises ValueError; runtime failures return incomplete.

    Use SweepCancellation for cooperative cancellation. progress(sample) runs
    after each scan and may cancel or raise; it must not edit the document. Call
    on the document thread with no active transaction, simulation task or drag.
    """
    assembly = _require_assembly(assembly)
    _validate_mechanism(assembly, joint)
    start, end, count, unit = _interval(joint, interval, step)
    requirement = _number(assembly.InterferenceClearance if clearance is None else clearance,
                          "mm", "clearance")
    if requirement < 0:
        raise ValueError("clearance must be nonnegative")
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
                  firstContact=None, firstClearanceViolation=None, minimumClearance=None,
                  completeness="incomplete", reason="Sweep did not finish",
                  message="no finding at the sampled poses")
    tolerance = 1e-6

    def run():
        created = []
        original_clearance = assembly.InterferenceClearance.Value
        previous_contact_clear = previous_requirement_clear = None
        try:
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled before simulation")
                return
            assembly.InterferenceClearance = requirement
            if unit == "deg":
                current = _coordinate(joint, unit)
                delta = math.remainder(start - current, 360)
                if abs(delta) > 45:
                    # The native rotational driver has two direction-cosine
                    # branches. Initialize near the requested branch through
                    # the existing simulation path, rather than allowing a
                    # large initial-condition jump to choose its opposite.
                    # Only the solved seed endpoint is applied to the document;
                    # these initialization frames are outside sweep coverage.
                    seed = _driver(assembly.Document, joint, current, current + delta,
                                   math.ceil(abs(delta) / 30), unit, created)
                    code = assembly.generateSimulation(seed)
                    if code != 0 or assembly.numberOfFrames() < 2:
                        raise RuntimeError("Simulation could not initialize the angular start")
                    assembly.updateForFrame(assembly.numberOfFrames() - 1)
                    if abs(math.remainder(_coordinate(joint, unit) - start, 360)) > tolerance:
                        raise RuntimeError("Simulation could not solve the angular start branch")
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled during initialization")
                return
            sim = _driver(assembly.Document, joint, start, end, count, unit, created)
            code = assembly.generateSimulation(sim)
            if code != 0:
                raise RuntimeError(f"Simulation generation failed ({code})")
            frames = assembly.numberOfFrames()
            if frames > count + 3:
                raise RuntimeError("Simulation returned an unexpected frame count")
            # Ondsel System::runKINEMATIC outputs INPUT before solving initial
            # conditions. Frame 0 is the document's unsolved input, not a sample;
            # frame 1 is the actual solved start, even for nonzero endpoints.
            for frame in range(1, frames):
                if cancel.cancelled:
                    result.update(completeness="cancelled", reason="Cancelled during sweep")
                    return
                assembly.updateForFrame(frame)
                coordinate = _coordinate(joint, unit)
                previous = result["samples"][-1] if result["samples"] else None
                if unit == "deg":
                    anchor = previous["coordinate"] if previous else start
                    coordinate += 360 * round((anchor - coordinate) / 360)
                if not math.isfinite(coordinate):
                    raise RuntimeError("Solver returned a nonfinite joint coordinate")
                if coordinate < min(start, end) - tolerance or coordinate > max(start, end) + tolerance:
                    raise RuntimeError("Solved pose is outside the permitted interval")
                if previous:
                    delta = coordinate - previous["coordinate"]
                    if abs(delta) <= tolerance:
                        continue  # Native zero-start simulations repeat their initial frame.
                    if delta * (end - start) < 0 or abs(delta) > abs(end - start) / count + tolerance:
                        raise RuntimeError("Unexpected solver motion or missing sample")
                elif abs(coordinate - start) > tolerance:
                    raise RuntimeError(f"Simulation solved start {coordinate:g} {unit}; "
                                       f"requested {start:g} {unit}")
                scan = scan_pair(
                    assembly, first, second, measureDistances=True,
                    maxFacePairCandidates=maxFacePairCandidates, cancel=cancel._token)
                if scan["cancelled"] or cancel.cancelled:
                    result.update(completeness="cancelled", reason="Static scan cancelled")
                    return
                sample = dict(coordinate=coordinate, frame=frame, index=len(result["samples"]))
                result["samples"].append(sample)
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

                def snapshot(previous_clear):
                    return dict(sample, previousClearSample=previous_clear,
                                resolution=abs(coordinate - previous_clear["coordinate"])
                                if previous_clear else None, scan=scan,
                                placements={o.Name: App.Placement(o.Placement)
                                            for o in assembly.Document.Objects
                                            if "Placement" in o.PropertiesList})

                if contact and result["firstContact"] is None:
                    result["firstContact"] = snapshot(previous_contact_clear)
                if violation and result["firstClearanceViolation"] is None:
                    result["firstClearanceViolation"] = snapshot(previous_requirement_clear)
                if result["minimumClearance"] is None or distance < result["minimumClearance"]["value"]:
                    result["minimumClearance"] = dict(snapshot(None), value=distance)
                if not contact:
                    previous_contact_clear = sample
                if not violation:
                    previous_requirement_clear = sample
                if progress:
                    progress(dict(sample))
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled during sweep")
            elif not result["samples"] or abs(result["samples"][-1]["coordinate"] - end) > tolerance:
                raise RuntimeError("Simulation did not cover the requested end endpoint")
            else:
                result.update(completeness="complete", reason="")
        finally:
            try:
                for name in reversed(created):
                    assembly.Document.removeObject(name)
            finally:
                assembly.InterferenceClearance = original_clearance

    try:
        AssemblyApp._withMotionSweepState(assembly, run)
    except Exception as error:
        result.update(completeness="incomplete", reason=str(error))
    if result["firstContact"] or result["firstClearanceViolation"]:
        result["message"] = "Finding at the sampled poses; see event coordinate and resolution"
    return result
