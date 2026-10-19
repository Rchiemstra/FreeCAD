# SPDX-License-Identifier: LGPL-2.1-or-later
"""Associative Assembly component patterns.

Instance zero is the supplied link; Copies contains only the additional links.
A private native Part link array evaluates the same pattern extensions as PartDesign.
It is a hidden placement calculator outside the assembly, never a solver component.
Joint references are matched geometrically on every recompute, rather than assuming
that PartDesign assigns contiguous face numbers to successive instances.
"""

import re

import FreeCAD as App
import Part
import Assembly
import UtilsAssembly

_PATTERN_TYPES = {
    "PartDesign::PolarPattern": "Polar",
    "PartDesign::LinearPattern": "Linear",
}
_COMMON = ("Occurrences", "Mode", "Reversed", "Offset", "Spacings", "SpacingPattern")
_LINEAR = (
    "Direction",
    "Length",
    "Occurrences2",
    "Direction2",
    "Mode2",
    "Length2",
    "Reversed2",
    "Offset2",
    "Spacings2",
    "SpacingPattern2",
)
_JOINT_PROPERTIES = (
    "Distance",
    "Distance2",
    "Angle",
    "Offset1",
    "Offset2",
    "Detach1",
    "Detach2",
    "Placement1",
    "Placement2",
    "EnableLengthMin",
    "EnableLengthMax",
    "EnableAngleMin",
    "EnableAngleMax",
    "LengthMin",
    "LengthMax",
    "AngleMin",
    "AngleMax",
    "Suppressed",
)


_parameter_view_observer = None


class _HiddenParameterViews:
    def slotCreatedObject(self, view):
        if isinstance(
            getattr(getattr(view, "Object", None), "Proxy", None),
            PatternPlacementParameters,
        ):
            view.Visibility = False
            view.ShowInTree = False

    def slotDeletedObject(self, view):
        pass

    def slotDeletedDocument(self, document):
        pass


def _ensureParameterViews():
    global _parameter_view_observer
    if App.GuiUp and _parameter_view_observer is None:
        import FreeCADGui as Gui

        _parameter_view_observer = _HiddenParameterViews()
        Gui.addDocumentObserver(_parameter_view_observer)


def _global(obj):
    return UtilsAssembly.getGlobalPlacement((obj, [""]))


def _owner(assembly, source):
    body = source.getParentGeoFeatureGroup()
    candidates = [
        o
        for o in UtilsAssembly.getMovablePartsWithin(assembly)
        if UtilsAssembly.isLink(o)
        and (
            o.getLinkedObject() in (body, source)
            or source in o.getLinkedObject().OutListRecursive
        )
    ]
    if len(candidates) > 1:
        raise ValueError("Choose the assembly component containing the pattern source")
    return candidates[0] if candidates else None


