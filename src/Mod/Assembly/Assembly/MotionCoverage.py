# SPDX-License-Identifier: LGPL-2.1-or-later

"""Separation certificates for a single rigid joint motion, independent of scanning."""

import math

import FreeCAD as App


class RigidSweepCoverage:
    """Bound the minimum over all selected solid pairs using endpoint distances.

    Rigid translation has speed 1 mm/mm. Rotation has speed at most the largest
    radius of the moving solids' world bounding-box corners about the fixed axis.
    The minimum across leaf pairs is Lipschitz with the same constant. Angular
    coordinates remain unwrapped degrees in records; the bound uses radians.
    """

    # Match the sweep's solved-coordinate acceptance tolerance and leave a
    # conservative margin for exact-distance/kernel tolerances (in mm).
    margin = 1e-6

    def __init__(self, assembly, joint, unit, mechanism, start, end, budget, result):
        self.assembly, self.joint, self.unit = assembly, joint, unit
        self.mechanism, self.budget, self.result = mechanism, budget, result
        self.start, self.end = sorted((start, end))
        self.points = {}
        self.ready = False
        result.update(coverage="unresolved", coverageReason="Sweep has not finished",
                      unresolvedIntervals=[(self.start, self.end)], provenLowerBound=None,
                      boundConstant=None, boundUnit=None, coveragePoses=0, coverageBudget=budget)
        if result.get("maximumGap") is not None:
            result.update(gapCoverage="unresolved", gapCoverageReason="Sweep has not finished",
                          provenUpperBound=None, unresolvedGapIntervals=[(self.start, self.end)])
        if joint is None:
            self.unavailable("Prescribed simulation trajectory has no continuous motion bound")

    def unavailable(self, reason):
        if self.result.get("maximumGap") is not None:
            self.result.update(gapCoverage="not available", gapCoverageReason=reason,
                               unresolvedGapIntervals=[])
        self.result.update(coverage="not available", coverageReason=reason,
                           unresolvedIntervals=[], provenLowerBound=None)

    def observe(self, record):
        if self.result["coverage"] == "not available":
            return
        if not self.ready:
            moving, fixed_reference = self.mechanism
            leaves = record["scan"]["leaves"]
            sides = [leaf["occurrenceSubName"].split(".")[0] in moving for leaf in leaves]
            if any(sides[p["leafIndexA"]] == sides[p["leafIndexB"]]
                   for p in record["scan"]["pairs"]):
                self.unavailable("Selected solids must be on opposite sides of the swept joint")
                return
            constant = 1.0
            if self.unit == "deg":
                import UtilsAssembly
                placement = UtilsAssembly.getJcsGlobalPlc(
                    getattr(self.joint, "Placement" + str(fixed_reference)),
                    getattr(self.joint, "Reference" + str(fixed_reference)))
                origin = placement.Base
                axis = placement.Rotation.multVec(App.Vector(0, 0, 1))
                radii = []
                for leaf, is_moving in zip(leaves, sides):
                    if not is_moving:
                        continue
                    box = leaf["worldShape"].BoundBox
                    for x in (box.XMin, box.XMax):
                        for y in (box.YMin, box.YMax):
                            for z in (box.ZMin, box.ZMax):
                                offset = App.Vector(x, y, z) - origin
                                radii.append(offset.cross(axis).Length)
                constant = max(radii)
            self.result.update(boundConstant=constant,
                               boundUnit="mm/rad" if self.unit == "deg" else "mm/mm")
            self.ready = True
        self.points[record["coordinate"]] = max(0.0, record["minimumClearance"])

    def intervals(self):
        constant = self.result["boundConstant"]
        scale = math.pi / 180 if self.unit == "deg" else 1
        points = list(self.points.items())
        # Bound unevaluated endpoints from the nearest actual pose. These
        # estimates are never stored as observations, even for a one-pose sweep.
        for endpoint in (self.start, self.end):
            if endpoint not in self.points:
                coordinate, distance = min(self.points.items(), key=lambda p: abs(p[0] - endpoint))
                points.append((endpoint, distance - constant * abs(endpoint - coordinate) * scale))
        points = sorted((s, d) for s, d in points if self.start <= s <= self.end)
        return [(max(0.0, (d0 + d1 - constant * (s1 - s0) * scale) / 2 - self.margin),
                 s0, s1)
                for (s0, d0), (s1, d1) in zip(points, points[1:])]

    def refine(self, evaluate, notify):
        result = self.result
        if not self.ready or result["coverage"] == "not available":
            return
        threshold = result["clearance"] + self.margin
        reason = "Coverage budget exhausted" if self.budget else "Coverage disabled (coverageBudget=0)"
        for used in range(self.budget + 1):
            intervals = self.intervals()
            pending = [(bound, low, high) for bound, low, high in intervals if bound <= threshold]
            lower = min((bound for bound, _, _ in intervals),
                        default=max(0.0, min(self.points.values()) - self.margin))
            result.update(unresolvedIntervals=[(low, high) for _, low, high in pending],
                          provenLowerBound=lower)
            if result["firstClearanceViolation"] is not None:
                reason = "Event found; remaining intervals are unresolved"
                break
            if not pending and self.budget:
                result.update(coverage="proven", coverageReason="Every interval is above clearance")
                return
            if used == self.budget:
                break
            # Explore the weakest certificate first. Each accepted midpoint also
            # contributes to the existing event/minimum/witness controller.
            _, low, high = min(pending)
            middle = (low + high) / 2
            if middle == low or middle == high:
                reason = "Coordinate precision exhausted"
                break
            record = evaluate(middle)
            result["coveragePoses"] += 1
            notify(record)
        if not self.budget:
            result["unresolvedIntervals"] = [(self.start, self.end)]
        result.update(coverage="unresolved", coverageReason=reason)

    def upper_intervals(self):
        constant = self.result["boundConstant"]
        scale = math.pi / 180 if self.unit == "deg" else 1
        points = list(self.points.items())
        for endpoint in (self.start, self.end):
            if endpoint not in self.points:
                coordinate, distance = min(self.points.items(), key=lambda p: abs(p[0] - endpoint))
                points.append((endpoint, distance + constant * abs(endpoint - coordinate) * scale))
        points = sorted((s, d) for s, d in points if self.start <= s <= self.end)
        return [((d0 + d1 + constant * (s1 - s0) * scale) / 2 + self.margin, s0, s1)
                for (s0, d0), (s1, d1) in zip(points, points[1:])]

    def finish_gap(self):
        """Certify maximum separation using the existing observed poses only.

        Upper cones use the same rigid-motion Lipschitz constant as contact
        coverage. No extra solves are introduced: pair-mode event coordinates,
        witnesses and contact-coverage results retain their existing values.
        """
        result = self.result
        maximum = result.get("maximumGap")
        if maximum is None or not self.ready or result["gapCoverage"] == "not available":
            return
        intervals = self.upper_intervals()
        pending = [(low, high) for upper, low, high in intervals if upper >= maximum]
        result.update(provenUpperBound=max((upper for upper, _, _ in intervals),
                                          default=max(self.points.values()) + self.margin),
                      unresolvedGapIntervals=pending)
        if not self.budget:
            reason = "Coverage disabled (coverageBudget=0)"
            result["unresolvedGapIntervals"] = [(self.start, self.end)]
        elif result["firstGapViolation"] is not None:
            reason = "Gap violation found; remaining intervals are unresolved"
        elif pending:
            reason = "Upper separation bound reaches maximumGap; intervals remain unresolved"
        else:
            result.update(gapCoverage="proven",
                          gapCoverageReason="Every interval is below maximumGap")
            return
        result.update(gapCoverage="unresolved", gapCoverageReason=reason)
