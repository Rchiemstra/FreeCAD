"""
RobotCAD export cache keyed by FCStd document hash + export policy fingerprint.

Cache layout::

    generated/<robot>/.export_cache/
      index.yaml
      entries/<cache_key>/
        manifest.yaml
        <robot>_description/...

Hits skip FreeCADCmd when the cached tree is valid for the current FCStd and policy.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Optional

EXPORT_CACHE_SCHEMA = 1
INDEX_NAME = "index.yaml"
MANIFEST_NAME = "manifest.yaml"
ENTRIES_DIR = "entries"


def is_cache_enabled() -> bool:
    return os.environ.get("BRIDGE_EXPORT_CACHE", "1").strip().lower() not in (
        "0",
        "false",
        "no",
        "off",
    )


def should_invalidate() -> bool:
    return os.environ.get("BRIDGE_EXPORT_CACHE_INVALIDATE", "").strip().lower() in (
        "1",
        "true",
        "yes",
    )


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def export_policy_fingerprint(root: Optional[Path] = None) -> Dict[str, Any]:
    """Version/policy inputs that affect RobotCAD export output."""
    base = root or Path(__file__).resolve().parents[1]
    policy: Dict[str, Any] = {
        "export_cache_schema": EXPORT_CACHE_SCHEMA,
        "headless_exporter": "robotcad_headless.export_fcstd_to_urdf",
    }
    try:
        from bridge.runtime_versions import load_runtime_lock

        lock = load_runtime_lock(base)
        robotcad = (lock.get("docker_e2e") or {}).get("robotcad") or {}
        if isinstance(robotcad, dict):
            policy["robotcad_commit"] = robotcad.get("commit", "")
            policy["cross_version"] = robotcad.get("cross_version", "")
        policy["lock_updated"] = lock.get("updated", "")
    except Exception:
        policy["robotcad_commit"] = os.environ.get("ROBOTCAD_COMMIT", "")
    return policy


def build_cache_key(
    robot_name: str,
    fcstd_sha256: str,
    *,
    root: Optional[Path] = None,
) -> str:
    payload = {
        "robot": robot_name,
        "fcstd_sha256": fcstd_sha256.lower(),
        "policy": export_policy_fingerprint(root),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def cache_root(generated_dir: Path) -> Path:
    return Path(generated_dir).resolve() / ".export_cache"


def _index_path(generated_dir: Path) -> Path:
    return cache_root(generated_dir) / INDEX_NAME


def _entry_dir(generated_dir: Path, cache_key: str) -> Path:
    return cache_root(generated_dir) / ENTRIES_DIR / cache_key


def _load_yaml(path: Path) -> Dict[str, Any]:
    import yaml  # type: ignore

    if not path.is_file():
        return {}
    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    return data if isinstance(data, dict) else {}


def _write_yaml(path: Path, data: Dict[str, Any]) -> None:
    import yaml  # type: ignore

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        yaml.dump(data, default_flow_style=False, sort_keys=False),
        encoding="utf-8",
    )


def _log_cache_event(message: str, **fields: Any) -> None:
    try:
        from bridge.run_context import record_event

        record_event("export_cache", message, **fields)
    except Exception:
        pass


def record_fcstd_source(fcstd_path: Path) -> str:
    """Hash FCStd and attach to active run metadata when present."""
    digest = sha256_file(fcstd_path)
    try:
        from bridge.run_context import current_run

        ctx = current_run()
        if ctx is not None:
            ctx.source_hashes["fcstd"] = digest
            ctx.add_file_hash("fcstd", fcstd_path)
    except Exception:
        pass
    return digest


def invalidate_cache(generated_dir: Path, *, cache_key: Optional[str] = None) -> None:
    """Remove cache entries (all, or one key)."""
    root = cache_root(generated_dir)
    if cache_key is None:
        if root.is_dir():
            shutil.rmtree(root, ignore_errors=True)
        _log_cache_event("cache_invalidated", scope="all", generated=str(generated_dir))
        return

    entry = _entry_dir(generated_dir, cache_key)
    if entry.is_dir():
        shutil.rmtree(entry, ignore_errors=True)
    idx = _load_yaml(_index_path(generated_dir))
    entries = idx.get("entries") or {}
    if isinstance(entries, dict) and cache_key in entries:
        del entries[cache_key]
        idx["entries"] = entries
        _write_yaml(_index_path(generated_dir), idx)
    _log_cache_event("cache_invalidated", cache_key=cache_key)


def _description_dir_name(robot_name: str) -> str:
    return f"{robot_name}_description"


def _copy_description_tree(src_root: Path, dest_root: Path, robot_name: str) -> None:
    """Copy ``<robot>_description`` tree from src export dir into dest export dir."""
    name = _description_dir_name(robot_name)
    src = src_root / name
    if not src.is_dir():
        raise FileNotFoundError(f"Missing cached description tree: {src}")
    dest = dest_root / name
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest)


def try_restore_cached_export(
    robot_name: str,
    fcstd_path: Path,
    generated_dir: Path,
    *,
    fcstd_sha256: Optional[str] = None,
    root: Optional[Path] = None,
) -> Optional[Path]:
    """
    Restore RobotCAD export from cache when valid.

    Returns canonical URDF path on hit, else ``None``.
    """
    if not is_cache_enabled():
        return None

    from bridge.freecad_bridge import expected_exported_urdf_path

    gen = Path(generated_dir).resolve()
    fcstd = Path(fcstd_path).resolve()
    if not fcstd.is_file():
        return None

    if should_invalidate():
        invalidate_cache(gen)
        _log_cache_event("cache_miss", reason="invalidation_requested")
        return None

    digest = (fcstd_sha256 or sha256_file(fcstd)).lower()
    record_fcstd_source(fcstd)
    key = build_cache_key(robot_name, digest, root=root)
    entry = _entry_dir(gen, key)
    manifest_path = entry / MANIFEST_NAME
    urdf = expected_exported_urdf_path(robot_name, gen)

    if not manifest_path.is_file():
        _log_cache_event(
            "cache_miss",
            cache_key=key,
            fcstd_sha256=digest,
            reason="no_entry",
        )
        return None

    manifest = _load_yaml(manifest_path)
    if str(manifest.get("fcstd_sha256", "")).lower() != digest:
        invalidate_cache(gen, cache_key=key)
        _log_cache_event(
            "cache_miss",
            cache_key=key,
            fcstd_sha256=digest,
            reason="fcstd_hash_mismatch",
        )
        return None

    if str(manifest.get("cache_key", "")) != key:
        invalidate_cache(gen, cache_key=key)
        _log_cache_event("cache_miss", cache_key=key, reason="policy_mismatch")
        return None

    cached_desc = entry / _description_dir_name(robot_name)
    if not cached_desc.is_dir():
        invalidate_cache(gen, cache_key=key)
        _log_cache_event("cache_miss", cache_key=key, reason="missing_tree")
        return None

    try:
        gen.mkdir(parents=True, exist_ok=True)
        _copy_description_tree(entry, gen, robot_name)
    except OSError as exc:
        _log_cache_event("cache_miss", cache_key=key, reason=f"restore_failed:{exc}")
        return None

    if not urdf.is_file():
        invalidate_cache(gen, cache_key=key)
        _log_cache_event("cache_miss", cache_key=key, reason="urdf_missing_after_restore")
        return None

    _log_cache_event(
        "cache_hit",
        cache_key=key,
        fcstd_sha256=digest,
        urdf=str(urdf),
    )
    return urdf


def store_cached_export(
    robot_name: str,
    fcstd_path: Path,
    generated_dir: Path,
    urdf_path: Path,
    *,
    fcstd_sha256: Optional[str] = None,
    root: Optional[Path] = None,
) -> str:
    """Persist export tree under ``.export_cache``; return cache key."""
    gen = Path(generated_dir).resolve()
    fcstd = Path(fcstd_path).resolve()
    digest = (fcstd_sha256 or sha256_file(fcstd)).lower()
    record_fcstd_source(fcstd)
    key = build_cache_key(robot_name, digest, root=root)
    entry = _entry_dir(gen, key)
    desc_name = _description_dir_name(robot_name)
    src_desc = gen / desc_name

    if entry.exists():
        shutil.rmtree(entry)
    entry.mkdir(parents=True, exist_ok=True)

    if src_desc.is_dir():
        shutil.copytree(src_desc, entry / desc_name)

    manifest = {
        "schema_version": EXPORT_CACHE_SCHEMA,
        "cache_key": key,
        "robot": robot_name,
        "fcstd_path": str(fcstd),
        "fcstd_sha256": digest,
        "policy": export_policy_fingerprint(root),
        "urdf_path": str(Path(urdf_path).resolve()),
        "stored_at": datetime.now(timezone.utc).isoformat(),
    }
    _write_yaml(entry / MANIFEST_NAME, manifest)

    idx = _load_yaml(_index_path(gen))
    entries = idx.get("entries")
    if not isinstance(entries, dict):
        entries = {}
    entries[key] = {
        "fcstd_sha256": digest,
        "stored_at": manifest["stored_at"],
        "urdf_path": manifest["urdf_path"],
    }
    idx["schema_version"] = EXPORT_CACHE_SCHEMA
    idx["entries"] = entries
    _write_yaml(_index_path(gen), idx)

    _log_cache_event(
        "cache_store",
        cache_key=key,
        fcstd_sha256=digest,
        urdf=str(urdf_path),
    )
    return key
