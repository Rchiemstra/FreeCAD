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
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path, PureWindowsPath
from unittest import mock

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


def _executable_gui_hook_lines(source: str) -> list[int]:
    """Return AST lines containing direct executable GUI integration hooks."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []
    lines: set[int] = set()
    gui_attributes = {
        "GuiUp",
        "ViewObject",
        "Selection",
        "activeView",
        "showDialog",
        "addSelection",
        "removeSelection",
    }
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
            if any(
                name == "FreeCADGui"
                or name.startswith(("PySide", "draftutils.gui_utils", "draftviewproviders"))
                for name in names
            ):
                lines.add(node.lineno)
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            if (
                module == "FreeCADGui"
                or module.startswith(("PySide", "draftutils.gui_utils", "draftviewproviders"))
                or module == "FreeCAD"
                and any(alias.name == "Gui" for alias in node.names)
            ):
                lines.add(node.lineno)
        elif (
            isinstance(node, ast.Name)
            and node.id in {"FreeCADGui", "Gui"}
            or isinstance(node, ast.Attribute)
            and node.attr in gui_attributes
        ):
            lines.add(node.lineno)
    return sorted(lines)


def _dotted_name(node: ast.AST) -> str | None:
    if isinstance(node, ast.Name):
        return node.id
    if isinstance(node, ast.Attribute):
        prefix = _dotted_name(node.value)
        return f"{prefix}.{node.attr}" if prefix else None
    return None


def _imported_module_candidates(source: str) -> set[str]:
    """Return normalized module and imported-target names from Python imports."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return set()
    candidates: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                candidates.add(alias.name)
        elif isinstance(node, ast.ImportFrom):
            prefix = "." * node.level + (node.module or "")
            if prefix:
                candidates.add(prefix)
            for alias in node.names:
                target = f"{prefix}.{alias.name}" if prefix else f".{alias.name}"
                candidates.add(target)
    return candidates


def _imports_module(source: str, target: str) -> bool:
    """Return whether imports contain an exact module-segment target."""
    return any(
        target in candidate.lstrip(".").split(".")
        for candidate in _imported_module_candidates(source)
    )


def _static_string_expression(node: ast.AST) -> bool:
    return _static_string_value(node) is not None


def _static_string_value(node: ast.AST) -> str | None:
    """Return a value for a literal-only string expression, if available."""
    try:
        value = ast.literal_eval(node)
    except (ValueError, TypeError, SyntaxError):
        if isinstance(node, ast.JoinedStr) and all(
            isinstance(part, ast.Constant) and isinstance(part.value, str) for part in node.values
        ):
            return "".join(part.value for part in node.values)
        return None
    return value if isinstance(value, str) else None


def _runtime_loader_calls(source: str) -> list[tuple[int, str]]:
    """Return runtime-selected import/load calls and their loader kind."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []

    importlib_modules = {"importlib"}
    importlib_util_modules = {"importlib.util"}
    import_module_names = {"import_module"}
    builtin_modules = {"builtins"}
    builtin_import_names = {"__import__"}
    spec_names = {"spec_from_file_location"}
    module_from_spec_names = {"module_from_spec"}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                bound = alias.asname or alias.name
                if alias.name == "importlib":
                    importlib_modules.add(bound)
                    importlib_util_modules.add(f"{bound}.util")
                elif alias.name == "importlib.util":
                    importlib_util_modules.add(bound)
                elif alias.name == "builtins":
                    builtin_modules.add(bound)
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                bound = alias.asname or alias.name
                if node.module == "importlib" and alias.name == "import_module":
                    import_module_names.add(bound)
                elif node.module == "importlib" and alias.name == "util":
                    importlib_util_modules.add(bound)
                elif node.module == "importlib.util":
                    if alias.name == "spec_from_file_location":
                        spec_names.add(bound)
                    elif alias.name == "module_from_spec":
                        module_from_spec_names.add(bound)
                elif node.module == "builtins" and alias.name == "__import__":
                    builtin_import_names.add(bound)
    known_import_module_calls = import_module_names | {
        f"{module}.import_module" for module in importlib_modules
    }
    known_spec_calls = spec_names | {
        f"{module}.spec_from_file_location" for module in importlib_util_modules
    }
    known_module_from_spec_calls = module_from_spec_names | {
        f"{module}.module_from_spec" for module in importlib_util_modules
    }
    known_builtin_import_calls = builtin_import_names | {
        f"{module}.__import__" for module in builtin_modules
    }
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        value = _dotted_name(node.value)
        if not isinstance(target, ast.Name) or value is None:
            continue
        for aliases, known in (
            (import_module_names, known_import_module_calls),
            (spec_names, known_spec_calls),
            (module_from_spec_names, known_module_from_spec_calls),
            (builtin_import_names, known_builtin_import_calls),
        ):
            if value in known:
                aliases.add(target.id)
                known.add(target.id)
                break

    def has_unpacked_keywords(call: ast.Call) -> bool:
        """Return whether ``**kwargs`` can supply an unknown loader argument."""
        return any(item.arg is None for item in call.keywords)

    def keyword_or_positional(call: ast.Call, keyword: str, index: int) -> ast.AST | None:
        for item in call.keywords:
            if item.arg == keyword:
                return item.value
        return call.args[index] if len(call.args) > index else None

    calls: list[tuple[int, str]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        function = _dotted_name(node.func)
        if function in known_import_module_calls:
            if has_unpacked_keywords(node):
                calls.append((node.lineno, "import_module"))
                continue
            name = keyword_or_positional(node, "name", 0)
            package = keyword_or_positional(node, "package", 1)
            nonliteral_name = name is not None and not _static_string_expression(name)
            relative_package = False
            if name is not None:
                name_value = _static_string_value(name)
                relative_package = (
                    name_value is not None
                    and name_value.startswith(".")
                    and package is not None
                    and not _static_string_expression(package)
                )
            if nonliteral_name or relative_package:
                calls.append((node.lineno, "import_module"))
        elif function in known_builtin_import_calls:
            if has_unpacked_keywords(node):
                calls.append((node.lineno, "__import__"))
                continue
            name = keyword_or_positional(node, "name", 0)
            if name is not None and not _static_string_expression(name):
                calls.append((node.lineno, "__import__"))
        elif function in known_spec_calls:
            if has_unpacked_keywords(node):
                calls.append((node.lineno, "spec_from_file_location"))
                continue
            name = keyword_or_positional(node, "name", 0)
            location = keyword_or_positional(node, "location", 1)
            if any(
                value is not None and not _static_string_expression(value)
                for value in (name, location)
            ):
                calls.append((node.lineno, "spec_from_file_location"))
        elif function in known_module_from_spec_calls:
            calls.append((node.lineno, "module_from_spec"))
        elif isinstance(node.func, ast.Attribute) and node.func.attr == "exec_module":
            calls.append((node.lineno, "exec_module"))
    return sorted(calls)


def _nonliteral_runtime_loader_lines(source: str) -> list[int]:
    return [line for line, _kind in _runtime_loader_calls(source)]


def dynamic_import_manifest_violations(
    manifest: dict[str, tuple[str, ...]], repository_root: Path
) -> list[str]:
    """Validate deterministic source/pattern entries in the dynamic manifest."""
    problems: list[str] = []
    for source, patterns in manifest.items():
        source_problem = _path_violation(source)
        if source_problem:
            problems.append(f"unsafe dynamic source path: {source!r}: {source_problem}")
            continue
        source_path = repository_root / source
        if not source_path.is_file():
            problems.append(f"dynamic source does not exist: {source}")
            continue
        lines = _nonliteral_runtime_loader_lines(
            source_path.read_text(encoding="utf-8", errors="surrogateescape")
        )
        if not lines:
            problems.append(f"dynamic source has no nonliteral import_module call: {source}")
        if not patterns:
            problems.append(f"dynamic source has no target patterns: {source}")
        for pattern in patterns:
            path = Path(pattern)
            windows_path = PureWindowsPath(pattern)
            if (
                not pattern
                or path.is_absolute()
                or windows_path.drive
                or windows_path.root
                or "\\" in pattern
                or ".." in path.parts
            ):
                problems.append(f"unsafe dynamic target pattern: {source}: {pattern!r}")
                continue
            matches = sorted(repository_root.glob(pattern))
            if not matches or any(not match.is_file() for match in matches):
                problems.append(f"dynamic target pattern has no files: {source}: {pattern}")
    return problems


def dynamic_import_source_violations(manifest: dict[str, str], repository_root: Path) -> list[str]:
    """Validate source keys and loader semantics for a runtime policy map."""
    problems: list[str] = []
    for source in manifest:
        source_problem = _path_violation(source)
        if source_problem:
            problems.append(f"unsafe dynamic source path: {source!r}: {source_problem}")
            continue
        source_path = repository_root / source
        if not source_path.is_file():
            problems.append(f"dynamic source does not exist: {source}")
            continue
        if not _nonliteral_runtime_loader_lines(
            source_path.read_text(encoding="utf-8", errors="surrogateescape")
        ):
            problems.append(f"dynamic source has no runtime loader call: {source}")
    return problems


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
    if (
        path.startswith("/")
        or "\\" in path
        or PureWindowsPath(path).drive
        or PureWindowsPath(path).root
        or any(character.isspace() for character in path)
        or path != path.strip()
        or path != Path(path).as_posix()
    ):
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
            "socket.waitForConnected(timeout);",
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
        self.assertIsNotNone(
            py.search(scanner.mask_py_non_code("socket.waitForConnected(timeout)")),
            "QLocalSocket waitForConnected",
        )
        for source in (
            '".".join(parts)',
            "Path(directory).joinpath(name)",
            '"".join(parts)',
        ):
            self.assertEqual(scanner._python_thread_join_matches(source), [], source)
        self.assertEqual(
            scanner._python_thread_join_matches("machine.join()"),
            [(1, "machine.join()")],
        )

    def test_thread_wait_structural_python_join_covers_workers(self) -> None:
        source = """\
