# SPDX-License-Identifier: LGPL-2.1-or-later
"""Saved model comparison, independent of the GUI.

compare_files() owns and closes its input snapshots. Results contain detached
geometry and ordinary Python values; show_result() never needs the source files.
No source model is recomputed or saved.
"""

import hashlib
import os
import shutil
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass, field

import FreeCAD as App

translate = App.Qt.translate


@dataclass
class ObjectChange:
    name: str
    label: str
    kind: str
    properties: dict = field(default_factory=dict)
    # (old volume, new volume, old area, new area), in mm^3 and mm^2.
    measures: tuple | None = None


@dataclass
class Visual:
    name: str
    label: str
    group: str
    role: str
    geometry: object
    mesh: bool = False


@dataclass
class DiffResult:
    local: str
    remote: str
    properties: dict = field(default_factory=dict)
    objects: list = field(default_factory=list)
    visuals: list = field(default_factory=list)
    warnings: list = field(default_factory=list)

    @property
    def identical(self):
        return not self.properties and not self.objects

    def report(self):
        lines = [
            translate("ModelDiff", "Model diff: {0} -> {1}").format(
                self.local, self.remote
            )
        ]
        if self.identical:
            lines.append(translate("ModelDiff", "No differences."))
        if self.properties:
            lines.append(translate("ModelDiff", "Document properties:"))
            lines.extend(_property_lines(self.properties))
        for change in self.objects:
            kind = {
                "Added": translate("ModelDiff", "Added"),
                "Removed": translate("ModelDiff", "Removed"),
                "Changed": translate("ModelDiff", "Changed"),
            }[change.kind]
            lines.append(f"{kind}: {change.name} ({change.label})")
            lines.extend(_property_lines(change.properties))
            if change.measures is not None:
                v0, v1, a0, a1 = change.measures
                lines.append(
                    translate(
                        "ModelDiff",
                        "  Shape volume (mm^3): {0:g} -> {1:g} (change {2:+g})",
                    ).format(v0, v1, v1 - v0)
                )
                lines.append(
                    translate(
                        "ModelDiff",
                        "  Shape area (mm^2): {0:g} -> {1:g} (change {2:+g})",
                    ).format(a0, a1, a1 - a0)
                )
        lines.extend(
            translate("ModelDiff", "Warning: {0}").format(warning)
            for warning in self.warnings
        )
        return "\n".join(lines) + "\n"


def _property_lines(properties):
    absent = translate("ModelDiff", "<absent>")
    return [
        f"  {name}: {absent if old is None else old} -> {absent if new is None else new}"
        for name, (old, new) in sorted(properties.items())
    ]


def _scalar_value(tag, value):
    try:
        if tag == "Integer":
            return str(int(value))
        if tag == "Float":
            number = float(value)
            return (
                str(int(number))
                if number.is_integer() and abs(number) < 1e16
                else repr(number)
            )
    except ValueError:
        pass
    return value


def _value(node, archive):
    """Canonical saved values, including binary-backed properties.

    IDs, document names assigned on restore, Python wrapper addresses, and GUI
    runtime state are deliberately never used as object identity.
    """
    attributes = []
    for key, value in sorted(node.attrib.items()):
        if key == "file" and value in archive.NameToInfo:
            value = hashlib.sha256(archive.read(value)).hexdigest()
        elif key == "value":
            value = _scalar_value(node.tag, value)
        attributes.append((key, value))
    return (
        node.tag,
        tuple(attributes),
        (node.text or "").strip(),
        tuple(_value(child, archive) for child in node),
    )


def _display(node, archive):
    if len(node) == 1 and not len(node[0]) and "value" in node[0].attrib:
        return _scalar_value(node[0].tag, node[0].get("value"))
    files = [
        child.get("file")
        for child in node.iter()
        if child.get("file") in archive.NameToInfo
    ]
    if files:
        return "; ".join(
            f"{name} (sha256 {hashlib.sha256(archive.read(name)).hexdigest()})"
            for name in files
        )
    return " ".join(ET.tostring(child, encoding="unicode").strip() for child in node)