def createComponentPattern(
    assembly,
    component,
    source=None,
    *,
    patternComponent=None,
    joints=(),
    count=6,
    angle=360,
    length=100,
    patternType="Polar",
    center=None,
    axis=None,
    direction=None,
    recompute=True,
):
    """Create a pattern. Explicit parameters use the assembly coordinate system.

    patternComponent identifies the flange instance when source belongs to a linked
    body. Joints is an explicit subset of the seed's ordinary (non-grounded) joints.
    The source component remains instance zero and is never duplicated or deleted.
    """
    if assembly is None or not assembly.isDerivedFrom("Assembly::AssemblyObject"):
        raise ValueError("An Assembly is required")
    if (
        component is None
        or not UtilsAssembly.isLink(component)
        or component not in UtilsAssembly.getMovablePartsWithin(assembly)
        or UtilsAssembly.isLinkArray(component)
    ):
        raise ValueError("Choose one component link in the assembly")
    if source is not None and source.TypeId not in _PATTERN_TYPES:
        raise ValueError("Choose a PartDesign PolarPattern or LinearPattern")
    if count < 1 or patternType not in ("Polar", "Linear"):
        raise ValueError("Invalid explicit pattern parameters")
    if source is not None and patternComponent is None:
        patternComponent = _owner(assembly, source)
    members = UtilsAssembly.getMovablePartsWithin(assembly)
    if patternComponent is not None and patternComponent not in members:
        raise ValueError("The pattern component must belong to this assembly")
    if source is not None and patternComponent is not None:
        linked = patternComponent.getLinkedObject()
        if linked != source and source not in linked.OutListRecursive:
            raise ValueError("The patterned component must contain the pattern source")
    group = UtilsAssembly.getJointGroup(assembly)
    for joint in joints:
        if (
            joint not in group.Group
            or not hasattr(joint, "Reference1")
            or component
            not in (
                UtilsAssembly.getMovingPart(joint.Reference1),
                UtilsAssembly.getMovingPart(joint.Reference2),
            )
        ):
            raise ValueError("Choose joints of the source component in this assembly")
    obj = assembly.newObject("App::FeaturePython", "ComponentPattern")
    ComponentPattern(obj)
    obj.Component = component
    obj.PatternSource = source
    obj.UseSource = source is not None
    obj.PatternComponent = patternComponent
    obj.Joints = list(joints)
    obj.PatternType = _PATTERN_TYPES[source.TypeId] if source else patternType
    obj.Count = count
    obj.Angle = angle
    obj.Length = length
    if center is not None:
        obj.Center = center
    if axis is not None:
        obj.Axis = axis
    if direction is not None:
        obj.Direction = direction
    if App.GuiUp:
        import JointObject

        view = JointObject.guiViewObject(obj)
        if view:
            ViewProviderComponentPattern(view)
    if recompute:
        assembly.Document.recompute()
    return obj