machine = createMachine()
machine.join()
task.join()
str.join(parts)
"""
        matches = scanner._python_thread_join_matches(source)
        self.assertEqual(matches, [(2, "machine.join()"), (3, "task.join()")])

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
        # update-data-provider uses AST classification on Python.
        self.assertIsNone(scanner.compiled_pattern("update-data-provider", "py"))

    def test_python_pattern_none_set_matches_rules_documentation(self) -> None:
        none_keys = {category.key for category in rules.CATEGORIES if category.py_pattern is None}
        self.assertEqual(none_keys, {"update-data-provider"})
        readme = (PACKAGE_DIR / "README.md").read_text(encoding="utf-8")
        self.assertRegex(readme, r"`update-data-provider` Python side uses an AST classifier")
        self.assertNotIn(
            "blocking-invokes` (the Qt `BlockingQueuedConnection` connection type) and "
            "`update-data-provider`",
            readme,
        )

    def test_python_update_data_provider_classifier_excludes_qt_models(self) -> None:
        provider = "class ViewProviderThing:\n    def updateData(self, obj, prop):\n        pass\n"
        model = "class Model:\n    def updateData(self, topLeft, bottomRight):\n        pass\n"
        provider_findings = scanner.scan_source(provider, ".py", "snippet.py")
        model_findings = scanner.scan_source(
            model, ".py", "src/Mod/CAM/Path/Base/Gui/PropertyBag.py"
        )
        self.assertIn(
            (2, "update-data-provider"),
            {(finding.line, finding.category) for finding in provider_findings},
        )
        self.assertNotIn(
            (2, "update-data-provider"),
            {(finding.line, finding.category) for finding in model_findings},
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
        live_finding = next(
            finding for finding in multiline_findings if finding.category == "live-app-dereference"
        )
        self.assertEqual(live_finding.line, 2)
        self.assertIn('"FreeCAD."', live_finding.evidence)
        self.assertIn('"ActiveDocument.recompute()"', live_finding.evidence)
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
        escaped_fstring = 'Gui.doCommand(f"FreeCAD\\x2eActiveDocument.recompute()")'
        escaped_fstring_findings = scanner.scan_source(escaped_fstring, ".py", "snippet.py")
        escaped_fstring_categories = {finding.category for finding in escaped_fstring_findings}
        self.assertIn("live-app-dereference", escaped_fstring_categories)
        self.assertIn("direct-recompute", escaped_fstring_categories)
        self.assertTrue(
            any(
                finding.category == "live-app-dereference"
                and finding.line == 1
                and "ActiveDocument" in finding.evidence
                for finding in escaped_fstring_findings
            )
        )
        interpolated_fstring = "Gui.doCommand(f\"FreeCAD\\x2eActiveDocument.getObject('{name}')\")"
        interpolated_findings = scanner.scan_source(interpolated_fstring, ".py", "snippet.py")
        self.assertIn(
            "live-app-dereference", {finding.category for finding in interpolated_findings}
        )
        self.assertTrue(
            all(finding.line == 1 for finding in interpolated_findings), interpolated_findings
        )
        separated_interpolation = 'Gui.doCommand(f"FreeCAD{name}.ActiveDocument")'
        self.assertNotIn(
            "live-app-dereference",
            {
                finding.category
                for finding in scanner.scan_source(separated_interpolation, ".py", "snippet.py")
            },
        )
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

    def test_cpp_gui_command_wrappers_expand_executable_literals(self) -> None:
        source = (
            'Gui::cmdAppDocument(doc, "recompute()");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument."\n'
            '    "recompute()");\n'
            'FCMD_DOC_CMD(doc, R"tag(recompute())tag");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, "%s.recompute()", name);\n'
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        direct = [finding for finding in findings if finding.category == "direct-recompute"]
        self.assertEqual([finding.line for finding in direct], [1, 2, 4, 5])
        self.assertIn('"recompute()"', direct[0].evidence)
        self.assertIn('"App.ActiveDocument."', direct[1].evidence)
        self.assertIn('"recompute()"', direct[1].evidence)
        self.assertIn('R"tag(recompute())tag"', direct[2].evidence)
        self.assertIn('"%s.recompute()"', direct[3].evidence)

    def test_cpp_gui_wrappers_resolve_global_local_member_and_anonymous_names(self) -> None:
        source = (
            "namespace Gui {\n"
            "struct Command { enum Type { Doc }; "
            "static void doCommand(Type, const char*); };\n"
            "void cmdAppDocument(void*, const char*);\n"
            "}\n"
            "namespace Other { namespace Gui { "
            "void cmdAppDocument(void*, const char*); } }\n"
            "struct Holder : ::Gui::Command {};\n"
            "void sample(void* doc) {\n"
            "    :: /*comment*/ Gui::cmdAppDocument(doc, "
            '"App.ActiveDocument.recompute()");\n'
            "    Other::Gui::cmdAppDocument(doc, "
            '"App.ActiveDocument.recompute()");\n'
            "    Holder h;\n"
            "    h.Gui::Command::doCommand(Gui::Command::Doc, "
            '"App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace { namespace Gui { "
            "void cmdAppDocument(void*, const char*); }\n"
            "void anonymous_sample(void* doc) {\n"
            '    Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "    :: /*comment*/ Gui::cmdAppDocument(doc, "
            '"App.ActiveDocument.recompute()");\n'
            "}}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_wrapper_resolution.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++17", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertIn((8, "direct-recompute"), categories)
        self.assertIn((8, "live-app-dereference"), categories)
        self.assertNotIn((9, "direct-recompute"), categories)
        self.assertNotIn((11, "direct-recompute"), categories)
        self.assertNotIn((15, "direct-recompute"), categories)
        self.assertIn((16, "direct-recompute"), categories)
        self.assertIn((16, "live-app-dereference"), categories)

    def test_cpp_gui_wrappers_resolve_nested_named_and_anonymous_shadowing(self) -> None:
        source = (
            "namespace Gui {\n"
            "void cmdAppDocument(void*, const char*);\n"
            "namespace Nested {\n"
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "void named(void* doc) {\n"
            '    Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            '    :: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
            "namespace {\n"
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "void anonymous(void* doc) {\n"
            '    Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            '    :: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_nested_shadowing.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++17", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertNotIn((6, "direct-recompute"), categories)
        self.assertIn((7, "direct-recompute"), categories)
        self.assertNotIn((13, "direct-recompute"), categories)
        self.assertIn((14, "direct-recompute"), categories)

    def test_cpp_gui_qualified_wrappers_all_decode_explicit_global_calls(self) -> None:
        source = (
            "namespace Gui {\n"
            "struct Command { static void doCommand(...); "
            "static void runCommand(...); };\n"
            "void cmdAppDocument(...);\n"
            "void cmdGuiDocument(...);\n"
            "void cmdAppObject(...);\n"
            "void cmdGuiObject(...);\n"
            "void cmdAppDocumentArgs(...);\n"
            "void cmdAppObjectArgs(...);\n"
            "void cmdGuiObjectArgs(...);\n"
            "void doCommandT(...);\n"
            "}\n"
            "void sample(void* doc) {\n"
            "    :: /*comment*/ Gui::Command::doCommand(0, "
            '"App.ActiveDocument.recompute()");\n'
            '    ::Gui::Command::runCommand(0, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdGuiDocument(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppObject(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdGuiObject(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppDocumentArgs(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppObjectArgs(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdGuiObjectArgs(doc, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::doCommandT(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_qualified_wrappers.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++17", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        expected_lines = set(range(13, 23))
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            expected_lines,
        )
        self.assertEqual(
            {line for line, category in categories if category == "live-app-dereference"},
            expected_lines,
        )

    def test_cpp_gui_wrappers_skip_unevaluated_operands_but_keep_evaluated_calls(self) -> None:
        source = (
            "namespace Gui {\n"
            "struct Result { virtual ~Result() = default; };\n"
            "Result& cmdAppDocument(void*, const char*) noexcept;\n"
            "}\n"
            "using Declared = decltype((:: /*comment*/ Gui::cmdAppDocument(\n"
            '    nullptr, "App.ActiveDocument.recompute()")));\n'
            "bool no_throw(void* doc) {\n"
            '    return noexcept(:: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()"));\n'
            "}\n"
            "auto sized = sizeof((:: /*comment*/ Gui::cmdAppDocument(\n"
            '    nullptr, "App.ActiveDocument.recompute()")));\n'
            'auto directly_sized = sizeof :: /*comment*/ Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "template <typename T> concept HasCommand = requires(T value) {\n"
            '    :: /*comment*/ Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "Gui::Result& returned(void* doc) {\n"
            '    return :: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "void thrown(void* doc) {\n"
            '    throw :: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "struct Task { struct promise_type {\n"
            "    Task get_return_object();\n"
            "    std::suspend_never initial_suspend() noexcept;\n"
            "    std::suspend_never final_suspend() noexcept;\n"
            "    void unhandled_exception();\n"
            "    void return_value(Gui::Result&);\n"
            "}; };\n"
            "Task co_returned(void* doc) {\n"
            '    co_return :: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "const std::type_info& identified(void* doc) {\n"
            '    return typeid(:: /*comment*/ Gui::cmdAppDocument(doc, "App.ActiveDocument.recompute()"));\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_unevaluated_operands.cpp"
                snippet.write_text(
                    "#include <coroutine>\n#include <typeinfo>\n" + source, encoding="utf-8"
                )
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {17, 20, 30, 33},
        )
        self.assertEqual(
            {line for line, category in categories if category == "live-app-dereference"},
            {17, 20, 30, 33},
        )

    def test_cpp_requires_clause_keeps_function_and_lambda_bodies_evaluated(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "template<class T> void constrained(T) requires (sizeof(T)>0) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "template<class T> concept HasCommand = requires(T value) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "    requires requires {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "    };\n"
            "};\n"
            "auto constrained_lambda = []<class T>() requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "auto mutable_lambda = []<class T>() mutable requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "auto constexpr_lambda = []<class T>() constexpr requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "auto consteval_lambda = []<class T>() consteval requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_requires_clause.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {3, 12, 15, 18, 21},
        )

    def test_cpp_requires_expression_logical_operator_and_pointer_return_clause(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "template<class T> constexpr bool predicate() { return true; }\n"
            "template<class T> concept Logical = predicate<T>() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "recompute()");\n'
            "};\n"
            "template<class T>\n"
            "auto pointer_clause(T value) -> T* requires (sizeof(T)>0) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "    return nullptr;\n"
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_requires_clause_tokens.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {8},
        )

    def test_cpp_sizeof_unparenthesized_unary_operands_stop_at_evaluated_boundary(self) -> None:
        source = (
            "namespace Gui {\n"
            "long* cmdAppDocument(void*, const char*);\n"
            "long cmdAppDocument(int, const char*);\n"
            "long& cmdAppDocument(char, const char*);\n"
            "}\n"
            "void sized() {\n"
            '    auto star = sizeof * /* comment */ ::Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    auto plus = sizeof + /* comment */ ::Gui::cmdAppDocument(1, "recompute()");\n'
            '    auto minus = sizeof - ::Gui::cmdAppDocument(1, "recompute()");\n'
            '    auto logical = sizeof !! ::Gui::cmdAppDocument(1, "recompute()");\n'
            '    auto bitwise = sizeof ~ ::Gui::cmdAppDocument(1, "recompute()");\n'
            '    auto address = sizeof & ::Gui::cmdAppDocument((char)0, "recompute()");\n'
            '    auto increment = sizeof ++ ::Gui::cmdAppDocument((char)0, "recompute()");\n'
            '    auto decrement = sizeof -- ::Gui::cmdAppDocument((char)0, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_sizeof_unary.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {15},
        )

    def test_cpp_sizeof_unary_operands_consume_transparent_parentheses_and_casts(self) -> None:
        source = (
            "struct Result {};\n"
            "namespace Gui { Result* cmdAppDocument(void*, const char*); }\n"
            "void sized() {\n"
            '    auto parenthesized = sizeof *(::Gui::cmdAppDocument(nullptr, "recompute()"));\n'
            '    auto casted = sizeof static_cast<Result*>(::Gui::cmdAppDocument(nullptr, "recompute()"));\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_sizeof_transparent.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {6},
        )

    def test_cpp_gui_wrappers_reject_type_alias_and_local_gui_shadows(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "namespace Foreign { void cmdAppDocument(void*, const char*); }\n"
            "struct ForeignType { static void cmdAppDocument(void*, const char*); };\n"
            "namespace StructScope {\n"
            "struct Gui { static void cmdAppDocument(void*, const char*); };\n"
            "void check() {\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
            "namespace AliasScope {\n"
            "using Gui = ForeignType;\n"
            'void check() { Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "}\n"
            "namespace TypedefScope {\n"
            "typedef ForeignType Gui;\n"
            'void check() { Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "}\n"
            "namespace NamespaceAliasScope {\n"
            "namespace Gui = Foreign;\n"
            'void check() { Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "}\n"
            "void block_check() {\n"
            "    struct Gui { static void cmdAppDocument(void*, const char*); };\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "template<class Gui> void template_check() {\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_type_shadows.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {8, 14, 19, 24, 29, 33},
        )

    def test_cpp_gui_shadow_scope_order_and_reopened_namespaces(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "namespace OrderedScope {\n"
            'void before() { Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "struct Gui { static void cmdAppDocument(void*, const char*); };\n"
            'void after() { Gui::cmdAppDocument(nullptr, "recompute()"); }\n'
            "}\n"
            "namespace ReopenedScope {\n"
            "struct Gui { static void cmdAppDocument(void*, const char*); };\n"
            "}\n"
            "namespace ReopenedScope {\n"
            'void later() { Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "}\n"
            "void block_lifetime() {\n"
            "    {\n"
            "        struct Gui { static void cmdAppDocument(void*, const char*); };\n"
            '        Gui::cmdAppDocument(nullptr, "recompute()");\n'
            "    }\n"
            '    Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_shadow_scope.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {3, 12, 19},
        )

    def test_cpp_generic_lambda_and_constrained_template_gui_shadow_lifetimes(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "auto generic = []<class Gui>() {\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            'void after_generic() { Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "template<class Gui> requires requires { typename Gui::value_type; }\n"
            "void constrained() {\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "recompute()");\n'
            "}\n"
            'void after_constrained() { Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_template_shadow_lifetimes.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {4, 6, 10, 12},
        )

    def test_cpp_constrained_gui_template_parameters_shadow_only_parameter_scopes(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "template<class T> concept Type = true;\n"
            "template<Type Gui> void constrained() {\n"
            '    Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "auto constrained_lambda = []<Type Gui>() {\n"
            '    Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "void after_constrained() {\n"
            '    Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace External { struct Gui {}; }\n"
            "namespace DefaultScope {\n"
            "template<class T = External::Gui> void defaulted() {\n"
            '    Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_constrained_template_parameter.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {5, 9, 12, 17, 18},
        )

    def test_cpp_requires_clause_operator_names_keep_evaluated_bodies(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "struct Widget {};\n"
            "template<class T> constexpr bool predicate() { return true; }\n"
            "template<class T> concept Logical = predicate<T>() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "template<class T> constexpr bool returned() {\n"
            "    return predicate<T>() && requires {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "    };\n"
            "}\n"
            "template<class T> struct P {\n"
            "    Widget* pointer(T) && requires (sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return nullptr;\n"
            "    }\n"
            "};\n"
            "template<class T> struct S {\n"
            "    auto operator==(T) && requires (sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return true;\n"
            "    }\n"
            "    S& operator=(T) && requires (sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return *this;\n"
            "    }\n"
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_requires_operator_names.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {14, 20, 24},
        )

    def test_cpp_requires_clause_suffixes_keep_evaluated_bodies(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "struct Widget {};\n"
            "template<class T> constexpr bool predicate() { return true; }\n"
            "template<class T> concept Logical = predicate<T>() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "template<class T> constexpr bool returned() {\n"
            "    return predicate<T>() && requires {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "    };\n"
            "}\n"
            "template<class T> struct S {\n"
            "    Widget* noexcepted(T) && noexcept requires(sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return nullptr;\n"
            "    }\n"
            "    Widget* conditionally_noexcept(T) & noexcept(false) requires(sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return nullptr;\n"
            "    }\n"
            "    auto trailing(T) && -> Widget* requires(sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return nullptr;\n"
            "    }\n"
            "    auto operator==(T) && noexcept [[nodiscard]] requires(sizeof(T)>0) {\n"
            '        ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "        return true;\n"
            "    }\n"
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_requires_clause_suffixes.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {14, 18, 22, 26},
        )

    def test_cpp_requires_clause_distinguishes_lambda_assignment_from_subscript(self) -> None:
        source = (
            "#include <array>\n"
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "constexpr bool predicate() { return true; }\n"
            "constexpr auto callbacks = std::array{&predicate};\n"
            "auto assigned = []<class T>() requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "constexpr bool invoked = []() { return true; }() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "template<class T> concept C = callbacks[0]() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_requires_lambda_subscript.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {6},
        )

    def test_cpp_constrained_generic_lambda_template_head_keeps_body_evaluated(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "auto templated = []<class T> requires true (T) requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "constexpr bool invoked = []() { return true; }() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_constrained_generic_lambda.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {3},
        )

    def test_cpp_generic_lambda_requires_clause_without_parameter_list(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "auto constrained = []<class T> requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_generic_lambda_requires.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertIn((3, "direct-recompute"), categories)

    def test_cpp_generic_lambda_nested_init_capture_requires_clause_without_parameter_list(
        self,
    ) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "int arr[1]{};\n"
            "auto constrained = [p = arr[0]]<class T = int> requires (true) { "
            '::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); };\n'
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_generic_lambda_nested_capture.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertIn((3, "direct-recompute"), categories)

    def test_cpp_nested_capture_invocation_requires_body_is_not_misclassified(self) -> None:
        source = (
            "#include <utility>\n"
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "void f() { int arr[1]{}; bool C = [p=arr[0]]<class U>{ return true; }.\n"
            "template operator()<std::pair<int, std::pair<int,int>>>() && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}; (void)C; }\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_nested_capture_invocation.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            finding.category
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertNotIn("direct-recompute", categories)
        self.assertNotIn("live-app-dereference", categories)

    def test_cpp_generic_lambda_invocations_keep_requires_bodies_evaluated(
        self,
    ) -> None:
        sources = (
            (
                "template<class T> concept Direct = []<class U = int> requires(true) { return true; }()"
                " && requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
                "};\n"
            ),
            (
                "template<class T> concept Explicit = []<class U> requires(true) { return true; }."
                " template /*comment*/ operator()<int>() && requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
                "};\n"
            ),
            (
                "constexpr bool Nondependent = []<class U> { return true; }."
                "operator()<int>() && requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
                "};\n"
            ),
            (
                "constexpr bool Spaced = []<class U> { return true; }"
                ". /*comment*/ template /*comment*/ operator /*comment*/ ()"
                " < /*comment*/ std::pair<int, std::pair<int, int>> > () && requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
                "};\n"
            ),
            (
                "auto long_trivia = []<class T>"
                + "/*"
                + "x" * 700
                + "*/"
                + " requires (true) {\n"
                + '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
                + "};\n"
            ),
        )
        for index, body in enumerate(sources):
            source = (
                "#include <utility>\n"
                "namespace Gui { void cmdAppDocument(void*, const char*); }\n" + body
            )
            if shutil.which("g++"):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    snippet = (
                        Path(temporary_directory)
                        / f"gui_generic_lambda_requires_invocation_{index}.cpp"
                    )
                    snippet.write_text(source, encoding="utf-8")
                    result = subprocess.run(
                        ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
            categories = {
                (finding.line, finding.category)
                for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
            }
            if index == len(sources) - 1:
                self.assertIn((4, "direct-recompute"), categories)
                self.assertIn((4, "live-app-dereference"), categories)
            else:
                self.assertNotIn((4, "direct-recompute"), categories)
                self.assertNotIn((4, "live-app-dereference"), categories)

    def test_cpp_generic_lambda_invocation_suffixes_are_not_requires_clauses(self) -> None:
        sources = (
            (
                "template<class T> concept Direct = []<class U>() -> bool { return true; }() && "
                "requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n};\n'
            ),
            (
                "template<class T> concept Explicit = []<class U>() mutable noexcept -> bool { "
                "return true; }.template operator()<int>() && requires {\n"
                '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n};\n'
            ),
        )
        for index, body in enumerate(sources):
            source = "namespace Gui { void cmdAppDocument(void*, const char*); }\n" + body
            if shutil.which("g++"):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    snippet = Path(temporary_directory) / f"gui_lambda_suffix_{index}.cpp"
                    snippet.write_text(source, encoding="utf-8")
                    result = subprocess.run(
                        ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
            categories = {
                finding.category
                for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
            }
            self.assertNotIn("direct-recompute", categories)
            self.assertNotIn("live-app-dereference", categories)

    def test_cpp_generic_lambda_invocations_with_full_heads_are_unevaluated(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "int values[1]{};\n"
            "template<class T> concept Direct = "
            "[value = values[0]]<class U = int>(int unused [[maybe_unused]]) -> bool "
            "{ return true; }(0) && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "template<class T> concept Explicit = "
            "[value = values[0]]<class U = int>(int unused [[maybe_unused]]) -> bool "
            "{ return true; }.template operator()<int>(0) && requires {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "template<class T> void trailing(T) requires (sizeof(T) > 0) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_lambda_full_heads.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            finding.category
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(categories, {"direct-recompute", "live-app-dereference"})
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        self.assertEqual({finding.line for finding in findings}, {10})

    def test_cpp_requires_prefix_boundaries_use_one_forward_pass(self) -> None:
        for operator in (" && ", "&&"):
            clauses = operator.join("requires { typename T::type; }" for _ in range(400))
            source = (
                "namespace N {\n"
                f"template<class T> concept Chained = requires {{ requires ({clauses}); }};\n"
                "}\n"
            )
            with (
                mock.patch.object(
                    scanner,
                    "_cpp_requires_prefix_starts",
                    wraps=scanner._cpp_requires_prefix_starts,
                ) as boundary_pass,
                mock.patch.object(
                    scanner,
                    "_cpp_requires_prefix_start",
                    side_effect=AssertionError("unexpected fallback boundary scan"),
                ),
                mock.patch.object(
                    scanner,
                    "_cpp_requires_generic_lambda_invocation_expression",
                    wraps=scanner._cpp_requires_generic_lambda_invocation_expression,
                ) as invocation_classifier,
                mock.patch.object(
                    scanner,
                    "_cpp_requires_declarator_candidates",
                    wraps=scanner._cpp_requires_declarator_candidates,
                ) as declarator_classifier,
            ):
                self.assertEqual(scanner.scan_source(source, ".cpp", "snippet.cpp"), [])
            self.assertEqual(boundary_pass.call_count, 1)
            # Only the enclosing and first nested requires-expressions reach
            # the expensive classifiers; later operands must not enter either path.
            self.assertEqual(invocation_classifier.call_count, 2)
            self.assertEqual(declarator_classifier.call_count, 4)

    def test_cpp_requires_generic_lambda_assignment_search_is_declaration_bounded(self) -> None:
        source = "\n".join(
            f"auto value{index} = []<class T>() requires (true) {{ return true; }};"
            for index in range(80)
        )
        observed_lengths: list[int] = []
        original = scanner._cpp_requires_lambda_assignment_rhs

        def record_rhs(rhs: str, *, require_generic_template_head: bool = False) -> bool:
            observed_lengths.append(len(rhs))
            return original(rhs, require_generic_template_head=require_generic_template_head)

        with mock.patch.object(scanner, "_cpp_requires_lambda_assignment_rhs", record_rhs):
            self.assertEqual(scanner.scan_source(source, ".cpp", "snippet.cpp"), [])
        self.assertTrue(observed_lengths)
        self.assertLess(max(observed_lengths), 100)

    def test_cpp_nested_requires_constraints_keep_lambda_bodies_evaluated(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "struct X { void foo(); };\n"
            "auto nested = []<class T> requires requires(T t) { t.foo(); } (T) "
            "requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
            "auto logical = []<class T> requires (requires(T t) { t.foo(); } && true) (T) "
            "requires (true) {\n"
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "};\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_nested_requires_constraint.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {4, 7},
        )

    def test_cpp_nested_requires_constraint_rejects_unbalanced_and_top_level_semicolon(
        self,
    ) -> None:
        for rhs in (
            "[]<class T> requires requires(T t) { t.foo(); (T) requires (true)",
            "[]<class T> requires requires(T t) { t.foo(); } ; (T) requires (true)",
        ):
            self.assertFalse(scanner._cpp_requires_lambda_assignment_rhs(rhs), rhs)

    def test_cpp_nested_requires_constraint_equality_keeps_lambda_body_evaluated(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "struct X { int value; };\n"
            "auto nested_eq = []<class T> requires requires(T t) { "
            "requires sizeof(T) == sizeof(X); } (T) requires (true) { "
            '::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); };\n'
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_nested_requires_equality.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {3},
        )

    def test_cpp_nested_requires_lambda_assignment_ignores_prior_operator_token(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "struct X { void foo(); };\n"
            "bool operator==(X, X);\n"
            "auto nested = []<class T> requires requires(T t) { t.foo(); } (T) "
            'requires (true) { ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); };\n'
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_nested_requires_prior_operator.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-pedantic-errors", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {4},
        )

    def test_cpp_using_command_through_local_gui_alias_is_not_global(self) -> None:
        source = (
            "namespace Gui {\n"
            "struct Command { enum Type { Doc }; static void doCommand(Type, const char*); };\n"
            "}\n"
            "namespace Foreign {\n"
            "struct Command { enum Type { Doc }; static void doCommand(Type, const char*); };\n"
            "}\n"
            "namespace Scope {\n"
            "namespace Gui = Foreign;\n"
            "using Gui::Command;\n"
            'void local() { Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()"); }\n'
            "}\n"
            "using Gui::Command;\n"
            'void global() { Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()"); }\n'
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_using_alias.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {13},
        )

    def test_cpp_template_gui_forward_declaration_does_not_leak_a_shadow(self) -> None:
        source = (
            "namespace Gui { void cmdAppDocument(void*, const char*); }\n"
            "template<class Gui> void forward_template();\n"
            'void unrelated() { Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()"); }\n'
            "template<class Gui> void defined_template() {\n"
            '    Gui::cmdAppDocument(nullptr, "recompute()");\n'
            '    ::Gui::cmdAppDocument(nullptr, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        if shutil.which("g++"):
            with tempfile.TemporaryDirectory() as temporary_directory:
                snippet = Path(temporary_directory) / "gui_template_forward.cpp"
                snippet.write_text(source, encoding="utf-8")
                result = subprocess.run(
                    ["g++", "-std=c++20", "-fsyntax-only", str(snippet)],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {3, 6},
        )

    def test_cpp_gui_command_extraction_ignores_inert_and_unrelated_strings(self) -> None:
        source = (
            '// Gui::cmdAppDocument(doc, "recompute()");\n'
            'const char *inert = "App.ActiveDocument.recompute()";\n'
            'unrelated(doc, "App.ActiveDocument.recompute()");\n'
            "Gui::cmdAppDocument(doc, runtime_command);\n"
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        self.assertEqual(
            [finding for finding in findings if finding.category == "direct-recompute"], []
        )

    def test_cpp_decoded_commands_use_python_patterns_and_mask_nested_code(self) -> None:
        source = (
            "using namespace Gui;\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            'Command::doCommand(Command::Doc, "print(\\"App.ActiveDocument.recompute()\\")");\n'
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        categories = {(finding.line, finding.category) for finding in findings}
        self.assertIn((2, "live-app-dereference"), categories)
        self.assertIn((2, "direct-recompute"), categories)
        self.assertIn((3, "process-events-polling"), categories)
        self.assertNotIn((4, "direct-recompute"), categories)

    def test_cpp_command_namespace_context_rejects_foreign_blocks(self) -> None:
        source = (
            "namespace Gui {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace Other {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace Gui {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        self.assertIn(
            (2, "direct-recompute"),
            {(finding.line, finding.category) for finding in findings},
        )
        self.assertNotIn(
            (5, "direct-recompute"),
            {(finding.line, finding.category) for finding in findings},
        )
        self.assertNotIn(
            (7, "direct-recompute"),
            {(finding.line, finding.category) for finding in findings},
        )

        nested_and_using = (
            "{\n"
            "using namespace Gui;\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace GuiExtra {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        nested_findings = scanner.scan_source(nested_and_using, ".cpp", "src/Gui/Snippet.cpp")
        nested_categories = {(finding.line, finding.category) for finding in nested_findings}
        self.assertIn((3, "direct-recompute"), nested_categories)
        self.assertNotIn((5, "direct-recompute"), nested_categories)
        self.assertNotIn((7, "direct-recompute"), nested_categories)

        using_declaration = (
            "using Gui::Command;\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
        )
        self.assertIn(
            (2, "direct-recompute"),
            {
                (finding.line, finding.category)
                for finding in scanner.scan_source(using_declaration, ".cpp", "src/Gui/Snippet.cpp")
            },
        )

    def test_cpp_command_namespace_resolution_covers_global_and_using_lookup(self) -> None:
        source = (
            '::Gui::Command::doCommand(::Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            '::Gui::Command::runCommand(::Gui::Command::Gui, "Gui.updateGui()");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace Gui {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace Nested {\n"
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            "}\n"
            "}\n"
            "namespace Other {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace Gui {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            '::Gui::Command::doCommand(::Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "using Gui::Command;\n"
            "using ::Gui::Command;\n"
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace GuiExtra {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "using namespace ::Gui;\n"
            "namespace OtherInherited {\n"
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            "}\n"
            "namespace Another {\n"
            "using ::Gui::Command;\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        categories = {(finding.line, finding.category) for finding in findings}
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {1, 3, 5, 15, 31},
        )
        self.assertEqual(
            {line for line, category in categories if category == "process-events-polling"},
            {2, 7, 19, 27},
        )
        self.assertEqual(
            {line for line, category in categories if category == "live-app-dereference"},
            {1, 3, 5, 15, 31},
        )

    def test_cpp_namespace_scope_using_survives_reopening_and_respects_order(self) -> None:
        source = (
            "namespace Reopened {\n"
            "using ::Gui::Command;\n"
            "}\n"
            "namespace Reopened {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "namespace Nested {\n"
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            "}\n"
            "}\n"
            "namespace ReopenedSibling {\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace Directive {\n"
            "using namespace ::Gui;\n"
            "}\n"
            "namespace Directive {\n"
            'Command::runCommand(Command::Gui, "Gui.updateGui()");\n'
            "}\n"
            "namespace OrderBefore {\n"
            "using Gui::Command;\n"
            "}\n"
            "namespace OrderBefore {\n"
            "namespace Gui {}\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "namespace OrderAfter {\n"
            "namespace Gui {}\n"
            "using Gui::Command;\n"
            'Command::doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
        )
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {5, 24},
        )
        self.assertEqual(
            {line for line, category in categories if category == "process-events-polling"},
            {7, 17},
        )
        self.assertEqual(
            {line for line, category in categories if category == "live-app-dereference"},
            {5, 24},
        )

    def test_cpp_local_gui_namespace_shadows_unqualified_global_gui(self) -> None:
        source = (
            "namespace Other {\n"
            "namespace Gui {}\n"
            "namespace Nested {\n"
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            ':: /*comment*/ Gui::Command::runCommand(::Gui::Command::Gui, "Gui.updateGui()");\n'
            "}\n"
            "}\n"
            "namespace OtherLater {\n"
            "namespace Nested {\n"
            'Gui::Command::doCommand(Gui::Command::Doc, "App.ActiveDocument.recompute()");\n'
            "}\n"
            "}\n"
        )
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertEqual(
            {line for line, category in categories if category == "direct-recompute"},
            {10},
        )
        self.assertEqual(
            {line for line, category in categories if category == "process-events-polling"},
            {5},
        )
        self.assertEqual(
            {line for line, category in categories if category == "live-app-dereference"},
            {10},
        )

    def test_cpp_global_command_after_expression_keywords_is_not_contextual(self) -> None:
        source = (
            "namespace Other::Gui {\n"
            "void command() {\n"
            "return :: /*comment*/ Gui::Command::doCommand(\n"
            '    :: /*comment*/ Gui::Command::Doc, "App.ActiveDocument.recompute()"\n'
            ");\n"
            "}\n"
            "}\n"
            "namespace Other::Gui {\n"
            "void contextual() {\n"
            "Other /*comment*/ :: Gui::Command::doCommand(\n"
            '    Gui::Command::Doc, "App.ActiveDocument.recompute()"\n'
            ");\n"
            "}\n"
            "}\n"
        )
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertIn((4, "direct-recompute"), categories)
        self.assertIn((4, "live-app-dereference"), categories)
        self.assertNotIn((10, "direct-recompute"), categories)

    def test_cpp_global_command_after_return_with_spaced_scope_is_found(self) -> None:
        source = (
            "namespace Other::Gui { void f(){ return :: Gui::Command::doCommand("
            ':: Gui::Command::Doc, "App.ActiveDocument.recompute()"); } }\n'
            "namespace Other::Gui { void g(){ Other :: Gui::Command::doCommand("
            'Gui::Command::Doc, "App.ActiveDocument.recompute()"); } }\n'
        )
        categories = {
            (finding.line, finding.category)
            for finding in scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        }
        self.assertIn((1, "direct-recompute"), categories)
        self.assertIn((1, "live-app-dereference"), categories)
        self.assertNotIn((2, "direct-recompute"), categories)

    def test_cpp_command_extraction_rejects_dynamic_and_member_expressions(self) -> None:
        source = (
            'other.doCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            'other->runCommand(Command::Doc, "App.ActiveDocument.recompute()");\n'
            'make_command("App.ActiveDocument.recompute()");\n'
            'prefix + "App.ActiveDocument.recompute()";\n'
            'Command::doCommand(Command::Doc, "App." + "ActiveDocument.recompute()");\n'
            'Gui::cmdAppDocument(doc, "");\n'
        )
        findings = scanner.scan_source(source, ".cpp", "src/Gui/Snippet.cpp")
        self.assertNotIn("direct-recompute", {finding.category for finding in findings})

    def test_cpp_command_extraction_handles_empty_literals_and_spaced_qualifiers(self) -> None:
        empty = scanner.scan_source(
            'Gui::Command::doCommand(Gui::Command::Doc, "");\n'
            'Gui::Command::doCommand(Gui::Command::Doc, R"tag()tag");\n',
            ".cpp",
            "src/Gui/Snippet.cpp",
        )
        self.assertEqual(
            [finding for finding in empty if finding.category == "direct-recompute"], []
        )
        for command, line_map in scanner._decoded_cpp_command_literals(
            'Gui::cmdAppDocument(doc, "");'
        ):
            self.assertEqual(len(command), len(line_map))
        spaced = scanner.scan_source(
            "Gui /*comment*/ :: Command /*comment*/ :: doCommand("
            'Gui::Command::Doc, "App.ActiveDocument.recompute()");\n',
            ".cpp",
            "src/Gui/Snippet.cpp",
        )
        self.assertIn(
            (1, "direct-recompute"),
            {(finding.line, finding.category) for finding in spaced},
        )
        near_miss = scanner.scan_source(
            "Other /*comment*/ :: Command :: doCommand("
            'Gui::Command::Doc, "App.ActiveDocument.recompute()");\n',
            ".cpp",
            "src/Gui/Snippet.cpp",
        )
        self.assertNotIn("direct-recompute", {finding.category for finding in near_miss})
        nested_near_miss = scanner.scan_source(
            "Other /*comment*/ :: Gui /*comment*/ :: Command :: doCommand("
            'Gui::Command::Doc, "App.ActiveDocument.recompute()");\n',
            ".cpp",
            "src/Gui/Snippet.cpp",
        )
        self.assertNotIn("direct-recompute", {finding.category for finding in nested_near_miss})

    def test_runtime_loader_detects_unpacked_and_relative_package_arguments(self) -> None:
        source = """
