#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Update config/runtime-versions.lock.yaml from a sim_runs/e2e_*/versions.yaml.

Usage:
  python scripts/sync_runtime_lock_from_versions.py sim_runs/e2e_20260529T143215Z/versions.yaml
  python scripts/sync_runtime_lock_from_versions.py sim_runs/e2e_*/versions.yaml --check
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any, Dict

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("versions_yaml", type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Print apt/base diffs only; do not write the lock file",
    )
    args = parser.parse_args()

    import yaml  # type: ignore

    from bridge.runtime_versions import load_runtime_lock, lock_path

    observed = yaml.safe_load(args.versions_yaml.read_text(encoding="utf-8")) or {}
    lock = load_runtime_lock(ROOT)
    docker = lock.setdefault("docker_e2e", {})

    changes: Dict[str, Any] = {}
    apt_obs = observed.get("apt_versions") or {}
    if apt_obs:
        old_apt = docker.get("apt_versions") or {}
        if apt_obs != old_apt:
            changes["apt_versions"] = apt_obs

    ref = observed.get("docker_base_image_builtin") or observed.get("docker_base_image_ref")
    if ref and "sha256:" in str(ref):
        digest = str(ref).split("@", 1)[-1]
        if docker.get("base_image_digest") != digest:
            changes["base_image_digest"] = digest

    if not changes:
        print("No lock updates needed.")
        return 0

    print("Proposed lock updates (docker_e2e):")
    for key, val in changes.items():
        print(f"  {key}: {val}")

    if args.check:
        return 0

    docker.update(changes)
    lock_path(ROOT).write_text(
        yaml.dump(lock, default_flow_style=False, sort_keys=False),
        encoding="utf-8",
    )
    print(f"Updated {lock_path(ROOT)}")
    print("Also update docker/Dockerfile.e2e ARG/FROM lines — see docs/docker-e2e-reproducibility.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