class ComponentPattern:
    def __init__(self, obj):
        obj.Proxy = self
        self._executing = False
        self._configuring = False
        for kind, name, description in (
            ("String", "Type", "Assembly object type"),
            ("Link", "Component", "Source component (instance zero)"),
            ("XLink", "PatternSource", "PartDesign polar or linear pattern"),
            (
                "Bool",
                "UseSource",
                "Follow a PartDesign pattern instead of explicit parameters",
            ),
            (
                "Link",
                "PatternComponent",
                "Assembly instance containing the pattern source",
            ),
            (
                "LinkList",
                "Joints",
                "Source joints to copy for each additional instance",
            ),
            ("Enumeration", "PatternType", "Pattern type when no source is supplied"),
            (
                "IntegerConstraint",
                "Count",
                "Total number of components, including the source",
            ),
            ("Angle", "Angle", "Total polar angle; 360 degrees closes the pattern"),
            ("Length", "Length", "Overall linear length"),
            ("Vector", "Center", "Polar center in assembly coordinates"),
            ("Vector", "Axis", "Polar axis in assembly coordinates"),
            ("Vector", "Direction", "Linear direction in assembly coordinates"),
            (
                "LinkList",
                "Copies",
                "Generated component links, excluding instance zero",
            ),
            ("LinkList", "JointCopies", "Generated joints"),
            ("XLink", "PlacementDriver", "Private native placement calculator"),
            ("XLink", "ParameterSource", "Ordered source parameter evaluator"),
            (
                "XLinkList",
                "ReferenceDrivers",
                "Local geometry for external pattern references",
            ),
        ):
            obj.addProperty(
                "App::Property" + kind, name, "Component Pattern", description
            )
        obj.Type = "ComponentPattern"
        obj.PatternType = ["Polar", "Linear"]
        obj.Count = (6, 1, 10000, 1)
        obj.Angle = 360
        obj.Length = 100
        obj.Axis = App.Vector(0, 0, 1)
        obj.Direction = App.Vector(1, 0, 0)
        obj.setEditorMode("Type", 2)
        for name in (
            "Copies",
            "JointCopies",
            "PlacementDriver",
            "ParameterSource",
            "ReferenceDrivers",
        ):
            obj.setEditorMode(
                name,
                (
                    2
                    if name
                    in ("PlacementDriver", "ParameterSource", "ReferenceDrivers")
                    else 1
                ),
            )
            obj.setPropertyStatus(name, ["NoRecompute", "Output"])

    def dumps(self):
        return None

    def loads(self, state):
        self._executing = False
        self._configuring = False

    def unsetupObject(self, obj):
        # Document-side deletion also handles scripts and transaction rollback.
        self._deleting = True
        children = list(obj.JointCopies) + list(obj.Copies) + list(obj.ReferenceDrivers)
        if obj.ParameterSource:
            children.insert(0, obj.ParameterSource)
        if obj.PlacementDriver:
            children.append(obj.PlacementDriver)
        for child in children:
            if obj.Document.getObject(child.Name) is not None:
                obj.Document.removeObject(child.Name)

    def onDocumentRestored(self, obj):
        self._executing = False
        self._configuring = False
        _ensureParameterViews()
        self._ensureViewProvider(obj)

    def _ensureViewProvider(self, obj):
        if App.GuiUp:
            import JointObject

            view = JointObject.guiViewObject(obj)
            if view is not None and view.Proxy is None:
                ViewProviderComponentPattern(view)

    def onChanged(self, obj, prop):
        if prop not in ("Component", "PatternSource", "UseSource"):
            return
        if (
            App.isRestoring()
            or getattr(self, "_configuring", False)
            or getattr(self, "_deleting", False)
            or not all(
                p in obj.PropertiesList
                for p in ("ParameterSource", "ReferenceDrivers", "UseSource")
            )
        ):
            return
        if not obj.UseSource or obj.Component is None or obj.PatternSource is None:
            return
        self._configuring = True
        try:
            _ensureParameterViews()
            source = obj.PatternSource
            kind = _PATTERN_TYPES.get(source.TypeId)
            if kind is None:
                raise ValueError("Unsupported pattern source")
            driver = obj.PlacementDriver
            if driver is not None and driver.TypeId != "Part::LinkArray" + kind:
                obj.Document.removeObject(driver.Name)
                driver = None
            if driver is None:
                driver = obj.Document.addObject(
                    "Part::LinkArray" + kind, obj.Name + "Placements"
                )
                driver.ShowElement = False
                obj.PlacementDriver = driver
            parameters = obj.ParameterSource
            if parameters is None:
                parameters = obj.Document.addObject(
                    "App::FeaturePython", obj.Name + "Parameters"
                )
                PatternPlacementParameters(parameters)
                obj.ParameterSource = parameters
            parameters.Owner = obj
            parameters.Source = source
            parameters.Component = obj.Component
            parameters.Driver = driver
            if "Parameters" not in driver.PropertiesList:
                driver.addProperty(
                    "App::PropertyLink", "Parameters", "Component Pattern"
                )
                driver.setEditorMode("Parameters", 2)
            driver.Parameters = parameters
            # The helper graph exists before recompute starts: source -> parameters
            # -> native array -> component pattern. No nested recompute is needed.
            if source.Document != obj.Document:
                for name in (
                    ("Axis",) if kind == "Polar" else ("Direction", "Direction2")
                ):
                    self._referenceProxy(obj, name)
            self._updateDriver(obj, driver)
            for helper in (parameters, driver):
                helper.Visibility = False
                self._hideHelper(helper)
        finally:
            self._configuring = False

    @staticmethod
    def _hideHelper(helper):
        import JointObject

        view = JointObject.guiViewObject(helper)
        if view:
            view.Visibility = False
            view.ShowInTree = False

    def _updateDriver(self, obj, driver):
        source = obj.PatternSource
        if source is None or obj.Component is None or driver is None:
            return
        values = {
            "LinkedObject": obj.Component.getLinkedObject(),
            "Placement": source.Placement,
        }
        names = _COMMON + (
            ("Axis", "Angle") if _PATTERN_TYPES[source.TypeId] == "Polar" else _LINEAR
        )
        for name in names:
            if name in source.PropertiesList and name in driver.PropertiesList:
                value = getattr(source, name)
                if (
                    name in ("Axis", "Direction", "Direction2")
                    and value
                    and value[0] is not None
                ):
                    value = self._localReference(obj, name, value)
                values[name] = value
        # Mode precedes dimensions, whose native setters synchronize spacing/extent.
        for name, value in values.items():
            if getattr(driver, name) != value:
                setattr(driver, name, value)

    def _referenceProxy(self, obj, name):
        proxies = list(obj.ReferenceDrivers)
        proxy = next((p for p in proxies if p.Label == obj.Name + ": " + name), None)
        if proxy is None:
            proxy = obj.Document.addObject("Part::Feature", obj.Name + name)
            proxy.Label = obj.Name + ": " + name
            proxy.Visibility = False
            proxies.append(proxy)
            obj.ReferenceDrivers = proxies
            self._hideHelper(proxy)
        return proxy

    def _placements(self, obj, assembly):
        source = obj.PatternSource if obj.UseSource else None
        if obj.UseSource and source is None:
            raise ValueError("The pattern source is missing")
        if source is None:
            count = obj.Count
            if count < 1:
                raise ValueError("At least one component is required")
            if obj.PatternType == "Linear":
                if count > 1 and obj.Length.Value <= 0:
                    raise ValueError("Linear length must be positive")
                direction = App.Vector(obj.Direction)
                if direction.Length < 1e-12:
                    raise ValueError("Linear direction must be nonzero")
                direction.normalize()
                return [
                    App.Placement(
                        direction * (obj.Length.Value * k / max(1, count - 1)),
                        App.Rotation(),
                    )
                    for k in range(count)
                ]
            if obj.Axis.Length < 1e-12:
                raise ValueError("Polar axis must be nonzero")
            if count > 1 and abs(obj.Angle.Value) < 1e-12:
                raise ValueError("Polar angle must be nonzero")
            step = obj.Angle.Value / max(
                1, count if abs(obj.Angle.Value - 360) < 1e-7 else count - 1
            )
            pivot = App.Placement(obj.Center, App.Rotation())
            return [
                pivot
                * App.Placement(App.Vector(), App.Rotation(obj.Axis, step * k))
                * pivot.inverse()
                for k in range(count)
            ]
        if source.TypeId not in _PATTERN_TYPES:
            raise ValueError("Unsupported pattern source")
        if "Invalid" in source.State:
            raise ValueError("The pattern source is invalid")
        driver = obj.PlacementDriver
        if driver is None or "Invalid" in driver.State:
            raise ValueError("Cannot evaluate source pattern placements")
        frame = source.getGlobalPlacement()
        if obj.PatternComponent:
            linked = obj.PatternComponent.getLinkedObject()
            frame = (
                _global(obj.PatternComponent)
                * linked.getGlobalPlacement().inverse()
                * frame
            )
        frame = _global(assembly).inverse() * frame
        placements = list(driver.PlacementList)
        if not placements:
            raise ValueError("The source pattern produced no instances")
        origin = placements[0].inverse()
        suppressed = set(getattr(source, "SuppressedIndices", []))
        if 0 in suppressed:
            raise ValueError("The source pattern's seed instance is suppressed")
        return [
            frame * p * origin * frame.inverse()
            for i, p in enumerate(placements)
            if i not in suppressed
        ]

    def _localReference(self, obj, name, reference):
        """Native Part pattern axis properties require same-document references.

        Copy only the reference geometry when the source lives in an imported
        document. PatternSource is an XLink and carries the live dependency.
        """
        ref, subs = reference
        if ref.Document == obj.Document:
            return reference
        proxy = self._referenceProxy(obj, name)
        proxy.Visibility = False
        sub = subs[0] if subs else ""
        axes = {"H_Axis": -1, "V_Axis": -2, "N_Axis": -3}
        if hasattr(ref, "getAxis") and (sub in axes or sub.startswith("Axis")):
            axis = ref.getAxis(axes[sub] if sub in axes else int(sub[4:]))
            base = ref.Placement.multVec(axis.Base)
            direction = ref.Placement.Rotation.multVec(axis.Direction)
            proxy.Shape = Part.makeLine(base, base + direction)
            subs = ["Edge1"]
        elif ref.TypeId in ("App::Line", "App::Plane"):
            base = App.Vector(ref.Placement.Base)
            rotation = ref.Placement.Rotation
            lcs = next(
                (p for p in ref.InList if p.TypeId == "App::LocalCoordinateSystem"),
                None,
            )
            if lcs:
                # Match App::DatumElement's getBasePoint()/getDirection().
                base += lcs.Placement.Base
                rotation = lcs.Placement.Rotation * rotation
            direction = rotation.multVec(
                App.Vector(1, 0, 0)
                if ref.TypeId == "App::Line"
                else App.Vector(0, 0, 1)
            )
            proxy.Shape = Part.makeLine(base, base + direction)
            subs = ["Edge1"]
        elif hasattr(ref, "Shape"):
            proxy.Shape = ref.Shape.copy()
            if ref.TypeId in ("PartDesign::Line", "PartDesign::Plane"):
                subs = ["Edge1" if ref.TypeId == "PartDesign::Line" else "Face1"]
        else:
            raise ValueError(
                "External pattern reference requires axes or feature geometry"
            )
        import JointObject

        view = JointObject.guiViewObject(proxy)
        if view:
            view.Visibility = False
            view.ShowInTree = False
        return (proxy, subs)

    def execute(self, obj):
        if getattr(self, "_executing", False):
            return
        if obj.Component is None:
            raise ValueError("The source component is missing")
        self._executing = True
        try:
            self._ensureViewProvider(obj)
            assembly = obj.getParentGeoFeatureGroup()
            if assembly is None or not assembly.isDerivedFrom(
                "Assembly::AssemblyObject"
            ):
                raise ValueError("Component pattern must be inside an Assembly")
            if not obj.Joints:
                # A seed joint can move instance zero when the hole's radius changes.
                # Seat it before evaluating copies even when joint copying is off.
                source_joints = UtilsAssembly.getJointGroup(assembly).Group
                if any(
                    hasattr(j, "Reference1")
                    and obj.Component
                    in (
                        UtilsAssembly.getMovingPart(j.Reference1),
                        UtilsAssembly.getMovingPart(j.Reference2),
                    )
                    for j in source_joints
                ):
                    result = assembly.solve()
                    if result != 0:
                        raise ValueError(
                            "Cannot solve the source component (" + str(result) + ")"
                        )
            transforms = self._placements(obj, assembly)
            # Resolve every reference before changing the generated objects. A missing
            # or ambiguous face is an error, never a silent attachment to another hole.
            refs = [
                [self._jointReferences(obj, joint, delta) for joint in obj.Joints]
                for delta in transforms[1:]
            ]
            copies = list(obj.Copies)
            joint_copies = list(obj.JointCopies)
            needed = len(transforms) - 1
            while len(joint_copies) > needed * len(obj.Joints):
                obj.Document.removeObject(joint_copies.pop().Name)
            while len(copies) > needed:
                obj.Document.removeObject(copies.pop().Name)
            for k, delta in enumerate(transforms[1:]):
                if k == len(copies):
                    copies.append(
                        assembly.newObject("App::Link", obj.Component.Name + "Pattern")
                    )
                copy = copies[k]
                copy.LinkedObject = obj.Component.LinkedObject
                copy.LinkTransform = obj.Component.LinkTransform
                copy.ScaleVector = obj.Component.ScaleVector
                copy.Label = obj.Component.Label + " (" + str(k + 2) + ")"
                copy.Placement = delta * obj.Component.Placement
                for j, seed in enumerate(obj.Joints):
                    index = k * len(obj.Joints) + j
                    ref1, ref2 = [
                        (copy if r[0] == obj.Component else r[0], r[1])
                        for r in refs[k][j]
                    ]
                    if index == len(joint_copies):
                        joint_copies.append(
                            Assembly.createJoint(
                                assembly,
                                seed.JointType,
                                ref1,
                                ref2,
                                solve=False,
                                presolve=False,
                                recompute=False,
                            )
                        )
                    joint = joint_copies[index]
                    # Clear references while setting joint parameters, to suppress
                    # interactive per-property solving in JointObject.onChanged.
                    joint.Reference1 = None
                    joint.Reference2 = None
                    joint.JointType = seed.JointType
                    for prop in _JOINT_PROPERTIES:
                        if prop in seed.PropertiesList:
                            setattr(joint, prop, getattr(seed, prop))
                    joint.Proxy.setJointConnectors(
                        joint, [ref1, ref2], solve=False, presolve=False
                    )
                    joint.Label = seed.Label + " (" + str(k + 2) + ")"
            obj.Copies = copies
            obj.JointCopies = joint_copies
            if obj.Joints:
                # The native Assembly solve stages and validates the whole assembly
                # before committing placements. No second solver lives here.
                result = assembly.solve()
                if result != 0:
                    raise ValueError(
                        "Assembly solve failed for component pattern ("
                        + str(result)
                        + ")"
                    )
        finally:
            self._executing = False

    def _jointReferences(self, obj, joint, delta):
        refs = []
        for ref in (joint.Reference1, joint.Reference2):
            if ref is None:
                raise ValueError("A selected joint has an incomplete reference")
            if ref[0] == obj.Component:
                refs.append((obj.Component, list(ref[1])))
            else:
                refs.append(
                    (ref[0], [_matchingElement(ref[0], sub, delta) for sub in ref[1]])
                )
        return refs


