# SPDX-License-Identifier: LGPL-2.1-or-later

"""Sampled joint-range and prescribed-simulation checks on the document thread.

Rigid one-joint sweeps can certify whole-solid separation between poses. Other
results describe sampled + refined poses, with no global-minimum claim. Event
scans own their world-space geometry after state restoration.
"""

import bisect
import math
import operator
import sys
import threading

import FreeCAD as App
import AssemblyApp
from PySide import QtCore

from .api import _require_assembly
from .MotionCoverage import RigidSweepCoverage


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


def _interval(joint, interval, step, motionType=None, *, _poseLimit=10000):
    angular = joint.JointType == "Revolute" or motionType == "Angular"
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
    if _poseLimit is not None and count > _poseLimit:
        raise ValueError("Sweep exceeds 10001 sampled poses; increase step or narrow interval")
    return start, end, count, unit


def _validate_mechanism(assembly, joint):
    """Require a grounded tree of rigid components with exactly one moving joint."""
    import UtilsAssembly

    group = next((o for o in assembly.Group if o.TypeId == "Assembly::JointGroup"), None)
    joints = list(group.Group) if group else []
    applied = list(assembly.Joints)
    if joint not in applied:
        raise ValueError("Choose an active joint in this assembly")
    if joint.JointType not in ("Revolute", "Slider", "Cylindrical"):
        raise ValueError("Only a single revolute, slider or cylindrical joint is supported")
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
            if item not in applied:
                continue
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
    moving = {obj.Name for obj in components if root(obj) != root(ground)}
    fixed_reference = 1 if root(refs[0]) == root(ground) else 2
    connect(*refs)
    if any(root(obj) != root(ground) for obj in components):
        raise ValueError("Additional ungrounded moving components are not supported")
    return moving, fixed_reference


def _coordinate(joint, unit):
    import UtilsAssembly

    if unit == "deg":
        return math.degrees(UtilsAssembly.getJointXYAngle(joint))
    return UtilsAssembly.getJointDistance(joint)


def _driver(doc, joint, start, end, count, unit, created, heldCoordinate=None):
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
    if joint.JointType == "Cylindrical":
        # Prescribe the second degree of freedom as a constant: a cylindrical
        # sweep is one rigid translation OR rotation, never a free screw path.
        other_unit = "mm" if unit == "deg" else "deg"
        other = doc.addObject("App::FeaturePython", "MotionInterferenceHeldCoordinate")
        created.append(other.Name)
        other.addProperty("App::PropertyXLinkSubHidden", "Joint")
        other.Joint = (joint, [""])
        other.addProperty("App::PropertyEnumeration", "MotionType")
        other.MotionType = ["Angular", "Linear"]
        other.MotionType = "Linear" if unit == "deg" else "Angular"
        other.addProperty("App::PropertyString", "Formula")
        value = _coordinate(joint, other_unit) if heldCoordinate is None else heldCoordinate
        other.Formula = repr(math.radians(value) if other_unit == "deg" else value)
        sim.Group = [motion, other]
    return sim


def _simulations(assembly):
    return [sim for group in assembly.Group if group.TypeId == "Assembly::SimulationGroup"
            for sim in group.Group]


class _InfeasiblePose(RuntimeError):
    """A solved pose demonstrably misses a prescribed coordinate or enabled limit."""


