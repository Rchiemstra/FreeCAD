# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the committed ViewProvider updateData provider inventory."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ARCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = ARCH_DIR.parents[1]
OUTPUT = ARCH_DIR / "view_provider_updatedata_inventory.md"

sys.path.insert(0, str(ARCH_DIR))

from gui_blocking_live_model.scanner import (  # noqa: E402
    _python_update_data_provider_matches,
    iter_source_files,
)
from view_provider_updatedata_classifications import resolve_classification  # noqa: E402

CPP_IMPL = re.compile(
    r"void\s+((?:\w+::)*\w+)::updateData\s*\([^)]*App::Property",
    re.MULTILINE,
)
CPP_INLINE = re.compile(
    r"void\s+updateData\s*\(\s*const\s+App::Property[^)]*\)\s*(?:override)?\s*\{",
    re.MULTILINE,
)
def _production_gui_sources() -> list[Path]:
    return [
        path
        for path in iter_source_files(REPO_ROOT)
        if path.suffix.lower() in {".cpp", ".h", ".hpp", ".py"}
    ]


def _class_name_from_source(text: str, line_index: int) -> str | None:
    """Return the nearest enclosing class/struct name above ``line_index``.

    Only definition-like lines count (``class Name`` / ``struct Name`` at the
    start of a line after optional indent). Prose such as ``class docstring``
    or ``class for`` inside comments/docstrings is ignored. The search walks
    to the top of the file and prefers a class whose indent is strictly less
    than the provider line (so nested ``def updateData`` finds its enclosing
    ViewProvider class even when it is more than 80 lines above).
    """
    lines = text.splitlines()
    if line_index < 0 or line_index >= len(lines):
        return None
    definition = re.compile(r"^(\s*)(?:class|struct)\s+(?:(?:\w+::)*)(\w+)\b")
    target_indent = len(lines[line_index]) - len(lines[line_index].lstrip())
    for index in range(line_index, -1, -1):
        match = definition.search(lines[index])
        if not match:
            continue
        indent = len(match.group(1))
        if indent < target_indent or (indent == 0 and target_indent == 0):
            return match.group(2)
    return None


def _collect_cpp_providers(path: Path, text: str) -> list[tuple[str, str, int]]:
    relative = path.relative_to(REPO_ROOT).as_posix()
    rows: list[tuple[str, str, int]] = []
    for match in CPP_IMPL.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        rows.append((relative, match.group(1), line))
    if path.suffix.lower() == ".cpp":
        for match in CPP_INLINE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            class_name = _class_name_from_source(text, line - 1)
            if class_name:
                rows.append((relative, class_name, line))
    return rows


def _collect_python_providers(path: Path, text: str) -> list[tuple[str, str, int]]:
    if path.suffix.lower() != ".py":
        return []
    relative = path.relative_to(REPO_ROOT).as_posix()
    rows: list[tuple[str, str, int]] = []
    for line, _evidence in _python_update_data_provider_matches(text, relative):
        class_name = _class_name_from_source(text, line - 1) or path.stem
        rows.append((relative, class_name, line))
    return rows


def _collect_rows() -> list[tuple[str, str, int]]:
    rows: list[tuple[str, str, int]] = []
    for path in _production_gui_sources():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        rows.extend(_collect_cpp_providers(path, text))
        rows.extend(_collect_python_providers(path, text))
    return sorted({(file, symbol, line) for file, symbol, line in rows})


def _render(rows: list[tuple[str, str, int]]) -> str:
    header = (
        "# ViewProvider updateData provider inventory\n\n"
        "Production presentation providers discovered under `src/Gui`, every "
        "`src/Mod/*/Gui`, and the reviewed module-aware Python GUI packages "
        "(Draft, BIM, Assembly, Fem, OpenSCAD, CAM, and peers). Wave 3 "
        "classifies every row as `adapted` (pointer-free presentation adapter) "
        "or `unsupported` (explicit capability; no synchronous updateData fallback).\n\n"
        "| file | symbol/caller | line | classification |\n"
        "| --- | --- | ---: | --- |\n"
    )
    body = "\n".join(
        f"| `{file}` | `{symbol}` | {line} | {resolve_classification(file, symbol, line)} |"
        for file, symbol, line in rows
    )
    return f"{header}{body}\n"


def main() -> None:
    rows = _collect_rows()
    OUTPUT.write_text(_render(rows), encoding="utf-8")
    print(f"wrote {len(rows)} rows to {OUTPUT.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
