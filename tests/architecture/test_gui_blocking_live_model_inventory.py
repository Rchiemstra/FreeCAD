# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validation test for the GUI blocking and live-model ingress inventory.

This test loads the committed ``inventory.json`` and ``exclusions.json``, then:

1. rejects duplicate, malformed, or nonexistent inventory entries;
2. rejects malformed or stale exclusions; and
3. re-runs the reproducible scanner and asserts the committed inventory exactly
   matches the scanner output minus the documented exclusions (drift detection).

It is pure-Python (stdlib only) and is registered as a CTest so it runs inside
the locked Pixi environment alongside the rest of the test suite.
"""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

_ARCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_ARCH_DIR))

from gui_blocking_live_model import rules, scanner  # noqa: E402

REPOSITORY_ROOT = Path(
    os.environ.get("FREECAD_SOURCE_ROOT", _ARCH_DIR.parents[1])
).resolve()

PACKAGE_DIR = _ARCH_DIR / "gui_blocking_live_model"
INVENTORY_PATH = PACKAGE_DIR / "inventory.json"
EXCLUSIONS_PATH = PACKAGE_DIR / "exclusions.json"

REQUIRED_ENTRY_FIELDS = frozenset(
    {"path", "line", "category", "subsystem", "disposition", "evidence"}
)


def _load_inventory() -> dict:
    return json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))


def _load_exclusions() -> dict:
    return json.loads(EXCLUSIONS_PATH.read_text(encoding="utf-8"))


def entry_violations(
    entry: dict,
    repository_root: Path,
    category_keys: frozenset[str],
    dispositions: frozenset[str],
) -> list[str]:
    """Return human-readable violations for a single inventory entry."""
    problems: list[str] = []
    location = f"{entry.get('path', '<missing>')}:{entry.get('line', '<missing>')}"

    if set(entry.keys()) != REQUIRED_ENTRY_FIELDS:
        problems.append(
            f"{location}: fields {sorted(entry.keys())} != {sorted(REQUIRED_ENTRY_FIELDS)}"
        )

    path = entry.get("path")
    line = entry.get("line")
    category = entry.get("category")
    subsystem = entry.get("subsystem")
    disposition = entry.get("disposition")
    evidence = entry.get("evidence")

    if not isinstance(path, str) or not path:
        problems.append(f"{location}: 'path' must be a non-empty string")
    elif path.startswith("/") or "\\" in path or path != path.strip():
        problems.append(f"{location}: 'path' must be a clean repository-relative path")

    if not isinstance(line, int) or isinstance(line, bool) or line < 1:
        problems.append(f"{location}: 'line' must be a positive integer")
        return problems  # cannot range-check a malformed line

    if not isinstance(category, str) or category not in category_keys:
        problems.append(f"{location}: unknown category {category!r}")
    if not isinstance(subsystem, str) or not subsystem:
        problems.append(f"{location}: 'subsystem' must be a non-empty string")
    if not isinstance(disposition, str) or disposition not in dispositions:
        problems.append(f"{location}: unknown disposition {disposition!r}")
    if not isinstance(evidence, str):
        problems.append(f"{location}: 'evidence' must be a string")

    if isinstance(path, str) and path:
        source_path = repository_root / path
        if not source_path.is_file():
            problems.append(f"{location}: source file does not exist")
            return problems
        source_lines = source_path.read_text(
            encoding="utf-8", errors="surrogateescape"
        ).splitlines()
        if line > len(source_lines):
            problems.append(
                f"{location}: line {line} exceeds file length {len(source_lines)}"
            )
        elif isinstance(evidence, str) and source_lines[line - 1].strip() != evidence:
            problems.append(f"{location}: evidence does not match the source line")

    return problems


def duplicate_violations(findings: list[dict]) -> list[str]:
    seen: dict[tuple, dict] = {}
    problems: list[str] = []
    for entry in findings:
        key = (entry.get("path"), entry.get("line"), entry.get("category"))
        if key in seen:
            problems.append(f"duplicate entry for {key}")
        else:
            seen[key] = entry
    return problems


def _exclusion_key(entry: dict) -> tuple:
    return (entry["path"], entry["line"], entry["category"])


def exclusion_violations(
    exclusions: list[dict], category_keys: frozenset[str]
) -> list[str]:
    problems: list[str] = []
    seen: set[tuple] = set()
    for entry in exclusions:
        key = (entry.get("path"), entry.get("line"), entry.get("category"))
        location = f"{entry.get('path', '<missing>')}:{entry.get('line', '<missing>')}"
        if set(entry.keys()) != {"path", "line", "category", "reason"}:
            problems.append(f"{location}: exclusion fields malformed")
        if not isinstance(entry.get("path"), str) or not entry.get("path"):
            problems.append(f"{location}: exclusion path must be a non-empty string")
        if not isinstance(entry.get("line"), int) or entry.get("line", 0) < 1:
            problems.append(f"{location}: exclusion line must be a positive integer")
        if entry.get("category") not in category_keys:
            problems.append(f"{location}: unknown exclusion category {entry.get('category')!r}")
        if not isinstance(entry.get("reason"), str) or not entry.get("reason").strip():
            problems.append(f"{location}: exclusion reason must be a non-empty string")
        if key in seen:
            problems.append(f"{location}: duplicate exclusion")
        seen.add(key)
    return problems


class MaskingTests(unittest.TestCase):
    def test_masks_comments_strings_chars_and_raw_strings(self) -> None:
        source = (
            "// processEvents()\n"
            "/* ->recompute() */\n"
            'auto a = "App::GetApplication().getDocument(x)";\n'
            "auto b = R\"tag(waitForFinished() inside raw string)tag\";\n"
            "auto c = 'Qt::BlockingQueuedConnection';\n"
            "realCall->recompute();\n"
        )
        masked = scanner.mask_cpp_non_code(source)
        self.assertNotIn("processEvents", masked)
        self.assertNotIn("getDocument", masked)
        self.assertNotIn("waitForFinished", masked)
        self.assertNotIn("BlockingQueuedConnection", masked)
        self.assertIn("recompute", masked)

    def test_preserves_length_and_newlines(self) -> None:
        source = '"first\\nsecond"\n/* third\nfourth */\nR"tag(fifth\nsixth)tag"\n'
        masked = scanner.mask_cpp_non_code(source)
        self.assertEqual(len(masked), len(source))
        self.assertEqual(masked.count("\n"), source.count("\n"))


class InventoryEntryValidationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.category_keys = frozenset(rules.CATEGORY_BY_KEY)
        self.dispositions = frozenset(rules.DISPOSITIONS)

    def _valid_entry(self) -> dict:
        return {
            "path": "src/Gui/MainWindow.cpp",
            "line": 1,
            "category": "process-events-polling",
            "subsystem": "Gui",
            "disposition": "investigate",
            "evidence": "qApp->processEvents();",
        }

    def test_rejects_duplicate_entries(self) -> None:
        entry = self._valid_entry()
        violations = duplicate_violations([entry, dict(entry)])
        self.assertEqual(len(violations), 1, violations)
        self.assertIn("duplicate", violations[0])

    def test_rejects_malformed_entry(self) -> None:
        bad = self._valid_entry()
        bad.pop("subsystem")
        violations = entry_violations(
            bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
        )
        self.assertTrue(any("subsystem" in v for v in violations), violations)

    def test_rejects_unknown_category_and_disposition(self) -> None:
        bad = self._valid_entry()
        bad["category"] = "not-a-category"
        bad["disposition"] = "not-a-disposition"
        violations = entry_violations(
            bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
        )
        self.assertTrue(any("category" in v for v in violations), violations)
        self.assertTrue(any("disposition" in v for v in violations), violations)

    def test_rejects_nonexistent_path(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/DoesNotExist.cpp"
        violations = entry_violations(
            bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
        )
        self.assertTrue(any("does not exist" in v for v in violations), violations)

    def test_rejects_out_of_range_line(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/MainWindow.cpp"
        bad["line"] = 10_000_000
        violations = entry_violations(
            bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
        )
        self.assertTrue(any("exceeds file length" in v for v in violations), violations)

    def test_rejects_stale_evidence(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/MainWindow.cpp"
        bad["line"] = 1
        bad["evidence"] = "// not the first line"
        violations = entry_violations(
            bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
        )
        self.assertTrue(any("evidence" in v for v in violations), violations)


class ExclusionsValidationTests(unittest.TestCase):
    def test_rejects_malformed_exclusion(self) -> None:
        bad = [{"path": "x", "line": 1, "category": "thread-waits"}]
        violations = exclusion_violations(bad, frozenset(rules.CATEGORY_BY_KEY))
        self.assertTrue(any("reason" in v for v in violations), violations)

    def test_rejects_duplicate_exclusion(self) -> None:
        ok = {
            "path": "src/Gui/Application.h",
            "line": 167,
            "category": "live-reference-callback",
            "reason": "signal declaration",
        }
        violations = exclusion_violations([ok, dict(ok)], frozenset(rules.CATEGORY_BY_KEY))
        self.assertTrue(any("duplicate" in v for v in violations), violations)


class RepositoryInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.inventory = _load_inventory()
        cls.exclusions = _load_exclusions()["exclusions"]
        cls.category_keys = frozenset(rules.CATEGORY_BY_KEY)
        cls.dispositions = frozenset(rules.DISPOSITIONS)
        cls.scanned = scanner.scan(REPOSITORY_ROOT)

    def test_inventory_entries_are_well_formed_and_real(self) -> None:
        findings = self.inventory["findings"]
        self.assertGreater(len(findings), 0, "inventory must not be empty")
        problems: list[str] = []
        for entry in findings:
            problems.extend(
                entry_violations(
                    entry, REPOSITORY_ROOT, self.category_keys, self.dispositions
                )
            )
        problems.extend(duplicate_violations(findings))
        self.assertFalse(problems, "\n".join(problems))

    def test_exclusions_are_well_formed_and_not_stale(self) -> None:
        problems = exclusion_violations(self.exclusions, self.category_keys)
        scanned_keys = {finding.key() for finding in self.scanned}
        for entry in self.exclusions:
            key = _exclusion_key(entry)
            if key not in scanned_keys:
                problems.append(
                    f"stale exclusion {key}: no longer produced by the scanner"
                )
        self.assertFalse(problems, "\n".join(problems))

    def test_inventory_is_reproducible(self) -> None:
        """Committed inventory must equal scanner output minus exclusions."""
        scanned_keys = {finding.key() for finding in self.scanned}
        excluded_keys = {_exclusion_key(entry) for entry in self.exclusions}
        inventory_keys = {
            (entry["path"], entry["line"], entry["category"])
            for entry in self.inventory["findings"]
        }
        expected = scanned_keys - excluded_keys
        missing = expected - inventory_keys
        unexpected = inventory_keys - expected
        messages: list[str] = []
        if missing:
            messages.append(
                "scanner produced findings missing from the inventory (run the "
                "scanner with --write to refresh):\n"
                + "\n".join(f"  {key}" for key in sorted(missing))
            )
        if unexpected:
            messages.append(
                "inventory contains findings the scanner no longer produces:\n"
                + "\n".join(f"  {key}" for key in sorted(unexpected))
            )
        self.assertFalse(messages, "\n".join(messages))

    def test_inventory_metadata_matches_scanner(self) -> None:
        self.assertEqual(self.inventory["generator"], "tests/architecture/gui_blocking_live_model/scanner.py")
        self.assertEqual(self.inventory["categories"], [c.key for c in rules.CATEGORIES])

    def test_disposition_defaults_are_applied(self) -> None:
        defaults = {c.key: c.default_disposition for c in rules.CATEGORIES}
        for entry in self.inventory["findings"]:
            self.assertEqual(
                entry["disposition"],
                defaults[entry["category"]],
                f"{entry['path']}:{entry['line']} disposition mismatch",
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
