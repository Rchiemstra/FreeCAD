# SPDX-License-Identifier: LGPL-2.1-or-later
"""Wave 3 classifications for ViewProvider updateData inventory rows."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Final

ARCH_DIR = Path(__file__).resolve().parent
CLASSIFICATIONS_PATH = ARCH_DIR / "view_provider_updatedata_classifications.json"

VALID_CLASSIFICATIONS: Final[frozenset[str]] = frozenset({"adapted", "unsupported"})

# Only providers with a real pointer-free capture override are Adapted.
# GeometryObject/Part base stubs and Coin-only Sketch paths stay Unsupported.
_ADAPTED_GUI_FILES = frozenset()

_ADAPTED_PART_GUI_FILES = frozenset(
    {
        "src/Mod/Part/Gui/ViewProvider2DObject.cpp",
        "src/Mod/Part/Gui/ViewProviderBoolean.cpp",
        "src/Mod/Part/Gui/ViewProviderCompound.cpp",
        "src/Mod/Part/Gui/ViewProviderCurveNet.cpp",
        "src/Mod/Part/Gui/ViewProviderExt.cpp",
        "src/Mod/Part/Gui/ViewProviderMirror.cpp",
        "src/Mod/Part/Gui/ViewProviderRuledSurface.cpp",
    }
)

_UNSUPPORTED_PART_GUI = frozenset(
    {
        ("src/Mod/Part/Gui/CrossSections.cpp", "ViewProviderCrossSections"),
        ("src/Mod/Part/Gui/ViewProviderPython.cpp", "ViewProviderCustom"),
    }
)

_ADAPTED_MESH_GUI_FILES = frozenset(
    {
        "src/Mod/Mesh/Gui/ViewProvider.cpp",
        "src/Mod/Mesh/Gui/ViewProviderMeshFaceSet.cpp",
        "src/Mod/Mesh/Gui/ViewProviderTransform.cpp",
    }
)

_UNSUPPORTED_MESH = frozenset(
    {
        ("src/Mod/Mesh/Gui/ViewProviderCurvature.cpp", "ViewProviderMeshCurvature"),
    }
)

_ADAPTED_PARTDESIGN_PREFIX = "src/Mod/PartDesign/Gui/"


def classify_provider(file: str, symbol: str, line: int) -> str:
    """Return ``adapted`` or ``unsupported`` for one inventoried provider row."""
    key = (file, symbol)
    if key in _UNSUPPORTED_PART_GUI or key in _UNSUPPORTED_MESH:
        return "unsupported"
    if file in _ADAPTED_GUI_FILES or file in _ADAPTED_PART_GUI_FILES:
        return "adapted"
    if file in _ADAPTED_MESH_GUI_FILES:
        return "adapted"
    if file == "src/Mod/Points/Gui/ViewProvider.cpp":
        return "adapted"
    # Sketcher and PartDesign still rely on Coin/live geometry — Unsupported until
    # they ship pointer-free buffers (Wave 3 inventory must match C++).
    if file == "src/Mod/Sketcher/Gui/ViewProviderSketch.cpp" and symbol == "ViewProviderSketch":
        return "unsupported"
    if file.startswith(_ADAPTED_PARTDESIGN_PREFIX):
        return "unsupported"
    if file == "src/Gui/ViewProviderLink.cpp":
        return "unsupported"
    if file == "src/Mod/Sketcher/Gui/ViewProviderPython.cpp":
        return "unsupported"
    return "unsupported"


def _entry_key(entry: dict[str, object]) -> tuple[str, str, int]:
    return (str(entry["file"]), str(entry["symbol"]), int(entry["line"]))


def load_sidecar_overrides() -> dict[tuple[str, str, int], str]:
    if not CLASSIFICATIONS_PATH.is_file():
        return {}
    payload = json.loads(CLASSIFICATIONS_PATH.read_text(encoding="utf-8"))
    overrides: dict[tuple[str, str, int], str] = {}
    for entry in payload.get("overrides", []):
        classification = str(entry["classification"])
        if classification not in VALID_CLASSIFICATIONS:
            raise ValueError(f"invalid sidecar classification {classification!r}")
        overrides[_entry_key(entry)] = classification
    return overrides


def resolve_classification(
    file: str,
    symbol: str,
    line: int,
    *,
    overrides: dict[tuple[str, str, int], str] | None = None,
) -> str:
    key = (file, symbol, line)
    sidecar = overrides if overrides is not None else load_sidecar_overrides()
    if key in sidecar:
        return sidecar[key]
    return classify_provider(file, symbol, line)
