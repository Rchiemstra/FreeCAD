# SPDX-License-Identifier: LGPL-2.1-or-later
"""Validation test for the GUI blocking and live-model ingress inventory.

This test loads the committed ``inventory.json`` and ``exclusions.json``, then:

1. rejects duplicate, malformed, or nonexistent inventory entries (including
   ``..`` path components);
2. rejects malformed or stale exclusions (including ``..`` path components);
3. checks the search rules with focused regressions (thread/process waits,
   multiline and ``getDocuments`` live-model dereferences, inline ``updateData``
   overrides, and Python GUI ingress);
4. re-runs the reproducible scanner and asserts the committed inventory exactly
   matches the complete ordered generated payload (every finding dict, the
   scope, the categories, and the excluded count) -- drift and mutation
   detection; and
5. validates each finding's subsystem against ``scanner.subsystem_for(path)``.

It is pure-Python (stdlib only) and is registered as a CTest so it runs inside
the locked Pixi environment alongside the rest of the test suite.
"""

from __future__ import annotations

import ast
import copy
import json
import os
import re
import sys
import unittest
from pathlib import Path

_ARCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_ARCH_DIR))

from gui_blocking_live_model import rules, scanner

REPOSITORY_ROOT = Path(os.environ.get("FREECAD_SOURCE_ROOT", _ARCH_DIR.parents[1])).resolve()

PACKAGE_DIR = _ARCH_DIR / "gui_blocking_live_model"
INVENTORY_PATH = PACKAGE_DIR / "inventory.json"
EXCLUSIONS_PATH = PACKAGE_DIR / "exclusions.json"
REPORT_PATH = PACKAGE_DIR / "REPORT.md"

REQUIRED_ENTRY_FIELDS = frozenset(
    {"path", "line", "category", "subsystem", "disposition", "evidence"}
)

_scan_cache: dict[str, list[scanner.Finding]] = {}


def _scan_file(repository_root: Path, relative_path: str) -> list[scanner.Finding]:
    key = f"{repository_root}:{relative_path}"
    if key not in _scan_cache:
        source_path = repository_root / relative_path
        source = source_path.read_text(encoding="utf-8", errors="surrogateescape")
        _scan_cache[key] = scanner.scan_source(source, source_path.suffix, relative_path)
    return _scan_cache[key]


def _load_inventory() -> dict:
    return json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))


def _load_exclusions() -> dict:
    return json.loads(EXCLUSIONS_PATH.read_text(encoding="utf-8"))


def report_count_violations(report_text: str, payload: dict) -> list[str]:
    """Compare every machine-derived count printed in REPORT.md with payload."""
    findings = list(payload["findings"])
    category_counts = {key: 0 for key in payload["categories"]}
    subsystem_counts: dict[str, int] = {}
    language_counts = {"C++": 0, "Python": 0}
    for finding in findings:
        category_counts[str(finding["category"])] += 1
        subsystem = str(finding["subsystem"])
        subsystem_counts[subsystem] = subsystem_counts.get(subsystem, 0) + 1
        language_counts["Python" if str(finding["path"]).endswith(".py") else "C++"] += 1

    problems: list[str] = []
    table_match = re.search(
        r"## Inventory at a glance(?P<table>.*?)(?:\n## |\Z)", report_text, re.DOTALL
    )
    if not table_match:
        return ["REPORT.md is missing its inventory table"]
    rows = {
        category: int(count.replace(",", ""))
        for category, count in re.findall(
            r"\| `([^`]+)` \| ([\d,]+) \|", table_match.group("table")
        )
    }
    if rows != category_counts:
        problems.append(f"category counts {rows} != {category_counts}")

    total_match = re.search(r"\(([\d,]+)\s+findings across seven\s+categories", report_text)
    if not total_match or int(total_match.group(1).replace(",", "")) != len(findings):
        problems.append("REPORT total finding count does not match inventory")
    language_match = re.search(r"spans ([\d,]+) C\+\+ and ([\d,]+) Python\s+findings", report_text)
    expected_languages = (language_counts["C++"], language_counts["Python"])
    if (
        not language_match
        or tuple(int(value.replace(",", "")) for value in language_match.groups())
        != expected_languages
    ):
        problems.append("REPORT language counts do not match inventory")
    subsystem_match = re.search(
        r"Owning subsystems:\n?(?P<subsystems>.*?)(?:\n\n|\Z)", report_text, re.DOTALL
    )
    if not subsystem_match:
        problems.append("REPORT is missing its owning-subsystems counts")
    else:
        rows = {
            name: int(count.replace(",", ""))
            for name, count in re.findall(
                r"`([^`]+)`\s*\(([\d,]+)\)", subsystem_match.group("subsystems")
            )
        }
        if rows != subsystem_counts:
            problems.append(f"subsystem counts {rows} != {subsystem_counts}")
    return problems


