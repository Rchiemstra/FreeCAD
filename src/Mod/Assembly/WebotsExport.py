# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export FreeCAD Assembly objects as self-contained Webots R2025a PROTO files."""

from dataclasses import dataclass, field
import json
import math
import os
import re
import tempfile

import FreeCAD as App

import UtilsAssembly

_SUPPORTED_JOINTS = {"Fixed", "Revolute", "Slider", "Ball"}
_DEFAULT_DENSITY = 1000.0  # kg/m^3
_DEFAULT_MATERIAL_UUID = "7f9fd73b-50c9-41d8-b7b2-575a030c1eeb"
_MM_TO_M = 0.001
_KG_MM2_TO_KG_M2 = 1.0e-6
_EPSILON = 1.0e-9


class WebotsExportError(ValueError):
    """Raised when an Assembly cannot be represented by the supported PROTO subset."""


class _UnionFind:
    def __init__(self, size):
        self.parent = list(range(size))

    def find(self, item):
        while self.parent[item] != item:
            self.parent[item] = self.parent[self.parent[item]]
            item = self.parent[item]
        return item

    def union(self, first, second):
        first = self.find(first)
        second = self.find(second)
        if first != second:
            self.parent[second] = first


class _NamePool:
    def __init__(self):
        self._identifiers = set()
        self._strings = set()

    def identifier(self, value, fallback):
        value = re.sub(r"[^A-Za-z0-9_]", "_", str(value))
        value = re.sub(r"_+", "_", value).strip("_") or fallback
        if not re.match(r"[A-Za-z_]", value[0]):
            value = "_" + value
        return self._unique(value, self._identifiers)

    def string(self, value, fallback):
        value = str(value).strip() or fallback
        return self._unique(value, self._strings)

    @staticmethod
    def _unique(value, used):
        candidate = value
        suffix = 2
        while candidate in used:
            candidate = f"{value}_{suffix}"
            suffix += 1
        used.add(candidate)
        return candidate


@dataclass
class _ShapeRecord:
    source: object
    material_objects: list
    shape: object


@dataclass
class _Geometry:
    label: str
    def_name: str
    points: list
    triangles: list
    color: tuple
    transparency: float


@dataclass
class _RigidLink:
    index: int
    members: list
    order: int
    frame: object
    grounded: bool = False
    solid_name: str = ""
    def_name: str = ""
    geometries: list = field(default_factory=list)
    mass: float = 0.0
    center_of_mass: object = None
    inertia: list = field(default_factory=list)


@dataclass
class _JointEdge:
    joint: object
    ref1_link: int
    ref2_link: int
    order: int


@dataclass
class _OrientedJoint:
    edge: _JointEdge
    parent: int
    child: int
    parent_export: int
    child_export: int
    reversed: bool
    def_name: str = ""
    sensor_names: list = field(default_factory=list)
    axis: object = None
    secondary_axes: list = field(default_factory=list)
    anchor: object = None
    position: float = 0.0
    limits: tuple | None = None
    endpoint: object = None


def export(objects, filename):
    """FreeCAD File -> Export bridge."""
    objects = list(objects)
    if len(objects) != 1:
        raise WebotsExportError(
            "Webots PROTO export requires exactly one selected Assembly::AssemblyObject"
        )
    assembly = objects[0]
    if not assembly.isDerivedFrom("Assembly::AssemblyObject"):
        raise WebotsExportError("Expected one Assembly::AssemblyObject")
    assembly.exportAsWebotsPROTO(filename)


def exportAssembly(assembly, filename):
    """Export one Assembly::AssemblyObject to ``filename``."""
    if assembly is None or not hasattr(assembly, "isDerivedFrom"):
        raise WebotsExportError("Expected one Assembly::AssemblyObject")
    if not assembly.isDerivedFrom("Assembly::AssemblyObject"):
        raise WebotsExportError("Expected one Assembly::AssemblyObject")
    if not isinstance(filename, str) or not filename:
        raise ValueError("Passed string is empty")

    exporter = _Exporter(assembly, filename)
    content = exporter.build()
    exporter.write(content)


