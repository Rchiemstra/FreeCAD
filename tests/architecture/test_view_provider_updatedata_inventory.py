# SPDX-License-Identifier: LGPL-2.1-or-later
"""Wave 0 inventory gate for production ViewProvider updateData providers."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys


ARCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = ARCH_DIR.parents[1]
INVENTORY_PATH = ARCH_DIR / "view_provider_updatedata_inventory.md"
GENERATOR = ARCH_DIR / "generate_view_provider_updatedata_inventory.py"

sys.path.insert(0, str(ARCH_DIR))

from gui_blocking_live_model.scanner import (  # noqa: E402
    _python_update_data_provider_matches,
    iter_source_files,
)

INVENTORY_HEADER = ("file", "symbol/caller", "line", "classification")
CPP_IMPL = re.compile(
    r"void\s+((?:\w+::)*\w+)::updateData\s*\([^)]*App::Property",
    re.MULTILINE,
)
CPP_INLINE = re.compile(
    r"void\s+updateData\s*\(\s*const\s+App::Property[^)]*\)\s*(?:override)?\s*\{",
    re.MULTILINE,
)

@dataclass(frozen=True)
class InventoryRow:
    number: int
    file: str
    symbol: str
    line: int
    classification: str

    @property
    def source_path(self) -> str:
        return self.file.strip("`")

    @property
    def stable_symbol(self) -> str:
        return self.symbol.strip("`")

    def key(self) -> tuple[str, str, int]:
        return (self.source_path, self.stable_symbol, self.line)

    def diagnostic(self, message: str) -> str:
        return f"{self.source_path}:{self.line} {self.stable_symbol}: {message}"


def _parse_inventory(text: str) -> list[InventoryRow]:
    rows: list[InventoryRow] = []
    in_table = False
    for number, line in enumerate(text.splitlines(), 1):
        if not line.startswith("|"):
            if in_table and rows:
                break
            continue
        cells = tuple(cell.strip() for cell in line.strip("|").split("|"))
        if cells == INVENTORY_HEADER:
            in_table = True
            continue
        if not in_table or len(cells) != len(INVENTORY_HEADER):
            continue
        if all(re.fullmatch(r":?-+:?", cell) for cell in cells):
            continue
        file, symbol, line_text, classification = cells
        rows.append(
            InventoryRow(
                number,
                file,
                symbol,
                int(line_text),
                classification,
            )
        )
    return rows


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


def _discover_providers() -> set[tuple[str, str, int]]:
    discovered: set[tuple[str, str, int]] = set()
    for path in _production_gui_sources():
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        relative = path.relative_to(REPO_ROOT).as_posix()
        for match in CPP_IMPL.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            discovered.add((relative, match.group(1), line))
        if path.suffix.lower() == ".cpp":
            for match in CPP_INLINE.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                class_name = _class_name_from_source(text, line - 1)
                if class_name:
                    discovered.add((relative, class_name, line))
        if path.suffix.lower() == ".py":
            for line, _evidence in _python_update_data_provider_matches(text, relative):
                class_name = _class_name_from_source(text, line - 1) or path.stem
                discovered.add((relative, class_name, line))
    return discovered


def _inventory_shape_violations(rows: list[InventoryRow]) -> list[str]:
    violations: list[str] = []
    keys = [row.key() for row in rows]
    if len(keys) != len(set(keys)):
        violations.append("<inventory>: duplicate file/symbol/line rows")
    for row in rows:
        if row.classification != "unclassified":
            violations.append(row.diagnostic(f"classification is {row.classification!r}, expected unclassified"))
        if not (REPO_ROOT / row.source_path).is_file():
            violations.append(row.diagnostic("named source file does not exist"))
        source = (REPO_ROOT / row.source_path).read_text(encoding="utf-8", errors="surrogateescape")
        lines = source.splitlines()
        if row.line <= 0 or row.line > len(lines):
            violations.append(row.diagnostic("line is outside the source file"))
            continue
        if "updateData" not in lines[row.line - 1]:
            neighborhood = "\n".join(
                lines[max(0, row.line - 3) : min(len(lines), row.line + 2)]
            )
            if "updateData" not in neighborhood:
                violations.append(row.diagnostic("line is not an updateData provider anchor"))
    return violations


def test_inventory_lists_only_unclassified_rows() -> None:
    rows = _parse_inventory(INVENTORY_PATH.read_text(encoding="utf-8"))
    assert rows, "inventory table is empty"
    violations = _inventory_shape_violations(rows)
    assert not violations, "inventory shape violations:\n" + "\n".join(violations)


def test_every_production_provider_is_inventoried() -> None:
    rows = _parse_inventory(INVENTORY_PATH.read_text(encoding="utf-8"))
    inventoried = {row.key() for row in rows}
    discovered = _discover_providers()
    missing = sorted(discovered - inventoried)
    stale = sorted(inventoried - discovered)
    failures: list[str] = []
    for file, symbol, line in missing:
        failures.append(f"missing inventory row for {file}:{line} {symbol}")
    for file, symbol, line in stale:
        failures.append(f"stale inventory row for {file}:{line} {symbol}")
    assert not failures, "updateData provider inventory drift:\n" + "\n".join(failures)


def test_generator_reproduces_committed_inventory() -> None:
    before = INVENTORY_PATH.read_text(encoding="utf-8")
    completed = subprocess.run(
        [sys.executable, str(GENERATOR)],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    assert completed.returncode == 0, completed.stderr or completed.stdout
    after = INVENTORY_PATH.read_text(encoding="utf-8")
    INVENTORY_PATH.write_text(before, encoding="utf-8")
    assert before == after, "committed inventory does not match generator output"