def _properties(parent, archive, *, document=False):
    result = {}
    if parent is None:
        return result
    for prop in parent.findall("Property"):
        name = prop.get("name")
        # Document paths describe the checkout. Object properties with these
        # names can be meaningful model parameters and must still be compared.
        if document and name in {"FileName", "TransientDir"}:
            continue
        cells = prop.find("Cells")
        if cells is not None:
            for cell in cells.findall("Cell"):
                address = cell.get("address")
                result[name + "." + address] = (
                    _value(cell, archive),
                    cell.get("content", "")
                    + (
                        " "
                        + str(
                            sorted(
                                (k, v)
                                for k, v in cell.attrib.items()
                                if k not in {"address", "content"}
                            )
                        )
                        if len(cell.attrib) > 2
                        else ""
                    ),
                )
        else:
            canonical = (
                prop.get("type"),
                tuple(_value(child, archive) for child in prop),
            )
            result[name] = (canonical, _display(prop, archive))
    return result


def _read_archive(path):
    with zipfile.ZipFile(path) as archive:
        root = ET.fromstring(archive.read("Document.xml"))
        properties = _properties(root.find("Properties"), archive, document=True)
        objects = {
            obj.get("name"): {"TypeId": ((obj.get("type"),), obj.get("type"))}
            for obj in root.findall("./Objects/Object")
        }
        for obj in root.findall("./ObjectData/Object"):
            objects.setdefault(obj.get("name"), {}).update(
                _properties(obj.find("Properties"), archive)
            )
        visibility = {}
        if "GuiDocument.xml" in archive.namelist():
            gui = ET.fromstring(archive.read("GuiDocument.xml"))
            for view in gui.findall(".//ViewProvider"):
                visible = view.find("./Properties/Property[@name='Visibility']/Bool")
                if visible is not None:
                    name = view.get("name")
                    visibility[name] = visible.get("value") == "true"
                    if name in objects:
                        objects[name]["View.Visibility"] = (
                            visibility[name],
                            visible.get("value"),
                        )
        return properties, objects, visibility


def _changes(left, right):
    return {
        key: (
            left[key][1] if key in left else None,
            right[key][1] if key in right else None,
        )
        for key in sorted(left.keys() | right.keys())
        if left.get(key) != right.get(key)
    }


class _SnapshotFiles:
    """A private file namespace for the native dependency loader.

    Preserve relative paths and copy archives unchanged in the usual case.
    Repack only when an absolute reference or symlink needs redirecting. Native
    restore can then resolve links without attaching to an open, edited file.
    """

    def __init__(self):
        self.directory = tempfile.TemporaryDirectory(prefix="freecad-modeldiff-inputs-")
        self.root = os.path.realpath(self.directory.name)
        self.existing = set(App.listDocuments())
        self.active = App.ActiveDocument.Name if App.ActiveDocument else ""
        self.paths = {}
        self.missing = set()

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        try:
            owned = []
            for name, doc in App.listDocuments().items():
                if name in self.existing or not doc.FileName:
                    continue
                filename = os.path.realpath(doc.FileName)
                if os.path.commonpath((self.root, filename)) == self.root:
                    owned.append(name)
            App.setActiveDocument(
                self.active if self.active in App.listDocuments() else ""
            )
            for name in reversed(owned):
                if name in App.listDocuments():
                    App.closeDocument(name)
        finally:
            self.directory.cleanup()

    def prepare(self, path):
        original = os.path.realpath(path)
        if original in self.paths:
            return self.paths[original]
        drive, tail = os.path.splitdrive(original)
        destination = os.path.join(
            self.root,
            "files",
            hashlib.sha256(drive.encode()).hexdigest()[:8],
            tail.lstrip(os.sep + (os.altsep or "")),
        )
        # Register before following links: dependency graphs can contain cycles.
        self.paths[original] = destination
        if not os.path.isfile(original):
            self.missing.add(original)
            return destination
        with zipfile.ZipFile(original) as archive:
            root = ET.fromstring(archive.read("Document.xml"))
            rewrite = False
            for node in root.iter():
                reference = node.get("file")
                if not node.tag.startswith("XLink") or not reference:
                    continue
                if "://" in reference:
                    target = os.path.join(
                        self.root,
                        "unavailable",
                        hashlib.sha256(reference.encode()).hexdigest() + ".FCStd",
                    )
                    self.missing.add(reference)
                else:
                    target = self.prepare(
                        os.path.join(os.path.dirname(original), reference)
                    )
                resolved = os.path.normpath(
                    os.path.join(os.path.dirname(destination), reference)
                )
                if resolved != target or "://" in reference:
                    node.set(
                        "file", os.path.relpath(target, os.path.dirname(destination))
                    )
                    rewrite = True
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            if rewrite:
                xml = ET.tostring(root, encoding="utf-8", xml_declaration=True)
                with zipfile.ZipFile(destination, "w") as output:
                    for member in archive.infolist():
                        output.writestr(
                            member,
                            (
                                xml
                                if member.filename == "Document.xml"
                                else archive.read(member)
                            ),
                        )
            else:
                shutil.copyfile(original, destination)
        # Link timestamps must describe the saved files, not the snapshot copy.
        shutil.copystat(original, destination)
        return destination


