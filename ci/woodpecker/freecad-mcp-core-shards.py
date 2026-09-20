#!/usr/bin/env python3
"""Partition pytest-mark 'core' files into CI shards.

Woodpecker used to run every native core test in one ~2.5 h step. A single
failure kept that whole step red until the end. These shards are meant to be
run sequentially (later shards depend_on earlier ones) so the pipeline can go
red after the first failing group.

Classification is filename-based and disjoint. Anything not collab/sketch/part
lands in rest, so a new test_native_*.py still runs.

Usage (from tools/mcp/freecad-mcp):

    python3 $CI_WORKSPACE/ci/woodpecker/freecad-mcp-core-shards.py --root . --shard collab
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

SHARDS = ("collab", "sketch", "part", "rest")

# Document lifecycle + collaboration + undo/redo. These were the 409 cluster.
_COLLAB_NAMES = frozenset(
    {
        "test_native_collaboration_api.py",
        "test_native_undo.py",
        "test_native_redo.py",
        "test_native_snapshot.py",
        "test_native_restore.py",
        "test_native_capture_state.py",
        "test_native_open_document.py",
        "test_native_close_document.py",
        "test_native_activate_document.py",
        "test_native_reload_document.py",
        "test_native_recompute_document.py",
        "test_native_recompute_and_wait.py",
        "test_native_create_document.py",
        "test_native_validate_movement_follow.py",
        "test_native_validate_geometry.py",
    }
)

_SKETCH_PREFIX = "test_native_sketch_"

# CAD mutations. Longer prefixes first is not required (startswith).
_PART_PREFIXES = (
    "test_native_pad_",
    "test_native_pocket_",
    "test_native_boolean_",
    "test_native_fillet_",
    "test_native_chamfer_",
    "test_native_loft_",
    "test_native_sweep_",
    "test_native_helical_",
    "test_native_linear_pattern_",
    "test_native_polar_pattern_",
    "test_native_mirror_",
    "test_native_revolve_",
    "test_native_create_",  # create_document is already collab
    "test_native_body_",
    "test_native_delete_",
    "test_native_edit_",
    "test_native_move_",
    "test_native_translate",
    "test_native_rotate",
    "test_native_scale",
    "test_native_insert_",
    "test_native_build_path_",
    "test_native_common_volume_",
    "test_native_solve_assembly",
    "test_native_preview_attachment",
)

# Direct native regressions that predate the tests/native naming convention.
# Keep their marker and this selector coupled: the Python-only unit lane has
# FreeCAD stubs, while these tests require real OCCT/Origin behavior.
_EXTRA_CORE_FILE_SHARDS = {
    "tests/test_link_array_placement_list.py": "part",
    "tests/test_object_property_postcondition.py": "part",
    "tests/test_pocket_requires_base_solid.py": "part",
    "tests/test_find_subshapes_filters.py": "rest",
    "tests/test_inspect_geometry_subshape.py": "rest",
    "tests/measure/test_world_shape_real_freecad.py": "rest",
}


def classify(name: str) -> str:
    for relative, shard in _EXTRA_CORE_FILE_SHARDS.items():
        if name == Path(relative).name:
            return shard
    if name in _COLLAB_NAMES:
        return "collab"
    if name.startswith(_SKETCH_PREFIX):
        return "sketch"
    if any(name.startswith(p) for p in _PART_PREFIXES):
        return "part"
    return "rest"


def core_files(root: Path) -> list[Path]:
    native = sorted(root.joinpath("tests", "native").glob("test_native_*.py"))
    extra = root.joinpath("tests", "e2e", "test_p1_cross_body_datum_placement.py")
    files = list(native)
    if extra.is_file():
        files.append(extra)
    for relative in _EXTRA_CORE_FILE_SHARDS:
        path = root / relative
        if not path.is_file():
            raise SystemExit(f"required direct-native core test is missing: {path}")
        if "pytestmark = pytest.mark.core" not in path.read_text(encoding="utf-8"):
            raise SystemExit(
                f"direct-native core test lost its core marker: {path}"
            )
        files.append(path)
    return files


def paths_for_shard(root: Path, shard: str) -> list[str]:
    if shard not in SHARDS:
        raise SystemExit(f"unknown CORE_SHARD={shard!r}; want one of {SHARDS}")
    out: list[str] = []
    for path in core_files(root):
        if classify(path.name) == shard:
            out.append(path.relative_to(root).as_posix())
    return out


def check(root: Path) -> int:
    files = core_files(root)
    if not files:
        print(f"no core files under {root}", file=sys.stderr)
        return 1
    buckets: dict[str, list[str]] = {s: [] for s in SHARDS}
    for path in files:
        buckets[classify(path.name)].append(path.name)
    rc = 0
    for shard in SHARDS:
        names = buckets[shard]
        print(f"{shard}: {len(names)}")
        if not names:
            print(f"  EMPTY SHARD {shard}", file=sys.stderr)
            rc = 1
    print(f"total: {len(files)}")
    return rc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--shard", choices=SHARDS)
    parser.add_argument(
        "--check",
        action="store_true",
        help="print shard sizes and fail if any shard is empty",
    )
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if args.check:
        return check(root)
    if not args.shard:
        parser.error("either --shard or --check is required")
    paths = paths_for_shard(root, args.shard)
    if not paths:
        print(f"CORE_SHARD={args.shard} selected 0 files under {root}", file=sys.stderr)
        return 1
    print(" ".join(paths))
    return 0


if __name__ == "__main__":
    sys.exit(main())
