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
    r"([A-Za-z_]\w*)\s*(?:=\s*[^;{}]*|{\s*[^;{}]*)\brecomputeAsync\s*\(",
)
RECOMPUTE_HANDLE_BOUND_LOOKBACK_LINES = 40
SHADOW_NAME_EXCLUDE = frozenset(
    {
        "if",
        "while",
        "for",
        "switch",
        "catch",
        "else",
        "try",
        "do",
        "return",
        "case",
        "default",
    }
)
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


def _auto_shadow_name_in_line(line: str) -> str | None:
    if re.search(r"\brecomputeAsync\s*\(", line):
        return None
    match = re.match(r"\s*auto\s+(\w+)\s*=", line)
    if not match:
        return None
    if re.search(r"=\s*[\w:]*\s*\{", line) or re.search(r"=\s*[\w:]*\s*\(\s*$", line.rstrip()):
        return None
    return match.group(1)


def _brace_inner_has_declaration(text: str, open_index: int) -> bool:
    close_index = _matching_close_brace(text, open_index)
    inner = text[open_index + 1 : close_index]
    offset = open_index + 1
    for line in inner.splitlines():
        if _shadow_names_in_line(line, text, offset):
            return True
        if _auto_shadow_name_in_line(line) is not None:
            return True
        offset += len(line) + 1
    return False


def _is_scope_brace(text: str, index: int) -> bool:
    if text[index] != "{":
        return False
    prefix = text[max(0, index - 80) : index].rstrip()
    if re.search(r"\)\s*$|\b(?:else|try|do)\s*$|\]\s*$", prefix):
        return True
    if re.search(r"\b(?:struct|class|enum|union|namespace)\s+[\w:]+\s*$", prefix):
        return True
    brace_index = index - 1
    while brace_index >= 0 and text[brace_index].isspace():
        brace_index -= 1
    if brace_index >= 0 and text[brace_index] == "(":
        return _brace_inner_has_declaration(text, index)
    if brace_index >= 0 and text[brace_index] in "_":
        return False
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


def _matching_close_paren(text: str, open_index: int) -> int:
    balance = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "(":
            balance += 1
        elif char == ")":
            balance -= 1
            if balance == 0:
                return index
    return len(text) - 1


def _matching_close_brace(text: str, open_index: int) -> int:
    balance = 0
    for index in range(open_index, len(text)):
        char = text[index]
        if char == "{":
            balance += 1
        elif char == "}":
            balance -= 1
            if balance == 0:
                return index
    return len(text) - 1


def _split_top_level_commas(inner: str) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    depth = 0
    for char in inner:
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
            continue
        current.append(char)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def _looks_like_type_declarator_part(stripped: str) -> bool:
    if re.search(r"\(\s*\*", stripped):
        return True
    if re.search(r"[*&]{1,2}(?:[A-Za-z_]\w*)?\s*$", stripped):
        return True
    if re.match(
        r"^(?:const\s+|volatile\s+)?(?:unsigned\s+|signed\s+|short\s+|long\s+)?"
        r"(?:[\w:]+\s*::\s*)*[\w:]+\s*(?:<[^>]*>)?\s+"
        r"(?:const\s+)?(?:[*&]{1,2})?[A-Za-z_]\w*\s*$",
        stripped,
    ):
        return True
    if re.match(r"^[\w:]+\s*<[^>]*>\s*$", stripped):
        return True
    if re.match(
        r"^(?:unsigned|signed|short|long|int|void|char|bool|float|double)\b(?:\s+\w+)?\s*$",
        stripped,
    ):
        return True
    if re.match(r"^std::string(?:\s+\w+)?\s*$", stripped):
        return True
    return False