def _restore(path):
    return App.openDocument(path, hidden=True)


def _visible_objects(doc, visibility):
    visible = {
        obj.Name
        for obj in doc.Objects
        if visibility.get(obj.Name, getattr(obj, "Visibility", True))
    }
    # A Body already exposes its tip as Shape. Never diff its feature history.
    for obj in doc.Objects:
        # Origin axes and planes are construction aids, not model results.
        if obj.TypeId in {"App::Origin", "App::Line", "App::Plane", "App::Point"}:
            visible.discard(obj.Name)
        if obj.isDerivedFrom("PartDesign::Body"):
            visible.difference_update(child.Name for child in obj.Group)
        if obj.isDerivedFrom("App::Part") and obj.Name not in visible:
            pending = list(obj.Group)
            while pending:
                child = pending.pop()
                visible.discard(child.Name)
                # A Link's target can also be visible outside this container.
                if child.getLinkedObject(False) == child:
                    pending.extend(getattr(child, "Group", []))
    # Archives saved without a GUI have no view-provider visibility. Infer the
    # inputs hidden by the standard Boolean features in that case.
    if not visibility:
        for obj in doc.Objects:
            if obj.TypeId in {
                "Part::Cut",
                "Part::Fuse",
                "Part::Common",
                "Part::MultiFuse",
            }:
                for prop in ("Base", "Tool", "Shapes"):
                    value = getattr(obj, prop, None)
                    for child in value if isinstance(value, list) else [value]:
                        if child is not None:
                            visible.discard(child.Name)
    return visible


def _geometry(obj):
    if obj is None or obj.isDerivedFrom("App::Part"):
        return None
    mesh = getattr(obj, "Mesh", None)
    if mesh is not None:
        geometry = mesh.copy()
        is_mesh = True
    else:
        import Part

        geometry = Part.getShape(obj)
        if geometry.isNull():
            return None
        # Analysis results do not need persistent topological element naming.
        geometry = geometry.copy(noElementMap=True)
        is_mesh = False
    if hasattr(obj, "getGlobalPlacement") and hasattr(obj, "Placement"):
        parent = obj.getGlobalPlacement().multiply(obj.Placement.inverse())
        geometry.Placement = parent.multiply(geometry.Placement)
    return geometry, is_mesh


def _solid_parts(old, new):
    """Kept separate so tests can exercise a kernel failure deterministically."""
    pieces = (new.cut(old), old.cut(new), old.common(new))
    if any(not part.isNull() and not part.isValid() for part in pieces):
        raise ValueError(translate("ModelDiff", "Boolean produced an invalid shape"))
    return pieces


