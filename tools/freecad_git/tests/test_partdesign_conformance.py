"""Conformance tests against real FreeCAD Document.xml serialization."""

from __future__ import annotations

from pathlib import Path

import pytest

from freecad_git.export import export_to_bytes, export_to_dict

REPO_ROOT = Path(__file__).resolve().parents[3]
PARTDESIGN_EXAMPLE = REPO_ROOT / "data" / "examples" / "PartDesignExample.FCStd"


@pytest.mark.skipif(not PARTDESIGN_EXAMPLE.is_file(), reason="PartDesignExample.FCStd not available")
class TestPartDesignConformance:
    def test_body_group_members(self):
        data = export_to_dict(PARTDESIGN_EXAMPLE)
        group = data["objects"]["Body"]["membership"]["group"]
        assert len(group) == 8
        assert group == [
            "Sketch",
            "Pad",
            "Sketch001",
            "Sketch002",
            "Sketch003",
            "Pocket",
            "Pocket001",
            "Pocket002",
        ]

    def test_origin_features(self):
        data = export_to_dict(PARTDESIGN_EXAMPLE)
        features = data["objects"]["Origin"]["properties"]["OriginFeatures"]
        assert len(features) == 7
        assert features == [
            "X_Axis",
            "Y_Axis",
            "Z_Axis",
            "XY_Plane",
            "XZ_Plane",
            "YZ_Plane",
            "Origin001",
        ]

    def test_sketch_constraints_and_geometry(self):
        data = export_to_dict(PARTDESIGN_EXAMPLE)
        sketch = data["objects"]["Sketch"]["sketch"]
        assert len(sketch["constraints"]) == 12
        assert len(sketch["geometries"]) == 4
        assert sketch["geometries"][0]["type"] == "Part::GeomLineSegment"
        assert "primitive" in sketch["geometries"][0]

    def test_export_twice_identical(self):
        first = export_to_bytes(PARTDESIGN_EXAMPLE)
        second = export_to_bytes(PARTDESIGN_EXAMPLE)
        assert first == second