def _looks_like_expression_part(part: str) -> bool:
    stripped = part.strip()
    if not stripped:
        return False
    if re.fullmatch(r"[A-Za-z_]\w*", stripped):
        return True
    if re.search(r"->|\.|\brecomputeAsync\s*\(", stripped):
        return True
    if _looks_like_type_declarator_part(stripped):
        return False
    angle_depth = 0
    index = 0
    while index < len(stripped):
        char = stripped[index]
        if char == "<":
            angle_depth += 1
        elif char == ">":
            angle_depth -= 1
        elif char == "(" and angle_depth == 0:
            before = stripped[:index].rstrip()
            if before.endswith("<") or re.search(r"\(\s*\*$", before):
                index += 1
                continue
            return True
        index += 1
    if re.match(r"^(?:[\w:]+\s*::\s*)+[A-Za-z_]\w*\s*$", stripped):
        return True
    return False


def _is_type_only_param_list(inner: str) -> bool:
    parts = _split_top_level_commas(inner)
    if not parts:
        return True
    return not any(_looks_like_expression_part(part) for part in parts)


def _after_name_kind(text: str, name_end: int) -> str | None:
    index = name_end
    while index < len(text) and text[index].isspace():
        index += 1
    if index >= len(text):
        return None
    if text[index] == "{" and not _is_scope_brace(text, index):
        return "init"
    if text[index] != "(":
        return None
    close_index = _matching_close_paren(text, index)
    inner = text[index + 1 : close_index].strip()
    if not inner:
        return "function"
    if _is_type_only_param_list(inner):
        return "function"
    return "init"


def _is_function_body_brace(text: str, index: int) -> bool:
    if not _is_scope_brace(text, index):
        return False
    prefix = text[max(0, index - 200) : index].rstrip()
    if not prefix.endswith(")"):
        return False
    if re.search(r"\b(?:if|while|for|switch|catch)\s*\([^)]*\)\s*$", prefix):
        return False
    if re.search(r"\]\s*\(\s*\)\s*$", prefix):
        return False
    if re.search(r"\b[A-Z][A-Z0-9_]*\s*\([^)]*\)\s*$", prefix):
        return False
    return bool(re.search(r"[\w:]+\s*\([^)]*\)\s*$", prefix))


def _closes_scope_brace_at(text: str, index: int) -> bool:
    return _scope_depth_at(text, index) > _scope_depth_at(text, index + 1)


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
        if _after_name_kind(text, name_end) != "function":
            names.append((name, name_offset))
        cursor += len(part) + 1
    return names


def _wait_line_end(masked: str, wait_offset: int) -> int:
    line_end = masked.find("\n", wait_offset)
    return len(masked) if line_end < 0 else line_end


RECOMPUTE_HANDLE_DIRECT_INIT = re.compile(
    rf"{RECOMPUTE_HANDLE_TYPED}\s*(?:const\s+)?(?:[&*]\s*)?(\w+)\s*\("
)


def _function_param_bindings(text: str, brace_index: int) -> dict[str, str]:
    paren_start = _function_param_paren_start(text, brace_index)
    if paren_start is None:
        return {}
    close_index = brace_index - 1
    while close_index > paren_start and text[close_index].isspace():
        close_index -= 1
    if close_index <= paren_start or text[close_index] != ")":
        return {}
    bindings: dict[str, str] = {}
    param_region = text[paren_start : close_index + 1]
    for match in RECOMPUTE_HANDLE_TYPED_DECL.finditer(param_region):
        list_start = paren_start + match.start(1)
        for name, _offset in _declarator_names(match.group(1), text, list_start):
            bindings[name] = "recompute"
    return bindings


def _async_bind_name_from_local(local: str) -> tuple[int, str] | None:
    assign_match: re.Match[str] | None = None
    for match in re.finditer(r"([A-Za-z_]\w*)\s*=\s*", local):
        assign_match = match
    if assign_match is not None:
        return assign_match.start(1), assign_match.group(1)
    brace_match = re.search(r"([A-Za-z_]\w*)\s*{", local)
    if brace_match is not None:
        return brace_match.start(1), brace_match.group(1)
    return None


def _init_call_name_before(text: str, open_paren_index: int, region_start: int) -> tuple[int, str] | None:
    before = text[region_start:open_paren_index].rstrip()
    bind = _async_bind_name_from_local(before)
    if bind is not None:
        return region_start + bind[0], bind[1]
    return None


