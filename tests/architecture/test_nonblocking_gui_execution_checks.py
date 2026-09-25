# SPDX-License-Identifier: LGPL-2.1-or-later
"""Focused Wave 0 static checks for nonblocking GUI execution contracts."""

from __future__ import annotations

import io
import json
from pathlib import Path
import re
import tokenize


REPO_ROOT = Path(__file__).resolve().parents[2]
ARCH_DIR = Path(__file__).resolve().parent
INVENTORY_PATH = ARCH_DIR / "gui_blocking_live_model" / "inventory.json"
EXCLUSIONS_PATH = ARCH_DIR / "gui_blocking_live_model" / "exclusions.json"

EXCLUDED_WORKBENCHES = frozenset({"Test", "TemplatePyMod"})
EXCLUDED_FILES = frozenset(
    {
        "src/Gui/CommandTest.cpp",
        "src/Gui/FreeCADGuiTest.py",
    }
)

BLOCKING_QUEUED_CONNECTION = re.compile(r"Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection")
DOCUMENT_THREAD_WAIT = re.compile(
    r"(?:QThread[^\S\n]*::[^\S\n]*wait[^\S\n]*\(|"
    r"pthread_join[^\S\n]*\(|"
    r"(?:->|\.)wait[^\S\n]*\()"
)
RECOMPUTE_HANDLE_WAIT = re.compile(
    r"(?:RecomputeHandle|recomputeAsync)[^\n]{0,160}\bwait[^\S\n]*\(|"
    r"\bwait[^\S\n]*\([^\n]{0,160}RecomputeHandle"
)
LIVE_REFERENCE_PAYLOAD = re.compile(
    r"fastsignals::signal<[^>]*(?:App::)?(?:Property\s*\*|DocumentObject\s*\*|"
    r"Property\s*&|DocumentObject\s*&)[^>]*>"
)
LIVE_APP_DEREFERENCE = re.compile(
    r"App::GetApplication\s*\(\s*\)\s*\.\s*"
    r"(?:getActiveDocument|getDocuments|getDocumentOrActive|getDocumentByPath|getDocument)"
    r"[^\S\n]*\("
)
MODEL_INGRESS_PATHS = (
    "src/Gui/Command.cpp",
    "src/Gui/CommandDoc.cpp",
    "src/Gui/Tree.cpp",
    "src/Gui/MainWindow.cpp",
    "src/Gui/propertyeditor/PropertyItem.cpp",
    "src/Gui/Application.cpp",
)


def _raw_literal_end(source: str, start: int) -> int | None:
    prefixes = ("u8R\"", "uR\"", "UR\"", "LR\"", "R\"")
    prefix = next((item for item in prefixes if source.startswith(item, start)), None)
    if prefix is None or (start and (source[start - 1].isalnum() or source[start - 1] == "_")):
        return None
    delimiter_start = start + len(prefix)
    opening = source.find("(", delimiter_start, delimiter_start + 17)
    if opening < 0:
        return None
    delimiter = source[delimiter_start:opening]
    if any(char.isspace() or char in "()\\" for char in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    closing = source.find(terminator, opening + 1)
    return len(source) if closing < 0 else closing + len(terminator)


def _blank_non_newlines(characters: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if characters[index] not in "\r\n":
            characters[index] = " "


def _suppress_cpp_non_code(source: str) -> str:
    result = list(source)
    index = 0
    while index < len(source):
        raw_end = _raw_literal_end(source, index)
        if raw_end is not None:
            _blank_non_newlines(result, index, raw_end)
            index = raw_end
            continue
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            _blank_non_newlines(result, index, end)
            index = end
            continue
        if source.startswith("/*", index):
            closing = source.find("*/", index + 2)
            end = len(source) if closing < 0 else closing + 2
            _blank_non_newlines(result, index, end)
            index = end
            continue
        if source[index] in {'"', "'"}:
            quote = source[index]
            end = index + 1
            escaped = False
            while end < len(source):
                char = source[end]
                end += 1
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    break
            _blank_non_newlines(result, index, end)
            index += 1
            continue
        index += 1
    return "".join(result)


def _python_token_suppressed(source: str) -> str:
    result = list(source)
    offsets: list[int] = []
    offset = 0
    for line in source.splitlines(keepends=True):
        offsets.append(offset)
        offset += len(line)
    if not offsets:
        offsets.append(0)
    for token in tokenize.generate_tokens(io.StringIO(source).readline):
        if token.type != tokenize.COMMENT and token.type != tokenize.STRING:
            continue
        start = offsets[token.start[0] - 1] + token.start[1]
        end = offsets[token.end[0] - 1] + token.end[1]
        _blank_non_newlines(result, start, end)
    return "".join(result)


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
            and path.relative_to(REPO_ROOT).as_posix() not in EXCLUDED_FILES
        }
    )


