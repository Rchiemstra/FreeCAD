# SPDX-License-Identifier: LGPL-2.1-or-later

"""Product-grid possible movement, with a fresh native solve at every pose."""

import itertools
import math

import FreeCAD as App
import AssemblyApp
from PySide import QtCore

from .api import _require_assembly
from .MotionInterference import (
    _coordinate, _driver, _InfeasiblePose, _interval, _SimulationTrajectory, _validate_options,
)


class _Unreachable(Exception):
    pass


class _Cancelled(Exception):
    pass


def _per_joint(value, joints, default=None):
    if value is None:
        return [default] * len(joints)
    if isinstance(value, dict):
        return [value.get(j, value.get(j.Name, default)) for j in joints]
    if isinstance(value, (list, tuple)):
        if len(value) != len(joints):
            raise ValueError("Supply one range/step per joint")
        return list(value)
    return [value] * len(joints)


def sweepJoints(assembly, joints, ranges=None, steps=1, *, first=None, second=None,
                clearance=None, maximumGap=None, refinementTolerance=None,
                refinementPoseBudget=20, cancel=None, progress=None, maxFacePairCandidates=10000,
                coverageBudget=64, motionType=None, scope="pair", pairBudget=1000,
                poseBudget=10000, includeHidden=False, components=None, _scan=None):
    """Sample the Cartesian product of bounded joint coordinates in input order.

    Ranges and steps are sequences in joint order or dictionaries keyed by joint
    object/name; a scalar steps applies to every joint. None ranges use enabled
    limits, as in sweepJoint. Cylindrical joints hold the other coordinate fixed.
    Every pose is solved from the entry placements, including refinement probes.
    An inconsistent or unreached prescribed pose increments unreachable and is
    never scanned. Results are sampled + refined; coverage is not available.
    First means product traversal order, not a simultaneous motion trajectory.
    Event records retain all drivenCoordinates and owned scan/placement witnesses.
    Refinement bisects one axis to a clear adjacent grid pose, holding all other
    coordinates fixed. No continuous multi-joint or global-minimum claim is made.
    Grid size above poseBudget raises before any mutation or solve. Thereafter
    poseBudget bounds scan calls, including refinement and all leaf pairs.
    """
    assembly = _require_assembly(assembly)
    joints = list(joints)
    if len(joints) < 2 or len(set(joints)) != len(joints):
        raise ValueError("Choose at least two distinct active joints")
    if any(j not in assembly.Joints or getattr(j, "Suppressed", False) or
           j.JointType not in ("Revolute", "Slider", "Cylindrical") for j in joints):
        raise ValueError("Choose active revolute, slider or cylindrical joints in this assembly")
    if scope not in ("pair", "selection", "all"):
        raise ValueError('scope must be "pair", "selection" or "all"')
    if motionType not in (None, "Angular", "Linear"):
        raise ValueError("motionType must be Angular or Linear")
    for name, value in (("poseBudget", poseBudget), ("pairBudget", pairBudget)):
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ValueError(name + " must be a nonnegative integer")
    ranges = _per_joint(ranges, joints)
    steps = _per_joint(steps, joints, 1)
    intervals = [_interval(j, r, s, motionType if j.JointType == "Cylindrical" else None,
                           _poseLimit=None) for j, r, s in zip(joints, ranges, steps)]
    grid_size = math.prod(n+1 if a != b else 1 for a, b, n, _ in intervals)
    if grid_size > poseBudget:
        raise ValueError(f"Grid has {grid_size} poses; poseBudget is {poseBudget}")
    axes = [[a + (b-a)*i/n for i in range(n+1)] if a != b else [a]
            for a, b, n, _ in intervals]
    validated = [_validate_options(assembly, s, unit, clearance, maximumGap,
                                   refinementTolerance, refinementPoseBudget, coverageBudget,
                                   cancel, maxFacePairCandidates)
                 for s, (_, _, _, unit) in zip(steps, intervals)]
    requirement, maximumGap, _, cancel = validated[0]
    tolerances = [v[2] for v in validated]
    if scope != "pair":
        from .MotionAllPairs import sweepAll

        result = sweepAll(
            assembly, lambda a, b, **opts: sweepJoints(
                assembly, joints, ranges, steps, first=a, second=b,
                poseBudget=poseBudget, **opts),
            scope=scope, components=components, pairBudget=pairBudget, poseBudget=poseBudget,
            clearance=clearance, maximumGap=maximumGap, refinementTolerance=refinementTolerance,
            refinementPoseBudget=refinementPoseBudget, cancel=cancel, progress=progress,
            maxFacePairCandidates=maxFacePairCandidates, motionType=motionType,
            includeHidden=includeHidden, _scan=_scan)
        result.update(mode="Several joints", gridSize=grid_size, sampling="sampled + refined",
                      coverage="not available", unreachable=len({
                          tuple(pose["pose"]) for child in result["pairResults"]
                          for pose in child["unreachablePoses"]}))
        minima = [dict(p["minimumClearance"], pair=p["pair"]) for p in result["pairResults"]
                  if p["minimumClearance"] is not None]
        result["minimumClearance"] = min(minima, key=lambda p: p["value"], default=None)
        return result
    AssemblyApp._prepareSelectedPairScan(assembly, first, second, cancel=cancel._token,
                                        maxFacePairCandidates=maxFacePairCandidates)
    result = dict(mode="Several joints", scope="pair", sampling="sampled + refined",
                  gridSize=grid_size, joints=[j.Name for j in joints],
                  ranges=[(a, b) for a, b, _, _ in intervals],
                  units=[unit for _, _, _, unit in intervals],
                  interval=(0, grid_size-1), unit="grid pose", samples=[], unreachable=0,
                  unreachablePoses=[], posesUsed=0, poseBudget=poseBudget,
                  firstContact=None, firstClearanceViolation=None, firstGapViolation=None,
                  minimumClearance=None, sampledMinimumClearance=None, maximumGap=maximumGap,
                  clearance=requirement, completeness="incomplete", reason="Sweep did not finish",
                  coverage="not available", coverageReason="No multi-joint coverage proof",
                  boundConstant=None, provenLowerBound=None, unresolvedIntervals=[],
                  gapCoverage="not available", gapCoverageReason="No multi-joint coverage proof",
                  message="No finding at the sampled + refined poses")
    scan_pair = AssemblyApp.scanSelectedPair if _scan is None else _scan
    flags = (("firstContact", "contact"), ("firstClearanceViolation", "clearanceViolation"),
             ("firstGapViolation", "gapViolation"))

    def write(callback):
        return AssemblyApp._withMotionSweepMutation(assembly, callback)

    def run():
        doc = assembly.Document
        initial = {o.Name: App.Placement(o.Placement) for o in doc.Objects
                   if "Placement" in o.PropertiesList}
        entry = [_coordinate(j, unit) for j, (_, _, _, unit) in zip(joints, intervals)]
        held = [_coordinate(j, "mm" if unit == "deg" else "deg")
                if j.JointType == "Cylindrical" else None
                for j, (_, _, _, unit) in zip(joints, intervals)]
        angular_entry = {j.Name: _coordinate(j, "deg") for j in assembly.Joints
                         if not getattr(j, "Suppressed", False) and
                         (j.JointType in ("Revolute", "Cylindrical") or
                          getattr(j, "EnableAngleMin", False) or
                          getattr(j, "EnableAngleMax", False))}
        original_clearance = assembly.InterferenceClearance.Value
        grid = {}

        def check_cancel():
            QtCore.QCoreApplication.processEvents()
            if cancel.cancelled:
                raise _Cancelled()

        def solve(values):
            created = []
            try:
                for name, placement in initial.items():
                    obj = doc.getObject(name)
                    if obj.Placement.toMatrix().A != placement.toMatrix().A:
                        obj.Placement = placement
                count = max(1, *(math.ceil(abs(v-e)/30) if unit == "deg" else 1
                                  for v, e, (_, _, _, unit) in zip(values, entry, intervals)))
                motions = []
                simulations = []
                for j, e, v, h, (_, _, _, unit) in zip(joints, entry, values, held, intervals):
                    sim = _driver(doc, j, e, v, count, unit, created, heldCoordinate=h)
                    motions.extend(sim.Group)
                    simulations.append(sim)
                sim = simulations[0]
                sim.Group = motions
                sim.addProperty("App::PropertyBool", "RecordMotionCoordinates")
                sim.RecordMotionCoordinates = True
                code = assembly.generateSimulation(sim)
                if code != 0 or assembly.numberOfFrames() < 2:
                    raise RuntimeError(f"Native solver failed ({code})")
                frame = assembly.numberOfFrames()-1
                assembly.updateForFrame(frame)
                # Reuse simulation validation for signed solver coordinates,
                # placement agreement, dependent limits and missing drivers.
                validator = _SimulationTrajectory.__new__(_SimulationTrajectory)
                validator.assembly = assembly
                validator.motions = [(m.Joint[0], str(m.MotionType), m.Formula) for m in motions]
                offsets = {}
                for j in assembly.Joints:
                    if j.Name not in angular_entry:
                        continue
                    angular = any(j == m.Joint[0] and str(m.MotionType) == "Angular"
                                  for m in motions)
                    if not angular and not (getattr(j, "EnableAngleMin", False) or
                                            getattr(j, "EnableAngleMax", False)):
                        continue
                    # Frame 1 is the solved entry, before the prescribed movement.
                    # Fix the recorder's turn offset once; retain all subsequent turns.
                    recorded = AssemblyApp._motionSimulationJointState(assembly, j, 1, "", 0)
                    angle = math.degrees(recorded[0])
                    offsets[j.Name] = 360 * round((angular_entry[j.Name] - angle) / 360)
                coordinates = validator.coordinates(1.0, frame, offsets)
                for j, v, (_, _, _, unit) in zip(joints, values, intervals):
                    measured = coordinates[j.Name]
                    actual = measured.get("value", measured.get("coordinates", {}).get(unit))
                    if actual is None:
                        raise RuntimeError(f"Joint {j.Name} has no solved {unit} coordinate")
                    if abs(actual-v) > 1e-6:
                        raise _Unreachable(f"Joint {j.Name} did not reach {v:g} {unit}")
                return coordinates
            except _InfeasiblePose as error:
                raise _Unreachable(str(error)) from error
            finally:
                for name in reversed(created):
                    doc.removeObject(name)

        def order(record):
            # Fractional axis indices retain product traversal order in either direction.
            return tuple((v-a)/(b-a) if a != b else 0
                         for v, (a, b, _, _) in zip(record["pose"], intervals))

        def evaluate(values, index, phase, grid_index):
            check_cancel()
            if result["posesUsed"] >= poseBudget:
                raise RuntimeError("Pose budget exhausted during grid sweep")
            try:
                coordinates = write(lambda: solve(values))
            except _Unreachable as error:
                result["unreachable"] += 1
                result["unreachablePoses"].append(dict(pose=list(values), reason=str(error),
                                                       phase=phase, gridIndex=grid_index))
                check_cancel()
                return None
            check_cancel()
            result["posesUsed"] += 1
            scan = scan_pair(assembly, first, second, measureDistances=True,
                             maxFacePairCandidates=maxFacePairCandidates, cancel=cancel._token)
            if scan["cancelled"] or cancel.cancelled:
                raise _Cancelled()
            if not scan["complete"]:
                diagnostics = [i["diagnostic"] for i in scan["componentIssues"]]
                diagnostics += [p["faceEnumerationDiagnostic"] for p in scan["pairs"]]
                raise RuntimeError(
                    "Static scan incomplete: " + "; ".join(filter(None, diagnostics)))
            pairs = scan["pairs"]
            if not pairs or any(not p["distanceMeasured"] for p in pairs):
                raise RuntimeError("Static scan did not measure the whole selected pair")
            distance = min(p["solidDetection"]["minimumDistance"] for p in pairs)
            if not math.isfinite(distance) or distance < 0:
                raise RuntimeError("Static scan returned an invalid distance")
            active = [p for p in pairs if not p["excluded"]]
            contact = any(p["solidDetection"]["kind"] in ("Contact", "Penetration") or any(
                h["kind"] in ("Contact", "Penetration") and not h["suppressedByExclusion"]
                for h in p["faceHits"]) for p in active)
            violation = contact or any(p["detection"]["kind"] == "ClearanceViolation" or any(
                h["kind"] == "ClearanceViolation" and not h["suppressedByExclusion"]
                for h in p["faceHits"]) for p in active)
            record = dict(coordinate=index, index=index, phase=phase, pose=list(values),
                          gridIndex=grid_index, drivenCoordinates=coordinates,
                          contact=contact, clearanceViolation=violation,
                          gapViolation=maximumGap is not None and distance > maximumGap,
                          minimumClearance=distance, scan=scan,
                          placements={o.Name: App.Placement(o.Placement) for o in doc.Objects
                                      if "Placement" in o.PropertiesList})
            for key, flag in flags:
                if record[flag] and (result[key] is None or order(record) < order(result[key])):
                    result[key] = dict(record, sampledCoordinate=index, bracket=None,
                                       converged=False, refinementPoses=0, resolution=None,
                                       previousClearSample=None)
            minimum = result["minimumClearance"]
            if minimum is None or distance < minimum["value"]:
                result["minimumClearance"] = dict(
                    record, value=distance, bracket=None, converged=False,
                    refinementPoses=0, sampledCoordinate=index, sampledValue=distance)
            sample = {k: v for k, v in record.items() if k not in ("scan", "placements")}
            if phase == "sampling":
                result["samples"].append(sample)
                result["sampledMinimumClearance"] = dict(result["minimumClearance"])
            if progress:
                progress(sample)
            if cancel.cancelled:
                raise _Cancelled()
            return record

        def refine(key, flag):
            event = result[key]
            if event is None or not refinementPoseBudget:
                return
            neighbours = []
            # Preserve first-in-product-order events, including initial violations.
            # A later clear neighbour must not move an event past its first grid hit.
            for axis in reversed(range(len(joints))):
                neighbour = list(event["gridIndex"])
                neighbour[axis] -= 1
                clear = grid.get(tuple(neighbour))
                if clear is not None and not clear[flag]:
                    neighbours.append((axis, clear))
            if not neighbours:
                return
            axis, clear = neighbours[0]
            hit = dict(event)
            used = 0
            while abs(hit["pose"][axis]-clear["pose"][axis]) > tolerances[axis]:
                if used >= refinementPoseBudget:
                    break
                values = list(hit["pose"])
                values[axis] = (hit["pose"][axis]+clear["pose"][axis])/2
                probe = evaluate(values, event["index"], "refinement", event["gridIndex"])
                used += 1
                if probe is None:
                    break  # Unreachable is never a clear endpoint.
                if probe[flag]:
                    hit = probe
                else:
                    clear = probe
            event.update(hit)
            result[key] = event
            low, high = sorted((hit["pose"][axis], clear["pose"][axis]))
            event.update(refinementAxis=joints[axis].Name, refinementUnit=intervals[axis][3],
                         bracket=(low, high), resolution=high-low, refinementPoses=used,
                         converged=high-low <= tolerances[axis], previousClearSample={
                             k: v for k, v in clear.items() if k not in ("scan", "placements")})

        try:
            check_cancel()
            write(lambda: setattr(assembly, "InterferenceClearance", requirement))
            indices = itertools.product(*(range(len(axis)) for axis in axes))
            for index, grid_index in enumerate(indices):
                values = [axis[i] for axis, i in zip(axes, grid_index)]
                record = evaluate(values, index, "sampling", grid_index)
                # Neighbours need coordinates and flags, not owned BRep witnesses.
                grid[grid_index] = (None if record is None else {
                    k: v for k, v in record.items() if k not in ("scan", "placements")})
            refined = {}
            # Another event's probes can replace an already refined first event.
            while any(result[key] is not None and result[key] is not refined.get(key)
                      for key, _ in flags):
                for key, flag in flags:
                    if result[key] is not None and result[key] is not refined.get(key):
                        refine(key, flag)
                        refined[key] = result[key]
            check_cancel()
            result.update(completeness="complete", reason="")
        except _Cancelled:
            result.update(completeness="cancelled", reason="Cancelled during grid sweep")
        finally:
            write(lambda: setattr(assembly, "InterferenceClearance", original_clearance))

    try:
        AssemblyApp._withMotionSweepState(assembly, run)
    except Exception as error:
        result.update(completeness="incomplete", reason=str(error))
    if any(result[key] is not None for key, _ in flags):
        result["message"] = "Finding at the sampled + refined poses; no multi-joint coverage proof"
    return result