def _path_violation(path: object) -> str | None:
    """Return a violation message for an unclean path, or None when clean."""
    if not isinstance(path, str) or not path:
        return "must be a non-empty string"
    if path.startswith("/") or "\\" in path or path != path.strip():
        return "must be a clean repository-relative path"
    if any(part == ".." for part in Path(path).parts):
        return "must not contain '..' path components"
    return None


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

    path_problem = _path_violation(path)
    if path_problem:
        problems.append(f"{location}: 'path' {path_problem}")

    if not isinstance(line, int) or isinstance(line, bool) or line < 1:
        problems.append(f"{location}: 'line' must be a positive integer")
        return problems  # cannot range-check a malformed line

    if not isinstance(category, str) or category not in category_keys:
        problems.append(f"{location}: unknown category {category!r}")
    if not isinstance(subsystem, str) or not subsystem:
        problems.append(f"{location}: 'subsystem' must be a non-empty string")
    elif isinstance(path, str) and path and not _path_violation(path):
        expected_subsystem = scanner.subsystem_for(path)
        if subsystem != expected_subsystem:
            problems.append(
                f"{location}: subsystem {subsystem!r} != {expected_subsystem!r} derived from path"
            )
    if not isinstance(disposition, str) or disposition not in dispositions:
        problems.append(f"{location}: unknown disposition {disposition!r}")
    if not isinstance(evidence, str) or not evidence:
        problems.append(f"{location}: 'evidence' must be a non-empty string")

    if isinstance(path, str) and path and not _path_violation(path):
        source_path = repository_root / path
        if not source_path.is_file():
            problems.append(f"{location}: source file does not exist")
            return problems
        source_lines = source_path.read_text(
            encoding="utf-8", errors="surrogateescape"
        ).splitlines()
        if line > len(source_lines):
            problems.append(f"{location}: line {line} exceeds file length {len(source_lines)}")
        elif isinstance(category, str) and category in category_keys:
            matches = [
                finding
                for finding in _scan_file(repository_root, path)
                if finding.line == line and finding.category == category
            ]
            if not matches:
                problems.append(
                    f"{location}: evidence does not match a {category} match at line {line}"
                )
            elif isinstance(evidence, str) and all(f.evidence != evidence for f in matches):
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


def exclusion_violations(exclusions: list[dict], category_keys: frozenset[str]) -> list[str]:
    problems: list[str] = []
    seen: set[tuple] = set()
    for entry in exclusions:
        key = (entry.get("path"), entry.get("line"), entry.get("category"))
        location = f"{entry.get('path', '<missing>')}:{entry.get('line', '<missing>')}"
        if set(entry.keys()) != {"path", "line", "category", "reason"}:
            problems.append(f"{location}: exclusion fields malformed")
        path_problem = _path_violation(entry.get("path"))
        if path_problem:
            problems.append(f"{location}: exclusion path {path_problem}")
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


def payload_differences(expected: dict, actual: dict) -> list[str]:
    """Return concise human-readable differences between two payloads."""
    differences: list[str] = []
    if expected == actual:
        return differences
    if expected.get("generator") != actual.get("generator"):
        differences.append(
            f"generator: {expected.get('generator')!r} != {actual.get('generator')!r}"
        )
    if expected.get("excluded_count") != actual.get("excluded_count"):
        differences.append(
            f"excluded_count: {expected.get('excluded_count')} != {actual.get('excluded_count')}"
        )
    if expected.get("scope") != actual.get("scope"):
        differences.append("scope differs")
    if expected.get("categories") != actual.get("categories"):
        differences.append("categories differs")
    expected_findings = expected.get("findings", [])
    actual_findings = actual.get("findings", [])
    if len(expected_findings) != len(actual_findings):
        differences.append(f"findings length: {len(expected_findings)} != {len(actual_findings)}")
    if expected_findings != actual_findings:
        for index, (left, right) in enumerate(zip(expected_findings, actual_findings)):
            if left != right:
                differences.append(f"findings[{index}] differs: {left} != {right}")
                if len(differences) > 20:
                    differences.append("... (truncated)")
                    break
    return differences