def _scan_pattern(
    pattern: re.Pattern[str],
    *,
    paths: list[Path] | None = None,
) -> list[tuple[str, int, str]]:
    matches: list[tuple[str, int, str]] = []
    for path in paths or _production_gui_sources():
        relative = path.relative_to(REPO_ROOT).as_posix()
        source = path.read_text(encoding="utf-8", errors="surrogateescape")
        masked = (
            _python_token_suppressed(source)
            if path.suffix.lower() == ".py"
            else _suppress_cpp_non_code(source)
        )
        for match in pattern.finditer(masked):
            line = masked.count("\n", 0, match.start()) + 1
            evidence = source.splitlines()[line - 1].strip()
            matches.append((relative, line, evidence))
    return matches


def _inventory_keys(category: str) -> set[tuple[str, int]]:
    payload = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    return {
        (item["path"], int(item["line"]))
        for item in payload["findings"]
        if item["category"] == category
    }


def _excluded_keys() -> set[tuple[str, int, str]]:
    payload = json.loads(EXCLUSIONS_PATH.read_text(encoding="utf-8"))
    return {
        (entry["path"], int(entry["line"]), entry["category"])
        for entry in payload["exclusions"]
    }


def _assert_inventoried(
    discovered: list[tuple[str, int, str]],
    category: str,
    label: str,
) -> None:
    inventoried = _inventory_keys(category)
    excluded = {(path, line) for path, line, cat in _excluded_keys() if cat == category}
    missing = sorted(
        (path, line, evidence)
        for path, line, evidence in discovered
        if (path, line) not in inventoried and (path, line) not in excluded
    )
    assert not missing, (
        f"uninventoried {label}:\n"
        + "\n".join(f"{path}:{line}: {evidence}" for path, line, evidence in missing)
    )


def test_blocking_queued_connections_are_inventoried() -> None:
    _assert_inventoried(
        _scan_pattern(BLOCKING_QUEUED_CONNECTION),
        "blocking-invokes",
        "blocking queued connections",
    )


def test_document_thread_waits_are_inventoried() -> None:
    _assert_inventoried(
        _scan_pattern(DOCUMENT_THREAD_WAIT),
        "thread-waits",
        "document-thread waits",
    )


def test_recompute_handle_wait_is_absent_from_gui_production_paths() -> None:
    violations = _scan_pattern(RECOMPUTE_HANDLE_WAIT)
    assert not violations, "RecomputeHandle::wait in GUI production paths:\n" + "\n".join(
        f"{path}:{line}: {evidence}" for path, line, evidence in violations
    )


def test_live_reference_payload_signals_are_inventoried() -> None:
    payload = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    inventoried = {
        (item["path"], int(item["line"]))
        for item in payload["findings"]
        if item["category"] == "live-reference-callback"
    }
    excluded = {
        (path, line)
        for path, line, category in _excluded_keys()
        if category == "live-reference-callback"
    }
    missing = sorted(
        (path, line, evidence)
        for path, line, evidence in _scan_pattern(LIVE_REFERENCE_PAYLOAD)
        if (path, line) not in inventoried and (path, line) not in excluded
    )
    assert not missing, "uninventoried live-reference payload signals:\n" + "\n".join(
        f"{path}:{line}: {evidence}" for path, line, evidence in missing
    )


def test_gui_model_ingress_paths_do_not_add_undocumented_live_app_dereference() -> None:
    paths = [REPO_ROOT / path for path in MODEL_INGRESS_PATHS if (REPO_ROOT / path).is_file()]
    _assert_inventoried(
        _scan_pattern(LIVE_APP_DEREFERENCE, paths=paths),
        "live-app-dereference",
        "live App dereference in model ingress",
    )


def test_scanner_ignores_comments_strings_and_raw_literals() -> None:
    inert = r'''
        // Qt::BlockingQueuedConnection
        /* QThread::wait(); */
        const char* text = "RecomputeHandle::wait()";
        const char* raw = R"tag(Qt::BlockingQueuedConnection)tag";
    '''
    assert not BLOCKING_QUEUED_CONNECTION.search(_suppress_cpp_non_code(inert))
    assert not DOCUMENT_THREAD_WAIT.search(_suppress_cpp_non_code(inert))
    assert not RECOMPUTE_HANDLE_WAIT.search(_suppress_cpp_non_code(inert))