def _async_bind_from_enclosing_init(
    text: str,
    async_start: int,
    region_start: int,
) -> tuple[int, str] | None:
    index = async_start
    brace_depth = 0
    paren_depth = 0
    while index > region_start:
        index -= 1
        char = text[index]
        if char == ")":
            paren_depth += 1
        elif char == "(":
            if paren_depth > 0:
                paren_depth -= 1
            elif brace_depth == 0:
                bind = _init_call_name_before(text, index, region_start)
                if bind is not None:
                    return bind
        elif char == "}":
            brace_depth += 1
        elif char == "{":
            if brace_depth > 0:
                brace_depth -= 1
    return None


def _async_bind_name_at(text: str, async_start: int, region_start: int) -> tuple[int, str] | None:
    statement_start = text.rfind(";", region_start, async_start) + 1
    local = text[statement_start:async_start]
    bind = _async_bind_name_from_local(local)
    if bind is not None:
        return statement_start + bind[0], bind[1]
    if re.search(r"\breturn\b", local):
        return _async_bind_from_enclosing_init(text, async_start, region_start)
    return None


def _async_recompute_binds(text: str, start: int, end: int) -> list[tuple[int, str]]:
    binds: list[tuple[int, str]] = []
    search = start
    while search < end:
        match = re.search(r"\brecomputeAsync\s*\(", text[search:end])
        if not match:
            break
        async_start = search + match.start()
        bind = _async_bind_name_at(text, async_start, start)
        if bind is not None:
            binds.append(bind)
        search = async_start + match.end()
    return binds


def _recompute_bind_names_in_text(text: str, start: int, end: int) -> list[tuple[int, str]]:
    binds: list[tuple[int, str]] = []
    search = start
    while search < end:
        match = re.search(r"\brecomputeAsync\s*\(", text[search:end])
        if not match:
            break
        async_start = search + match.start()
        bind = _async_bind_name_at(text, async_start, start)
        if bind is not None:
            binds.append(bind)
        search = async_start + match.end()
    return binds


def _recompute_names_on_line(
    line: str,
    text: str,
    line_start: int,
    async_binds: list[tuple[int, str]] | None = None,
) -> list[str]:
    names: list[str] = []
    for match in RECOMPUTE_HANDLE_TYPED_DECL.finditer(line):
        list_start = line_start + match.start(1)
        for name, name_offset in _declarator_names(match.group(1), text, list_start):
            names.append(name)
    for match in RECOMPUTE_HANDLE_DIRECT_INIT.finditer(line):
        name = match.group(1)
        name_end = line_start + match.start(1) + len(name)
        if _after_name_kind(text, name_end) == "init":
            names.append(name)
    line_binds = _recompute_bind_names_in_text(text, line_start, line_start + len(line))
    for bind_offset, name in line_binds:
        if name not in names:
            names.append(name)
    if async_binds is not None:
        line_end = line_start + len(line)
        for bind_start, name in async_binds:
            if line_start <= bind_start < line_end and name not in names:
                names.append(name)
    return names


def _shadow_names_in_line(
    line: str,
    text: str,
    line_start: int,
    async_binds: list[tuple[int, str]] | None = None,
) -> list[str]:
    if re.search(r"\brecomputeAsync\s*\(", line):
        return []
    if re.match(r"\s*(?:struct|class|union|enum|return)\b", line):
        return []
    recompute_names = set(_recompute_names_on_line(line, text, line_start, async_binds))
    names: list[str] = []
    for match in re.finditer(r"(?:^|[\s,{}])(\w+)(?=\s*[,;={]|\s*\()", line):
        name = match.group(1)
        if name in SHADOW_NAME_EXCLUDE or name in recompute_names:
            continue
        name_end = line_start + match.end(1)
        if _after_name_kind(text, name_end) == "function":
            continue
        names.append(name)
    return names