class _Exporter:
    def __init__(self, assembly, filename):
        self.assembly = assembly
        self.filename = os.path.abspath(filename)
        self.names = _NamePool()
        self.warnings = []
        self.components = []
        self.component_index = {}
        self.links = {}
        self.base_index = -1
        self.oriented_joints = []
        self.children = {}

    def build(self):
        if self.assembly.Document is None:
            raise WebotsExportError("The assembly must belong to a document")
        self.assembly.Document.recompute()

        self.components = self._collect_components(self.assembly)
        if not self.components:
            raise WebotsExportError(
                f'Assembly "{self.assembly.Label}" contains no exportable components'
            )
        self.component_index = {
            self._object_key(component): index for index, component in enumerate(self.components)
        }

        joints = self._collect_joints(self.assembly)
        self._validate_joint_types(joints)
        self._build_graph(joints)
        self._prepare_links()
        self._prepare_joints()
        return self._serialize()

    def write(self, content):
        directory = os.path.dirname(self.filename) or os.curdir
        if not os.path.isdir(directory):
            raise WebotsExportError(f'Destination directory does not exist: "{directory}"')

        temporary_name = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w",
                encoding="utf-8",
                newline="\n",
                prefix=f".{os.path.basename(self.filename)}.",
                suffix=".tmp",
                dir=directory,
                delete=False,
            ) as temporary:
                temporary_name = temporary.name
                temporary.write(content)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_name, self.filename)
        except Exception:
            if temporary_name and os.path.exists(temporary_name):
                try:
                    os.unlink(temporary_name)
                except OSError:
                    pass
            raise

    def _collect_components(self, container):
        components = []
        seen = set()

        def append_component(obj):
            key = self._object_key(obj)
            if key not in seen:
                seen.add(key)
                components.append(obj)

        def visit_children(parent):
            if UtilsAssembly.isLinkGroup(parent):
                children = list(parent.ElementList)
            else:
                children = list(getattr(parent, "Group", []))

            for obj in children:
                if obj is None or obj.TypeId == "Assembly::JointGroup":
                    continue
                if obj.isDerivedFrom("Assembly::AssemblyLink"):
                    if bool(obj.Rigid):
                        append_component(obj)
                    else:
                        visit_children(obj)
                    continue
                if UtilsAssembly.isLinkGroup(obj):
                    visit_children(obj)
                    continue
                if obj.TypeId == "App::DocumentObjectGroup":
                    visit_children(obj)
                    continue
                if (
                    obj.isDerivedFrom("Part::Feature")
                    or obj.isDerivedFrom("App::Part")
                    or UtilsAssembly.isLink(obj)
                ):
                    append_component(obj)

        visit_children(container)
        return components

    def _collect_joints(self, container):
        joints = []

        def visit(parent):
            for obj in list(getattr(parent, "Group", [])):
                if obj is None:
                    continue
                if obj.TypeId == "Assembly::JointGroup":
                    for joint in list(getattr(obj, "Group", [])):
                        if joint is None or hasattr(joint, "ObjectToGround"):
                            continue
                        if not hasattr(joint, "JointType"):
                            continue
                        if bool(getattr(joint, "Suppressed", False)):
                            continue
                        joints.append(joint)
                elif obj.isDerivedFrom("Assembly::AssemblyLink") and not bool(obj.Rigid):
                    visit(obj)

        visit(container)
        return joints

    def _validate_joint_types(self, joints):
        unsupported = [
            f'"{joint.Label}" ({joint.JointType})'
            for joint in joints
            if joint.JointType not in _SUPPORTED_JOINTS
        ]
        if unsupported:
            raise WebotsExportError(
                "Unsupported Assembly joints for Webots PROTO export: " + ", ".join(unsupported)
            )

    def _build_graph(self, joints):
        component_union = _UnionFind(len(self.components))
        endpoints = {}
        incomplete = []

        for joint in joints:
            try:
                first = self._component_for_reference(joint.Reference1)
                second = self._component_for_reference(joint.Reference2)
            except WebotsExportError:
                incomplete.append(f'"{joint.Label}" ({joint.JointType})')
                continue
            if first == second:
                incomplete.append(f'"{joint.Label}" ({joint.JointType}, self-reference)')
                continue
            endpoints[self._object_key(joint)] = (first, second)
            if joint.JointType == "Fixed":
                component_union.union(first, second)

        if incomplete:
            raise WebotsExportError(
                "Incomplete or incoherent Assembly joints: " + ", ".join(incomplete)
            )

        members = {}
        for index, component in enumerate(self.components):
            members.setdefault(component_union.find(index), []).append(component)

        root_to_link = {}
        sorted_members = sorted(
            members.items(),
            key=lambda item: min(self.component_index[self._object_key(obj)] for obj in item[1]),
        )
        for link_index, (_, link_members) in enumerate(sorted_members):
            order = min(self.component_index[self._object_key(obj)] for obj in link_members)
            frame = self._global_placement(link_members[0])
            grounded = any(bool(self.assembly.isPartGrounded(obj)) for obj in link_members)
            self.links[link_index] = _RigidLink(
                index=link_index,
                members=link_members,
                order=order,
                frame=frame,
                grounded=grounded,
            )
            for obj in link_members:
                component_root = component_union.find(self.component_index[self._object_key(obj)])
                root_to_link[component_root] = link_index

        edges = []
        for order, joint in enumerate(joints):
            if joint.JointType == "Fixed":
                continue
            first, second = endpoints[self._object_key(joint)]
            first_link = root_to_link[component_union.find(first)]
            second_link = root_to_link[component_union.find(second)]
            if first_link == second_link:
                raise WebotsExportError(
                    f'Joint "{joint.Label}" ({joint.JointType}) closes a cycle inside one rigid link'
                )
            edges.append(_JointEdge(joint, first_link, second_link, order))

        graph_union = _UnionFind(len(self.links))
        for edge in edges:
            if graph_union.find(edge.ref1_link) == graph_union.find(edge.ref2_link):
                raise WebotsExportError(
                    f'Joint "{edge.joint.Label}" ({edge.joint.JointType}) creates a closed cycle'
                )
            graph_union.union(edge.ref1_link, edge.ref2_link)

        trees = {}
        for link_index in self.links:
            trees.setdefault(graph_union.find(link_index), []).append(link_index)

        roots = []
        sorted_trees = sorted(
            trees.values(), key=lambda value: min(self.links[index].order for index in value)
        )
        for tree_links in sorted_trees:
            grounded = [link_index for link_index in tree_links if self.links[link_index].grounded]
            if len(grounded) > 1:
                names = ", ".join(f'"{self.links[index].members[0].Label}"' for index in grounded)
                raise WebotsExportError(
                    f"Connected Assembly tree has multiple grounded rigid links: {names}"
                )
            roots.append(
                grounded[0]
                if grounded
                else min(tree_links, key=lambda index: self.links[index].order)
            )

        if len(roots) > 1:
            self._warn(
                "Disconnected Assembly trees were folded into the Robot base, creating implicit fixed relationships"
            )

        base_members = []
        for root in roots:
            base_members.extend(self.links[root].members)
        base_frame = self._global_placement(self.assembly)
        base = _RigidLink(
            index=self.base_index,
            members=base_members,
            order=-1,
            frame=base_frame,
            grounded=any(self.links[root].grounded for root in roots),
        )
        self.links[self.base_index] = base

        adjacency = {link_index: [] for link_index in self.links if link_index != self.base_index}
        for edge in edges:
            adjacency[edge.ref1_link].append((edge, edge.ref2_link))
            adjacency[edge.ref2_link].append((edge, edge.ref1_link))
        for neighbors in adjacency.values():
            neighbors.sort(key=lambda item: item[0].order)

        root_set = set(roots)
        for root in roots:
            stack = [(root, None)]
            while stack:
                current, parent = stack.pop()
                for edge, other in reversed(adjacency[current]):
                    if other == parent:
                        continue
                    parent_export = self.base_index if current in root_set else current
                    oriented = _OrientedJoint(
                        edge=edge,
                        parent=current,
                        child=other,
                        parent_export=parent_export,
                        child_export=other,
                        reversed=current != edge.ref1_link,
                    )
                    self.oriented_joints.append(oriented)
                    stack.append((other, current))

        for root in roots:
            if root != self.base_index:
                del self.links[root]

        self.oriented_joints.sort(key=lambda oriented: oriented.edge.order)
        self.children = {link_index: [] for link_index in self.links}
        for oriented in self.oriented_joints:
            self.children[oriented.parent_export].append(oriented)

    def _component_for_reference(self, reference):
        if reference is None or len(reference) != 2 or reference[0] is None:
            raise WebotsExportError("Incomplete joint reference")
        referenced = reference[0]
        key = self._object_key(referenced)
        if key in self.component_index:
            return self.component_index[key]

        owners = []
        for index, component in enumerate(self.components):
            if not hasattr(component, "hasObject"):
                continue
            try:
                if component.hasObject(referenced):
                    owners.append(index)
            except Exception:
                continue
        if len(owners) == 1:
            return owners[0]
        raise WebotsExportError("Joint reference does not resolve to an Assembly component")

    def _prepare_links(self):
        ordered = [self.links[self.base_index]] + sorted(
            (link for index, link in self.links.items() if index != self.base_index),
            key=lambda link: link.order,
        )
        for link in ordered:
            labels = [str(obj.Label) for obj in link.members]
            label = labels[0] if len(labels) == 1 else "_".join(labels)
            link.solid_name = self.names.string(label, "link")
            link.def_name = self.names.identifier(label + "_solid", "SOLID")
            self._prepare_link_geometry_and_physics(link)

    def _prepare_link_geometry_and_physics(self, link):
        records = []
        for member in link.members:
            records.extend(self._shape_records(member))

        if not records:
            labels = ", ".join(f'"{member.Label}"' for member in link.members)
            raise WebotsExportError(f"Rigid link has no visible shape geometry: {labels}")

        mass_items = []
        inverse_frame = link.frame.inverse()
        mesh_tolerance = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Mesh").GetFloat(
            "MaxDeviationExport", 0.1
        )
        if not math.isfinite(mesh_tolerance) or mesh_tolerance <= 0:
            mesh_tolerance = 0.1

        for record_index, record in enumerate(records):
            topology = record.shape.tessellate(mesh_tolerance)
            points = [inverse_frame.multVec(point) * _MM_TO_M for point in topology[0]]
            triangles = [tuple(int(index) for index in triangle) for triangle in topology[1]]
            if not points or not triangles:
                raise WebotsExportError(f'Shape "{record.source.Label}" produced an empty mesh')

            color, transparency = self._material_appearance(record.material_objects)
            geometry_label = f"{record.source.Label}_geometry"
            geometry_name = self.names.identifier(geometry_label, f"GEOMETRY_{record_index + 1}")
            link.geometries.append(
                _Geometry(
                    label=str(record.source.Label),
                    def_name=geometry_name,
                    points=points,
                    triangles=triangles,
                    color=color,
                    transparency=transparency,
                )
            )

            volume = float(record.shape.Volume)
            if volume <= _EPSILON:
                continue
            density = self._material_density(record.material_objects, record.source)
            mass = volume * density
            center = inverse_frame.multVec(record.shape.CenterOfMass)
            inertia_global = self._matrix_from_freecad(record.shape.MatrixOfInertia)
            inertia_local = self._rotate_tensor(inertia_global, inverse_frame.Rotation)
            inertia_local = self._matrix_scale(inertia_local, density)
            mass_items.append((mass, center, inertia_local))

        total_mass = sum(item[0] for item in mass_items)
        if total_mass <= _EPSILON:
            labels = ", ".join(f'"{member.Label}"' for member in link.members)
            raise WebotsExportError(f"Rigid link has no positive solid B-rep volume: {labels}")

        center = App.Vector()
        for mass, item_center, _ in mass_items:
            center += item_center * mass
        center /= total_mass

        inertia = self._zero_matrix()
        for mass, item_center, item_inertia in mass_items:
            inertia = self._matrix_add(inertia, item_inertia)
            offset = item_center - center
            distance_squared = offset.dot(offset)
            parallel = [
                [
                    mass * (distance_squared if row == column else 0.0)
                    - mass * offset[row] * offset[column]
                    for column in range(3)
                ]
                for row in range(3)
            ]
            inertia = self._matrix_add(inertia, parallel)

        link.mass = total_mass
        link.center_of_mass = center * _MM_TO_M
        link.inertia = self._matrix_scale(inertia, _KG_MM2_TO_KG_M2)

    def _shape_records(self, component):
        records = []
        stack = set()

        def visible(obj):
            if hasattr(obj, "Visibility"):
                return bool(obj.Visibility)
            try:
                return bool(obj.ViewObject.Visibility)
            except Exception:
                return True

        def walk(obj, transform=None, material_owners=None, honor_visibility=True):
            if obj is None or (honor_visibility and not visible(obj)):
                return
            if "Invalid" in getattr(obj, "State", []):
                raise WebotsExportError(f'Object "{obj.Label}" failed to recompute')
            key = self._object_key(obj)
            if key in stack:
                raise WebotsExportError(f'Cyclic object/link hierarchy at "{obj.Label}"')
            stack.add(key)
            try:
                if UtilsAssembly.isLink(obj):
                    linked = obj.getLinkedObject(True)
                    if linked is None:
                        raise WebotsExportError(f'Link "{obj.Label}" has no linked object')
                    occurrence = self._global_placement(obj)
                    if transform is not None:
                        occurrence = transform * occurrence
                    source = self._global_placement(linked)
                    owners = [obj]
                    owners.extend(owner for owner in material_owners or [] if owner is not obj)
                    walk(linked, occurrence * source.inverse(), owners, honor_visibility=False)
                    return

                if obj.isDerivedFrom("Part::Feature"):
                    shape = obj.Shape.copy()
                    if shape.isNull():
                        return
                    placement = self._global_placement(obj)
                    if transform is not None:
                        placement = transform * placement
                    shape.Placement = placement
                    material_objects = list(material_owners or [])
                    if obj not in material_objects:
                        material_objects.append(obj)
                    records.append(_ShapeRecord(obj, material_objects, shape))
                    return

                if UtilsAssembly.isLinkGroup(obj):
                    children = list(obj.ElementList)
                else:
                    children = list(getattr(obj, "Group", []))
                for child in children:
                    if child is None or child.TypeId == "Assembly::JointGroup":
                        continue
                    walk(child, transform, material_owners)
            finally:
                stack.remove(key)

        walk(component, material_owners=[component])
        return records

    def _material_density(self, objects, source):
        for obj in objects:
            material = getattr(obj, "ShapeMaterial", None)
            if material is None or not hasattr(material, "getPhysicalValue"):
                continue
            try:
                if str(getattr(material, "UUID", "")) == _DEFAULT_MATERIAL_UUID:
                    continue
                if hasattr(material, "hasPhysicalProperty") and not material.hasPhysicalProperty(
                    "Density"
                ):
                    continue
                density = material.getPhysicalValue("Density")
                value = float(density.Value)
            except Exception:
                continue
            if math.isfinite(value) and value > 0:
                # FreeCAD's density base unit is kg/mm^3.
                return value

        self._warn(
            f'Shape "{source.Label}" has no positive ShapeMaterial density; using 1000 kg/m^3'
        )
        return _DEFAULT_DENSITY * 1.0e-9

    @staticmethod
    def _material_appearance(objects):
        for obj in objects:
            material = getattr(obj, "ShapeMaterial", None)
            if material is None or not hasattr(material, "getAppearanceValue"):
                continue
            try:
                if str(getattr(material, "UUID", "")) == _DEFAULT_MATERIAL_UUID:
                    continue
                if hasattr(
                    material, "hasAppearanceProperty"
                ) and not material.hasAppearanceProperty("DiffuseColor"):
                    continue
                raw_color = material.getAppearanceValue("DiffuseColor")
                transparency = material.getAppearanceValue("Transparency")
                values = [float(value) for value in re.findall(r"[-+0-9.eE]+", raw_color)]
                if len(values) >= 3:
                    transparency = 0.0 if transparency is None else float(transparency)
                    return (
                        tuple(max(0.0, min(1.0, value)) for value in values[:3]),
                        max(0.0, min(1.0, transparency)),
                    )
            except Exception:
                pass

        if App.GuiUp:
            for obj in objects:
                try:
                    appearance = obj.ViewObject.ShapeAppearance[0]
                    color = tuple(float(value) for value in appearance.DiffuseColor[:3])
                    return color, max(0.0, min(1.0, float(appearance.Transparency)))
                except Exception:
                    continue
        return (0.7, 0.7, 0.7), 0.0

    def _prepare_joints(self):
        for oriented in self.oriented_joints:
            joint = oriented.edge.joint
            joint_type = joint.JointType
            oriented.def_name = self.names.identifier(joint.Label + "_joint", "JOINT")
            if joint_type == "Ball":
                oriented.sensor_names = [
                    self.names.string(f"{joint.Label}_sensor_{axis}", f"ball_sensor_{axis}")
                    for axis in (1, 2, 3)
                ]
            else:
                oriented.sensor_names = [
                    self.names.string(f"{joint.Label}_sensor", "position_sensor")
                ]

            parent_link = self.links[oriented.parent_export]
            child_link = self.links[oriented.child_export]
            parent_frame = parent_link.frame
            child_frame = child_link.frame
            parent_jcs = self._joint_global_placement(joint, oriented.reversed)
            oriented.axis = parent_frame.Rotation.inverted().multVec(
                parent_jcs.Rotation.multVec(App.Vector(0, 0, 1))
            )
            if oriented.axis.Length <= _EPSILON:
                raise WebotsExportError(f'Joint "{joint.Label}" has a zero-length JCS Z-axis')
            oriented.axis.normalize()
            if joint_type == "Ball":
                oriented.secondary_axes = [
                    parent_frame.Rotation.inverted().multVec(
                        parent_jcs.Rotation.multVec(local_axis)
                    )
                    for local_axis in (App.Vector(1, 0, 0), App.Vector(0, 1, 0))
                ]
                for axis in oriented.secondary_axes:
                    if axis.Length <= _EPSILON:
                        raise WebotsExportError(
                            f'Joint "{joint.Label}" has an invalid secondary JCS axis'
                        )
                    axis.normalize()
            oriented.anchor = parent_frame.inverse().multVec(parent_jcs.Base) * _MM_TO_M

            current_relative = parent_frame.inverse() * child_frame
            if joint_type == "Revolute":
                position = float(UtilsAssembly.getJointXYAngle(joint))
                if oriented.reversed:
                    position = -position
                oriented.position = position
                oriented.limits = self._joint_limits(joint, "angle", position, oriented.reversed)
                oriented.endpoint = self._rotate_placement_about_axis(
                    current_relative,
                    oriented.axis,
                    parent_frame.inverse().multVec(parent_jcs.Base),
                    -position,
                )
            elif joint_type == "Slider":
                position_mm = float(UtilsAssembly.getJointDistance(joint))
                if oriented.reversed:
                    position_mm = -position_mm
                oriented.position = position_mm * _MM_TO_M
                oriented.limits = self._joint_limits(
                    joint, "length", oriented.position, oriented.reversed
                )
                endpoint = App.Placement(current_relative)
                endpoint.Base = endpoint.Base - oriented.axis * position_mm
                oriented.endpoint = endpoint
            else:
                # Ball joints use the current endpoint transform as their zero pose.
                oriented.position = 0.0
                oriented.endpoint = current_relative

    def _joint_limits(self, joint, kind, position, reversed_axis):
        if kind == "angle":
            min_enabled = bool(joint.EnableAngleMin)
            max_enabled = bool(joint.EnableAngleMax)
            minimum = math.radians(float(joint.AngleMin.Value))
            maximum = math.radians(float(joint.AngleMax.Value))
        else:
            min_enabled = bool(joint.EnableLengthMin)
            max_enabled = bool(joint.EnableLengthMax)
            minimum = float(joint.LengthMin.Value) * _MM_TO_M
            maximum = float(joint.LengthMax.Value) * _MM_TO_M

        if min_enabled != max_enabled:
            raise WebotsExportError(
                f'Joint "{joint.Label}" has a one-sided {kind} limit; enable both bounds or neither'
            )
        if not min_enabled:
            return None
        if not math.isfinite(minimum) or not math.isfinite(maximum) or minimum >= maximum:
            raise WebotsExportError(f'Joint "{joint.Label}" has inverted or invalid {kind} limits')
        if kind == "angle" and (minimum < -math.pi or maximum > math.pi):
            raise WebotsExportError(
                f'Joint "{joint.Label}" angle limits exceed the Webots [-pi, pi] range'
            )
        if minimum > 0 or maximum < 0:
            raise WebotsExportError(
                f'Joint "{joint.Label}" {kind} limits do not bracket the exported zero pose'
            )
        if reversed_axis:
            minimum, maximum = -maximum, -minimum
        if position < minimum - _EPSILON or position > maximum + _EPSILON:
            raise WebotsExportError(
                f'Joint "{joint.Label}" current position is outside its enabled {kind} limits'
            )
        return minimum, maximum

    @staticmethod
    def _rotate_placement_about_axis(placement, axis, anchor, angle_radians):
        rotation = App.Rotation(axis, math.degrees(angle_radians))
        base = anchor + rotation.multVec(placement.Base - anchor)
        return App.Placement(base, rotation * placement.Rotation)

    def _joint_global_placement(self, joint, reversed_joint):
        if reversed_joint:
            return UtilsAssembly.getJcsGlobalPlc(joint.Placement2, joint.Reference2)
        return UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)

    def _serialize(self):
        proto_name = self.names.identifier(
            os.path.splitext(os.path.basename(self.filename))[0], "FreeCADAssembly"
        )
        assembly_frame = self.links[self.base_index].frame
        lines = ["#VRML_SIM R2025a utf8", "# template language: javascript"]
        lines.append("# Generated by FreeCAD Assembly Webots PROTO export.")
        for warning in self.warnings:
            lines.append(f"# WARNING: {warning}")
        lines.extend(["", f"PROTO {proto_name} ["])
        lines.extend(
            [
                f"  field SFVec3f translation {self._vector(assembly_frame.Base * _MM_TO_M)}",
                f"  field SFRotation rotation {self._rotation(assembly_frame.Rotation)}",
                f"  field SFString name {self._string(self.assembly.Label)}",
                '  field SFString controller "<generic>"',
                "  field MFString controllerArgs []",
                "  field SFBool supervisor FALSE",
                "  field SFBool synchronization TRUE",
                "  field SFBool selfCollision FALSE",
                "  field SFBool enableBoundingObject FALSE",
                "  field SFBool enablePhysics FALSE",
                "]",
                "{",
                f"  DEF {self.links[self.base_index].def_name} Robot {{",
                "    translation IS translation",
                "    rotation IS rotation",
                "    name IS name",
                "    controller IS controller",
                "    controllerArgs IS controllerArgs",
                "    supervisor IS supervisor",
                "    synchronization IS synchronization",
                "    selfCollision IS selfCollision",
            ]
        )
        self._emit_link_body(lines, self.links[self.base_index], 4, root=True)
        lines.extend(["  }", "}", ""])
        return "\n".join(lines)

    def _emit_link_body(self, lines, link, indent, root=False):
        prefix = " " * indent
        lines.append(prefix + "children [")
        for geometry in link.geometries:
            self._emit_geometry(lines, geometry, indent + 2)
        for oriented in self.children.get(link.index, []):
            self._emit_joint(lines, oriented, indent + 2)
        lines.append(prefix + "]")
        self._emit_bounding_object(lines, link, indent)
        if not (root and link.grounded):
            self._emit_physics(lines, link, indent)

    def _emit_geometry(self, lines, geometry, indent):
        prefix = " " * indent
        lines.extend(
            [
                prefix + "Shape {",
                prefix + "  appearance PBRAppearance {",
                prefix + f"    baseColor {self._triple(geometry.color)}",
                prefix + f"    transparency {self._number(geometry.transparency)}",
                prefix + "    roughness 0.6",
                prefix + "    metalness 0",
                prefix + "  }",
                prefix + f"  geometry DEF {geometry.def_name} IndexedFaceSet {{",
                prefix + "    coord Coordinate {",
                prefix + "      point [",
            ]
        )
        for point in geometry.points:
            lines.append(prefix + f"        {self._vector(point)},")
        lines.extend([prefix + "      ]", prefix + "    }", prefix + "    coordIndex ["])
        for triangle in geometry.triangles:
            lines.append(prefix + f"      {triangle[0]} {triangle[1]} {triangle[2]} -1,")
        lines.extend(
            [
                prefix + "    ]",
                prefix + "    creaseAngle 1",
                prefix + "  }",
                prefix + "}",
            ]
        )

    def _emit_bounding_object(self, lines, link, indent):
        prefix = " " * indent
        lines.append(
            prefix + "%< if (fields.enableBoundingObject.value || fields.enablePhysics.value) { >%"
        )
        lines.append(prefix + "boundingObject Group {")
        lines.append(prefix + "  children [")
        for geometry in link.geometries:
            lines.extend(
                [
                    prefix + "    Shape {",
                    prefix + f"      geometry USE {geometry.def_name}",
                    prefix + "    }",
                ]
            )
        lines.extend([prefix + "  ]", prefix + "}", prefix + "%< } >%"])

    def _emit_physics(self, lines, link, indent):
        prefix = " " * indent
        inertia = link.inertia
        lines.extend(
            [
                prefix + "%< if (fields.enablePhysics.value) { >%",
                prefix + "physics Physics {",
                prefix + "  density -1",
                prefix + f"  mass {self._number(link.mass)}",
                prefix + "  centerOfMass [",
                prefix + f"    {self._vector(link.center_of_mass)}",
                prefix + "  ]",
                prefix + "  inertiaMatrix [",
                prefix + "    " + self._triple((inertia[0][0], inertia[1][1], inertia[2][2])),
                prefix + "    " + self._triple((inertia[0][1], inertia[0][2], inertia[1][2])),
                prefix + "  ]",
                prefix + "}",
                prefix + "%< } >%",
            ]
        )

    def _emit_joint(self, lines, oriented, indent):
        joint = oriented.edge.joint
        joint_type = joint.JointType
        prefix = " " * indent
        node_type = {"Revolute": "HingeJoint", "Slider": "SliderJoint", "Ball": "BallJoint"}[
            joint_type
        ]
        parameter_type = {
            "Revolute": "HingeJointParameters",
            "Slider": "JointParameters",
            "Ball": "BallJointParameters",
        }[joint_type]
        lines.append(prefix + f"DEF {oriented.def_name} {node_type} {{")
        lines.append(prefix + f"  jointParameters {parameter_type} {{")
        if joint_type in {"Revolute", "Slider"}:
            lines.append(prefix + f"    axis {self._vector(oriented.axis)}")
        if joint_type in {"Revolute", "Slider"}:
            lines.append(prefix + f"    position {self._number(oriented.position)}")
        if joint_type in {"Revolute", "Ball"}:
            lines.append(prefix + f"    anchor {self._vector(oriented.anchor)}")
        if oriented.limits is not None:
            lines.append(prefix + f"    minStop {self._number(oriented.limits[0])}")
            lines.append(prefix + f"    maxStop {self._number(oriented.limits[1])}")
        lines.append(prefix + "  }")

        if joint_type == "Ball":
            for parameter_index, axis in enumerate(oriented.secondary_axes, start=2):
                lines.extend(
                    [
                        prefix + f"  jointParameters{parameter_index} JointParameters {{",
                        prefix + f"    axis {self._vector(axis)}",
                        prefix + "  }",
                    ]
                )

        device_fields = ["device", "device2", "device3"]
        for index, sensor_name in enumerate(oriented.sensor_names):
            lines.extend(
                [
                    prefix + f"  {device_fields[index]} [",
                    prefix + "    PositionSensor {",
                    prefix + f"      name {self._string(sensor_name)}",
                    prefix + "    }",
                    prefix + "  ]",
                ]
            )

        child = self.links[oriented.child_export]
        endpoint = oriented.endpoint
        lines.extend(
            [
                prefix + f"  endPoint DEF {child.def_name} Solid {{",
                prefix + f"    translation {self._vector(endpoint.Base * _MM_TO_M)}",
                prefix + f"    rotation {self._rotation(endpoint.Rotation)}",
                prefix + f"    name {self._string(child.solid_name)}",
            ]
        )
        self._emit_link_body(lines, child, indent + 4)
        lines.extend([prefix + "  }", prefix + "}"])

    def _warn(self, message):
        if message in self.warnings:
            return
        self.warnings.append(message)
        App.Console.PrintWarning("Webots PROTO export: " + message + "\n")

    @staticmethod
    def _global_placement(obj):
        if hasattr(obj, "getGlobalPlacement"):
            return obj.getGlobalPlacement()
        return obj.Placement

    @staticmethod
    def _object_key(obj):
        document = getattr(obj, "Document", None)
        document_name = getattr(document, "Name", "")
        return f"{document_name}:{getattr(obj, 'Name', id(obj))}"

    @staticmethod
    def _matrix_from_freecad(matrix):
        return [
            [matrix.A11, matrix.A12, matrix.A13],
            [matrix.A21, matrix.A22, matrix.A23],
            [matrix.A31, matrix.A32, matrix.A33],
        ]

    @staticmethod
    def _zero_matrix():
        return [[0.0, 0.0, 0.0] for _ in range(3)]

    @staticmethod
    def _matrix_add(first, second):
        return [
            [first[row][column] + second[row][column] for column in range(3)] for row in range(3)
        ]

    @staticmethod
    def _matrix_scale(matrix, scalar):
        return [[matrix[row][column] * scalar for column in range(3)] for row in range(3)]

    @classmethod
    def _rotate_tensor(cls, tensor, rotation):
        basis = [
            rotation.multVec(App.Vector(1, 0, 0)),
            rotation.multVec(App.Vector(0, 1, 0)),
            rotation.multVec(App.Vector(0, 0, 1)),
        ]
        transform = [[basis[column][row] for column in range(3)] for row in range(3)]
        intermediate = [
            [
                sum(transform[row][item] * tensor[item][column] for item in range(3))
                for column in range(3)
            ]
            for row in range(3)
        ]
        return [
            [
                sum(intermediate[row][item] * transform[column][item] for item in range(3))
                for column in range(3)
            ]
            for row in range(3)
        ]

    @staticmethod
    def _number(value):
        value = float(value)
        if not math.isfinite(value):
            raise WebotsExportError("Cannot serialize a non-finite numeric value")
        if abs(value) < 5.0e-14:
            value = 0.0
        return format(value, ".12g")

    @classmethod
    def _vector(cls, vector):
        return cls._triple((vector.x, vector.y, vector.z))

    @classmethod
    def _triple(cls, values):
        return " ".join(cls._number(value) for value in values)

    @classmethod
    def _rotation(cls, rotation):
        axis = rotation.Axis
        return f"{cls._vector(axis)} {cls._number(rotation.Angle)}"

    @staticmethod
    def _string(value):
        return json.dumps(str(value), ensure_ascii=False)
