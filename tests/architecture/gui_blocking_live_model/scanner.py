# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reproducible scanner for GUI blocking and live-model ingress.

The scanner walks the production GUI source (``src/Gui`` and every ``Gui``
directory under ``src/Mod``, C++ and Python alike), masks comments and string/
char/raw literals per language, then applies the category search rules from
:mod:`rules`. Its output is a deterministically sorted list of findings, each
recording the repository-relative path, 1-based line, category key, owning
subsystem, migration disposition, and the matched source line(s) as evidence.

This module is both importable (used by the validator) and runnable as a
standalone reproducibility command::

    python3 tests/architecture/gui_blocking_live_model/scanner.py

    FREECAD_SOURCE_ROOT=/path/to/repo \\
        python3 tests/architecture/gui_blocking_live_model/scanner.py --write

The ``--write`` flag writes ``inventory.json`` next to this module, which is the
committed machine-readable snapshot.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path

try:
    from . import rules
except ImportError:  # pragma: no cover - direct script execution
    import rules  # type: ignore

REPOSITORY_ROOT = Path(
    os.environ.get("FREECAD_SOURCE_ROOT", Path(__file__).resolve().parents[3])
).resolve()


class Finding:
    """One candidate or inventoried site."""

    __slots__ = ("category", "disposition", "evidence", "line", "path", "subsystem")

    def __init__(
        self, path: str, line: int, category: str, subsystem: str, disposition: str, evidence: str
    ) -> None:
        self.path = path
        self.line = line
        self.category = category
        self.subsystem = subsystem
        self.disposition = disposition
        self.evidence = evidence

    def key(self) -> tuple[str, int, str]:
        return (self.path, self.line, self.category)

    def to_dict(self) -> dict[str, object]:
        return {
            "path": self.path,
            "line": self.line,
            "category": self.category,
            "subsystem": self.subsystem,
            "disposition": self.disposition,
            "evidence": self.evidence,
        }

    @classmethod
    def from_dict(cls, data: dict[str, object]) -> Finding:
        return cls(
            path=str(data["path"]),
            line=int(data["line"]),
            category=str(data["category"]),
            subsystem=str(data["subsystem"]),
            disposition=str(data["disposition"]),
            evidence=str(data["evidence"]),
        )


def _mask_range(buffer: list[str], source: str, start: int, end: int) -> None:
    for index in range(start, end):
        if source[index] not in "\r\n":
            buffer[index] = " "


def _quoted_literal_end(source: str, start: int) -> int:
    quote = source[start]
    index = start + 1
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source[index] == quote:
            return index + 1
        index += 1
    return len(source)


def _raw_literal_end(source: str, start: int) -> int | None:
    delimiter_start = start + 2
    delimiter_limit = min(len(source), delimiter_start + 17)
    opening_parenthesis = source.find("(", delimiter_start, delimiter_limit)
    if opening_parenthesis < 0:
        return None
    delimiter = source[delimiter_start:opening_parenthesis]
    if any(character.isspace() or character in "\\()" for character in delimiter):
        return None
    terminator = f'){delimiter}"'
    terminator_start = source.find(terminator, opening_parenthesis + 1)
    if terminator_start < 0:
        return len(source)
    return terminator_start + len(terminator)


def mask_cpp_non_code(source: str) -> str:
    """Replace C++ comments and literals with spaces while retaining newlines."""
    masked = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            _mask_range(masked, source, index, end)
            index = end
        elif source.startswith("/*", index):
            closing = source.find("*/", index + 2)
            end = len(source) if closing < 0 else closing + 2
            _mask_range(masked, source, index, end)
            index = end
        elif source.startswith('R"', index):
            end = _raw_literal_end(source, index)
            if end is None:
                index += 1
                continue
            _mask_range(masked, source, index, end)
            index = end
        elif source[index] in "\"'":
            end = _quoted_literal_end(source, index)
            _mask_range(masked, source, index, end)
            index = end
        else:
            index += 1
    result = "".join(masked)
    assert len(result) == len(source)
    assert result.count("\n") == source.count("\n")
    return result


def _triple_quoted_end(source: str, start: int, quote: str) -> int:
    terminator = quote * 3
    index = start + 3
    while index < len(source):
        if source[index] == "\\":
            index += 2
            continue
        if source.startswith(terminator, index):
            return index + 3
        index += 1
    return len(source)


