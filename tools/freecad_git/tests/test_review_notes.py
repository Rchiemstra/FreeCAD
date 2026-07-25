"""ReviewNote / ReviewNoteGroup freecad-git export and restore coverage."""

from __future__ import annotations

import copy
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from freecad_git.errors import InvalidSchemaError, MalformedSidecarError
from freecad_git.export import export_to_bytes, export_to_dict
from freecad_git.restore import (
    RestoreError,
    find_group_owner,
    find_note_group,
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

    def test_export_whole_object_target_preserves_link_and_local_links(self, fixtures_dir: Path):
        data = export_to_dict(fixtures_dir / "review_note_whole_target.FCStd")
        note = data["objects"]["ReviewNote"]
        assert note["properties"]["Target"] == {"type": "link", "target": "Box"}
        assert "Box" in note.get("local_links", [])

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


def test_find_note_group_no_arbitrary_fallback():
    group_entries = [
        ("GroupA", {"membership": {"group": ["NoteA"]}}),
        ("GroupB", {"membership": {"group": ["NoteB"]}}),
    ]
    assert find_note_group(group_entries, "NoteA") == "GroupA"
    assert find_note_group(group_entries, "NoteB") == "GroupB"
    assert find_note_group(group_entries, "Orphan") is None


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
        self._txn_stack: list[dict[str, object]] = []
        self._aborted = False
        self.recompute_calls = 0

    def addObject(self, type_id, name):
        obj = _FakeObject(type_id, name, self)
        self._objects[name] = obj
        return obj

    def getObject(self, name):
        return self._objects.get(name)

    def removeObject(self, name):
        self._objects.pop(name, None)

    def recompute(self):
        self.recompute_calls += 1
        return None

    def openTransaction(self, _name):
        # Snapshot object identities and core fields for abort restore.
        snap = {}
        for name, obj in self._objects.items():
            snap[name] = obj
            obj._txn_snapshot = {
                "Label": obj.Label,
                "Group": list(obj.Group),
                "LabelText": list(obj.LabelText),
                "LocalAnchor": obj.LocalAnchor,
                "TextPosition": obj.TextPosition,
                "BasePosition": obj.BasePosition,
                "Resolved": obj.Resolved,
                "AttachmentBroken": obj.AttachmentBroken,
                "LeaderPort": obj.LeaderPort,
                "Target": obj.Target,
                "JointSide": obj.JointSide,
            }
        self._txn_stack.append(snap)

    def commitTransaction(self):
        if self._txn_stack:
            self._txn_stack.pop()
        for obj in self._objects.values():
            if hasattr(obj, "_txn_snapshot"):
                del obj._txn_snapshot

    def abortTransaction(self):
        self._aborted = True
        if not self._txn_stack:
            return
        snap = self._txn_stack.pop()
        self._objects.clear()
        self._objects.update(snap)
        for obj in self._objects.values():
            state = getattr(obj, "_txn_snapshot", None)
            if not state:
                continue
            obj.Label = state["Label"]
            obj.Group = list(state["Group"])
            obj.LabelText = list(state["LabelText"])
            obj.LocalAnchor = state["LocalAnchor"]
            obj.TextPosition = state["TextPosition"]
            obj.BasePosition = state["BasePosition"]
            obj.Resolved = state["Resolved"]
            obj.AttachmentBroken = state["AttachmentBroken"]
            obj.LeaderPort = state["LeaderPort"]
            obj.Target = state["Target"]
            obj.JointSide = state["JointSide"]
            del obj._txn_snapshot


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


def _install_fake_freecad(monkeypatch):
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
    return Vector


def _seed_assembly_doc():
    doc = _FakeDoc()
    assembly = _FakeObject("Assembly::AssemblyObject", "Assembly", doc)
    box = _FakeObject("Part::Box", "Box", doc)
    doc._objects["Assembly"] = assembly
    doc._objects["Box"] = box
    return doc, assembly, box


def test_restore_review_notes_with_fake_freecad(fixtures_dir: Path, monkeypatch):
    """Export → restore into a fake FreeCAD document (no FreeCAD install required)."""
    Vector = _install_fake_freecad(monkeypatch)

    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    assert [name for name, _ in iter_review_note_objects(sidecar)] == [
        "ReviewNote",
        "ReviewNotes",
    ]

    doc, assembly, box = _seed_assembly_doc()
    original = copy.deepcopy(sidecar)

    result = restore_review_notes(doc, sidecar)
    assert result["groups"] == ["ReviewNotes"]
    assert result["notes"] == ["ReviewNote"]
    assert result["owners"]["ReviewNotes"] == "Assembly"
    # Caller sidecar must not gain temporary restore fields.
    assert sidecar == original
    assert "_pending_members" not in sidecar["objects"]["ReviewNotes"]

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


def test_restore_whole_object_target(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note_whole_target.FCStd")
    assert sidecar["objects"]["ReviewNote"]["properties"]["Target"] == {
        "type": "link",
        "target": "Box",
    }
    assert "Box" in sidecar["objects"]["ReviewNote"]["local_links"]

    doc, _, box = _seed_assembly_doc()
    restore_review_notes(doc, sidecar)
    note = doc.getObject("ReviewNote")
    assert note.Target[0] is box
    assert note.Target[1] == []

    # Re-export semantic shape for Target + local_links stays stable.
    re_exported_target = sidecar["objects"]["ReviewNote"]["properties"]["Target"]
    assert re_exported_target == {"type": "link", "target": "Box"}


def test_replace_existing_wrong_type_preserves_box(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc, _, box = _seed_assembly_doc()
    # Name collision: ReviewNote is a Part::Box, not a ReviewNote.
    colliding = _FakeObject("Part::Box", "ReviewNote", doc)
    doc._objects["ReviewNote"] = colliding
    box_id = id(colliding)
    before_names = set(doc._objects.keys())

    with pytest.raises(RestoreError, match="expected Assembly::ReviewNote"):
        restore_review_notes(doc, sidecar, replace_existing=True)

    assert doc.getObject("ReviewNote") is colliding
    assert id(doc.getObject("ReviewNote")) == box_id
    assert colliding.TypeId == "Part::Box"
    assert set(doc._objects.keys()) == before_names
    assert doc.recompute_calls == 0
    assert not doc._aborted  # failed in preflight — no transaction mutation


def test_replace_existing_matching_types_succeeds(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc, assembly, box = _seed_assembly_doc()
    old_group = _FakeObject("Assembly::ReviewNoteGroup", "ReviewNotes", doc)
    old_note = _FakeObject("Assembly::ReviewNote", "ReviewNote", doc)
    old_note.LabelText = ["stale"]
    doc._objects["ReviewNotes"] = old_group
    doc._objects["ReviewNote"] = old_note
    assembly.Group = [box, old_group]
    old_group.Group = [old_note]

    result = restore_review_notes(doc, sidecar, replace_existing=True)
    assert result["notes"] == ["ReviewNote"]
    note = doc.getObject("ReviewNote")
    group = doc.getObject("ReviewNotes")
    assert note is not old_note
    assert group is not old_group
    assert note.LabelText[0].startswith("Check clearance")
    assert note in group.Group


def test_missing_target_aborts_without_mutation(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc = _FakeDoc()
    assembly = _FakeObject("Assembly::AssemblyObject", "Assembly", doc)
    doc._objects["Assembly"] = assembly
    # Box (Target) intentionally missing.
    before = dict(doc._objects)
    before_membership = list(assembly.Group)

    with pytest.raises(RestoreError, match="Target object not found"):
        restore_review_notes(doc, sidecar)

    assert dict(doc._objects) == before
    assert list(assembly.Group) == before_membership
    assert doc.getObject("ReviewNote") is None
    assert doc.getObject("ReviewNotes") is None
    assert doc.recompute_calls == 0


def test_invalid_property_after_valid_notes_rolls_back(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    # Add a second note with an invalid vector so preflight rejects before mutation,
    # and a mutation-time failure path via monkeypatched recompute.
    sidecar["objects"]["ReviewNoteB"] = copy.deepcopy(sidecar["objects"]["ReviewNote"])
    sidecar["objects"]["ReviewNoteB"]["properties"]["LocalAnchor"] = ["bad"]

    doc, assembly, _box = _seed_assembly_doc()
    before = set(doc._objects.keys())
    before_membership = list(assembly.Group)

    with pytest.raises(RestoreError, match="LocalAnchor"):
        restore_review_notes(doc, sidecar)

    assert set(doc._objects.keys()) == before
    assert list(assembly.Group) == before_membership
    assert doc.getObject("ReviewNote") is None
    assert doc.getObject("ReviewNoteB") is None


def test_mutation_time_failure_aborts_transaction(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc, assembly, _box = _seed_assembly_doc()
    before_ids = {name: id(obj) for name, obj in doc._objects.items()}
    before_names = set(doc._objects.keys())
    before_membership = list(assembly.Group)

    def boom():
        raise RuntimeError("recompute failed")

    doc.recompute = boom  # type: ignore[method-assign]

    with pytest.raises(RuntimeError, match="recompute failed"):
        restore_review_notes(doc, sidecar)

    assert doc._aborted is True
    assert set(doc._objects.keys()) == before_names
    assert {name: id(obj) for name, obj in doc._objects.items()} == before_ids
    assert list(assembly.Group) == before_membership
    assert doc.getObject("ReviewNote") is None
    assert doc.getObject("ReviewNotes") is None


def test_malformed_sidecar_rejected_before_mutation(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    doc, _, _ = _seed_assembly_doc()
    before = set(doc._objects.keys())

    with pytest.raises((InvalidSchemaError, MalformedSidecarError)):
        restore_review_notes(doc, {"schema": "wrong"})

    assert set(doc._objects.keys()) == before
    assert doc.recompute_calls == 0


def test_wrong_schema_identifier_rejected(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    sidecar["schema"] = "freecad-git-sidecar/v0"
    doc, _, _ = _seed_assembly_doc()
    before = set(doc._objects.keys())

    with pytest.raises(InvalidSchemaError):
        restore_review_notes(doc, sidecar)

    assert set(doc._objects.keys()) == before


def test_multiple_groups_ordered_membership_no_fallback(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    # Two groups with explicit ordered memberships; orphan note has no group.
    sidecar["objects"]["ReviewNotes"]["membership"]["group"] = ["NoteA", "NoteC"]
    sidecar["objects"]["ReviewNotesB"] = {
        "type": "Assembly::ReviewNoteGroup",
        "label": "Other Notes",
        "membership": {"group": ["NoteB"]},
        "visibility": True,
        "view": {},
    }
    sidecar["objects"]["Assembly"]["membership"]["group"] = [
        "Box",
        "ReviewNotes",
        "ReviewNotesB",
    ]
    base_note = copy.deepcopy(sidecar["objects"]["ReviewNote"])
    del sidecar["objects"]["ReviewNote"]
    for name, text in (("NoteA", "A"), ("NoteB", "B"), ("NoteC", "C"), ("Orphan", "O")):
        entry = copy.deepcopy(base_note)
        entry["label"] = text
        entry["properties"]["LabelText"] = [text]
        sidecar["objects"][name] = entry

    doc, assembly, box = _seed_assembly_doc()
    result = restore_review_notes(doc, sidecar)
    assert set(result["groups"]) == {"ReviewNotes", "ReviewNotesB"}
    group_a = doc.getObject("ReviewNotes")
    group_b = doc.getObject("ReviewNotesB")
    assert [o.Name for o in group_a.Group] == ["NoteA", "NoteC"]
    assert [o.Name for o in group_b.Group] == ["NoteB"]
    orphan = doc.getObject("Orphan")
    assert orphan is not None
    assert orphan not in group_a.Group
    assert orphan not in group_b.Group
    assert group_a in assembly.Group
    assert group_b in assembly.Group


def test_replace_existing_failure_restores_original_notes(fixtures_dir: Path, monkeypatch):
    _install_fake_freecad(monkeypatch)
    sidecar = export_to_dict(fixtures_dir / "review_note.FCStd")
    doc, assembly, box = _seed_assembly_doc()
    old_group = _FakeObject("Assembly::ReviewNoteGroup", "ReviewNotes", doc)
    old_note = _FakeObject("Assembly::ReviewNote", "ReviewNote", doc)
    old_note.LabelText = ["original-keep"]
    doc._objects["ReviewNotes"] = old_group
    doc._objects["ReviewNote"] = old_note
    assembly.Group = [box, old_group]
    old_group.Group = [old_note]
    old_note_id = id(old_note)
    old_group_id = id(old_group)

    def boom():
        raise RuntimeError("boom after replace")

    doc.recompute = boom  # type: ignore[method-assign]

    with pytest.raises(RuntimeError, match="boom after replace"):
        restore_review_notes(doc, sidecar, replace_existing=True)

    assert doc._aborted is True
    assert doc.getObject("ReviewNote") is old_note
    assert doc.getObject("ReviewNotes") is old_group
    assert id(doc.getObject("ReviewNote")) == old_note_id
    assert id(doc.getObject("ReviewNotes")) == old_group_id
    assert old_note.LabelText == ["original-keep"]


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


def _find_freecad_cmd() -> str | None:
    env = os.environ.get("FREECAD_CMD")
    if env and Path(env).is_file():
        return env
    for name in ("FreeCADCmd", "freecadcmd", "FreeCADCmd.exe"):
        found = shutil.which(name)
        if found:
            return found
    return None


@pytest.mark.skipif(_find_freecad_cmd() is None, reason="FreeCADCmd not available")
def test_freecadcmd_restore_into_temporary_fcstd(fixtures_dir: Path, tmp_path: Path):
    """Open → restore → save → reopen and confirm note fields via FreeCADCmd."""
    freecad_cmd = _find_freecad_cmd()
    assert freecad_cmd is not None

    # Destination document without notes (assembly + box only).
    fcstd = tmp_path / "dest.FCStd"
    from tests.fixtures.builder import write_fixture

    write_fixture(
        fcstd,
        """<?xml version='1.0' encoding='utf-8'?>
<Document SchemaVersion="4" ProgramVersion="1.0R" FileVersion="1" name="Dest">
  <Properties Count="0"/>
  <Objects Count="2">
    <Object type="Assembly::AssemblyObject" name="Assembly" id="1"/>
    <Object type="Part::Box" name="Box" id="2"/>
  </Objects>
  <ObjectData Count="2">
    <Object name="Assembly">
      <Properties Count="2">
        <Property name="Label" type="App::PropertyString"><String value="Assembly"/></Property>
        <Property name="Group" type="App::PropertyLinkList" status="1">
          <LinkList count="1"><Link value="Box"/></LinkList>
        </Property>
      </Properties>
    </Object>
    <Object name="Box">
      <Properties Count="1">
        <Property name="Label" type="App::PropertyString"><String value="Box"/></Property>
      </Properties>
    </Object>
  </ObjectData>
</Document>
""",
    )
    sidecar = tmp_path / "dest.FCStd.git.json"
    sidecar.write_bytes(export_to_bytes(fixtures_dir / "review_note.FCStd"))
    # Point source filename at destination for realism.
    data = json.loads(sidecar.read_text(encoding="utf-8"))
    data["source"]["filename"] = "dest.FCStd"
    sidecar.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")

    script = tmp_path / "restore_check.py"
    out_json = tmp_path / "result.json"
    # Ensure freecad_git is importable inside FreeCADCmd.
    pkg_root = str(Path(__file__).resolve().parents[1] / "src")
    script.write_text(
        f"""
import json, sys
sys.path.insert(0, {pkg_root!r})
import FreeCAD as App
from freecad_git.restore import restore_review_notes_into_fcstd

result = restore_review_notes_into_fcstd(
    {str(sidecar)!r},
    {str(fcstd)!r},
    replace_existing=True,
    save=True,
    close=True,
)
# Reopen and inspect persistence.
doc = App.openDocument({str(fcstd)!r})
note = doc.getObject("ReviewNote")
group = doc.getObject("ReviewNotes")
payload = {{
    "groups": result["groups"],
    "notes": result["notes"],
    "label_text": list(note.LabelText) if note else None,
    "target": [note.Target[0].Name, list(note.Target[1])] if note and note.Target else None,
    "group_members": [o.Name for o in group.Group] if group else None,
    "visibility": bool(note.ViewObject.Visibility) if note and note.ViewObject else None,
    "font_name": note.ViewObject.FontName if note and note.ViewObject else None,
}}
App.closeDocument(doc.Name)
open({str(out_json)!r}, "w", encoding="utf-8").write(json.dumps(payload))
""",
        encoding="utf-8",
    )

    proc = subprocess.run(
        [freecad_cmd, str(script)],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr or proc.stdout
    payload = json.loads(out_json.read_text(encoding="utf-8"))
    assert payload["notes"] == ["ReviewNote"]
    assert payload["groups"] == ["ReviewNotes"]
    assert payload["label_text"][0].startswith("Check clearance")
    assert payload["target"] == ["Box", ["Face6"]]
    assert payload["group_members"] == ["ReviewNote"]
    assert payload["visibility"] is True