def _matchingElement(component, sub, delta):
    """Match a transformed face/edge/vertex in the same component's current shape."""
    match = re.search(r"(Face|Edge|Vertex)([0-9]+)$", sub)
    if not match:
        raise ValueError(
            "Joint counterpart must reference a patterned face, edge or vertex"
        )
    prefix = sub[: match.start()]
    selected = component.getSubObject(sub)
    if selected is None or selected.isNull():
        raise ValueError("Unresolved joint reference: " + sub)
    target = selected.copy()
    target.transformShape(delta.toMatrix())
    root = component.getSubObject(prefix) if prefix else component.Shape
    elements = getattr(
        root, {"Face": "Faces", "Edge": "Edges", "Vertex": "Vertexes"}[match[1]]
    )
    measure = {"Face": "Area", "Edge": "Length", "Vertex": None}[match[1]]
    center = target.CenterOfMass if measure else target.Point
    matches = []
    for i, candidate in enumerate(elements, 1):
        point = candidate.CenterOfMass if measure else candidate.Point
        if (point - center).Length > 1e-5:
            continue
        if (
            measure
            and abs(getattr(target, measure) - getattr(candidate, measure)) > 1e-5
        ):
            continue
        if candidate.distToShape(target)[0] > 1e-5:
            continue
        if measure:
            # Equal centers and areas also occur for intersecting, unrelated faces.
            # Require the complete transformed element to overlap its counterpart.
            overlap = candidate.common(target)
            if abs(getattr(overlap, measure) - getattr(target, measure)) > 1e-5:
                continue
        matches.append(prefix + match[1] + str(i))
    if len(matches) != 1:
        raise ValueError(
            "No unique patterned counterpart for " + component.Label + "." + sub
        )
    return matches[0]