def mask_py_non_code(source: str) -> str:
    """Replace Python comments and string literals with spaces, keeping newlines.

    Handles ``#`` line comments and single-/double-/triple-quoted string literals
    (including ``f``/``r``/``b``/``u`` prefixes, which are inert letters and left
    unmasked). A ``#`` inside a string is consumed by the string handler first, so
    it is never misread as a comment.
    """
    masked = list(source)
    index = 0
    while index < len(source):
        char = source[index]
        if char == "#":
            end = source.find("\n", index + 1)
            end = len(source) if end < 0 else end
            _mask_range(masked, source, index, end)
            index = end
        elif char in "\"'":
            if source.startswith(char * 3, index):
                end = _triple_quoted_end(source, index, char)
            else:
                end = _quoted_literal_end(source, index)
            _mask_range(masked, source, index, end)
            index = end
        else:
            index += 1
    result = "".join(masked)
    assert len(result) == len(source)
    assert result.count("\n") == source.count("\n")
    return result


def mask_source(source: str, suffix: str) -> str:
    """Mask comments/literals for the source's language based on its suffix."""
    if suffix == ".py":
        return mask_py_non_code(source)
    return mask_cpp_non_code(source)


def _language_for(suffix: str) -> str:
    return "py" if suffix == ".py" else "cpp"


_COMPILED: dict[tuple[str, str], re.Pattern[str] | None] = {}
for _category in rules.CATEGORIES:
    _COMPILED[(_category.key, "cpp")] = (
        re.compile(_category.cpp_pattern) if _category.cpp_pattern else None
    )
    _COMPILED[(_category.key, "py")] = (
        re.compile(_category.py_pattern) if _category.py_pattern else None
    )


def compiled_pattern(category_key: str, language: str) -> re.Pattern[str] | None:
    """Return the compiled regex for a category/language, or None if absent."""
    return _COMPILED[(category_key, language)]


def subsystem_for(relative_path: str) -> str:
    """Derive the owning subsystem from a repository-relative path."""
    parts = Path(relative_path).parts
    if parts and parts[0] == "src":
        if len(parts) >= 2 and parts[1] == "Gui":
            return "Gui"
        if len(parts) >= 3 and parts[1] == "Mod":
            return parts[2]
    return "Other"


def _is_excluded_dir(path: Path, root: Path) -> bool:
    return any(part in rules.EXCLUDED_DIR_NAMES for part in path.relative_to(root).parts)


def _is_excluded_file(relative_path: str) -> bool:
    return relative_path in rules.EXCLUDED_FILES


def iter_source_files(repository_root: Path) -> list[Path]:
    """Return the sorted list of C++/Python GUI source files in scope."""
    files: list[Path] = []
    gui_root = repository_root / "src" / "Gui"
    if gui_root.is_dir():
        files.extend(gui_root.rglob("*"))

    mod_root = repository_root / "src" / "Mod"
    mod_gui_roots = sorted(mod_root.rglob("Gui")) if mod_root.is_dir() else []
    for gui_dir in mod_gui_roots:
        workbench = gui_dir.relative_to(mod_root).parts[0]
        if workbench in rules.EXCLUDED_WORKBENCHES:
            continue
        files.extend(gui_dir.rglob("*"))

    result: list[Path] = []
    for path in files:
        if not path.is_file():
            continue
        if path.suffix not in rules.SOURCE_SUFFIXES:
            continue
        if _is_excluded_dir(path, repository_root):
            continue
        if _is_excluded_file(path.relative_to(repository_root).as_posix()):
            continue
        result.append(path)
    return sorted(result)


def evidence_for(source: str, source_lines: list[str], start: int, end: int) -> str:
    """Return whitespace-normalised evidence spanning the matched line(s)."""
    start_line = source.count("\n", 0, start)
    end_line = source.count("\n", 0, max(start, end - 1))
    parts = [source_lines[line].strip() for line in range(start_line, end_line + 1)]
    return " ".join(parts)


