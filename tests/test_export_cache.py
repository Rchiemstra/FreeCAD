"""Tests for RobotCAD export cache (FCStd hash + policy fingerprint)."""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

import pytest

from bridge.export_cache import (
    build_cache_key,
    export_policy_fingerprint,
    invalidate_cache,
    is_cache_enabled,
    sha256_file,
    store_cached_export,
    try_restore_cached_export,
)
from bridge.freecad_bridge import expected_exported_urdf_path
from bridge.handoff import resolve_robot_urdf


def _write_minimal_export_tree(gen_dir: Path, robot: str = "arm_2dof") -> Path:
    urdf = expected_exported_urdf_path(robot, gen_dir)
    urdf.parent.mkdir(parents=True, exist_ok=True)
    urdf.write_text("<robot name='arm'/>", encoding="utf-8")
    pkg = gen_dir / f"{robot}_description" / "package.xml"
    pkg.parent.mkdir(parents=True, exist_ok=True)
    pkg.write_text("<package/>", encoding="utf-8")
    return urdf


@pytest.fixture(autouse=True)
def _cache_enabled(monkeypatch):
    monkeypatch.setenv("BRIDGE_EXPORT_CACHE", "1")
    monkeypatch.delenv("BRIDGE_EXPORT_CACHE_INVALIDATE", raising=False)


def test_build_cache_key_changes_with_fcstd(tmp_path):
    a = tmp_path / "a.FCStd"
    b = tmp_path / "b.FCStd"
    a.write_bytes(b"v1")
    b.write_bytes(b"v2")
    ka = build_cache_key("arm_2dof", sha256_file(a), root=tmp_path)
    kb = build_cache_key("arm_2dof", sha256_file(b), root=tmp_path)
    assert ka != kb


def test_cache_miss_store_hit(tmp_path):
    fcstd = tmp_path / "robots" / "arm_2dof.FCStd"
    fcstd.parent.mkdir(parents=True)
    fcstd.write_bytes(b"fcstd-bytes-v1")
    gen = tmp_path / "generated" / "arm_2dof"
    gen.mkdir(parents=True)

    assert try_restore_cached_export("arm_2dof", fcstd, gen) is None

    urdf = _write_minimal_export_tree(gen)
    key = store_cached_export("arm_2dof", fcstd, gen, urdf)

    shutil_rm = gen / f"arm_2dof_description"
    if shutil_rm.exists():
        import shutil

        shutil.rmtree(shutil_rm)
    assert not urdf.is_file()

    restored = try_restore_cached_export("arm_2dof", fcstd, gen)
    assert restored is not None
    assert restored.is_file()
    assert (gen / ".export_cache" / "entries" / key / "manifest.yaml").is_file()


def test_fcstd_change_forces_miss(tmp_path):
    fcstd = tmp_path / "arm.FCStd"
    fcstd.write_bytes(b"v1")
    gen = tmp_path / "gen"
    gen.mkdir()
    urdf = _write_minimal_export_tree(gen)
    store_cached_export("arm_2dof", fcstd, gen, urdf)

    fcstd.write_bytes(b"v2-changed")
    assert try_restore_cached_export("arm_2dof", fcstd, gen) is None


def test_invalidate_clears_cache(tmp_path):
    fcstd = tmp_path / "arm.FCStd"
    fcstd.write_bytes(b"v1")
    gen = tmp_path / "gen"
    gen.mkdir()
    urdf = _write_minimal_export_tree(gen)
    store_cached_export("arm_2dof", fcstd, gen, urdf)

    invalidate_cache(gen)
    assert try_restore_cached_export("arm_2dof", fcstd, gen) is None


def test_invalidate_env(monkeypatch, tmp_path):
    fcstd = tmp_path / "arm.FCStd"
    fcstd.write_bytes(b"v1")
    gen = tmp_path / "gen"
    gen.mkdir()
    urdf = _write_minimal_export_tree(gen)
    store_cached_export("arm_2dof", fcstd, gen, urdf)

    monkeypatch.setenv("BRIDGE_EXPORT_CACHE_INVALIDATE", "1")
    assert try_restore_cached_export("arm_2dof", fcstd, gen) is None


def test_policy_change_invalidates_entry(tmp_path):
    import shutil

    fcstd = tmp_path / "arm.FCStd"
    fcstd.write_bytes(b"v1")
    gen = tmp_path / "gen"
    gen.mkdir()
    urdf = _write_minimal_export_tree(gen)
    key = store_cached_export("arm_2dof", fcstd, gen, urdf)
    shutil.rmtree(gen / "arm_2dof_description")

    alt = {"export_cache_schema": 999, "robotcad_commit": "other"}
    with patch("bridge.export_cache.export_policy_fingerprint", return_value=alt):
        new_key = build_cache_key("arm_2dof", sha256_file(fcstd), root=tmp_path)
        assert new_key != key
        restored = try_restore_cached_export("arm_2dof", fcstd, gen)
        assert restored is None
        assert not expected_exported_urdf_path("arm_2dof", gen).is_file()


def test_resolve_robot_urdf_cache_hit(tmp_path):
    robots = tmp_path / "robots"
    robots.mkdir()
    gen = tmp_path / "generated" / "arm_2dof"
    gen.mkdir(parents=True)
    fcstd = robots / "arm_2dof.FCStd"
    fcstd.write_bytes(b"robot-source")
    urdf = _write_minimal_export_tree(gen)
    store_cached_export("arm_2dof", fcstd, gen, urdf)

    import shutil

    shutil.rmtree(gen / "arm_2dof_description")

    path, msgs, needs = resolve_robot_urdf(
        "arm_2dof",
        robots_dir=robots,
        generated_dir=gen,
    )
    assert needs is False
    assert path is not None
    assert path.is_file()
    assert any("cached" in m.lower() for m in msgs)


def test_cache_hit_logs_structured_event(tmp_path):
    from bridge.run_context import begin_run, current_run, finalize_run

    fcstd = tmp_path / "arm.FCStd"
    fcstd.write_bytes(b"v1")
    gen = tmp_path / "gen"
    gen.mkdir()
    urdf = _write_minimal_export_tree(gen)
    store_cached_export("arm_2dof", fcstd, gen, urdf)

    import shutil

    shutil.rmtree(gen / "arm_2dof_description")

    begin_run("cache_evt", tmp_path / "sim_runs")
    try:
        try_restore_cached_export("arm_2dof", fcstd, gen)
        ctx = current_run()
        assert ctx is not None
        hits = [e for e in ctx.events if e.get("message") == "cache_hit"]
        assert hits
        assert hits[0].get("category") == "export_cache"
    finally:
        finalize_run()


def test_is_cache_disabled(monkeypatch):
    monkeypatch.setenv("BRIDGE_EXPORT_CACHE", "0")
    assert not is_cache_enabled()