class PatternPlacementParameters:
    """Update native array inputs before it executes in the document dependency graph."""

    def __init__(self, obj):
        obj.Proxy = self
        obj.addProperty("App::PropertyLinkHidden", "Owner", "Component Pattern")
        obj.addProperty("App::PropertyLinkHidden", "Driver", "Component Pattern")
        obj.addProperty("App::PropertyXLink", "Source", "Component Pattern")
        obj.addProperty("App::PropertyXLink", "Component", "Component Pattern")
        for name in ("Owner", "Driver", "Source", "Component"):
            obj.setEditorMode(name, 2)
        obj.Visibility = False

    def execute(self, obj):
        obj.Visibility = False
        if obj.Owner and obj.Driver and obj.Source and obj.Component:
            obj.Owner.Proxy._updateDriver(obj.Owner, obj.Driver)

    def dumps(self):
        return None

    def loads(self, state):
        return None


class ViewProviderComponentPattern:
    def __init__(self, view):
        self.Object = view.Object
        view.Proxy = self

    def getIcon(self):
        return ":/icons/Part_PolarLinkArray.svg"

    def claimChildren(self):
        children = (
            list(self.Object.Copies)
            + list(self.Object.JointCopies)
            + list(self.Object.ReferenceDrivers)
        )
        if self.Object.ParameterSource:
            children.append(self.Object.ParameterSource)
        if self.Object.PlacementDriver:
            children.append(self.Object.PlacementDriver)
        return children

    def attach(self, view):
        self.Object = view.Object

    def dumps(self):
        return None

    def loads(self, state):
        return None

    def onDelete(self, view, subelements):
        return True
