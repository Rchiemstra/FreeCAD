"""JSON schema validation tests."""

from __future__ import annotations

import json

import pytest

from freecad_git.errors import InvalidSchemaError, MalformedSidecarError
from freecad_git.export import export_to_bytes
from freecad_git.schema_validate import validate_sidecar_bytes, validate_sidecar_dict


class TestSchema:
    def test_valid_fixture_sidecar(self, fixtures_dir):
        data = export_to_bytes(fixtures_dir / "basic.FCStd")
        result = validate_sidecar_bytes(data)
        assert result["schema"] == "freecad-git-sidecar/v1"

    def test_all_fixtures_validate(self, fixtures_dir):
        for fcstd in fixtures_dir.glob("*.FCStd"):
            data = export_to_bytes(fcstd)
            validate_sidecar_bytes(data)

    def test_reject_unknown_schema(self):
        with pytest.raises(InvalidSchemaError):
            validate_sidecar_dict({"schema": "unknown/v99", "generator": {}, "source": {}, "document": {}, "objects": {}, "dependencies": [], "external_references": []})

    def test_reject_malformed_json(self):
        with pytest.raises(MalformedSidecarError):
            validate_sidecar_bytes(b"not json")

    def test_reject_missing_required_fields(self):
        with pytest.raises(MalformedSidecarError):
            validate_sidecar_dict(
                {
                    "schema": "freecad-git-sidecar/v1",
                    "generator": {"name": "x", "version": "0", "profile": "semantic"},
                }
            )

    def test_reject_extra_root_fields(self, fixtures_dir):
        import json

        valid = json.loads(export_to_bytes(fixtures_dir / "basic.FCStd"))
        valid["extra"] = True
        with pytest.raises(MalformedSidecarError):
            validate_sidecar_dict(valid)