def _visuals(result, name, label, old, new, kind):
    def add(geometry, role, mesh=False):
        if geometry is not None and (
            mesh or (not geometry.isNull() and geometry.countElement("Vertex"))
        ):
            result.visuals.append(Visual(name, label, kind, role, geometry, mesh))

    if old is None and new is None:
        return
    if old is None:
        add(new[0], "Added", new[1])
    elif new is None:
        add(old[0], "Removed", old[1])
    elif (
        not old[1]
        and not new[1]
        and old[0].countElement("Solid")
        and new[0].countElement("Solid")
    ):
        try:
            added, removed, common = _solid_parts(old[0], new[0])
        except Exception as exc:  # noqa: BLE001
            # Kernel failures of any Python exception type must use overlays.
            result.warnings.append(
                translate(
                    "ModelDiff", "{0}: Boolean failed; using two-colour overlay ({1})"
                ).format(name, exc)
            )
            add(old[0], "Removed")
            add(new[0], "Added")
        else:
            add(added, "Added")
            add(removed, "Removed")
            add(common, "Unchanged")
    else:
        add(old[0], "Removed", old[1])
        add(new[0], "Added", new[1])


def compare_files(local, remote):
    """Compare two saved FCStd files and return a detached DiffResult.

    Objects pair by internal name. All saved properties are compared, while
    Boolean operations are limited to displayed results. Units are mm, mm², mm³.
    Missing external files do not prevent reading cached properties/geometry.
    """
    local, remote = os.path.abspath(os.fspath(local)), os.path.abspath(
        os.fspath(remote)
    )
    lp, lo, lv = _read_archive(local)
    rp, ro, rv = _read_archive(remote)
    result = DiffResult(local, remote, _changes(lp, rp))
    for name in sorted(lo.keys() | ro.keys()):
        kind = "Added" if name not in lo else "Removed" if name not in ro else "Changed"
        properties = _changes(lo.get(name, {}), ro.get(name, {}))
        if properties:
            label = (ro.get(name, lo.get(name))).get("Label", (None, name))[1]
            result.objects.append(ObjectChange(name, label, kind, properties))
    with _SnapshotFiles() as snapshots:
        paths = [snapshots.prepare(path) for path in (local, remote)]
        for path in sorted(snapshots.missing):
            result.warnings.append(
                translate(
                    "ModelDiff",
                    "Missing external link: {0}; using saved properties and cached geometry",
                ).format(path)
            )
        roots = {}
        for path in paths:
            if path not in roots:
                roots[path] = _restore(path)
        left, right = (roots[path] for path in paths)
        visible_left, visible_right = _visible_objects(left, lv), _visible_objects(
            right, rv
        )
        by_name = {change.name: change for change in result.objects}
        for name in sorted(lo.keys() | ro.keys()):
            change = by_name.get(name)
            if change is None and name not in visible_left | visible_right:
                continue
            before, after = left.getObject(name), right.getObject(name)
            old, new = _geometry(before), _geometry(after)
            same_geometry = False
            # Geometry inherited through Links or container placements can change
            # even when the object's own stored properties did not change.
            if old is not None and new is not None:
                if old[1] != new[1]:
                    geometry_changed = True
                elif old[1]:
                    geometry_changed = old[0].Topology != new[0].Topology
                else:
                    geometry_changed = (
                        old[0].exportBrepToString() != new[0].exportBrepToString()
                    )
                same_geometry = not geometry_changed
                if geometry_changed and change is None:
                    change = ObjectChange(
                        name,
                        after.Label,
                        "Changed",
                        {
                            "Geometry": (
                                translate("ModelDiff", "LOCAL geometry"),
                                translate("ModelDiff", "REMOTE geometry"),
                            )
                        },
                    )
                    result.objects.append(change)
            if change is not None and ((old and not old[1]) or (new and not new[1])):
                change.measures = (
                    old[0].Volume if old and not old[1] else 0,
                    new[0].Volume if new and not new[1] else 0,
                    old[0].Area if old and not old[1] else 0,
                    new[0].Area if new and not new[1] else 0,
                )
                if "Shape" in change.properties:
                    change.properties["Shape"] = (
                        translate("ModelDiff", "LOCAL shape"),
                        translate("ModelDiff", "REMOTE shape"),
                    )
            old = old if name in visible_left else None
            new = new if name in visible_right else None
            label = (after or before).Label
            if change is None or (
                same_geometry and old is not None and new is not None
            ):
                geometry = new or old
                if geometry is not None:
                    group = change.kind if change is not None else "Unchanged"
                    result.visuals.append(
                        Visual(name, label, group, "Unchanged", *geometry)
                    )
            else:
                _visuals(result, name, label, old, new, change.kind)
        result.objects.sort(key=lambda change: change.name)
        return result