class _SimulationTrajectory:
    """Native finite trajectory, using disposable parameter/motion copies.

    Freeze initialValue at entry. Refinement re-solves the prefix ending at the
    requested time from the original placements, so closed-loop branch selection
    and initialValue never depend on the order of refinement probes.
    """

    def __init__(self, assembly, simulation):
        if simulation not in _simulations(assembly):
            raise ValueError("Choose a simulation belonging to this assembly")
        self.assembly = assembly
        self.simulation = simulation
        self.start = _number(simulation.aTimeStart, "s", "simulation start")
        self.end = _number(simulation.bTimeEnd, "s", "simulation end")
        self.step = _number(simulation.cTimeStepOutput, "s", "simulation output step")
        if (self.end <= self.start or self.step <= 0 or
                not math.isfinite((self.end - self.start) / self.step)):
            raise ValueError("Simulation needs an increasing finite time range and positive output step")
        self.count = math.ceil((self.end - self.start) / self.step)
        if self.count > 10000:
            raise ValueError("Simulation exceeds 10001 sampled frames")
        self.motions = []
        joints = list(assembly.Joints)
        for motion in simulation.Group:
            ref = getattr(motion, "Joint", None)
            joint = ref[0] if ref else None
            kind = str(getattr(motion, "MotionType", ""))
            if joint not in joints or getattr(joint, "Suppressed", False):
                raise ValueError("Simulation motion must reference an active joint in this assembly")
            if kind not in ("Angular", "Linear"):
                raise ValueError("Simulation motion must be Angular or Linear")
            unit = "deg" if kind == "Angular" else "mm"
            value = _coordinate(joint, unit)
            if unit == "deg":
                value = math.radians(value)
            formula = motion.Formula.replace("initialValue", f"({value:.10g})")
            self.motions.append((joint, kind, formula))
        if not self.motions:
            raise ValueError("Simulation has no driven joints")
        self.initial = {obj.Name: App.Placement(obj.Placement) for obj in assembly.Document.Objects
                        if "Placement" in obj.PropertiesList}

    def coordinates(self, time, frame, angleOffsets=None):
        # Check every enabled bound, including dependent and cylindrical joints.
        coordinates = {}
        prescribed = {}
        offsets = angleOffsets or {}
        for joint, kind, formula in self.motions:
            unit = "deg" if kind == "Angular" else "mm"
            try:
                values = AssemblyApp._motionSimulationJointState(
                    self.assembly, joint, frame, formula, time)
                wanted = math.degrees(values[0]) if unit == "deg" else values[0]
                actual = math.degrees(values[1]) if unit == "deg" else _coordinate(joint, unit)
                if unit == "deg":
                    actual += offsets.get(joint.Name, 0)
                if not math.isfinite(wanted) or not math.isfinite(actual):
                    raise RuntimeError("Nonfinite prescribed or solved coordinate")
                if abs(actual - wanted) > 1e-6:
                    raise _InfeasiblePose(
                        f"Prescribed {wanted:g} {unit}, solved {actual:g} {unit}; "
                        "angular turns must be unambiguous")
                if unit == "deg" and abs(math.remainder(
                        _coordinate(joint, unit) - wanted, 360)) > 1e-6:
                    raise _InfeasiblePose("Solved placement does not reach the prescribed angle")
                prescribed[(joint.Name, unit)] = wanted
            except Exception as error:
                limited = ", ".join(f"{j.Label} ({j.Name})" for j in self.assembly.Joints
                                    if not getattr(j, "Suppressed", False) and
                                    (getattr(j, "EnableAngleMin", False) or
                                     getattr(j, "EnableAngleMax", False)))
                diagnostic = f"; enabled angular limits unverified: {limited}" if limited else ""
                error_type = _InfeasiblePose if isinstance(error, _InfeasiblePose) else RuntimeError
                raise error_type(f"Joint {joint.Label} ({joint.Name}): "
                                 f"{error}{diagnostic}") from error
        for joint in self.assembly.Joints:
            if getattr(joint, "Suppressed", False):
                continue
            for unit, prefix in (("deg", "Angle"), ("mm", "Length")):
                driven = any(j == joint and (kind == "Angular") == (unit == "deg")
                             for j, kind, _ in self.motions)
                bounds = [(suffix, _number(getattr(joint, prefix + suffix), unit, "joint limit"))
                          for suffix in ("Min", "Max")
                          if getattr(joint, "Enable" + prefix + suffix, False)]
                if not driven and not bounds:
                    continue
                key = (joint.Name, unit)
                if driven:
                    value = prescribed[key]
                elif unit == "deg":
                    try:
                        values = AssemblyApp._motionSimulationJointState(
                            self.assembly, joint, frame, "", time)
                        value = math.degrees(values[0]) + offsets.get(joint.Name, 0)
                    except Exception as error:
                        raise RuntimeError(f"Joint {joint.Label} ({joint.Name}) enabled "
                                           f"angular limits unverified: {error}") from error
                else:
                    value = _coordinate(joint, unit)
                if not math.isfinite(value):
                    raise RuntimeError(f"Joint {joint.Label} has a nonfinite coordinate")
                for suffix, bound in bounds:
                    if (suffix == "Min" and value < bound - 1e-6 or
                            suffix == "Max" and value > bound + 1e-6):
                        raise _InfeasiblePose(
                            f"Joint {joint.Label} ({joint.Name}) exceeds enabled "
                            f"{prefix}{suffix} limit: {value:g} {unit}")
                if driven:
                    coordinates.setdefault(joint.Name, {})[unit] = value
        # Revolute and slider drivers have one coordinate; cylindrical drivers
        # can have both, retained without overwriting either motion.
        return {name: (dict(value=next(iter(values.values())), unit=next(iter(values)))
                       if len(values) == 1 else dict(coordinates=values))
                for name, values in coordinates.items()}

    def generate(self, end, step):
        doc = self.assembly.Document
        created = []
        try:
            for name, placement in self.initial.items():
                obj = doc.getObject(name)
                if obj.Placement.toMatrix().A != placement.toMatrix().A:
                    obj.Placement = placement
            sim = doc.addObject("App::FeaturePython", "MotionInterferenceSimulation")
            created.append(sim.Name)
            sim.addProperty("App::PropertyLinkList", "Group")
            sim.addProperty("App::PropertyBool", "RecordMotionCoordinates")
            sim.RecordMotionCoordinates = True
            for name, value in (("aTimeStart", self.start), ("bTimeEnd", end),
                                ("cTimeStepOutput", step),
                                ("fGlobalErrorTolerance", self.simulation.fGlobalErrorTolerance)):
                sim.addProperty("App::PropertyFloat", name)
                setattr(sim, name, float(value))
            motions = []
            for joint, kind, formula in self.motions:
                motion = doc.addObject("App::FeaturePython", "MotionInterferenceDriver")
                created.append(motion.Name)
                motion.addProperty("App::PropertyXLinkSubHidden", "Joint")
                motion.Joint = (joint, [""])
                motion.addProperty("App::PropertyEnumeration", "MotionType")
                motion.MotionType = ["Angular", "Linear"]
                motion.MotionType = kind
                motion.addProperty("App::PropertyString", "Formula")
                motion.Formula = formula
                motions.append(motion)
            sim.Group = motions
            code = self.assembly.generateSimulation(sim)
            if code != 0:
                raise RuntimeError(f"Simulation generation failed ({code})")
            times = AssemblyApp._motionSimulationFrameTimes(self.assembly)
            duration = end - self.start
            tolerance = duration * 1e-8
            expected = [self.start + i * step for i in range(
                math.floor(duration / step) + 1)]
            if len(expected) == 1 or abs(expected[-1] - end) > tolerance:
                expected.append(end)
            else:
                expected[-1] = end
            if (len(times) != len(expected) + 1 or
                    self.assembly.numberOfFrames() != len(times) or
                    any(not math.isfinite(t) or abs(t - wanted) > tolerance
                        or i > 0 and t <= times[i]
                        for i, (t, wanted) in enumerate(zip(times[1:], expected)))):
                raise RuntimeError("Simulation has missing or unexpected frames/times or end frame")
            return times
        finally:
            for name in reversed(created):
                doc.removeObject(name)

    def record(self, time, frame):
        try:
            return dict(time=time, frame=frame, drivenCoordinates=self.coordinates(time, frame))
        except Exception as error:
            raise RuntimeError(f"Frame {frame} (t = {time:g} s): {error}") from error


