# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the committed ViewProvider updateData provider inventory."""

from __future__ import annotations

import re
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OUTPUT = Path(__file__).resolve().parent / "view_provider_updatedata_inventory.md"

EXCLUDED_WORKBENCHES = frozenset({"Test", "TemplatePyMod"})
CPP_IMPL = re.compile(
    r"void\s+((?:\w+::)*\w+)::updateData\s*\([^)]*App::Property",
    re.MULTILINE,
)
CPP_INLINE = re.compile(
    r"void\s+updateData\s*\(\s*const\s+App::Property[^)]*\)\s*(?:override)?\s*\{",
    re.MULTILINE,
)
PY_PROVIDER = re.compile(
    r"^\s*def\s+updateData\s*\(\s*self\s*,\s*\w+\s*,\s*\w+",
    re.MULTILINE,
)


def _production_gui_sources() -> list[Path]:
    paths: list[Path] = []
    paths.extend(REPO_ROOT.joinpath("src", "Gui").rglob("*"))
    mod_root = REPO_ROOT / "src" / "Mod"
    if mod_root.is_dir():
        for module in mod_root.iterdir():
            if not module.is_dir() or module.name in EXCLUDED_WORKBENCHES:
                continue
            gui = module / "Gui"
            if gui.is_dir():
                paths.extend(gui.rglob("*"))
    return sorted(
        {
            path
            for path in paths
            if path.suffix.lower() in {".cpp", ".h", ".hpp", ".py"}
            and "_TEMPLATE_" not in path.parts
        }
    )


def _class_name_from_source(text: str, line_index: int) -> str | None:
    lines = text.splitlines()
    for index in range(line_index, max(-1, line_index - 80), -1):
        match = re.search(r"\b(?:class|struct)\s+(?:(?:\w+::)*)(\w+)", lines[index])
        if match:
            return match.group(1)
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
    for match in PY_PROVIDER.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
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
        "Production presentation providers discovered under `src/Gui` and "
        "`src/Mod/*/Gui`. Every row is classified `unclassified` until Wave 3 "
        "migration assigns a pointer-free adapter or explicit `unsupported` "
        "capability.\n\n"
        "| file | symbol/caller | line | classification |\n"
        "| --- | --- | ---: | --- |\n"
    )
    body = "\n".join(
        f"| `{file}` | `{symbol}` | {line} | unclassified |"
        for file, symbol, line in rows
    )
    return f"{header}{body}\n"


def main() -> None:
    rows = _collect_rows()
    OUTPUT.write_text(_render(rows), encoding="utf-8")
    print(f"wrote {len(rows)} rows to {OUTPUT.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