def show_result(result):
    """Create an unsaved document with detached, grouped diff geometry.

    Also usable in FreeCADCmd; colour and camera settings are applied only when
    a GUI is available. The returned document belongs to the caller.
    """
    doc = App.newDocument("ModelDifference", translate("ModelDiff", "Model difference"))
    try:
        groups = {}
        for name, label in (
            ("Added", translate("ModelDiff", "Added")),
            ("Removed", translate("ModelDiff", "Removed")),
            ("Changed", translate("ModelDiff", "Changed")),
            ("Unchanged", translate("ModelDiff", "Unchanged")),
        ):
            group = doc.addObject("App::DocumentObjectGroup", name)
            group.Label = label
            groups[name] = group
        doc.addProperty("App::PropertyString", "DiffReport", "ModelDiff")
        doc.DiffReport = result.report()
        colors = {
            "Added": (0.15, 0.8, 0.3),
            "Removed": (0.9, 0.2, 0.2),
            "Unchanged": (0.65, 0.65, 0.65),
        }
        for visual in result.visuals:
            obj = doc.addObject(
                "Mesh::Feature" if visual.mesh else "Part::Feature",
                "Diff_" + visual.name,
            )
            obj.Label = (
                visual.label
                + " ("
                + {
                    "Added": translate("ModelDiff", "added material"),
                    "Removed": translate("ModelDiff", "removed material"),
                    "Unchanged": translate("ModelDiff", "unchanged material"),
                }[visual.role]
                + ")"
            )
            obj.addProperty("App::PropertyString", "SourceName", "ModelDiff")
            obj.addProperty("App::PropertyString", "DiffRole", "ModelDiff")
            obj.SourceName, obj.DiffRole = visual.name, visual.role
            if visual.mesh:
                obj.Mesh = visual.geometry
            else:
                obj.Shape = visual.geometry
            groups[visual.group].addObject(obj)
            if App.GuiUp:
                view = obj.ViewObject
                view.ShapeColor = colors[visual.role]
                view.LineColor = colors[visual.role]
                if hasattr(view, "PointColor"):
                    view.PointColor = colors[visual.role]
                view.Transparency = 85 if visual.role == "Unchanged" else 15
        if App.GuiUp:
            import FreeCADGui as Gui

            Gui.activeDocument().activeView().viewAxonometric()
            Gui.activeDocument().activeView().fitAll()
        return doc
    except Exception:
        App.closeDocument(doc.Name)
        raise


def run_cli(local, remote):
    """Shared implementation of FreeCAD[Cmd] --diff LOCAL REMOTE."""
    result = compare_files(local, remote)
    App.Console.PrintMessage(result.report())
    if App.GuiUp:
        import FreeCADGui as Gui
        from PySide import QtWidgets

        report = Gui.getMainWindow().findChild(QtWidgets.QDockWidget, "Report view")
        if report is not None:
            report.show()
        return show_result(result)
    return result
