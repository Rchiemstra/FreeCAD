"""Tests for experimental deterministic ZIP repacking."""

from __future__ import annotations

import hashlib

from freecad_git.repack import repack_deterministic


def test_repack_produces_identical_bytes(fixtures_dir):
    path = fixtures_dir / "basic.FCStd"
    r1 = repack_deterministic(path)
    r2 = repack_deterministic(path)
    assert r1 == r2
    assert hashlib.sha256(r1).hexdigest() == hashlib.sha256(r2).hexdigest()


def test_repack_does_not_modify_source(fixtures_dir, tmp_path):
    path = fixtures_dir / "basic.FCStd"
    original = path.read_bytes()
    out = tmp_path / "repacked.FCStd"
    repack_deterministic(path, output=out)
    assert path.read_bytes() == original
    assert out.exists()