import builtins
import importlib
from importlib.util import spec_from_file_location
kwargs = runtime_kwargs
runtime_package = get_package()
importlib.import_module(**kwargs)
builtins.__import__(**kwargs)
spec_from_file_location(**kwargs)
importlib.import_module('.relative', package=runtime_package)
importlib.import_module('.relative', package='fixed.package')
importlib.import_module('literal', **kwargs)
"""
        self.assertEqual(
            [kind for _line, kind in _runtime_loader_calls(source)],
            [
                "import_module",
                "__import__",
                "spec_from_file_location",
                "import_module",
                "import_module",
            ],
        )
        self.assertEqual(
            _runtime_loader_calls("importlib.import_module('.relative', package='pkg')"), []
        )
        self.assertEqual(
            [
                kind
                for _line, kind in _runtime_loader_calls(
                    "importlib.import_module(f'.relative', package=runtime_package)"
                )
            ],
            ["import_module"],
        )
        self.assertEqual(
            _runtime_loader_calls("importlib.import_module(f'.relative', package='pkg')"), []
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

    def test_rejects_noncanonical_path_separators_and_dot_components(self) -> None:
        for path in (
            "src//Gui/MainWindow.cpp",
            "src/Gui//MainWindow.cpp",
            "src/./Gui/MainWindow.cpp",
            "C:/absolute/MainWindow.cpp",
            r"C:\absolute\MainWindow.cpp",
        ):
            bad = self._valid_entry()
            bad["path"] = path
            violations = entry_violations(
                bad, REPOSITORY_ROOT, self.category_keys, self.dispositions
            )
            self.assertTrue(any("clean repository-relative" in v for v in violations), path)

    def test_rejects_drive_qualified_exclusion_paths(self) -> None:
        for path in ("C:/absolute/MainWindow.cpp", r"C:\absolute\MainWindow.cpp"):
            exclusion = {
                "path": path,
                "line": 1,
                "category": "thread-waits",
                "reason": "test",
            }
            violations = exclusion_violations([exclusion], frozenset(rules.CATEGORY_BY_KEY))
            self.assertTrue(any("clean repository-relative" in v for v in violations), path)

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

    def test_source_file_enumeration_has_no_duplicates(self) -> None:
        files = [
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in scanner.iter_source_files(REPOSITORY_ROOT)
        ]
        self.assertEqual(len(files), len(set(files)))

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

    def test_thread_wait_connected_found(self) -> None:
        for line in (238, 301):
            self._assert_site("src/Gui/GuiApplication.cpp", line, "thread-waits")

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

    def test_decoded_cpp_command_payload_sites_found(self) -> None:
        for path, line, category in (
            ("src/Gui/CommandDoc.cpp", 1020, "live-app-dereference"),
            ("src/Mod/CAM/Gui/Command.cpp", 123, "live-app-dereference"),
            ("src/Gui/Document.cpp", 2764, "live-app-dereference"),
            ("src/Gui/Document.cpp", 2764, "direct-recompute"),
            ("src/Gui/Document.cpp", 2913, "live-app-dereference"),
            ("src/Gui/Document.cpp", 2913, "direct-recompute"),
        ):
            self._assert_site(path, line, category)

    def test_cpp_gui_command_wrapper_sites_found(self) -> None:
        for path, lines in (
            ("src/Gui/FileHandler.cpp", (181,)),
            ("src/Mod/Fem/Gui/TaskDlgMeshShapeNetgen.cpp", (120, 143)),
            ("src/Mod/PartDesign/Gui/TaskFeatureParameters.cpp", (214, 229, 317)),
            ("src/Mod/Spreadsheet/Gui/SpreadsheetView.cpp", (443,)),
        ):
            for line in lines:
                self._assert_site(path, line, "direct-recompute")

    def test_inline_updateData_override_found(self) -> None:
        self._assert_site("src/Gui/ViewProviderAnnotation.h", 76, "update-data-provider")

    def test_python_thread_wait_found(self) -> None:
        self._assert_site("src/Mod/CAM/Path/Main/Gui/Camotics.py", 151, "thread-waits")

    def test_python_thread_join_waits_found(self) -> None:
        self._assert_site("src/Mod/Fem/femsolver/run.py", 193, "thread-waits")
        self._assert_site("src/Mod/Fem/femsolver/run.py", 435, "thread-waits")
        self._assert_site("src/Mod/Fem/femsolver/task.py", 143, "thread-waits")

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
            "src/Mod/Draft/draftutils/params.py",
            "src/Mod/Draft/draftutils/utils.py",
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

    def test_transitive_gui_imports_are_scoped_or_reviewed(self) -> None:
        """GUI-bearing local imports cannot silently escape the reviewed closure."""
        scope_files = {
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in scanner.iter_source_files(REPOSITORY_ROOT)
        }
        exclusions = {
            **rules.REVIEWED_INITGUI_IMPORT_EXCLUSIONS,
            **rules.REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS,
        }
        mod_root = REPOSITORY_ROOT / "src" / "Mod"

        def candidates(base: Path) -> list[Path]:
            return [base.with_suffix(".py"), base / "__init__.py"]

        def import_targets(path: Path) -> list[Path]:
            try:
                tree = ast.parse(path.read_text(encoding="utf-8"))
            except (SyntaxError, UnicodeDecodeError):
                return []
            targets: list[Path] = []
            try:
                workbench_root = mod_root / path.relative_to(mod_root).parts[0]
            except ValueError:
                # Framework Python under src/Gui has no workbench package;
                # relative imports still resolve from its own directory.
                workbench_root = path.parent
            for node in ast.walk(tree):
                if isinstance(node, ast.Import):
                    names = [alias.name for alias in node.names]
                    bases = [
                        root / Path(*name.split("."))
                        for name in names
                        for root in (workbench_root, mod_root)
                    ]
                    imported_names: list[str] = []
                elif isinstance(node, ast.ImportFrom):
                    if node.level:
                        base = path.parent
                        for _ in range(node.level - 1):
                            base = base.parent
                        if node.module:
                            base = base / Path(*node.module.split("."))
                        bases = [base]
                    else:
                        module_path = Path(*node.module.split(".")) if node.module else Path()
                        bases = [workbench_root / module_path, mod_root / module_path]
                    imported_names = [alias.name for alias in node.names if alias.name != "*"]
                else:
                    continue
                for base in bases:
                    targets.extend(
                        candidate for candidate in candidates(base) if candidate.is_file()
                    )
                    for name in imported_names:
                        targets.extend(
                            candidate
                            for candidate in candidates(base / name)
                            if candidate.is_file()
                        )
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            for pattern in rules.REVIEWED_DYNAMIC_IMPORT_TARGETS.get(relative, ()):
                targets.extend(sorted(REPOSITORY_ROOT.glob(pattern)))
            return targets

        def is_qualifying_gui_module(path: Path) -> bool:
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            if relative in scope_files or relative in exclusions or path.suffix != ".py":
                return False
            source = path.read_text(encoding="utf-8", errors="surrogateescape")
            # The scanner is the source of truth for qualifying sites. Do not
            # let a module escape merely because it lacks a GUI-named symbol;
            # explicit App/model exclusions above keep the closure narrow.
            return bool(scanner.scan_source(source, path.suffix, relative))

        queue = [
            path for path in scanner.iter_source_files(REPOSITORY_ROOT) if path.suffix == ".py"
        ]
        seen: set[Path] = set(queue)
        missing: set[str] = set()
        bim_targets = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in import_targets(REPOSITORY_ROOT / "src/Mod/BIM/ArchStructure.py")
        }
        self.assertIn("src/Mod/BIM/ArchComponent.py", bim_targets)
        arch_targets = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in import_targets(REPOSITORY_ROOT / "src/Mod/BIM/Arch.py")
        }
        self.assertIn("src/Mod/BIM/ArchCovering.py", arch_targets)
        self.assertNotIn(
            "src/Mod/BIM/ArchWindowPresets.py",
            {
                alias.name.split(".")[-1]
                for node in ast.walk(
                    ast.parse((REPOSITORY_ROOT / "src/Mod/BIM/Arch.py").read_text())
                )
                if isinstance(node, ast.Import)
                for alias in node.names
            },
        )
        cam_targets = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in import_targets(
                REPOSITORY_ROOT / "src/Mod/CAM/Path/Tool/library/ui/cmd.py"
            )
        }
        self.assertIn("src/Mod/CAM/Path/Tool/library/ui/dock.py", cam_targets)
        relative_cam_targets = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in import_targets(
                REPOSITORY_ROOT / "src/Mod/CAM/Path/Tool/library/ui/dock.py"
            )
        }
        self.assertIn("src/Mod/CAM/Path/Tool/library/ui/editor.py", relative_cam_targets)
        self.assertIn(
            "src/Mod/CAM/Path/Op/Gui/Adaptive.py",
            {
                target.relative_to(REPOSITORY_ROOT).as_posix()
                for target in import_targets(REPOSITORY_ROOT / "src/Mod/CAM/Path/Op/Gui/Base.py")
            },
        )
        processor_path = REPOSITORY_ROOT / "src/Mod/CAM/Path/Post/Processor.py"
        processor_relative = processor_path.relative_to(REPOSITORY_ROOT).as_posix()
        command_targets = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in import_targets(REPOSITORY_ROOT / "src/Mod/CAM/Path/Post/Command.py")
        }
        self.assertIn(processor_relative, command_targets)
        self.assertTrue(_nonliteral_runtime_loader_lines(processor_path.read_text()))
        dynamic_policy = {
            *rules.REVIEWED_DYNAMIC_IMPORT_TARGETS,
            *rules.REVIEWED_DYNAMIC_IMPORT_EXTERNAL_SOURCES,
            *rules.REVIEWED_DYNAMIC_IMPORT_SCOPED_PACKAGES,
        }
        self.assertIn(processor_relative, dynamic_policy)
        self.assertIsInstance(import_targets(REPOSITORY_ROOT / "src/Gui/TreeParams.py"), list)
        self.assertNotIn("src/Mod/CAM/Path/Base/PropertyBag.py", exclusions)
        for relative in rules.REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS:
            self.assertTrue((REPOSITORY_ROOT / relative).is_file(), relative)
        for relative in (
            "src/Mod/CAM/Path/Post/Command.py",
            "src/Mod/BIM/ArchNesting.py",
            "src/Mod/Fem/femsolver/run.py",
            "src/Mod/Fem/femsolver/task.py",
        ):
            self.assertNotIn(relative, exclusions)
        while queue:
            current = queue.pop()
            for target in import_targets(current):
                relative = target.relative_to(REPOSITORY_ROOT).as_posix()
                if is_qualifying_gui_module(target):
                    missing.add(relative)
                if target not in seen:
                    seen.add(target)
                    queue.append(target)
        self.assertFalse(
            sorted(missing),
            "unreviewed transitive GUI imports: " + ", ".join(sorted(missing)),
        )
        reachable_dynamic = {
            target.relative_to(REPOSITORY_ROOT).as_posix()
            for target in seen
            if target.suffix == ".py"
            and _nonliteral_runtime_loader_lines(
                target.read_text(encoding="utf-8", errors="surrogateescape")
            )
        }
        self.assertFalse(
            reachable_dynamic - dynamic_policy,
            "unreviewed reachable dynamic imports: "
            + ", ".join(sorted(reachable_dynamic - dynamic_policy)),
        )
        # Mutation guard: removing the Processor policy must expose the exact
        # reachable loader rather than silently passing because it has no GUI
        # inventory finding of its own.
        self.assertIn(
            processor_relative, reachable_dynamic - (dynamic_policy - {processor_relative})
        )

    def test_reviewed_exclusions_have_no_executable_gui_hooks(self) -> None:
        exclusions = {
            **rules.REVIEWED_INITGUI_IMPORT_EXCLUSIONS,
            **rules.REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS,
        }
        for relative in exclusions:
            source = (REPOSITORY_ROOT / relative).read_text(
                encoding="utf-8", errors="surrogateescape"
            )
            self.assertEqual(
                _executable_gui_hook_lines(source),
                [],
                f"GUI hook hidden by exclusion {relative}",
            )

    def test_gui_hook_classifier_ignores_comments_and_docstrings(self) -> None:
        inert = '"""FreeCADGui ViewObject GuiUp"""\n# FreeCADGui\nvalue = "GuiUp"\n'
        self.assertEqual(_executable_gui_hook_lines(inert), [])
        self.assertEqual(_executable_gui_hook_lines("if App.GuiUp:\n    pass\n"), [1])
        self.assertEqual(
            _executable_gui_hook_lines("from FreeCAD import Gui\nGui.activeView()\n"), [1, 2]
        )

    def test_dynamic_import_manifest_is_existing_and_nonliteral(self) -> None:
        problems = dynamic_import_manifest_violations(
            rules.REVIEWED_DYNAMIC_IMPORT_TARGETS, REPOSITORY_ROOT
        )
        self.assertFalse(problems, "\n".join(problems))

    def test_dynamic_import_policy_sources_are_existing_and_nonliteral(self) -> None:
        for manifest in (
            rules.REVIEWED_DYNAMIC_IMPORT_EXTERNAL_SOURCES,
            rules.REVIEWED_DYNAMIC_IMPORT_SCOPED_PACKAGES,
        ):
            problems = dynamic_import_source_violations(manifest, REPOSITORY_ROOT)
            self.assertFalse(problems, "\n".join(problems))

    def test_runtime_loader_classifier_handles_aliases_keywords_and_constants(self) -> None:
        source = """