def _apply_line_bindings(
    stack: list[dict[str, str]],
    line: str,
    text: str,
    line_start: int,
    async_binds: list[tuple[int, str]],
) -> None:
    frame = stack[-1]
    for name in _recompute_names_on_line(line, text, line_start, async_binds):
        frame[name] = "recompute"
    for name in _shadow_names_in_line(line, text, line_start, async_binds):
        frame[name] = "shadow"
    auto_shadow = _auto_shadow_name_in_line(line)
    if auto_shadow is not None:
        frame[auto_shadow] = "shadow"


def _process_line_scope_and_bindings(
    stack: list[dict[str, str]],
    line: str,
    line_start: int,
    text: str,
    async_binds: list[tuple[int, str]],
) -> None:
    index = 0
    statement_start = 0
    while index < len(line):
        char = line[index]
        absolute = line_start + index
        if char == "{" and _is_scope_brace(text, absolute):
            _apply_line_bindings(
                stack,
                line[statement_start:index],
                text,
                line_start + statement_start,
                async_binds,
            )
            if _is_function_body_brace(text, absolute):
                stack.append(_function_param_bindings(text, absolute))
            else:
                stack.append(stack[-1].copy())
            statement_start = index + 1
            index += 1
            continue
        if char == "}" and _closes_scope_brace_at(text, absolute):
            _apply_line_bindings(
                stack,
                line[statement_start:index],
                text,
                line_start + statement_start,
                async_binds,
            )
            if len(stack) > 1:
                stack.pop()
            statement_start = index + 1
            index += 1
            continue
        index += 1
    _apply_line_bindings(
        stack,
        line[statement_start:],
        text,
        line_start + statement_start,
        async_binds,
    )


def _receiver_bound_to_recompute_handle(
    receiver: str,
    masked: str,
    wait_offset: int,
) -> bool:
    wait_line_index = _line_index_at(masked, wait_offset)
    bound_start = max(0, wait_line_index - RECOMPUTE_HANDLE_BOUND_LOOKBACK_LINES)
    scan_start = _line_start_offset(masked, bound_start)
    async_binds = _async_recompute_binds(masked, scan_start, wait_offset)
    stack: list[dict[str, str]] = [{}]
    position = scan_start
    while position < wait_offset:
        line_end = masked.find("\n", position)
        if line_end < 0:
            line_end = len(masked)
        segment_end = min(line_end, wait_offset)
        line = masked[position:segment_end]
        _process_line_scope_and_bindings(stack, line, position, masked, async_binds)
        if segment_end >= wait_offset:
            break
        position = line_end + 1
    return stack[-1].get(receiver) == "recompute"


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


