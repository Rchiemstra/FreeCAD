# SPDX-License-Identifier: LGPL-2.1-or-later

"""Typed public signatures for the ``Sketcher`` module-level helpers.

This source-adjacent stub file carries the standalone sketch import helpers
exposed directly by the Sketcher application module.
"""

from __future__ import annotations

from Sketcher.App.SketchObject import SketchObject

def open(name: str, /) -> None:
    """Open one sketch import file through the Sketcher module entry point."""
    ...

def insert(name: str, doc_name: str, /) -> None:
    """Insert one `.skf` sketch flat file into the named document."""
    ...


def listLinkedOffsets(sketch: SketchObject, /) -> list[dict[str, object]]:
    """Inspect persistent relations, stale snapshots and explicit-refresh refusal reasons."""
    ...

def refreshLinkedOffsets(sketch: SketchObject, ids: list[str] | tuple[str, ...] | None = None,
                         /) -> None:
    """Explicitly refresh selected UUIDs, or all relations with None, in one transaction."""
    ...

def editLinkedOffsets(sketch: SketchObject, ids: list[str] | tuple[str, ...] | None,
                      distance: float, tolerance: float, /) -> None:
    """Set signed distance/tolerance in mm and refresh; refusals raise ValueError atomically."""
    ...

def detachLinkedOffsets(sketch: SketchObject, ids: list[str] | tuple[str, ...] | None = None,
                        /) -> None:
    """Remove relation ownership and its proven Blocks, preserving geometry and user constraints."""
    ...
