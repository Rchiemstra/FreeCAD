"""ReviewNote / ReviewNoteGroup freecad-git export and restore coverage."""

from __future__ import annotations

import copy
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from freecad_git.export import export_to_bytes, export_to_dict
from freecad_git.restore import (
    find_group_owner,
    iter_review_note_objects,
    restore_review_notes,
)
from freecad_git.serialize import serialize_deterministic


class TestReviewNoteExport:
    def test_export_includes_review_note_semantics(self, fixtures_dir: Path):
        data = export_to_dict(fixtures_dir / "review_note.FCStd")
        note = data["objects"]["ReviewNote"]
        group = data["objects"]["ReviewNotes"]

        assert note["type"] == "Assembly::ReviewNote"
        assert group["type"] == "Assembly::ReviewNoteGroup"
        assert group["membership"]["group"] == ["ReviewNote"]
        assert data["objects"]["Assembly"]["membership"]["group"] == ["Box", "ReviewNotes"]

        props = note["properties"]
        assert props["LabelText"] == [
            "Check clearance @Box.Face6",
            "and keep the pocket edge free",
        ]
        assert props["Target"] == {
            "type": "link_sub",
            "object": "Box",
            "subelement": "Face6",
            "subelements": ["Face6"],
        }
        assert props["LocalAnchor"] == ["5", "10", "30"]
        assert props["TextPosition"] == ["25", "40", "30"]
        assert props["BasePosition"] == ["5", "10", "30"]
        assert props["Resolved"] is False
        assert props["LeaderPort"] == "-1"
        assert note["visibility"] is True
        assert note["view"]["FontName"] == "Segoe UI"
        assert note["view"]["FontSize"] == "9"
        assert note["view"]["Frame"] is True
        assert "Box" in note.get("local_links", [])
        assert ["ReviewNotes", "ReviewNote"] in data["dependencies"] or [
            "ReviewNotes",
            "ReviewNote",
        ] in [list(d) for d in data["dependencies"]]

    def test_export_is_deterministic(self, fixtures_dir: Path):
        first = export_to_bytes(fixtures_dir / "review_note.FCStd")
        second = export_to_bytes(fixtures_dir / "review_note.FCStd")
        assert first == second
        assert first.endswith(b"\n")
        assert b"\r" not in first


def test_find_group_owner():
    objects = {
        "Assembly": {
            "type": "Assembly::AssemblyObject",
            "membership": {"group": ["ReviewNotes"]},
        },
        "ReviewNotes": {
            "type": "Assembly::ReviewNoteGroup",
            "membership": {"group": ["ReviewNote"]},
        },
    }
    assert find_group_owner(objects, "ReviewNotes") == "Assembly"
    assert find_group_owner(objects, "Missing") is None


class _FakeProp:
    def __init__(self, value=None):
        self._value = value

    def setValue(self, value):
        self._value = value


class _FakeViewObject:
    def __init__(self):
        self.Visibility = True
        self.BackgroundColor = None
        self.TextColor = None
        self.FontName = "Arial"
        self.FontSize = 12.0
        self.Frame = True
        self.Justification = 0
        self.ShowInTree = True
        self.DisplayMode = 0
        self._props = {
            "BackgroundColor": _FakeProp(),
            "TextColor": _FakeProp(),
            "Justification": _FakeProp(0),
            "DisplayMode": _FakeProp(0),
        }

    def getPropertyByName(self, name):
        return self._props[name]


class _FakeDoc:
    def __init__(self):
        self._objects: dict[str, object] = {}

    def addObject(self, type_id, name):
        obj = _FakeObject(type_id, name, self)
        self._objects[name] = obj
        return obj

    def getObject(self, name):
        return self._objects.get(name)

    def removeObject(self, name):
        self._objects.pop(name, None)

    def recompute(self):
        return None


