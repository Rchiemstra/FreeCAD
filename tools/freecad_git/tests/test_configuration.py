"""Configuration tests."""

from __future__ import annotations

from pathlib import Path

import pytest

from freecad_git.config import discover_fcstd_files, load_config
from freecad_git.errors import InvalidConfigError


class TestConfiguration:
    def test_defaults(self):
        cfg = load_config()
        assert cfg.profile == "semantic"
        assert cfg.archive.max_entries == 10_000

    def test_invalid_key_rejected(self, tmp_path):
        config_file = tmp_path / ".freecad-git.toml"
        config_file.write_text('unknown_key = true\n', encoding="utf-8")
        with pytest.raises(InvalidConfigError, match="unknown"):
            load_config(config_file)

    def test_invalid_policy_rejected(self, tmp_path):
        config_file = tmp_path / ".freecad-git.toml"
        config_file.write_text('external_reference_policy = "invalid"\n', encoding="utf-8")
        with pytest.raises(InvalidConfigError):
            load_config(config_file)

    def test_discover_excludes_fixtures(self, fixtures_dir):
        cfg = load_config()
        cfg.repo_root = fixtures_dir
        cfg.include = ["**/*.FCStd"]
        cfg.exclude = ["**/*"]
        files = discover_fcstd_files(cfg, fixtures_dir)
        assert len(files) == 0
        cfg.exclude = []
        files = discover_fcstd_files(cfg, fixtures_dir)
        assert len(files) >= 1

    def test_empty_include_finds_nothing(self, fixtures_dir):
        cfg = load_config()
        cfg.repo_root = fixtures_dir
        cfg.include = []
        files = discover_fcstd_files(cfg, fixtures_dir)
        assert files == []

    def test_property_exclude_per_object_type(self, fixtures_dir, tmp_path):
        from freecad_git.config import Config
        from freecad_git.export import export_to_dict

        import shutil

        fcstd = tmp_path / "basic.FCStd"
        shutil.copy(fixtures_dir / "basic.FCStd", fcstd)
        cfg = Config()
        cfg.property_exclude = {"App::DocumentObject": ["Placement"]}
        data = export_to_dict(fcstd, cfg)
        assert "placement" not in data["objects"]["Box"]
        cfg.property_exclude = {"Part::Box": ["Placement"]}
        data2 = export_to_dict(fcstd, cfg)
        assert "placement" in data2["objects"]["Box"]

    def test_load_from_repo_config(self, tmp_path):
        config_file = tmp_path / ".freecad-git.toml"
        config_file.write_text(
            """
profile = "semantic"
[archive]
max_entries = 5000
""",
            encoding="utf-8",
        )
        cfg = load_config(config_file, repo_root=tmp_path)
        assert cfg.archive.max_entries == 5000