def sweepSimulation(assembly, simulation, first=None, second=None, *,
                    clearance=None, maximumGap=None,
                    refinementTolerance=None, refinementPoseBudget=20, cancel=None, progress=None,
                    maxFacePairCandidates=10000, scope="pair", pairBudget=1000,
                    poseBudget=10000, includeHidden=False, components=None, _scan=None):
    """Check a finite existing simulation's prescribed solver trajectory.

    Uses sweepJoint's scanner, events, minimum search and scoped restoration.
    Records contain native frame index, absolute time (seconds), and signed
    drivenCoordinates (degrees/mm). Frame 0 is unsolved input and is not scanned.
    Every enabled joint limit is checked before accepting each solved pose.
    Every driver must participate and reach its prescribed coordinate at each
    frame. Angles use solver iteration coordinates, never nearest output-angle
    unwrapping. Missing/unexpected frames or solver errors yield incomplete results.

    Refinement tolerance is in seconds (default output step/100). Disposable
    copies freeze initialValue at entry and re-solve [start, probe time] from the
    entry pose with output spacing at most the original step. No placement
    interpolation or permanent simulation edit is used. Refined records retain
    the preceding native frame and frameFraction. Zero budget gives explicitly
    frame-sampled results. Sampling/refinement is no continuous clearance proof.
    All-components scope excludes hidden leaves unless includeHidden=True.
    Other options, callbacks and validation follow sweepJoint.
    """
    assembly = _require_assembly(assembly)
    if scope not in ("pair", "selection", "all"):
        raise ValueError('scope must be "pair", "selection" or "all"')
    trajectory = _SimulationTrajectory(assembly, simulation)
    if scope != "pair":
        from .MotionAllPairs import sweepAll

        _validate_options(assembly, trajectory.step, "s", clearance, maximumGap,
                          refinementTolerance, refinementPoseBudget, 64, cancel,
                          maxFacePairCandidates)
        return sweepAll(assembly, lambda a, b, **opts: sweepSimulation(
            assembly, simulation, a, b, **opts), pairBudget=pairBudget, poseBudget=poseBudget,
            clearance=clearance, maximumGap=maximumGap, refinementTolerance=refinementTolerance,
            refinementPoseBudget=refinementPoseBudget, cancel=cancel, progress=progress,
            maxFacePairCandidates=maxFacePairCandidates, includeHidden=includeHidden,
            components=components, scope=scope, _scan=_scan)
    return _sweep(assembly, None, first, second, start=trajectory.start, end=trajectory.end,
                  count=trajectory.count, unit="s", step=trajectory.step, clearance=clearance,
                  maximumGap=maximumGap, refinementTolerance=refinementTolerance,
                  refinementPoseBudget=refinementPoseBudget, cancel=cancel, progress=progress,
                  maxFacePairCandidates=maxFacePairCandidates, _scan=_scan, trajectory=trajectory)