def scan_source(source: str, suffix: str, relative_path: str) -> list[Finding]:
    """Scan one in-memory source string and return its findings (unsorted)."""
    masked = mask_source(source, suffix)
    source_lines = source.splitlines()
    subsystem = subsystem_for(relative_path)
    language = _language_for(suffix)
    findings: list[Finding] = []
    for category in rules.CATEGORIES:
        compiled = _COMPILED[(category.key, language)]
        if compiled is None:
            continue
        for match in compiled.finditer(masked):
            line = source.count("\n", 0, match.start()) + 1
            evidence = evidence_for(source, source_lines, match.start(), match.end())
            findings.append(
                Finding(
                    path=relative_path,
                    line=line,
                    category=category.key,
                    subsystem=subsystem,
                    disposition=category.default_disposition,
                    evidence=evidence,
                )
            )
    return findings


def scan(repository_root: Path) -> list[Finding]:
    """Scan the GUI source and return a deterministically sorted finding list."""
    findings: list[Finding] = []
    for source_path in iter_source_files(repository_root):
        relative_path = source_path.relative_to(repository_root).as_posix()
        source = source_path.read_text(encoding="utf-8", errors="surrogateescape")
        findings.extend(scan_source(source, source_path.suffix, relative_path))
    findings.sort(key=lambda finding: finding.key())
    # Collapse multiple regex matches on the same line for the same category
    # into a single finding: a finding is identified by (path, line, category).
    unique: list[Finding] = []
    seen: set[tuple[str, int, str]] = set()
    for finding in findings:
        if finding.key() in seen:
            continue
        seen.add(finding.key())
        unique.append(finding)
    return unique


def scan_to_dicts(repository_root: Path) -> list[dict[str, object]]:
    return [finding.to_dict() for finding in scan(repository_root)]


EXCLUSIONS_PATH = Path(__file__).resolve().parent / "exclusions.json"


def load_exclusions() -> list[dict[str, object]]:
    """Load the committed false-positive exclusions, or [] when absent."""
    if not EXCLUSIONS_PATH.is_file():
        return []
    data = json.loads(EXCLUSIONS_PATH.read_text(encoding="utf-8"))
    return list(data.get("exclusions", []))


def apply_exclusions(findings: list[Finding], exclusions: list[dict[str, object]]) -> list[Finding]:
    """Return findings with the excluded (path, line, category) keys removed."""
    excluded = {
        (str(entry["path"]), int(entry["line"]), str(entry["category"])) for entry in exclusions
    }
    return [finding for finding in findings if finding.key() not in excluded]


def scope_dirs(repository_root: Path) -> list[str]:
    """Return the sorted, relative GUI directory strings the scanner covers."""
    mod_root = repository_root / "src" / "Mod"
    if not mod_root.is_dir():
        return ["src/Gui"]
    mod_dirs = [
        f"src/Mod/{path.relative_to(mod_root).as_posix()}"
        for path in sorted(mod_root.rglob("Gui"))
        if path.relative_to(mod_root).parts[0] not in rules.EXCLUDED_WORKBENCHES
    ]
    return ["src/Gui", *mod_dirs]


def build_payload(repository_root: Path) -> dict[str, object]:
    """Return the complete ordered payload that ``inventory.json`` stores."""
    findings = scan(repository_root)
    exclusions = load_exclusions()
    curated = apply_exclusions(findings, exclusions)
    return {
        "generator": "tests/architecture/gui_blocking_live_model/scanner.py",
        "scope": scope_dirs(repository_root),
        "categories": [category.key for category in rules.CATEGORIES],
        "excluded_count": len(exclusions),
        "findings": [finding.to_dict() for finding in curated],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Reproducibly scan GUI source for blocking and live-model ingress."
    )
    parser.add_argument(
        "--repo-root",
        default=str(REPOSITORY_ROOT),
        help="Repository root (defaults to $FREECAD_SOURCE_ROOT or inferred).",
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="Write inventory.json next to this module instead of stdout.",
    )
    args = parser.parse_args(argv)

    repository_root = Path(args.repo_root).resolve()
    findings = scan(repository_root)
    exclusions = load_exclusions()
    curated = apply_exclusions(findings, exclusions)
    payload = build_payload(repository_root)

    text = json.dumps(payload, indent=2, sort_keys=False) + "\n"
    if args.write:
        target = Path(__file__).resolve().parent / "inventory.json"
        target.write_text(text, encoding="utf-8")
        print(
            f"wrote {len(curated)} findings ({len(findings)} scanned, "
            f"{len(exclusions)} excluded) to {target}"
        )
        return 0

    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