class MaskingTests(unittest.TestCase):
    def test_masks_cpp_comments_strings_chars_and_raw_strings(self) -> None:
        source = (
            "// processEvents()\n"
            "/* ->recompute() */\n"
            'auto a = "App::GetApplication().getDocument(x)";\n'
            'auto b = R"tag(waitForFinished() inside raw string)tag";\n'
            "auto c = 'Qt::BlockingQueuedConnection';\n"
            "realCall->recompute();\n"
        )
        masked = scanner.mask_cpp_non_code(source)
        self.assertNotIn("processEvents", masked)
        self.assertNotIn("getDocument", masked)
        self.assertNotIn("waitForFinished", masked)
        self.assertNotIn("BlockingQueuedConnection", masked)
        self.assertIn("recompute", masked)

    def test_masks_python_comments_and_strings(self) -> None:
        source = (
            "# doc.recompute()\n"
            'x = "App.ActiveDocument.recompute()"\n'
            "y = '''\n"
            "FreeCAD.ActiveDocument.recompute()\n"
            "'''\n"
            'z = f"processEvents() {1}"\n'
            'w = r".wait()"\n'
            "real = FreeCAD.ActiveDocument.recompute()\n"
        )
        masked = scanner.mask_py_non_code(source)
        self.assertEqual(masked.count("recompute"), 1)  # only the real call remains
        self.assertNotIn("processEvents", masked)
        self.assertNotIn("wait", masked)

    def test_preserves_length_and_newlines(self) -> None:
        cpp_source = '"first\\nsecond"\n/* third\nfourth */\nR"tag(fifth\nsixth)tag"\n'
        masked = scanner.mask_cpp_non_code(cpp_source)
        self.assertEqual(len(masked), len(cpp_source))
        self.assertEqual(masked.count("\n"), cpp_source.count("\n"))

        py_source = '# comment\nx = "a\\nb"\ny = """doc\nstring"""\nz = 1\n'
        masked_py = scanner.mask_py_non_code(py_source)
        self.assertEqual(len(masked_py), len(py_source))
        self.assertEqual(masked_py.count("\n"), py_source.count("\n"))