def sweepJoint(assembly, joint, first=None, second=None, *, interval=None, step=None,
               clearance=None, maximumGap=None, refinementTolerance=None,
               refinementPoseBudget=20, cancel=None, progress=None, maxFacePairCandidates=10000,
               coverageBudget=64, motionType=None, scope="pair", pairBudget=1000,
               poseBudget=10000, includeHidden=False, components=None, _scan=None):
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
    coverageBudget limits additional midpoint scans across the entire stroke
    (default 64; zero disables coverage). A supported rigid pair gets coverage
    "proven" only when every interval's Lipschitz lower bound exceeds clearance;
    otherwise unresolvedIntervals retain unproved coordinate ranges. boundConstant
    is 1 mm/mm for translation or a bounding-box radius in mm/rad for rotation;
    provenLowerBound is a conservative stroke-wide separation bound in mm.
    Cylindrical joints default to Linear; motionType="Angular" sweeps rotation
    while holding translation fixed (or vice versa). Simulation coverage is never
    available. Maximum-gap coverage uses the same Lipschitz upper bound.
    Face rules remain sampled + refined. scope="all" sweeps changing leaf pairs
    with shared pairBudget (enumerated pairs) and poseBudget (static scan calls);
    exhaustion returns incomplete. Hidden leaves are excluded unless includeHidden=True.
    scope="selection", components=[...] restricts leaf pairs to distinct selected
    direct components (at least two). scope="pair" preserves selected-pair behavior.
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
    mechanism = _validate_mechanism(assembly, joint)
    if scope not in ("pair", "selection", "all"):
        raise ValueError('scope must be "pair", "selection" or "all"')
    if motionType is not None and (joint.JointType != "Cylindrical" or
                                   motionType not in ("Angular", "Linear")):
        raise ValueError("motionType is Angular or Linear for a cylindrical joint only")
    start, end, count, unit = _interval(joint, interval, step, motionType)
    if scope != "pair":
        from .MotionAllPairs import sweepAll

        _validate_options(assembly, step, unit, clearance, maximumGap, refinementTolerance,
                          refinementPoseBudget, coverageBudget, cancel, maxFacePairCandidates)
        return sweepAll(assembly, lambda a, b, **opts: sweepJoint(assembly, joint, a, b, **opts),
                        mechanism=mechanism, pairBudget=pairBudget, poseBudget=poseBudget,
                        interval=interval, step=step, clearance=clearance, maximumGap=maximumGap,
                        refinementTolerance=refinementTolerance,
                        refinementPoseBudget=refinementPoseBudget, cancel=cancel, progress=progress,
                        maxFacePairCandidates=maxFacePairCandidates, coverageBudget=coverageBudget,
                        motionType=motionType, includeHidden=includeHidden, components=components,
                        scope=scope, _scan=_scan)

    return _sweep(assembly, joint, first, second, start=start, end=end, count=count,
                  unit=unit, step=step, clearance=clearance, maximumGap=maximumGap,
                  refinementTolerance=refinementTolerance, refinementPoseBudget=refinementPoseBudget,
                  cancel=cancel, progress=progress, maxFacePairCandidates=maxFacePairCandidates,
                  _scan=_scan, coverageBudget=coverageBudget, mechanism=mechanism)