import builtins as bi
import importlib as il
from importlib import import_module as load
from importlib.util import spec_from_file_location as make_spec
fixed = "fixed"
il.import_module("literal")
load(name=fixed)
bi.__import__(name="literal")
module = il.import_module(name)
loaded = load(name=module_name)
spec = il.util.spec_from_file_location(name="post", location=path)
module_from_spec(spec)
spec.loader.exec_module(module)
"""
        calls = _runtime_loader_calls(source)
        self.assertEqual(
            [kind for _line, kind in calls],
            [
                "import_module",
                "import_module",
                "import_module",
                "spec_from_file_location",
                "module_from_spec",
                "exec_module",
            ],
        )
        self.assertEqual(_nonliteral_runtime_loader_lines('import_module("literal")'), [])
        self.assertEqual(_nonliteral_runtime_loader_lines('import_module("a" "b")'), [])
        self.assertEqual(_nonliteral_runtime_loader_lines('import_module(f"literal")'), [])

    def test_runtime_loader_classifier_handles_assignment_aliases(self) -> None:
        source = """
import builtins
import importlib
load = importlib.import_module
load_builtin = builtins.__import__
make_spec = importlib.util.spec_from_file_location
make_module = importlib.util.module_from_spec
load(runtime_name)
load_builtin(runtime_name)
spec = make_spec(runtime_name, location)
make_module(spec)
"""
        self.assertEqual(
            [kind for _line, kind in _runtime_loader_calls(source)],
            [
                "import_module",
                "__import__",
                "spec_from_file_location",
                "module_from_spec",
            ],
        )

    def test_runtime_loader_constants_are_invalidated_by_rebinding(self) -> None:
        source = """
