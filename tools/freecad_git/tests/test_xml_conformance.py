"""Regression tests for real-format XML property parsing."""

from __future__ import annotations

import pytest

from freecad_git.export import export_to_dict
from tests.fixtures.builder import load_xml_fixture, write_fixture


class TestXmlConformance:
    def test_link_list_all_members(self, fixtures_dir):
        data = export_to_dict(fixtures_dir / "link_list.FCStd")
        group = data["objects"]["Group"]["membership"]["group"]
        assert group == ["A", "B", "C"]

    def test_link_sub_and_link_sub_list(self, fixtures_dir):
        data = export_to_dict(fixtures_dir / "partdesign.FCStd")
        pad = data["objects"]["Pad"]
        assert pad["properties"]["Base"]["type"] == "link_sub"
        assert pad["properties"]["Base"]["object"] == "Sketch"
        sketch = data["objects"]["Sketch"]
        assert sketch["support"] == [{"object": "XY_Plane", "subelement": ""}]

    def test_real_constraint_and_geometry_tags(self, fixtures_dir):
        data = export_to_dict(fixtures_dir / "partdesign.FCStd")
        sketch = data["objects"]["Sketch"]["sketch"]
        assert len(sketch["constraints"]) == 1
        assert sketch["constraints"][0]["type"] == "DistanceX"
        assert len(sketch["geometries"]) == 1
        assert sketch["geometries"][0]["primitive"]["kind"] == "LineSegment"

    def test_nested_typed_fallback(self, fixtures_dir):
        data = export_to_dict(fixtures_dir / "custom_property.FCStd")
        custom = data["objects"]["CustomObj"]["properties"]["CustomData"]
        assert custom["type"] == "CustomWorkbench::PropertyCustom"
        assert custom["xml"][0]["tag"] == "CustomData"

    def test_malformed_status_raises(self, tmp_path):
        path = tmp_path / "bad_status.FCStd"
        write_fixture(path, load_xml_fixture("malformed_status.xml"))
        from freecad_git.errors import InvalidXmlError

        with pytest.raises(InvalidXmlError, match="status"):
            export_to_dict(path)
