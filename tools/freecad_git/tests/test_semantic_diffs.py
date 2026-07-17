"""Semantic diff focus tests."""

from __future__ import annotations

import difflib
import io
import zipfile

import pytest

from freecad_git.export import export_to_bytes, export_to_dict
from tests.fixtures.builder import (
    BASIC_DOCUMENT_XML,
    PLACEMENT_DOCUMENT_XML,
    SPREADSHEET_DOCUMENT_XML,
    _make_zip,
    write_fixture,
)


def _diff_lines(a: bytes, b: bytes) -> list[str]:
    return list(
        difflib.unified_diff(
            a.decode("utf-8").splitlines(keepends=True),
            b.decode("utf-8").splitlines(keepends=True),
            lineterm="",
        )
    )


class TestSemanticDiffs:
    def test_spreadsheet_value_change_focused(self, fixtures_dir, tmp_path):
        base = export_to_bytes(fixtures_dir / "spreadsheet.FCStd")

        modified_xml = SPREADSHEET_DOCUMENT_XML.replace("=0.2 mm", "=0.25 mm")
        modified_path = tmp_path / "spreadsheet_modified.FCStd"
        write_fixture(modified_path, modified_xml)
        modified = export_to_bytes(modified_path)

        diff = _diff_lines(base, modified)
        changed = [l for l in diff if l.startswith("+") or l.startswith("-")]
        changed = [l for l in changed if not l.startswith("+++") and not l.startswith("---")]
        # Should have focused changes (hash + cell content), not huge diff
        assert len(changed) < 20
        assert any("0.25" in l or "0.2" in l for l in changed)

    def test_placement_change_focused(self, fixtures_dir, tmp_path):
        base = export_to_bytes(fixtures_dir / "placement.FCStd")
        modified_xml = PLACEMENT_DOCUMENT_XML.replace('Px="1"', 'Px="5"')
        modified_path = tmp_path / "placement_modified.FCStd"
        write_fixture(modified_path, modified_xml)
        modified = export_to_bytes(modified_path)

        diff = _diff_lines(base, modified)
        changed = [l for l in diff if l.startswith("+") or l.startswith("-")]
        changed = [l for l in changed if not l.startswith("+++") and not l.startswith("---")]
        assert len(changed) < 15
        assert any("5" in l for l in changed)

    def test_label_change_once(self, fixtures_dir, tmp_path):
        base = export_to_dict(fixtures_dir / "basic.FCStd")
        modified_xml = BASIC_DOCUMENT_XML.replace('value="Box"', 'value="Box renamed"', 1)
        modified_xml = modified_xml.replace('value="Box"', 'value="Box renamed"')
        modified_path = tmp_path / "renamed.FCStd"
        write_fixture(modified_path, modified_xml)
        modified = export_to_dict(modified_path)
        assert base["objects"]["Box"]["label"] == "Box"
        assert modified["objects"]["Box"]["label"] == "Box renamed"
        # Label appears exactly once per object in semantic model
        assert list(modified["objects"]["Box"].values()).count("Box renamed") == 1

    def test_gui_only_no_semantic_diff(self, fixtures_dir):
        base = export_to_dict(fixtures_dir / "basic.FCStd")
        gui = export_to_dict(fixtures_dir / "gui_only.FCStd")
        assert base["source"]["semantic_sha256"] == gui["source"]["semantic_sha256"]
        assert base["objects"] == gui["objects"]

    def test_timestamp_only_no_diff(self, fixtures_dir):
        a = export_to_dict(fixtures_dir / "basic_timestamp_a.FCStd")
        b = export_to_dict(fixtures_dir / "basic_timestamp_b.FCStd")
        assert a["source"]["semantic_sha256"] == b["source"]["semantic_sha256"]

    def test_partdesign_parameters_present(self, fixtures_dir):
        data = export_to_bytes(fixtures_dir / "partdesign.FCStd")
        text = data.decode("utf-8")
        assert "Pad" in text
        assert "25" in text
        assert "Sketch" in text

    def test_constraint_in_sketch(self, fixtures_dir):
        data = export_to_bytes(fixtures_dir / "partdesign.FCStd")
        text = data.decode("utf-8")
        assert "DistanceX" in text or "constraints" in text