def test_recompute_handle_wait_rejects_direct_init_shadows() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_direct_shadow_probe.cpp"
    probe.write_text(
        "std::shared_ptr<App::RecomputeHandle> handle;\n"
        "std::future<int> handle(std::launch::async, fn);\n"
        "handle.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle2;\n"
        "QFuture<void> handle2(std::launch::async, fn);\n"
        "handle2.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle3;\n"
        "std::future<int> handle3{};\n"
        "handle3.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle4;\n"
        "QFuture<void> handle4{};\n"
        "handle4.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    assert found == []


def test_recompute_handle_wait_detects_direct_init_and_nested_scopes() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_nested_probe.cpp"
    probe.write_text(
        "App::RecomputeHandle handle(other);\n"
        "handle.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle2(doc->recomputeAsync());\n"
        "handle2->wait();\n"
        "void foo() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  if (ready) {\n"
        "    handle->wait();\n"
        "  }\n"
        "  while (ready) {\n"
        "    handle->wait();\n"
        "  }\n"
        "  for (;;) {\n"
        "    handle->wait();\n"
        "  }\n"
        "  if (ready) {\n"
        "  } else {\n"
        "    handle->wait();\n"
        "  }\n"
        "  try {\n"
        "    handle->wait();\n"
        "  } catch (...) {\n"
        "  }\n"
        "  do {\n"
        "    handle->wait();\n"
        "  } while (ready);\n"
        "  auto fn = [&]() {\n"
        "    handle->wait();\n"
        "  };\n"
        "  {\n"
        "    handle->wait();\n"
        "  }\n"
        "  if (ready) {\n"
        "    QFuture<void> handle;\n"
        "    handle.wait();\n"
        "  }\n"
        "  handle->wait();\n"
        "}\n"
        "void bar() {\n"
        "  {\n"
        "    std::shared_ptr<App::RecomputeHandle> handle;\n"
        "  }\n"
        "  handle->wait();\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[2] == "handle.wait();"
    assert by_line[4] == "handle2->wait();"
    assert by_line[8].startswith("handle->wait();")
    assert by_line[11].startswith("handle->wait();")
    assert by_line[14].startswith("handle->wait();")
    assert by_line[18].startswith("handle->wait();")
    assert by_line[21].startswith("handle->wait();")
    assert by_line[25].startswith("handle->wait();")
    assert by_line[28].startswith("handle->wait();")
    assert by_line[31].startswith("handle->wait();")
    assert 34 not in by_line
    assert by_line[37].startswith("handle->wait();")
    assert 41 not in by_line
    assert len(found) == 11


def test_recompute_handle_wait_binds_correct_async_name_in_nested_contexts() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_async_bind_probe.cpp"
    probe.write_text(
        "void foo() {\n"
        "  auto cb = [&]() {\n"
        "    auto handle = doc->recomputeAsync();\n"
        "    handle->wait();\n"
        "  };\n"
        "}\n"
        "Foo::Foo() : x{1} {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  handle->wait();\n"
        "}\n"
        "class GuiOp {\n"
        "  void run() {\n"
        "    auto handle = doc->recomputeAsync();\n"
        "    handle->wait();\n"
        "  }\n"
        "};\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[4].startswith("handle->wait();")
    assert by_line[9].startswith("handle->wait();")
    assert by_line[14].startswith("handle->wait();")
    assert len(found) == 3


def test_recompute_handle_wait_keeps_outer_handle_in_macro_blocks() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_macro_probe.cpp"
    probe.write_text(
        "void foo() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  Q_FOREACH(obj, objs) {\n"
        "    handle->wait();\n"
        "  }\n"
        "  BOOST_FOREACH(item, items) {\n"
        "    handle->wait();\n"
        "  }\n"
        "  Q_FOREVER {\n"
        "    handle->wait();\n"
        "  }\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[4].startswith("handle->wait();")
    assert by_line[7].startswith("handle->wait();")
    assert by_line[10].startswith("handle->wait();")
    assert len(found) == 3


def test_recompute_handle_wait_same_line_shadow_before_closing_brace() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_same_line_shadow_probe.cpp"
    probe.write_text(
        "void foo() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  if (ready) { QFuture<void> handle; handle.wait(); }\n"
        "  handle->wait();\n"
        "}\n"
        "void bar() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  if (ready) {\n"
        "    QFuture<void> handle;\n"
        "    handle.wait();\n"
        "  }\n"
        "  handle->wait();\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert 3 not in by_line
    assert by_line[4].startswith("handle->wait();")
    assert 7 not in by_line
    assert by_line[12].startswith("handle->wait();")
    assert len(found) == 2


def test_recompute_handle_wait_rejects_nested_type_function_declarators() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_nested_type_probe.cpp"
    probe.write_text(
        "App::RecomputeHandle createHandle(std::function<void()>);\n"
        "createHandle->wait();\n"
        "App::RecomputeHandle createHandle2(int (*fn)(int));\n"
        "createHandle2->wait();\n"
        "App::RecomputeHandle createHandle3();\n"
        "createHandle3->wait();\n"
        "App::RecomputeHandle createHandle4(int x);\n"
        "createHandle4->wait();\n"
        "App::RecomputeHandle handle(other);\n"
        "handle.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle2(doc->recomputeAsync());\n"
        "handle2->wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle3;\n"
        "std::future<int> handle3(std::launch::async, fn);\n"
        "handle3.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line == {10: "handle.wait();", 12: "handle2->wait();"}


def test_recompute_handle_wait_detects_move_and_factory_direct_inits() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_move_init_probe.cpp"
    probe.write_text(
        "App::RecomputeHandle handle(std::move(other));\n"
        "handle.wait();\n"
        "std::shared_ptr<App::RecomputeHandle> handle2(std::move(other));\n"
        "handle2->wait();\n"
        "App::RecomputeHandle handle3(App::makeHandle());\n"
        "handle3.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[2] == "handle.wait();"
    assert by_line[4] == "handle2->wait();"
    assert by_line[6] == "handle3.wait();"
    assert len(found) == 3


def test_recompute_handle_wait_rejects_qualified_parameter_types() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_param_type_probe.cpp"
    probe.write_text(
        "App::RecomputeHandle createHandle(const App::Document& doc);\n"
        "createHandle->wait();\n"
        "App::RecomputeHandle createHandle2(App::Document* doc);\n"
        "createHandle2->wait();\n"
        "App::RecomputeHandle createHandle3(const App::Document&);\n"
        "createHandle3->wait();\n"
        "App::RecomputeHandle createHandle4(std::string);\n"
        "createHandle4->wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    assert found == []


def test_recompute_handle_wait_wrap_block_shadow_keeps_outer_handle() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_wrap_shadow_probe.cpp"
    probe.write_text(
        "void f() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  auto wrapped = wrap({\n"
        "    QFuture<void> handle;\n"
        "    handle.wait();\n"
        "  });\n"
        "  handle->wait();\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert 5 not in by_line
    assert by_line[7].startswith("handle->wait();")
    assert len(found) == 1


def test_recompute_handle_wait_wrap_block_auto_shadow_keeps_outer_handle() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_wrap_auto_shadow_probe.cpp"
    probe.write_text(
        "void f() {\n"
        "  auto handle = doc->recomputeAsync();\n"
        "  auto wrapped = wrap({\n"
        "    auto handle = otherFuture();\n"
        "    handle.wait();\n"
        "  });\n"
        "  handle->wait();\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert 5 not in by_line
    assert by_line[7].startswith("handle->wait();")
    assert len(found) == 1


def test_recompute_handle_wait_binds_return_async_through_enclosing_init() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_return_async_probe.cpp"
    probe.write_text(
        "auto handle = wrap({\n"
        "  int x;\n"
        "  return doc->recomputeAsync();\n"
        "});\n"
        "handle->wait();\n"
        "auto handle2 = runAsync({\n"
        "  std::lock_guard<std::mutex> guard(mutex);\n"
        "  return doc->recomputeAsync();\n"
        "});\n"
        "handle2->wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[5].startswith("handle->wait();")
    assert by_line[10].startswith("handle2->wait();")
    assert len(found) == 2


def test_recompute_handle_wait_detects_qualified_value_direct_init() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_qualified_value_probe.cpp"
    probe.write_text(
        "App::RecomputeHandle handle(App::DefaultHandle);\n"
        "handle.wait();\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[2] == "handle.wait();"
    assert len(found) == 1


def test_recompute_handle_wait_binds_nested_brace_async_initializers() -> None:
    probe = ARCH_DIR / "_tmp_recompute_wait_nested_brace_async_probe.cpp"
    probe.write_text(
        "auto handle = Foo{doc->recomputeAsync()};\n"
        "handle->wait();\n"
        "auto handle2 = wrap({doc->recomputeAsync()});\n"
        "handle2->wait();\n"
        "auto handle3{doc->recomputeAsync()};\n"
        "handle3->wait();\n"
        "void foo() {\n"
        "  auto cb = [&]() {\n"
        "    auto handle = doc->recomputeAsync();\n"
        "    handle->wait();\n"
        "  };\n"
        "}\n",
        encoding="utf-8",
    )
    try:
        found = _scan_recompute_handle_waits(paths=[probe])
    finally:
        probe.unlink(missing_ok=True)
    by_line = {line: evidence for _path, line, evidence in found}
    assert by_line[2] == "handle->wait();"
    assert by_line[4] == "handle2->wait();"
    assert by_line[6] == "handle3->wait();"
    assert by_line[10].startswith("handle->wait();")
    assert len(found) == 4


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