def _validate_options(assembly, step, unit, clearance, maximumGap, refinementTolerance,
                      refinementPoseBudget, coverageBudget, cancel, maxFacePairCandidates):
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
    if (not isinstance(coverageBudget, int) or isinstance(coverageBudget, bool)
            or coverageBudget < 0):
        raise ValueError("coverageBudget must be a nonnegative integer")
    if maximumGap is not None:
        maximumGap = _number(maximumGap, "mm", "maximumGap")
        if maximumGap < 0:
            raise ValueError("maximumGap must be nonnegative")
    cap = operator.index(maxFacePairCandidates)
    if cap > sys.maxsize or cap < -sys.maxsize - 1:
        raise OverflowError("Python int too large to convert to C ssize_t")
    if cap < 0:
        raise ValueError("maxFacePairCandidates must be nonnegative (zero means unlimited)")
    if cancel is None:
        cancel = SweepCancellation()
    if not isinstance(cancel, SweepCancellation):
        raise TypeError("cancel must be a SweepCancellation")
    return requirement, maximumGap, refinementTolerance, cancel


def _sweep(assembly, joint, first, second, *, start, end, count, unit, step, clearance,
           maximumGap, refinementTolerance, refinementPoseBudget, cancel, progress,
           maxFacePairCandidates, _scan, trajectory=None, coverageBudget=64, mechanism=None):
    requirement, maximumGap, refinementTolerance, cancel = _validate_options(
        assembly, step, unit, clearance, maximumGap, refinementTolerance,
        refinementPoseBudget, coverageBudget, cancel, maxFacePairCandidates)
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
    if trajectory is not None:
        result.update(mode="Simulation", sampling="frame-sampled" if refinementPoseBudget == 0
                      else "sampled + refined", simulation=trajectory.simulation.Name)
    coverage = RigidSweepCoverage(
        assembly, joint, unit, mechanism, start, end, coverageBudget, result)
    tolerance = 1e-6

    def write(callback):
        # Permission is limited to synchronous controller-owned writes. Event
        # delivery, scans and user progress callbacks run with admission closed.
        return AssemblyApp._withMotionSweepMutation(assembly, callback)

    def run():
        created = []
        original_clearance = assembly.InterferenceClearance.Value
        held_coordinate = (_coordinate(joint, "mm" if unit == "deg" else "deg")
                           if joint is not None and joint.JointType == "Cylindrical" else None)
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
                options = {} if held_coordinate is None else dict(heldCoordinate=held_coordinate)
                sim = _driver(assembly.Document, joint, low, high, segments, unit, created, **options)
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

        def evaluate(coordinate, frame, phase, index, native_frame=None):
            if cancel.cancelled:
                raise Cancelled()
            sample = dict(coordinate=coordinate, frame=frame, phase=phase, index=index)
            if trajectory is not None:
                sample.update(trajectory.record(
                    coordinate, frame if native_frame is None else native_frame))
                sample["frame"] = frame
                sample["frameFraction"] = 1 + (coordinate - start) / step
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
            coverage.observe(record)
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
            if trajectory is not None:
                # Re-solve a prefix to preserve the original mechanism branch.
                segments = max(1, math.ceil((coordinate - start) / step))
                spacing = (coordinate - start) / segments
                times = write(lambda: trajectory.generate(coordinate, spacing))
                for native_frame in range(1, len(times)):
                    if cancel.cancelled:
                        raise Cancelled()
                    write(lambda: assembly.updateForFrame(native_frame))
                    trajectory.record(times[native_frame], native_frame)  # intermediate limits too
                return evaluate(coordinate, min(count, math.floor((coordinate-start)/step)) + 1,
                                "refinement", index, native_frame=len(times) - 1)
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
            if trajectory is not None:
                times = write(lambda: trajectory.generate(end, step))
                for frame, time in enumerate(times[1:], 1):
                    QtCore.QCoreApplication.processEvents()
                    if cancel.cancelled:
                        raise Cancelled()
                    write(lambda: assembly.updateForFrame(frame))
                    record = evaluate(time, frame, "sampling", len(result["samples"]))
                    result["samples"].append({k: v for k, v in record.items()
                                              if k not in ("scan", "placements")})
                    notify(record)
            else:
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
                coverage.refine(lambda coordinate: extra_pose(coordinate, 0), notify)
                coverage.finish_gap()
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
    if result["coverage"] == "proven" and result["completeness"] == "complete":
        result["message"] = "no contact proven between {} and {}".format(
            first.Label if hasattr(first, "Label") else str(first),
            second.Label if hasattr(second, "Label") else str(second))
    elif result["coverage"] == "proven":
        result.update(coverage="unresolved", coverageReason="Sweep did not finish",
                      unresolvedIntervals=[tuple(sorted((start, end)))])
    if maximumGap is not None and result["completeness"] != "complete":
        result.update(gapCoverage="unresolved", gapCoverageReason="Sweep did not finish")
    if trajectory is not None:
        result["message"] = result["message"].replace("sampled + refined", result["sampling"])
    return result


def setDragFeedback(assembly, pairs=None, enabled=True, *, clearance=None):
    """Enable transient GUI contact feedback at displayed drag poses; off by default."""
    from .DragFeedback import setDragFeedback as configure

    return configure(assembly, pairs, enabled, clearance=clearance)
