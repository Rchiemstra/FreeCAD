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
RECOMPUTE_HANDLE_TYPED = (
    r"(?:App::RecomputeHandle\b|"
    r"(?:std::)?(?:shared_ptr|unique_ptr)\s*<\s*App::RecomputeHandle\b\s*>)"
)
RECOMPUTE_HANDLE_TYPED_RE = re.compile(RECOMPUTE_HANDLE_TYPED)
RECOMPUTE_HANDLE_TYPED_DECL = re.compile(
    rf"{RECOMPUTE_HANDLE_TYPED}\s*"
    r"((?:const\s+)?(?:[&*]\s*)?\w+(?:\s*,\s*(?:const\s+)?(?:[&*]\s*)?\w+)*)"
)
RECOMPUTE_HANDLE_RECOMPUTE_BIND = re.compile(
    r"([A-Za-z_]\w*)\s*(?:=\s*|{\s*)[^;]*?\brecomputeAsync\s*\(",
    re.DOTALL,
)
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


def _line_start_offset(text: str, line_index: int) -> int:
    if line_index <= 0:
        return 0
    offset = 0
    for _ in range(line_index):
        next_newline = text.find("\n", offset)
        if next_newline < 0:
            return offset
        offset = next_newline + 1
    return offset


def _wait_receiver_name(masked: str, arrow_start: int) -> str | None:
    """Return the identifier immediately before ``->`` / ``.`` that precedes ``wait``."""
    prefix = masked[max(0, arrow_start - 120) : arrow_start]
    match = re.search(r"([A-Za-z_]\w*)\s*$", re.sub(r"\s+", " ", prefix))
    return match.group(1) if match else None


def _is_scope_brace(text: str, index: int) -> bool:
    if text[index] != "{":
        return False
    prefix = text[max(0, index - 80) : index].rstrip()
    if re.search(r"\)\s*$|\belse\s*$|\]\s*$", prefix):
        return True
    if re.search(r"\b(?:struct|class|enum|union|namespace)\s+[\w:]+\s*$", prefix):
        return True
    brace_index = index - 1
    while brace_index >= 0 and text[brace_index].isspace():
        brace_index -= 1
    if brace_index >= 0 and (text[brace_index].isalnum() or text[brace_index] == "_"):
        return False
    return True


def _scope_depth_at(text: str, offset: int) -> int:
    scope_depth = 0
    init_depth = 0
    for index in range(offset):
        char = text[index]
        if char == "{":
            if _is_scope_brace(text, index):
                scope_depth += 1
            else:
                init_depth += 1
        elif char == "}":
            if init_depth > 0:
                init_depth -= 1
            elif scope_depth > 0:
                scope_depth -= 1
    return scope_depth


def _enclosing_block_start(text: str, offset: int) -> int | None:
    if _scope_depth_at(text, offset) == 0:
        return None
    balance = 0
    init_depth = 0
    for index in range(offset - 1, -1, -1):
        char = text[index]
        if char == "}":
            if init_depth > 0:
                init_depth -= 1
            else:
                balance += 1
        elif char == "{":
            if init_depth > 0:
                init_depth += 1
            elif balance == 0 and _is_scope_brace(text, index):
                return index
            elif balance > 0:
                balance -= 1
    return None


def _function_param_paren_start(text: str, block_start: int) -> int | None:
    index = block_start - 1
    while index >= 0 and text[index].isspace():
        index -= 1
    if index < 0 or text[index] != ")":
        return None
    balance = 0
    for pos in range(index, -1, -1):
        char = text[pos]
        if char == ")":
            balance += 1
        elif char == "(":
            balance -= 1
            if balance == 0:
                return pos
    return None


def _scope_region_start(text: str, wait_offset: int) -> int:
    block_start = _enclosing_block_start(text, wait_offset)
    if block_start is not None:
        param_start = _function_param_paren_start(text, block_start)
        return param_start if param_start is not None else block_start + 1
    scope_depth = 0
    region_start = 0
    init_depth = 0
    for index, char in enumerate(text[:wait_offset]):
        if char == "{":
            if _is_scope_brace(text, index):
                scope_depth += 1
            else:
                init_depth += 1
        elif char == "}":
            if init_depth > 0:
                init_depth -= 1
            elif scope_depth > 0:
                scope_depth -= 1
                if scope_depth == 0:
                    region_start = index + 1
    return region_start


def _is_function_declarator(text: str, name_end: int) -> bool:
    index = name_end
    while index < len(text) and text[index].isspace():
        index += 1
    return index < len(text) and text[index] == "("


def _declarator_names(declarator_list: str, text: str, list_start: int) -> list[tuple[str, int]]:
    names: list[tuple[str, int]] = []
    cursor = list_start
    for part in declarator_list.split(","):
        stripped = part.strip()
        match = re.match(r"(?:const\s+)?(?:[*&]\s*)*(\w+)", stripped)
        if not match:
            cursor += len(part) + 1
            continue
        name = match.group(1)
        name_offset = text.find(name, cursor, cursor + len(part) + 1)
        if name_offset < 0:
            name_offset = cursor + part.find(name)
        name_end = name_offset + len(name)
        if not _is_function_declarator(text, name_end):
            names.append((name, name_offset))
        cursor += len(part) + 1
    return names


def _wait_line_end(masked: str, wait_offset: int) -> int:
    line_end = masked.find("\n", wait_offset)
    return len(masked) if line_end < 0 else line_end


