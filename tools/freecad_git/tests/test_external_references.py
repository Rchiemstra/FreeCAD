"""External reference tests."""

from __future__ import annotations

import pytest

from freecad_git.export import export_to_bytes, export_to_dict
from tests.fixtures.builder import load_xml_fixture, write_fixture


class TestExternalReferences:
    def test_external_reference_without_opening(self, fixtures_dir):
        data = export_to_bytes(fixtures_dir / "external_link.FCStd")
        text = data.decode("utf-8")
        assert "models/OtherModel.FCStd" in text or "OtherModel" in text
        assert "external_references" in text

    def test_absolute_path_redacted(self, tmp_path):
        path = tmp_path / "abs_link.FCStd"
        write_fixture(path, load_xml_fixture("absolute_external_link.xml"))
        data = export_to_bytes(path)
        text = data.decode("utf-8")
        assert "C:/Users" not in text
        assert "secret" not in text
        assert "<absolute>" in text or "absolute" in text

    def test_absolute_path_reject_policy(self, tmp_path):
        from freecad_git.config import Config
        from freecad_git.errors import UnsupportedDocumentError

        path = tmp_path / "abs_reject.FCStd"
        write_fixture(path, load_xml_fixture("absolute_external_link.xml"))
        cfg = Config(external_reference_policy="reject")
        with pytest.raises(UnsupportedDocumentError, match="absolute external path"):
            export_to_bytes(path, cfg)

    def test_external_reference_dedup_preserves_source_object(self, tmp_path):
        path = tmp_path / "dedup.FCStd"
        write_fixture(path, load_xml_fixture("duplicate_external_links.xml"))
        data = export_to_dict(path)
        refs = data["external_references"]
        assert len(refs) == 2
        sources = {ref["source_object"] for ref in refs}
        assert sources == {"LinkA", "LinkB"}
