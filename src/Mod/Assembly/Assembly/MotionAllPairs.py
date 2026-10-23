# SPDX-License-Identifier: LGPL-2.1-or-later

"""Bounded leaf-pair orchestration over the static checker's occurrence inventory."""

import itertools

import AssemblyApp


EVENTS = ("firstContact", "firstClearanceViolation", "firstGapViolation")


def _rigid_classes(assembly):
    import UtilsAssembly

    parent = {}

    def root(name):
        parent.setdefault(name, name)
        while parent[name] != name:
            name = parent[name]
        return name

    def connect(names):
        names = list(names)
        for name in names[1:]:
            parent[root(name)] = root(names[0])

    applied = list(assembly.Joints)
    group = next((o for o in assembly.Group if o.TypeId == "Assembly::JointGroup"), None)
    for joint in group.Group if group else ():
        if getattr(joint, "Suppressed", False):
            continue
        if hasattr(joint, "ObjectToGround") and joint.ObjectToGround:
            connect(["<ground>", joint.ObjectToGround.Name])
        elif hasattr(joint, "ObjectsToRigidGroup"):
            if "Invalid" not in joint.State:
                connect(o.Name for o in joint.ObjectsToRigidGroup
                        if "Invalid" not in o.State and assembly.hasObject(o)
                        and "Placement" in o.PropertiesList)
        elif joint in applied and getattr(joint, "JointType", "") == "Fixed":
            objects = [UtilsAssembly.getMovingPart(joint.Reference1),
                       UtilsAssembly.getMovingPart(joint.Reference2)]
            if all(objects):
                connect(o.Name for o in objects)
    return root


def _require_rigid_components(assembly):
    # A flexible occurrence's prefix does not certify its leaves' relative motion.
    pending = list(assembly.Group)
    seen = set()
    while pending:
        obj = pending.pop()
        if obj in seen:
            continue
        seen.add(obj)
        if obj.TypeId == "Assembly::AssemblyLink":
            if not obj.Rigid:
                raise ValueError(f"Flexible assembly {obj.Name} is not supported in all scope")
            continue
        if obj.isDerivedFrom("Assembly::AssemblyObject"):
            raise ValueError(f"Nested assembly {obj.Name} is not supported in all scope")
        pending.extend(getattr(obj, "Group", ()))
        linked = getattr(obj, "LinkedObject", None)
        if linked is not None:
            pending.append(linked)


def _selected_components(assembly, components):
    """Validate direct physical component occurrences, retaining link identity."""
    components = [] if components is None else list(components)
    names = set()
    for obj in components:
        if (obj not in assembly.Group or "Placement" not in obj.PropertiesList or
                not (hasattr(obj, "Shape") or obj.isDerivedFrom("App::Part") or
                     obj.isDerivedFrom("App::Link"))):
            raise ValueError("Selected components must belong directly to this assembly")
        names.add(obj.Name + ".")
    if len(names) < 2:
        raise ValueError("Select at least two distinct assembly components")
    return names