import importlib
name = "fixed"
name = runtime_name
importlib.import_module(name)
"""
        self.assertEqual([kind for _line, kind in _runtime_loader_calls(source)], ["import_module"])

    def test_runtime_loader_names_are_always_runtime_at_call_site(self) -> None:
        sources = (
            'import importlib\nname = "fixed"\nimportlib.import_module(name)\n',
            'import importlib\nname = "fixed"\ndef load():\n    importlib.import_module(name)\n',
            'import importlib\nif condition:\n    name = "fixed"\nimportlib.import_module(name)\n',
            'import importlib\nname = "fixed"\ndef load(name):\n    importlib.import_module(name)\n',
        )
        for source in sources:
            self.assertEqual(
                [kind for _line, kind in _runtime_loader_calls(source)], ["import_module"]
            )

    def test_runtime_loader_policy_mutations_reject_bad_sources(self) -> None:
        manifests = (
            ("target", rules.REVIEWED_DYNAMIC_IMPORT_TARGETS),
            ("external", rules.REVIEWED_DYNAMIC_IMPORT_EXTERNAL_SOURCES),
            ("scoped", rules.REVIEWED_DYNAMIC_IMPORT_SCOPED_PACKAGES),
        )
        bad_sources = (
            "src/Mod/BIM/../../Gui/FreeCADGuiInit.py",
            "/src/Gui/FreeCADGuiInit.py",
            "C:/src/Gui/FreeCADGuiInit.py",
            "src\\Gui\\FreeCADGuiInit.py",
            " src/Gui/FreeCADGuiInit.py",
            "src/Gui/Free CADGuiInit.py",
            "src/Gui/Free\tCADGuiInit.py",
            "src/Gui/Free\nCADGuiInit.py",
            "src/Gui/Free\u00a0CADGuiInit.py",
            "src/Mod/BIM/missing.py",
            "src/Mod/BIM/ArchStructure.py",
        )
        for kind, manifest in manifests:
            for bad_source in bad_sources:
                mutated = dict(manifest)
                sample = next(iter(manifest.values()))
                mutated[bad_source] = sample
                if kind == "target":
                    problems = dynamic_import_manifest_violations(mutated, REPOSITORY_ROOT)
                else:
                    problems = dynamic_import_source_violations(mutated, REPOSITORY_ROOT)
                self.assertTrue(problems, f"{kind}: {bad_source}")

    def test_dynamic_import_manifest_mutations_are_rejected(self) -> None:
        missing_source = dict(rules.REVIEWED_DYNAMIC_IMPORT_TARGETS)
        missing_source["src/Mod/BIM/missing.py"] = ("src/Mod/BIM/*.py",)
        self.assertTrue(dynamic_import_manifest_violations(missing_source, REPOSITORY_ROOT))
        missing_target = dict(rules.REVIEWED_DYNAMIC_IMPORT_TARGETS)
        missing_target["src/Mod/BIM/Arch.py"] = ("src/Mod/BIM/does-not-exist/*.py",)
        self.assertTrue(dynamic_import_manifest_violations(missing_target, REPOSITORY_ROOT))
        unsafe_target = dict(rules.REVIEWED_DYNAMIC_IMPORT_TARGETS)
        unsafe_target["src/Mod/BIM/Arch.py"] = ("C:/outside/*.py",)
        self.assertTrue(dynamic_import_manifest_violations(unsafe_target, REPOSITORY_ROOT))

    def test_scoped_python_dynamic_imports_have_reviewed_policy(self) -> None:
        policy = {
            *rules.REVIEWED_DYNAMIC_IMPORT_TARGETS,
            *rules.REVIEWED_DYNAMIC_IMPORT_EXTERNAL_SOURCES,
            *rules.REVIEWED_DYNAMIC_IMPORT_SCOPED_PACKAGES,
        }
        unreviewed: list[str] = []
        for path in scanner.iter_source_files(REPOSITORY_ROOT):
            if path.suffix != ".py":
                continue
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            source = path.read_text(encoding="utf-8", errors="surrogateescape")
            if _nonliteral_runtime_loader_lines(source) and relative not in policy:
                unreviewed.append(relative)
        self.assertFalse(unreviewed, "unreviewed dynamic imports: " + ", ".join(unreviewed))

    def test_dynamic_only_targets_are_not_static_imports(self) -> None:
        arch_source = (REPOSITORY_ROOT / "src/Mod/BIM/Arch.py").read_text()
        self.assertFalse(_imports_module(arch_source, "ArchCovering"))
        arch_targets = {
            path.relative_to(REPOSITORY_ROOT).as_posix()
            for path in REPOSITORY_ROOT.glob("src/Mod/BIM/Arch*.py")
        }
        self.assertIn("src/Mod/BIM/ArchCovering.py", arch_targets)
        cam_source = (REPOSITORY_ROOT / "src/Mod/CAM/Path/Op/Gui/Base.py").read_text()
        self.assertFalse(_imports_module(cam_source, "Adaptive"))
        self.assertTrue((REPOSITORY_ROOT / "src/Mod/CAM/Path/Op/Gui/Adaptive.py").is_file())

    def test_imported_module_candidates_include_relative_modules(self) -> None:
        candidates = _imported_module_candidates(
            "from .ArchCovering import X\nfrom .Adaptive import X as AdaptiveX\n"
        )
        self.assertIn(".ArchCovering", candidates)
        self.assertIn(".ArchCovering.X", candidates)
        self.assertIn(".Adaptive", candidates)
        self.assertIn(".Adaptive.X", candidates)

    def test_imports_module_matches_dotted_relative_and_from_forms(self) -> None:
        for source, target in (
            ("import ArchCovering\n", "ArchCovering"),
            ("import BIM.ArchCovering\n", "ArchCovering"),
            ("from .ArchCovering import X\n", "ArchCovering"),
            ("from Path.Op.Gui.Adaptive import X\n", "Adaptive"),
            ("from Path.Op.Gui import Adaptive\n", "Adaptive"),
            ("import Path.Op.Gui.Adaptive as AdaptiveAlias\n", "Adaptive"),
        ):
            self.assertTrue(_imports_module(source, target), source)
        for source in (
            "from Path.Op.Gui import AdaptiveExtra as Adaptive\n",
            "import Path.Op.Gui.AdaptiveExtra as Adaptive\n",
            "from Path.Op.Gui import AdaptiveExtra\n",
        ):
            self.assertFalse(_imports_module(source, "Adaptive"), source)

    def test_reviewed_python_gui_sites_found(self) -> None:
        for path, category in (
            ("src/Mod/Assembly/InitGui.py", "live-app-dereference"),
            ("src/Mod/CAM/InitGui.py", "live-app-dereference"),
            ("src/Mod/Draft/draftutils/gui_utils.py", "live-app-dereference"),
            ("src/Mod/BIM/nativeifc/ifc_viewproviders.py", "direct-recompute"),
            ("src/Mod/Fem/femguiutils/extract_link_view.py", "live-app-dereference"),
            ("src/Mod/Draft/draftutils/params.py", "live-app-dereference"),
            ("src/Mod/Draft/draftutils/utils.py", "live-app-dereference"),
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
        for path, category in (
            ("src/Mod/BIM/ArchCovering.py", "live-app-dereference"),
            ("src/Mod/BIM/ArchVRM.py", "live-app-dereference"),
            ("src/Mod/BIM/importers/importIFCmulticore.py", "direct-recompute"),
            ("src/Mod/CAM/Path/Post/UtilsExport.py", "live-app-dereference"),
            ("src/Mod/Draft/draftmake/make_array.py", "live-app-dereference"),
            ("src/Mod/Draft/draftmake/make_shapestring.py", "direct-recompute"),
            ("src/Mod/Fem/femresult/resulttools.py", "direct-recompute"),
            ("src/Mod/OpenSCAD/OpenSCAD2Dgeom.py", "live-app-dereference"),
        ):
            self.assertTrue(
                any(
                    finding.path == path and finding.category == category
                    for finding in self.scanned
                ),
                f"expected promoted GUI finding in {path}",
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
            "src/Mod/BIM/ArchBuildingPart.py",
            "src/Mod/BIM/ArchStructure.py",
            "src/Mod/BIM/ArchWindowPresets.py",
            "src/Mod/BIM/ArchNesting.py",
            "src/Mod/BIM/ArchCovering.py",
            "src/Mod/BIM/ArchVRM.py",
            "src/Mod/BIM/importers/importIFCmulticore.py",
            "src/Mod/BIM/nativeifc/ifc_tree.py",
            "src/Mod/CAM/Path/Post/Command.py",
            "src/Mod/CAM/Path/Post/Utils.py",
            "src/Mod/CAM/Path/Post/UtilsExport.py",
            "src/Mod/CAM/Path/Main/Sanity/ImageBuilder.py",
            "src/Mod/CAM/Path/Op/Adaptive.py",
            "src/Mod/Fem/femsolver/elmer/equations/equation.py",
            "src/Mod/Fem/femsolver/run.py",
            "src/Mod/OpenSCAD/replaceobj.py",
            "src/Mod/Fem/femresult/resulttools.py",
            "src/Mod/OpenSCAD/OpenSCAD2Dgeom.py",
            "src/Mod/Draft/draftmake/make_array.py",
            "src/Mod/Draft/draftmake/make_shapestring.py",
            "src/Mod/CAM/Path/Dressup/Utils.py",
            "src/Mod/Part/CompoundTools/CompoundFilter.py",
            "src/Mod/CAM/Path/Tool/library/ui/cmd.py",
            "src/Mod/CAM/Machine/ui/mtconnect_import_dialog.py",
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

    def test_python_provider_update_data_sites_are_inventoried(self) -> None:
        for path, line in (
            ("src/Mod/Draft/draftviewproviders/view_base.py", 191),
            ("src/Mod/Fem/femviewprovider/view_mesh_shape.py", 57),
            ("src/Mod/BIM/nativeifc/ifc_viewproviders.py", 66),
            ("src/Mod/BIM/ArchBuildingPart.py", 917),
            ("src/Mod/BIM/ArchStructure.py", 1505),
        ):
            self._assert_site(path, line, "update-data-provider")

    def test_transitive_gui_representative_sites_are_inventoried(self) -> None:
        for path, category in (
            ("src/Mod/CAM/Path/Tool/library/ui/dock.py", "direct-recompute"),
            ("src/Mod/BIM/ArchBuildingPart.py", "direct-recompute"),
            ("src/Mod/BIM/ArchStructure.py", "direct-recompute"),
            ("src/Mod/BIM/ArchWindowPresets.py", "direct-recompute"),
        ):
            self.assertTrue(
                any(
                    finding.path == path and finding.category == category
                    for finding in self.scanned
                ),
                f"expected representative transitive finding in {path}",
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
        excluded = {
            path
            for path in rules.REVIEWED_TRANSITIVE_IMPORT_EXCLUSIONS
            if path.startswith("src/Mod/Draft/")
        }
        for path in excluded:
            self.assertFalse(any(finding.path == path for finding in self.scanned), path)

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