class _FakeObject:
    def __init__(self, type_id, name, doc=None):
        self.TypeId = type_id
        self.Name = name
        self.Label = name
        self.Group = []
        self.ViewObject = _FakeViewObject()
        self.LabelText = []
        self.LocalAnchor = None
        self.TextPosition = None
        self.BasePosition = None
        self.Resolved = False
        self.AttachmentBroken = False
        self.LeaderPort = -1.0
        self.Target = None
        self.JointSide = 0
        self._doc = doc
        self._props = {"JointSide": _FakeProp(0)}

    def newObject(self, type_id, name):
        assert self._doc is not None
        obj = _FakeObject(type_id, name, self._doc)
        self._doc._objects[name] = obj
        self.Group.append(obj)
        return obj

    def addObject(self, obj):
        if obj not in self.Group:
            self.Group.append(obj)

    def getPropertyByName(self, name):
        return self._props[name]


def test_restore_review_notes_with_fake_freecad(fixtures_dir: Path, monkeypatch):
    """Export → restore into a fake FreeCAD document (no FreeCAD install required)."""

    class Vector:
        def __init__(self, x, y, z):
            self.x, self.y, self.z = float(x), float(y), float(z)

        def isEqual(self, other, tol):
            return (
                abs(self.x - other.x) <= tol
                and abs(self.y - other.y) <= tol
                and abs(self.z - other.z) <= tol
            )

    monkeypatch.setitem(sys.modules, "FreeCAD", SimpleNamespace(Vector=Vector))

    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    assert [name for name, _ in iter_review_note_objects(sidecar)] == [
        "ReviewNote",
        "ReviewNotes",
    ]

    doc = _FakeDoc()
    assembly = _FakeObject("Assembly::AssemblyObject", "Assembly", doc)
    box = _FakeObject("Part::Box", "Box", doc)
    doc._objects["Assembly"] = assembly
    doc._objects["Box"] = box

    result = restore_review_notes(doc, copy.deepcopy(sidecar))
    assert result["groups"] == ["ReviewNotes"]
    assert result["notes"] == ["ReviewNote"]
    assert result["owners"]["ReviewNotes"] == "Assembly"

    note = doc.getObject("ReviewNote")
    group = doc.getObject("ReviewNotes")
    assert note.TypeId == "Assembly::ReviewNote"
    assert group.TypeId == "Assembly::ReviewNoteGroup"
    assert note in group.Group
    assert group in assembly.Group
    assert note.LabelText == [
        "Check clearance @Box.Face6",
        "and keep the pocket edge free",
    ]
    assert note.Resolved is False
    assert float(note.LeaderPort) == -1.0
    assert note.LocalAnchor.isEqual(Vector(5, 10, 30), 1e-9)
    assert note.TextPosition.isEqual(Vector(25, 40, 30), 1e-9)
    assert note.Target[0] is box
    assert note.Target[1] == ["Face6"]
    assert note.ViewObject.FontName == "Segoe UI"
    assert float(note.ViewObject.FontSize) == 9.0


@pytest.mark.skipif("FreeCAD" not in sys.modules, reason="FreeCAD not imported in this process")
def test_restore_review_notes_round_trip_freecad(fixtures_dir: Path, tmp_path: Path):
    """Optional live FreeCAD save/export round-trip when FreeCAD is already loaded."""
    FreeCAD = pytest.importorskip("FreeCAD")
    if not hasattr(FreeCAD, "newDocument"):
        pytest.skip("FreeCAD App API unavailable")

    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc = FreeCAD.newDocument("ReviewNoteGitRoundTrip")
    try:
        assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
        box = doc.addObject("Part::Box", "Box")
        assembly.addObject(box)
        restore_review_notes(doc, sidecar)
        note = doc.getObject("ReviewNote")
        group = doc.getObject("ReviewNotes")
        assert note is not None and group is not None
        path = tmp_path / "roundtrip.FCStd"
        doc.saveAs(str(path))
        exported = export_to_dict(path)
        for key in (
            "LabelText",
            "Target",
            "LocalAnchor",
            "TextPosition",
            "Resolved",
            "LeaderPort",
        ):
            assert (
                exported["objects"]["ReviewNote"]["properties"][key]
                == sidecar["objects"]["ReviewNote"]["properties"][key]
            )
        left = serialize_deterministic(
            {"objects": {k: exported["objects"][k] for k in ("ReviewNote", "ReviewNotes")}}
        )
        right = serialize_deterministic(
            {"objects": {k: sidecar["objects"][k] for k in ("ReviewNote", "ReviewNotes")}}
        )
        assert left == right
    finally:
        FreeCAD.closeDocument(doc.Name)
