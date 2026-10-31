"""Owner-thread presentation producers must not enter Coin's global notification state."""

from pathlib import Path
import re

import pytest

from test_geometry_native_adapters import _suppress_cpp

REPOSITORY = Path(__file__).resolve().parents[2]
CAPTURES = [
    Path(f"src/Mod/{module}/Gui/DocumentPresentationCapture.cpp")
    for module in ("Part", "Mesh", "Points")
]
# The OCC helpers used by the Part tessellator are part of the worker boundary too.
HELPERS = [
    Path("src/Mod/Part/Gui/ShapeGeometry.cpp"),
    Path("src/Mod/Part/Gui/ShapeGeometry.h"),
    Path("src/Mod/Part/App/Tools.cpp"),
    Path("src/Mod/Part/App/Tools.h"),
]


def violations(source):
    includes = _suppress_cpp(source, suppress_literals=False)
    code = _suppress_cpp(source)
    return (
        re.findall(
            r'#\s*include\s*[<"]([^>"\n]*(?:Inventor|So[A-Z]|ViewProvider)[^>"\n]*)[>"]',
            includes,
        )
        + re.findall(r"\b(?:So[A-Z]\w*|SbVec\w*)\b", code)
        + re.findall(r"\bsetupCoinGeometry\b", code)
    )


def included_headers(path, seen=None):
    seen = set() if seen is None else seen
    if path in seen:
        return
    seen.add(path)
    source = _suppress_cpp(
        (REPOSITORY / path).read_text(encoding="utf-8"), suppress_literals=False
    )
    for include in re.findall(r'^\s*#\s*include\s*[<"]([^>"\n]+)[>"]', source, re.M):
        for header in (path.parent / include, Path("src") / include):
            if (REPOSITORY / header).is_file() and header.suffix == ".h":
                yield header
                yield from included_headers(header, seen)
                break


@pytest.mark.parametrize("path", CAPTURES + HELPERS, ids=str)
def test_capture_and_tessellation_sources_are_coin_free(path):
    source = (REPOSITORY / path).read_text(encoding="utf-8")
    assert not violations(source), f"{path}: {violations(source)}"
    for header in included_headers(path):
        assert not violations((REPOSITORY / header).read_text(encoding="utf-8")), header
    # New local helpers cannot silently escape the boundary's checked file set.
    if path in CAPTURES or path in HELPERS[:2]:
        checked = set(CAPTURES + HELPERS)
        for include in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', source, re.M):
            header = path.parent / include
            if header.name == "DocumentPresentationCapture.h":
                assert not violations((REPOSITORY / header).read_text())
                continue
            assert header in checked, f"Add worker helper {header} to the Coin-free boundary"


def test_coin_scanner_rejects_notifications_and_nodes():
    assert violations('#include <Inventor/nodes/SoNormal.h>\n')
    assert violations('auto node = new SoCoordinate3; node->ref();')
    assert violations('SoDB::getSensorManager()->processImmediateQueue();')
    assert violations('makeNode<SoNormal>();')
    assert not violations('// new SoNormal\nconst char* text = "SoDB::init()";')