def sweepAll(assembly, sweep, *, mechanism=None, pairBudget=1000, poseBudget=10000,
             cancel=None, progress=None, includeHidden=False, components=None,
             scope="all", _scan=None, **options):
    """Sweep each eligible leaf pair with shared scan and pair-enumeration budgets.

    Hidden physical occurrences participate only with includeHidden=True.
    Selection scope includes only pairs crossing distinct selected components.
    A pair consumes its sampled/refined controller before the next pair starts.
    Each controller restores the same entry state. This costs extra
    solver calls but bounds memory and keeps cancellation between every scan.
    Simulation pairs are skipped only with a structural rigid-motion certificate;
    identical sampled transforms alone cannot certify a static relative path.
    """
    from .MotionInterference import SweepCancellation

    for name, value in (("pairBudget", pairBudget), ("poseBudget", poseBudget)):
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise ValueError(name + " must be a nonnegative integer")
    cancel = SweepCancellation() if cancel is None else cancel
    if not isinstance(cancel, SweepCancellation):
        raise TypeError("cancel must be a SweepCancellation")
    selected = _selected_components(assembly, components) if scope == "selection" else None
    result = dict(scope=scope, includeHidden=includeHidden, pairResults=[], events=[],
                  firstEvent=None, staticPairs=0,
                  pairsVisited=0, posesUsed=0, pairBudget=pairBudget, poseBudget=poseBudget,
                  completeness="incomplete", reason="Sweep did not finish")
    result.update({key: None for key in EVENTS})
    scan_pair = AssemblyApp.scanSelectedPair if _scan is None else _scan
    visibility = "Hidden leaves included" if includeHidden else "Hidden leaves excluded"
    result["message"] = "0 pairs static: use the static check. " + visibility
    try:
        _require_rigid_components(assembly)
        root = _rigid_classes(assembly)
        if cancel.cancelled:
            result.update(completeness="cancelled", reason="Cancelled before leaf collection")
            return result
        inventory = AssemblyApp._motionInterferenceLeaves(assembly, includeHidden)
        leaves = [leaf for leaf in inventory["leaves"]
                  if selected is None or leaf["component"] in selected]
        exclusions = {frozenset((r["first"], r["second"])): r["rule"]
                      for r in inventory["exclusions"]}
        for a, b in itertools.combinations(leaves, 2):
            if selected is not None and a["component"] == b["component"]:
                continue
            if cancel.cancelled:
                result.update(completeness="cancelled", reason="Cancelled between pairs")
                break
            if result["pairsVisited"] >= pairBudget:
                result["reason"] = "Pair budget exhausted during leaf-pair enumeration"
                break
            result["pairsVisited"] += 1
            names = [leaf["rigidComponent"] for leaf in (a, b)]
            static = a["component"] == b["component"]
            if mechanism is not None:
                static |= (names[0] in mechanism[0]) == (names[1] in mechanism[0])
            else:
                static |= root(names[0]) == root(names[1])
            if static:
                result["staticPairs"] += 1
                continue
            pair = (a["path"], b["path"])
            if result["posesUsed"] >= poseBudget:
                result["reason"] = "Pose budget exhausted before leaf-pair sweep"
                break

            def scan(*args, **kwargs):
                if cancel.cancelled:
                    raise RuntimeError("Cancelled before pair scan")
                if result["posesUsed"] >= poseBudget:
                    raise RuntimeError("Pose budget exhausted during leaf-pair sweep")
                result["posesUsed"] += 1
                return scan_pair(*args, **kwargs, leafA=a["path"], leafB=b["path"])

            def notify(sample):
                if progress:
                    progress(dict(sample, pair=pair, posesUsed=result["posesUsed"]))

            child = sweep((assembly, a["component"]), (assembly, b["component"]),
                          cancel=cancel, progress=notify, _scan=scan, **options)
            if cancel.cancelled:
                child.update(completeness="cancelled", reason="Cancelled during leaf-pair sweep")
            child.update(pair=pair, exclusionRule=exclusions.get(
                frozenset((a["sourceId"], b["sourceId"]))))
            result["pairResults"].append(child)
            direction = 1 if child["interval"][1] >= child["interval"][0] else -1
            for key in EVENTS:
                event = child[key]
                if event is None:
                    continue
                rules = [dict(kind=h["ruleKind"], rows=h["sourceRows"],
                              comments=h["sourceComments"])
                         for p in event["scan"]["pairs"] for h in p["faceHits"]
                         if h["kind"] in ("Contact", "Penetration", "ClearanceViolation")
                         and not h["suppressedByExclusion"]]
                event.update(pair=pair, eventType=key, rules=rules)
                result["events"].append(event)
                previous = result[key]
                if previous is None or direction * event["coordinate"] < (
                        direction * previous["coordinate"]):
                    result[key] = event
            if child["completeness"] != "complete":
                result.update(completeness=child["completeness"], reason=child["reason"])
                break
        else:
            result.update(completeness="complete", reason="")
        if result["pairResults"]:
            direction = 1 if result["pairResults"][0]["interval"][1] >= (
                result["pairResults"][0]["interval"][0]) else -1
            result["events"].sort(key=lambda e: (direction * e["coordinate"], e["pair"]))
        result["firstEvent"] = next(iter(result["events"]), None)
    except Exception as error:
        result.update(completeness="incomplete", reason=str(error))
    result["message"] = (f'{result["staticPairs"]} pairs static: use the static check. '
                         + visibility)
    return result
