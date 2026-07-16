"""Determinism tests."""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

from freecad_git.export import export_to_bytes
from freecad_git.serialize import semantic_hash
from freecad_git.export import export_to_dict
from freecad_git.normalize import canonical_decimal, normalize_quaternion, placement_from_attributes
from freecad_git.repack import repack_deterministic


class TestNormalization:
    def test_negative_zero(self):
        assert canonical_decimal(-0.0) == "0"
        assert canonical_decimal("-0") == "0"

    def test_trailing_zeros_removed(self):
        assert canonical_decimal("1.5000") == "1.5"
        assert canonical_decimal("10.0") == "10"

    def test_quaternion_positive_w(self):
        q = normalize_quaternion(0, 0, 0, -1)
        assert q[3] != "-0"
        assert float(q[3]) > 0

    def test_placement_from_attributes(self):
        p = placement_from_attributes(
            {"Px": "0", "Py": "0", "Pz": "0", "Q0": "0", "Q1": "0", "Q2": "0", "Q3": "1"}
        )
        assert p["position_mm"] == ["0", "0", "0"]
        assert len(p["rotation_xyzw"]) == 4


class TestDeterminism:
    def test_repeated_export_identical(self, fixtures_dir):
        path = fixtures_dir / "basic.FCStd"
        h1 = hashlib.sha256(export_to_bytes(path)).hexdigest()
        h2 = hashlib.sha256(export_to_bytes(path)).hexdigest()
        assert h1 == h2

    def test_timestamp_noise_ignored(self, fixtures_dir):
        a = export_to_dict(fixtures_dir / "basic_timestamp_a.FCStd")
        b = export_to_dict(fixtures_dir / "basic_timestamp_b.FCStd")
        assert a["source"]["semantic_sha256"] == b["source"]["semantic_sha256"]
        assert a["objects"] == b["objects"]
        assert a["document"] == b["document"]

    def test_zip_repack_deterministic(self, fixtures_dir):
        path = fixtures_dir / "basic.FCStd"
        r1 = repack_deterministic(path)
        r2 = repack_deterministic(path)
        assert r1 == r2
        assert hashlib.sha256(r1).hexdigest() == hashlib.sha256(r2).hexdigest()

    def test_expression_crlf_normalized(self, fixtures_dir):
        # Spreadsheet content normalization is tested via export stability
        path = fixtures_dir / "spreadsheet.FCStd"
        data = export_to_bytes(path)
        assert b"\r" not in data

    def test_objects_sorted_by_name(self, fixtures_dir):
        data = export_to_bytes(fixtures_dir / "partdesign.FCStd")
        import json

        parsed = json.loads(data)
        keys = list(parsed["objects"].keys())
        assert keys == sorted(keys)

    def test_atomic_write_no_partial(self, fixtures_dir, tmp_path):
        from freecad_git.export import write_sidecar_atomic

        fcstd = fixtures_dir / "basic.FCStd"
        copy = tmp_path / "basic.FCStd"
        copy.write_bytes(fcstd.read_bytes())
        data = export_to_bytes(copy)
        write_sidecar_atomic(copy, data)
        sidecar = tmp_path / "basic.FCStd.git.json"
        assert sidecar.exists()
        assert sidecar.read_bytes() == data
        # No temp files left
        temps = list(tmp_path.glob("*.tmp-*"))
        assert len(temps) == 0
