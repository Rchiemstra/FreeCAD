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
    r"\b(?:waitForFinished|waitForDone|waitForStarted|waitForBytesWritten|waitForConnected)"
    r"[^\S\n]*\(|"
    r"QThread[^\S\n]*::[^\S\n]*wait[^\S\n]*\(|"
    r"pthread_join[^\S\n]*\(|"
    r"(?:->|\.)wait[^\S\n]*\("
)
RECOMPUTE_HANDLE_CONTEXT = re.compile(
    r"(?:RecomputeHandle|recomputeAsync|"
    r"shared_ptr\s*<\s*App::RecomputeHandle|unique_ptr\s*<\s*App::RecomputeHandle)"
)
RECOMPUTE_HANDLE_WAIT_CALL = re.compile(r"(?:->|\.)\s*wait\s*\(")
RECOMPUTE_HANDLE_ASSIGN = re.compile(
    r"([A-Za-z_]\w*)\s*=\s*[^;\n]*(?:recomputeAsync|RecomputeHandle)"
)
RECOMPUTE_HANDLE_TIGHT_LOOKBACK_LINES = 8
RECOMPUTE_HANDLE_BOUND_LOOKBACK_LINES = 40
LIVE_REFERENCE_PAYLOAD = re.compile(
    r"fastsignals::signal\s*<(?:[^<>]|<[^<>]*>)*\bApp::(?:Property\s*[*&]|DocumentObject\s*[*&])(?:[^<>]|<[^<>]*>)*>"
)
GUI_DOCUMENT_MODEL_INGRESS = re.compile(
    r"getDocument\s*\(\s*\)(?:[^\S\n]*\n){0,2}[^\S\n]*(?:->|\.)[^\S\n]*getObject\s*\(|"
    r"(?P<gd_var>[A-Za-z_]\w*)\s*=\s*getDocument\s*\(\s*\)\s*;"
    r"(?:[^\n]*\n){0,5}[^\n]*\b(?P=gd_var)[^\S\n]*(?:->|\.)[^\S\n]*getObject\s*\("
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
    "src/Gui/Document.cpp",
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
            index = end
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


def _line_index_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset)


def _wait_receiver_name(masked: str, wait_start: int) -> str | None:
    """Return the identifier immediately before ``->wait`` / ``.wait`` when present."""
    prefix = masked[max(0, wait_start - 120) : wait_start]
    match = re.search(r"([A-Za-z_]\w*)\s*$", re.sub(r"\s+", " ", prefix))
    return match.group(1) if match else None


def _scan_recompute_handle_waits(
    *,
    paths: list[Path] | None = None,
) -> list[tuple[str, int, str]]:
    matches: list[tuple[str, int, str]] = []
    for path in paths or _production_gui_sources():
        relative = path.relative_to(REPO_ROOT).as_posix()
        source = path.read_text(encoding="utf-8", errors="surrogateescape")
        masked = _suppress_cpp_non_code(source)
        lines = masked.splitlines()
        source_lines = source.splitlines()
        for wait_match in RECOMPUTE_HANDLE_WAIT_CALL.finditer(masked):
            line_index = _line_index_at(masked, wait_match.start())
            receiver = _wait_receiver_name(masked, wait_match.start())
            tight_start = max(0, line_index - RECOMPUTE_HANDLE_TIGHT_LOOKBACK_LINES)
            tight_context = "\n".join(lines[tight_start : line_index + 1])
            if RECOMPUTE_HANDLE_CONTEXT.search(tight_context):
                evidence = source_lines[line_index].strip()
                matches.append((relative, line_index + 1, evidence))
                continue
            if receiver is None:
                continue
            bound_start = max(0, line_index - RECOMPUTE_HANDLE_BOUND_LOOKBACK_LINES)
            bound_region = "\n".join(lines[bound_start:line_index])
            assigned = False
            for assign in RECOMPUTE_HANDLE_ASSIGN.finditer(bound_region):
                if assign.group(1) == receiver and RECOMPUTE_HANDLE_CONTEXT.search(
                    assign.group(0)
                ):
                    assigned = True
            if not assigned:
                continue
            evidence = source_lines[line_index].strip()
            matches.append((relative, line_index + 1, evidence))
    return matches


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
    violations = _scan_recompute_handle_waits()
    assert not violations, "RecomputeHandle::wait in GUI production paths:\n" + "\n".join(
        f"{path}:{line}: {evidence}" for path, line, evidence in violations
    )


def test_live_reference_payload_signals_are_inventoried() -> None:
    payload = json.loads(INVENTORY_PATH.read_text(encoding="utf-8"))
    inventoried = {
        (item["path"], int(item["line"]))
        for item in payload["findings"]
        if item["category"] == "live-reference-payload"
    }
    excluded = {
        (path, line)
        for path, line, category in _excluded_keys()
        if category == "live-reference-payload"
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


def test_gui_document_model_ingress_is_inventoried() -> None:
    paths = [
        REPO_ROOT / "src/Gui/Document.cpp",
        REPO_ROOT / "src/Gui/Command.cpp",
    ]
    paths = [path for path in paths if path.is_file()]
    _assert_inventoried(
        _scan_pattern(GUI_DOCUMENT_MODEL_INGRESS, paths=paths),
        "live-app-dereference",
        "Gui::Document getDocument/getObject ingress",
    )


def test_get_document_get_object_requires_real_chain() -> None:
    assert GUI_DOCUMENT_MODEL_INGRESS.search("getDocument()->getObject(name)")
    assert GUI_DOCUMENT_MODEL_INGRESS.search("getDocument()\n    ->getObject(name)")
    assert GUI_DOCUMENT_MODEL_INGRESS.search(
        "App::Document* pDoc = getDocument();\n"
        "    return pDoc ? pDoc->getObject(Name) : nullptr;"
    )
    assert not GUI_DOCUMENT_MODEL_INGRESS.search(
        "&& obj->getDocument() == vp->getObject()->getDocument()) {"
    )
    assert not GUI_DOCUMENT_MODEL_INGRESS.search(
        "getTree()->NewObjects[pDocument->getDocument()->getName()]"
        ".push_back(obj.getObject()->getID());"
    )
    assert not GUI_DOCUMENT_MODEL_INGRESS.search(
        "vp->getObject()->getDocument()->recomputeFeature(vp->getObject());"
    )


def test_recompute_handle_wait_detects_multiline_and_distant_bound_wait() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_probe.cpp"
    probe.write_text(
        "auto handle = doc->recomputeAsync();\n"
        "handle->\n"
        "    wait();\n"
        "auto handle2 = doc->recomputeAsync(objs, false, options);\n"
        + ("doSomething();\n" * 12)
        + "handle2->wait();\n"
        "other->wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    evidences = [evidence for _path, _line, evidence in found]
    assert any("wait" in evidence for evidence in evidences)
    assert not any("other" in evidence for evidence in evidences)
    assert len(found) >= 2


def test_scanner_ignores_comments_strings_and_raw_literals() -> None:
    inert = r'''
        // Qt::BlockingQueuedConnection
        /* QThread::wait(); */
        const char* text = "RecomputeHandle::wait()";
        const char* raw = R"tag(Qt::BlockingQueuedConnection)tag";
    '''
    masked = _suppress_cpp_non_code(inert)
    assert not BLOCKING_QUEUED_CONNECTION.search(masked)
    assert not DOCUMENT_THREAD_WAIT.search(masked)
    assert not RECOMPUTE_HANDLE_WAIT_CALL.search(masked)
    assert not RECOMPUTE_HANDLE_CONTEXT.search(masked)