def _shadow_names_in_line(line: str) -> list[str]:
    if RECOMPUTE_HANDLE_TYPED_RE.search(line):
        return []
    if re.search(r"\brecomputeAsync\s*\(", line):
        return []
    if re.match(
        r"\s*(?:auto|struct|class|union|enum|return|if|while|for|switch|catch|else)\b",
        line,
    ):
        return []
    names: list[str] = []
    for match in re.finditer(r"(?:^|[\s,{}])(\w+)\s*[,;=]", line):
        name = match.group(1)
        name_end = match.start(1) + len(name)
        if _is_function_declarator(line, name_end):
            continue
        names.append(name)
    return names


def _binding_events_in_scope(masked: str, scope_start: int, region_end: int) -> list[tuple[int, str, str]]:
    events: list[tuple[int, str, str]] = []
    for match in RECOMPUTE_HANDLE_TYPED_DECL.finditer(masked, scope_start, region_end):
        for name, _offset in _declarator_names(match.group(1), masked, match.start(1)):
            events.append((match.start(), "recompute", name))
    for match in RECOMPUTE_HANDLE_RECOMPUTE_BIND.finditer(masked, scope_start, region_end):
        events.append((match.start(), "recompute", match.group(1)))
    region = masked[scope_start:region_end]
    line_start = scope_start
    for line in region.splitlines(keepends=True):
        stripped = line.strip()
        if stripped:
            for name in _shadow_names_in_line(stripped):
                events.append((line_start, "shadow", name))
        line_start += len(line)
    events.sort(key=lambda item: item[0])
    return events


def _effective_binding(events: list[tuple[int, str, str]], receiver: str) -> str | None:
    state: dict[str, str] = {}
    for _offset, kind, name in events:
        state[name] = kind
    return state.get(receiver)


def _receiver_bound_to_recompute_handle(
    receiver: str,
    masked: str,
    wait_offset: int,
) -> bool:
    wait_line_index = _line_index_at(masked, wait_offset)
    bound_start = max(0, wait_line_index - RECOMPUTE_HANDLE_BOUND_LOOKBACK_LINES)
    bound_line_offset = _line_start_offset(masked, bound_start)
    scope_start = max(_scope_region_start(masked, wait_offset), bound_line_offset)
    region_end = _wait_line_end(masked, wait_offset)
    events = _binding_events_in_scope(masked, scope_start, region_end)
    return _effective_binding(events, receiver) == "recompute"


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
            wait_offset = wait_match.start() + wait_match.group().index("wait")
            wait_line_index = _line_index_at(masked, wait_offset)
            receiver = _wait_receiver_name(masked, wait_match.start())
            if receiver is None:
                continue
            if not _receiver_bound_to_recompute_handle(receiver, masked, wait_offset):
                continue
            evidence = source_lines[wait_line_index].strip()
            matches.append((relative, wait_line_index + 1, evidence))
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
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[3].startswith("wait")
    assert by_line[17].startswith("handle2")
    assert 4 not in by_line
    assert len(found) == 2


def test_recompute_handle_wait_ignores_unbound_nearby_waits() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_unbound_probe.cpp"
    probe.write_text(
        "auto handle = doc->recomputeAsync();\n"
        "handle->wait();\n"
        "other->wait();\n"
        "future.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line == {2: "handle->wait();"}


def test_recompute_handle_wait_detects_same_line_assignment_and_init_forms() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_forms_probe.cpp"
    probe.write_text(
        "auto handle = doc->recomputeAsync(); handle->wait();\n"
        "auto handle2{doc->recomputeAsync()};\n"
        "handle2->wait();\n"
        "auto handle3 = doc\n"
        "    ->recomputeAsync();\n"
        "handle3->wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[1].startswith("auto handle = doc->recomputeAsync(); handle->wait();")
    assert by_line[3] == "handle2->wait();"
    assert by_line[6].startswith("handle3->wait();")
    assert len(found) == 3


def test_recompute_handle_wait_rejects_type_name_suffix_and_function_declarator() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_false_pos_probe.cpp"
    probe.write_text(
        "App::RecomputeHandleFactory Factory;\n"
        "Factory->wait();\n"
        "App::RecomputeHandlePtr Ptr;\n"
        "Ptr->wait();\n"
        "App::RecomputeHandle createHandle();\n"
        "createHandle->wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    assert found == []


def test_recompute_handle_wait_rejects_cross_function_and_shadowed_names() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_scope_probe.cpp"
    probe.write_text(
        "void foo(std::shared_ptr<App::RecomputeHandle> handle) {}\n"
        "void bar() {\n"
        "  QFuture<void> handle;\n"
        "  handle.wait();\n"
        "}\n"
        "std::shared_ptr<App::RecomputeHandle> handle;\n"
        "std::future<int> handle;\n"
        "handle.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    assert found == []


def test_recompute_handle_wait_detects_multi_declarator_and_rejects_substring_async() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_multi_probe.cpp"
    probe.write_text(
        "std::shared_ptr<App::RecomputeHandle> handle, other;\n"
        "other->wait();\n"
        "auto status = check_recomputeAsync();\n"
        "status.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line == {2: "other->wait();"}


def test_recompute_handle_wait_detects_typed_declarations() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_decl_probe.cpp"
    probe.write_text(
        "std::shared_ptr<App::RecomputeHandle> handle;\n"
        "handle->wait();\n"
        "void foo(std::shared_ptr<App::RecomputeHandle> handle) {\n"
        "    handle->wait();\n"
        "}\n"
        "App::RecomputeHandle handle2 = other;\n"
        "handle2.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[2] == "handle->wait();"
    assert "handle->wait();" in by_line[4]
    assert by_line[7] == "handle2.wait();"
    assert len(found) == 3


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