class RuleRegressionTests(unittest.TestCase):
    """Focused regressions for the search rules themselves (no repository scan)."""

    def test_thread_wait_covers_process_future_and_condition_waits(self) -> None:
        cpp = scanner.compiled_pattern("thread-waits", "cpp")
        for token in (
            "proc->waitForFinished();",
            "pool->waitForDone();",
            "proc->waitForStarted();",
            "socket.waitForBytesWritten(timeout);",
            "thread->wait();",
            "QWaitCondition().wait(&mutex, 50);",
            "pthread_join(handle, nullptr);",
            "QThread::wait();",
        ):
            self.assertIsNotNone(cpp.search(scanner.mask_cpp_non_code(token)), token)

    def test_thread_wait_excludes_string_and_detach(self) -> None:
        cpp = scanner.compiled_pattern("thread-waits", "cpp")
        for token in (
            "QStringList::join(parts);",
            "std::thread().detach();",
        ):
            self.assertIsNone(cpp.search(scanner.mask_cpp_non_code(token)), token)
        py = scanner.compiled_pattern("thread-waits", "py")
        # Python .join() is a string/path join, never a thread wait.
        self.assertIsNone(py.search(scanner.mask_py_non_code('",".join(parts)')), "str.join")
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("proc.wait()")), "subprocess wait")

    def test_live_app_dereference_covers_multiline_and_getDocuments(self) -> None:
        cpp = scanner.compiled_pattern("live-app-dereference", "cpp")
        multiline = "auto obj = App::GetApplication()\n    .getDocument(name);"
        self.assertIsNotNone(cpp.search(scanner.mask_cpp_non_code(multiline)), "multiline")
        for accessor in (
            "getActiveDocument",
            "getDocument",
            "getDocuments",
            "getDocumentOrActive",
            "getDocumentByPath",
        ):
            self.assertIsNotNone(cpp.search(f"App::GetApplication().{accessor}()"), accessor)
        py = scanner.compiled_pattern("live-app-dereference", "py")
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("FreeCAD.ActiveDocument")))
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("App.getDocument('x')")))
        self.assertIsNone(py.search(scanner.mask_py_non_code("Gui.ActiveDocument")))

    def test_update_data_covers_inline_override_but_not_delegation(self) -> None:
        cpp = scanner.compiled_pattern("update-data-provider", "cpp")
        self.assertIsNotNone(cpp.search("void updateData(const App::Property*) override;"))
        self.assertIsNotNone(cpp.search("void updateData(const App::Property* prop) override {"))
        self.assertIsNotNone(cpp.search("virtual void updateData(const App::Property*);"))
        # base-class delegation call and the Qt model-style method are not matches.
        self.assertIsNone(cpp.search("ViewProviderX::updateData(prop);"))
        self.assertIsNone(cpp.search("bool updateData(const QModelIndex&, const QVariant&, int);"))

    def test_live_app_dereference_covers_active_document_method(self) -> None:
        py = scanner.compiled_pattern("live-app-dereference", "py")
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("App.activeDocument()")))
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("FreeCAD.activeDocument()")))
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("App.ActiveDocument")))
        self.assertIsNotNone(py.search(scanner.mask_py_non_code("FreeCAD.ActiveDocument")))
        self.assertIsNone(py.search(scanner.mask_py_non_code("App.activeDocument")))
        self.assertIsNone(py.search(scanner.mask_py_non_code("FreeCAD.activeDocument")))
        # Gui.activeDocument is the GUI-side handle, tracked separately.
        self.assertIsNone(py.search(scanner.mask_py_non_code("Gui.activeDocument()")))

    def test_blocking_invokes_python_pattern_is_documented_not_none(self) -> None:
        py = scanner.compiled_pattern("blocking-invokes", "py")
        self.assertIsNotNone(py)
        self.assertIsNotNone(py.search("QtCore.Qt.BlockingQueuedConnection"))
        self.assertIsNotNone(scanner.compiled_pattern("blocking-invokes", "cpp"))
        # update-data-provider is the only C++-only category.
        self.assertIsNone(scanner.compiled_pattern("update-data-provider", "py"))

    def test_python_pattern_none_set_matches_rules_documentation(self) -> None:
        none_keys = {category.key for category in rules.CATEGORIES if category.py_pattern is None}
        self.assertEqual(none_keys, {"update-data-provider"})
        readme = (PACKAGE_DIR / "README.md").read_text(encoding="utf-8")
        self.assertRegex(readme, r"Only\s+`update-data-provider` is C\+\+-only")
        self.assertNotIn(
            "blocking-invokes` (the Qt `BlockingQueuedConnection` connection type) and "
            "`update-data-provider`",
            readme,
        )

    def test_do_command_expands_executable_command_strings(self) -> None:
        py = scanner.compiled_pattern("live-app-dereference", "py")
        plain = 'FreeCADGui.doCommand("obj = FreeCAD.ActiveDocument.getObject(name)")'
        self.assertIsNotNone(py.search(scanner.mask_py_non_code(plain, expand_do_command=True)))
        fstring = "FreeCADGui.doCommand(f\"obj = FreeCAD.ActiveDocument.getObject('{name}')\")"
        self.assertIsNotNone(py.search(scanner.mask_py_non_code(fstring, expand_do_command=True)))
        gui_alias = 'Gui.doCommand("FreeCAD.ActiveDocument.recompute()")'
        self.assertIsNotNone(py.search(scanner.mask_py_non_code(gui_alias, expand_do_command=True)))
        parenthesized = 'Gui.doCommand(("FreeCAD.ActiveDocument"))'
        self.assertIsNotNone(
            py.search(scanner.mask_py_non_code(parenthesized, expand_do_command=True))
        )
        concatenated = 'Gui.doCommand("FreeCAD." + "ActiveDocument.recompute()")'
        self.assertIsNotNone(
            py.search(scanner.mask_py_non_code(concatenated, expand_do_command=True))
        )
        adjacent = 'Gui.doCommand(("FreeCAD." "ActiveDocument.recompute()"))'
        self.assertIsNotNone(py.search(scanner.mask_py_non_code(adjacent, expand_do_command=True)))
        multiline_adjacent = 'Gui.doCommand(\n    "FreeCAD."\n    "ActiveDocument.recompute()"\n)'
        multiline_findings = scanner.scan_source(multiline_adjacent, ".py", "snippet.py")
        self.assertIn("live-app-dereference", {finding.category for finding in multiline_findings})
        for prefixed in (
            'Gui.doCommand(u"FreeCAD.ActiveDocument.recompute()")',
            'Gui.doCommand(r"FreeCAD.ActiveDocument.recompute()")',
        ):
            self.assertIsNotNone(
                py.search(scanner.mask_py_non_code(prefixed, expand_do_command=True))
            )
        escaped = 'Gui.doCommand("FreeCAD\\x2eActiveDocument.recompute()")'
        escaped_findings = scanner.scan_source(escaped, ".py", "snippet.py")
        self.assertIn("live-app-dereference", {finding.category for finding in escaped_findings})
        escaped_newline = 'prefix = "ok"\nGui.doCommand("x=1\\nFreeCAD.ActiveDocument.recompute()")'
        escaped_newline_findings = scanner.scan_source(escaped_newline, ".py", "snippet.py")
        self.assertIn(
            (2, "live-app-dereference"),
            {(finding.line, finding.category) for finding in escaped_newline_findings},
        )
        triple = 'Gui.doCommand("""x=1\nFreeCAD.ActiveDocument.recompute()""")'
        triple_findings = scanner.scan_source(triple, ".py", "snippet.py")
        self.assertIn(
            (2, "live-app-dereference"),
            {(finding.line, finding.category) for finding in triple_findings},
        )
        nested_fstring = 'Gui.doCommand(f"FreeCAD.ActiveDocument.{name}")'
        self.assertIsNotNone(
            py.search(scanner.mask_py_non_code(nested_fstring, expand_do_command=True))
        )

    def test_do_command_does_not_expand_inert_or_nested_literals(self) -> None:
        py = scanner.compiled_pattern("live-app-dereference", "py")
        # A non-doCommand string with live-model text is an inert literal.
        inert = 'log("FreeCAD.ActiveDocument")'
        self.assertIsNone(py.search(scanner.mask_py_non_code(inert, expand_do_command=True)))
        # Live-model text inside a nested string of a command string is code-inert.
        nested = "FreeCADGui.doCommand(\"print('FreeCAD.ActiveDocument')\")"
        self.assertIsNone(py.search(scanner.mask_py_non_code(nested, expand_do_command=True)))
        # A doCommand call with a non-literal argument has no command string to expand.
        variable = "FreeCADGui.doCommand(snippet)"
        self.assertIsNone(py.search(scanner.mask_py_non_code(variable, expand_do_command=True)))
        nested_call = 'FreeCADGui.doCommand(make_command("FreeCAD.ActiveDocument"))'
        self.assertIsNone(py.search(scanner.mask_py_non_code(nested_call, expand_do_command=True)))
        for inert_command in (
            "Gui.doCommand(u\"print(\\'FreeCAD.ActiveDocument\\')\")",
            "Gui.doCommand(r\"print(\\'FreeCAD.ActiveDocument\\')\")",
        ):
            self.assertIsNone(
                py.search(scanner.mask_py_non_code(inert_command, expand_do_command=True))
            )


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
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("subsystem" in v for v in violations), violations)

    def test_rejects_unknown_category_and_disposition(self) -> None:
        bad = self._valid_entry()
        bad["category"] = "not-a-category"
        bad["disposition"] = "not-a-disposition"
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("category" in v for v in violations), violations)
        self.assertTrue(any("disposition" in v for v in violations), violations)

    def test_rejects_nonexistent_path(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/DoesNotExist.cpp"
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("does not exist" in v for v in violations), violations)

    def test_rejects_out_of_range_line(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/MainWindow.cpp"
        bad["line"] = 10_000_000
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("exceeds file length" in v for v in violations), violations)

    def test_rejects_stale_evidence(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "src/Gui/MainWindow.cpp"
        bad["line"] = 1
        bad["evidence"] = "// not the first line"
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("evidence" in v for v in violations), violations)

    def test_rejects_parent_path_component(self) -> None:
        bad = self._valid_entry()
        bad["path"] = "../Gui/MainWindow.cpp"
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any(".." in v for v in violations), violations)

    def test_rejects_wrong_subsystem(self) -> None:
        bad = self._valid_entry()
        bad["subsystem"] = "Part"
        violations = entry_violations(bad, REPOSITORY_ROOT, self.category_keys, self.dispositions)
        self.assertTrue(any("subsystem" in v for v in violations), violations)


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

    def test_rejects_parent_path_component(self) -> None:
        ok = {
            "path": "../Gui/Application.h",
            "line": 167,
            "category": "live-reference-callback",
            "reason": "signal declaration",
        }
        violations = exclusion_violations([ok], frozenset(rules.CATEGORY_BY_KEY))
        self.assertTrue(any(".." in v for v in violations), violations)


class RepositoryInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.inventory = _load_inventory()
        cls.exclusions = _load_exclusions()["exclusions"]
        cls.category_keys = frozenset(rules.CATEGORY_BY_KEY)
        cls.dispositions = frozenset(rules.DISPOSITIONS)
        cls.scanned = scanner.scan(REPOSITORY_ROOT)
        cls.payload = scanner.build_payload(REPOSITORY_ROOT)
        cls.excluded_keys = {_exclusion_key(entry) for entry in cls.exclusions}

    def test_inventory_entries_are_well_formed_and_real(self) -> None:
        findings = self.inventory["findings"]
        self.assertGreater(len(findings), 0, "inventory must not be empty")
        problems: list[str] = []
        for entry in findings:
            problems.extend(
                entry_violations(entry, REPOSITORY_ROOT, self.category_keys, self.dispositions)
            )
        problems.extend(duplicate_violations(findings))
        self.assertFalse(problems, "\n".join(problems))

    def test_exclusions_are_well_formed_and_not_stale(self) -> None:
        problems = exclusion_violations(self.exclusions, self.category_keys)
        scanned_keys = {finding.key() for finding in self.scanned}
        for entry in self.exclusions:
            key = _exclusion_key(entry)
            if key not in scanned_keys:
                problems.append(f"stale exclusion {key}: no longer produced by the scanner")
        self.assertFalse(problems, "\n".join(problems))

    def test_inventory_is_reproducible(self) -> None:
        """Committed inventory must equal the complete ordered generated payload."""
        differences = payload_differences(self.payload, self.inventory)
        self.assertFalse(differences, "\n".join(differences))

    def test_serialization_is_byte_stable(self) -> None:
        text = json.dumps(self.payload, indent=2, sort_keys=False) + "\n"
        self.assertEqual(text, INVENTORY_PATH.read_text(encoding="utf-8"))

    def test_inventory_metadata_matches_scanner(self) -> None:
        self.assertEqual(
            self.inventory["generator"],
            "tests/architecture/gui_blocking_live_model/scanner.py",
        )
        self.assertEqual(self.inventory["categories"], [c.key for c in rules.CATEGORIES])
        self.assertEqual(
            self.inventory["scope"],
            scanner.scope_entries(REPOSITORY_ROOT),
            "scope must be exact",
        )

    def test_report_counts_match_inventory(self) -> None:
        problems = report_count_violations(REPORT_PATH.read_text(encoding="utf-8"), self.inventory)
        self.assertFalse(problems, "\n".join(problems))

    def test_disposition_defaults_are_applied(self) -> None:
        defaults = {c.key: c.default_disposition for c in rules.CATEGORIES}
        for entry in self.inventory["findings"]:
            self.assertEqual(
                entry["disposition"],
                defaults[entry["category"]],
                f"{entry['path']}:{entry['line']} disposition mismatch",
            )

    def test_every_subsystem_derives_from_path(self) -> None:
        for entry in self.inventory["findings"]:
            self.assertEqual(
                entry["subsystem"],
                scanner.subsystem_for(entry["path"]),
                f"{entry['path']}:{entry['line']} subsystem mismatch",
            )

    # -- focused site regressions -------------------------------------------

    def _assert_site(self, path: str, line: int, category: str) -> None:
        self.assertIn(
            (path, line, category),
            {f.key() for f in self.scanned},
            f"expected finding {path}:{line} ({category})",
        )

    def test_thread_wait_instance_join_found(self) -> None:
        self._assert_site("src/Gui/Quarter/SensorManager.cpp", 82, "thread-waits")

    def test_thread_wait_started_found(self) -> None:
        self._assert_site("src/Gui/Assistant.cpp", 169, "thread-waits")

    def test_thread_wait_bytes_written_found(self) -> None:
        self._assert_site("src/Gui/GuiApplication.cpp", 328, "thread-waits")

    def test_thread_wait_condition_wait_found(self) -> None:
        self._assert_site("src/Gui/SplashScreen.cpp", 161, "thread-waits")

    def test_worker_thread_self_wait_is_excluded(self) -> None:
        key = ("src/Gui/Quarter/SignalThread.cpp", 71, "thread-waits")
        self.assertIn(key, {f.key() for f in self.scanned})
        self.assertIn(key, self.excluded_keys)
        self.assertNotIn(
            key,
            {(e["path"], e["line"], e["category"]) for e in self.inventory["findings"]},
        )

    def test_test_harness_file_is_excluded(self) -> None:
        for finding in self.scanned:
            self.assertNotIn("CommandTest.cpp", finding.path, finding.path)
            self.assertNotIn("FreeCADGuiTest.py", finding.path, finding.path)

    def test_multiline_live_dereference_found(self) -> None:
        self._assert_site("src/Mod/PartDesign/Gui/TaskFeaturePick.cpp", 288, "live-app-dereference")

    def test_getDocuments_live_dereference_found(self) -> None:
        self._assert_site("src/Gui/CommandDoc.cpp", 2232, "live-app-dereference")

    def test_inline_updateData_override_found(self) -> None:
        self._assert_site("src/Gui/ViewProviderAnnotation.h", 76, "update-data-provider")

    def test_python_thread_wait_found(self) -> None:
        self._assert_site("src/Mod/CAM/Path/Main/Gui/Camotics.py", 151, "thread-waits")

    def test_python_live_dereference_found(self) -> None:
        self._assert_site("src/Mod/CAM/Path/Base/Gui/GetPoint.py", 136, "live-app-dereference")

    def test_python_recompute_found(self) -> None:
        self._assert_site("src/Mod/CAM/Path/Base/Gui/PropertyBag.py", 296, "direct-recompute")

    def test_python_process_events_found(self) -> None:
        self._assert_site(
            "src/Mod/CAM/Path/Post/Gui/DlgPostProcess.py", 667, "process-events-polling"
        )

    def test_python_observer_callback_found(self) -> None:
        self._assert_site(
            "src/Mod/CAM/Path/Dressup/Gui/DogboneII.py", 115, "live-reference-callback"
        )

    def test_python_active_document_method_sites_found(self) -> None:
        for path, line in (
            ("src/Mod/CAM/Path/Op/Gui/Custom.py", 126),
            ("src/Mod/CAM/Path/Dressup/Gui/ZCorrect.py", 327),
            ("src/Mod/CAM/Path/Post/Gui/DlgPostProcess.py", 1337),
            ("src/Mod/CAM/Path/Main/Gui/Camotics.py", 86),
            ("src/Mod/CAM/Path/Main/Gui/Job.py", 1217),
        ):
            self._assert_site(path, line, "live-app-dereference")

    def test_draft_gui_scope_sites_found(self) -> None:
        self._assert_site("src/Mod/Draft/draftguitools/gui_base.py", 87, "live-app-dereference")
        self._assert_site("src/Mod/Draft/draftguitools/gui_base.py", 95, "live-app-dereference")
        self._assert_site("src/Mod/Draft/draftguitools/gui_layers.py", 187, "direct-recompute")
        self._assert_site("src/Mod/Draft/draftguitools/gui_layers.py", 197, "direct-recompute")

    def test_do_command_command_string_sites_found(self) -> None:
        self._assert_site("src/Mod/CAM/Path/Dressup/Gui/AxisMap.py", 283, "live-app-dereference")
        self._assert_site("src/Mod/CAM/Path/Dressup/Gui/DogboneII.py", 252, "live-app-dereference")
        self._assert_site("src/Mod/CAM/Path/Main/Gui/Inspect.py", 335, "live-app-dereference")

    def test_draft_bim_scope_entries_present(self) -> None:
        scope = scanner.scope_entries(REPOSITORY_ROOT)
        for entry in (
            "src/Mod/Draft/draftguitools",
            "src/Mod/Draft/drafttaskpanels",
            "src/Mod/Draft/draftviewproviders",
            "src/Mod/BIM/bimcommands",
            "src/Mod/Draft/DraftGui.py",
            "src/Mod/BIM/ArchCoveringGui.py",
        ):
            self.assertIn(entry, scope, f"expected GUI scope entry {entry}")

    def test_production_init_gui_and_reviewed_python_scope_entries_present(
        self,
    ) -> None:
        scope = scanner.scope_entries(REPOSITORY_ROOT)
        expected_init_gui = {
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in (REPOSITORY_ROOT / "src" / "Mod").glob("*/InitGui.py")
            if path.relative_to(REPOSITORY_ROOT / "src" / "Mod").parts[0]
            not in rules.EXCLUDED_WORKBENCHES
        }
        self.assertTrue(expected_init_gui.issubset(scope))
        for entry in (
            "src/Mod/Assembly/InitGui.py",
            "src/Mod/CAM/InitGui.py",
            "src/Mod/Draft/InitGui.py",
            "src/Mod/BIM/InitGui.py",
            "src/Mod/Fem/InitGui.py",
            "src/Mod/Robot/InitGui.py",
            "src/Mod/CAM/PathPythonGui",
            "src/Mod/Draft/draftutils/gui_utils.py",
            "src/Mod/Draft/draftutils/grid_observer.py",
            "src/Mod/Draft/draftutils/init_draft_statusbar.py",
            "src/Mod/Draft/draftutils/init_tools.py",
            "src/Mod/BIM/nativeifc/ifc_viewproviders.py",
            "src/Mod/BIM/nativeifc/ifc_commands.py",
            "src/Mod/BIM/nativeifc/ifc_observer.py",
            "src/Mod/BIM/nativeifc/ifc_status.py",
            "src/Mod/Fem/femguiutils/extract_link_view.py",
            "src/Mod/Robot/MovieTool.py",
            "src/Mod/Assembly/CommandCreateAssembly.py",
            "src/Mod/Assembly/CommandCreateJoint.py",
            "src/Mod/Assembly/CommandCreateSimulation.py",
            "src/Mod/Assembly/CommandCreateView.py",
            "src/Mod/Assembly/JointObject.py",
            "src/Mod/CAM/PathCommands.py",
            "src/Mod/MeshPart/Gui",
            "src/Mod/Points/pointscommands",
            "src/Mod/PartDesign/WizardShaft",
            "src/Mod/Tux/NavigationIndicatorGui.py",
            "src/Mod/Tux/PersistentToolbarsGui.py",
        ):
            self.assertIn(entry, scope, f"expected reviewed GUI scope entry {entry}")
        self.assertNotIn("src/Mod/Test/InitGui.py", scope)
        self.assertNotIn("src/Mod/TemplatePyMod/InitGui.py", scope)
        self.assertFalse(any("Test" in entry for entry in scope))

    def test_init_gui_local_imports_are_scoped_or_explicitly_excluded(self) -> None:
        """Every resolvable local InitGui import has a reviewed scope decision."""
        scope_files = {
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in scanner.iter_source_files(REPOSITORY_ROOT)
        }
        missing: list[str] = []
        for init_path in scanner._production_init_gui_files(REPOSITORY_ROOT):
            workbench_root = init_path.parent
            tree = ast.parse(init_path.read_text(encoding="utf-8"))
            modules: set[str] = set()
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    modules.update(alias.name for alias in node.names)
                elif isinstance(node, ast.ImportFrom) and node.module:
                    modules.add(node.module)
                    modules.update(
                        f"{node.module}.{alias.name}" for alias in node.names if alias.name != "*"
                    )
            for module in modules:
                module_path = Path(*module.split("."))
                candidates = (
                    workbench_root / f"{module_path}.py",
                    workbench_root / module_path / "__init__.py",
                    REPOSITORY_ROOT / "src" / "Mod" / f"{module_path}.py",
                    REPOSITORY_ROOT / "src" / "Mod" / module_path / "__init__.py",
                )
                for candidate in candidates:
                    if not candidate.is_file():
                        continue
                    relative = candidate.relative_to(REPOSITORY_ROOT).as_posix()
                    if (
                        relative not in scope_files
                        and relative not in rules.REVIEWED_INITGUI_IMPORT_EXCLUSIONS
                    ):
                        missing.append(relative)
        self.assertFalse(
            sorted(set(missing)),
            "unreviewed local InitGui imports: " + ", ".join(sorted(set(missing))),
        )
        self.assertTrue(rules.REVIEWED_INITGUI_IMPORT_EXCLUSIONS)

    def test_reviewed_python_gui_sites_found(self) -> None:
        for path, category in (
            ("src/Mod/Assembly/InitGui.py", "live-app-dereference"),
            ("src/Mod/CAM/InitGui.py", "live-app-dereference"),
            ("src/Mod/Draft/draftutils/gui_utils.py", "live-app-dereference"),
            ("src/Mod/BIM/nativeifc/ifc_viewproviders.py", "direct-recompute"),
            ("src/Mod/Fem/femguiutils/extract_link_view.py", "live-app-dereference"),
        ):
            findings = [finding for finding in self.scanned if finding.path == path]
            self.assertTrue(
                any(finding.category == category for finding in findings),
                f"expected {category} in {path}: {findings}",
            )
        self.assertIn(
            "src/Mod/Robot/MovieTool.py",
            [
                path.relative_to(REPOSITORY_ROOT).as_posix()
                for path in scanner.iter_source_files(REPOSITORY_ROOT)
            ],
        )

    def test_init_gui_loaded_command_and_gui_modules_are_inventoried(self) -> None:
        scoped_files = {
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in scanner.iter_source_files(REPOSITORY_ROOT)
        }
        for path in (
            "src/Mod/Assembly/CommandCreateAssembly.py",
            "src/Mod/Assembly/CommandCreateJoint.py",
            "src/Mod/Assembly/CommandCreateSimulation.py",
            "src/Mod/Assembly/CommandCreateView.py",
            "src/Mod/Assembly/JointObject.py",
            "src/Mod/Fem/femcommands/commands.py",
            "src/Mod/Fem/femcommands/manager.py",
            "src/Mod/CAM/PathCommands.py",
            "src/Mod/BIM/nativeifc/ifc_commands.py",
            "src/Mod/BIM/nativeifc/ifc_observer.py",
            "src/Mod/BIM/nativeifc/ifc_status.py",
            "src/Mod/MeshPart/Gui/MeshFlatteningCommand.py",
        ):
            self.assertIn(path, scoped_files, f"expected loaded GUI module {path}")
        for path in (
            "src/Mod/Assembly/CommandCreateAssembly.py",
            "src/Mod/Assembly/CommandCreateJoint.py",
            "src/Mod/Fem/femcommands/commands.py",
            "src/Mod/Fem/femcommands/manager.py",
            "src/Mod/CAM/PathCommands.py",
        ):
            self.assertTrue(
                any(finding.path == path for finding in self.scanned),
                f"expected representative finding in {path}",
            )

    def test_draft_bim_app_layer_not_in_scope(self) -> None:
        scope = scanner.scope_entries(REPOSITORY_ROOT)
        for entry in (
            "src/Mod/Draft/draftmake",
            "src/Mod/Draft/draftobjects",
            "src/Mod/Draft/DraftGeomUtils.py",
            "src/Mod/BIM/geometry",
        ):
            self.assertNotIn(entry, scope, f"App-layer path {entry} must stay out of scope")

    def test_draft_app_layer_files_produce_no_findings(self) -> None:
        for finding in self.scanned:
            self.assertNotIn("src/Mod/Draft/draftmake/", finding.path, finding.path)
            self.assertNotIn("src/Mod/Draft/draftobjects/", finding.path, finding.path)

    # -- mutation tests ------------------------------------------------------

    def test_mutation_subsystem_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["findings"][0]["subsystem"] = "WrongSubsystem"
        self.assertNotEqual(mutated, self.payload)

    def test_mutation_scope_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["scope"] = ["src/Gui", *mutated["scope"]]
        self.assertNotEqual(mutated, self.payload)

    def test_mutation_excluded_count_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["excluded_count"] = int(mutated["excluded_count"]) + 1
        self.assertNotEqual(mutated, self.payload)

    def test_mutation_ordering_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["findings"].reverse()
        self.assertNotEqual(mutated, self.payload)

    def test_mutation_finding_dict_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["findings"][0]["evidence"] = "corrupted evidence"
        self.assertNotEqual(mutated, self.payload)

    def test_mutation_category_detected(self) -> None:
        mutated = copy.deepcopy(self.inventory)
        mutated["categories"] = list(reversed(mutated["categories"]))
        self.assertNotEqual(mutated, self.payload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
