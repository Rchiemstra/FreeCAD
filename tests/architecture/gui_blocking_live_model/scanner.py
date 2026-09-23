# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reproducible scanner for GUI blocking and live-model ingress.

The scanner walks the production GUI source (``src/Gui``, every ``Gui``
directory under ``src/Mod``, top-level production ``InitGui.py`` entry points,
and the reviewed module-aware Python GUI paths in :mod:`rules`), masks comments
and string/char/raw literals per language, then applies the category search
rules from :mod:`rules`. Its output is a deterministically sorted list of
findings, each recording the repository-relative path, 1-based line, category
key, owning subsystem, migration disposition, and the matched source line(s) as
evidence.

This module is both importable (used by the validator) and runnable as a
standalone reproducibility command::

    python3 tests/architecture/gui_blocking_live_model/scanner.py

    FREECAD_SOURCE_ROOT=/path/to/repo \\
        python3 tests/architecture/gui_blocking_live_model/scanner.py --write

The ``--write`` flag writes ``inventory.json`` next to this module, which is the
committed machine-readable snapshot.

C++ command strings are additionally decoded only when they are direct or
adjacent literal arguments to known executable GUI command wrappers/macros.
Their decoded payloads use Python patterns and masking; arbitrary C++ strings,
member/foreign-namespace calls, stream expressions, and runtime substitutions
remain masked.
"""

from __future__ import annotations

import argparse
import ast
import io
import json
import os
import re
import tokenize
from functools import lru_cache
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
        self,
        path: str,
        line: int,
        category: str,
        subsystem: str,
        disposition: str,
        evidence: str,
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


_CPP_COMMAND_ARGUMENTS: tuple[tuple[str, int], ...] = (
    ("Gui::Command::doCommand", 1),
    ("Gui::Command::runCommand", 1),
    ("Gui::cmdAppDocument", 1),
    ("Gui::cmdGuiDocument", 1),
    ("Gui::cmdAppObject", 1),
    ("Gui::cmdGuiObject", 1),
    ("Gui::cmdAppDocumentArgs", 1),
    ("Gui::cmdAppObjectArgs", 1),
    ("Gui::cmdGuiObjectArgs", 1),
    ("Gui::doCommandT", 1),
    ("Command::doCommand", 1),
    ("Command::runCommand", 1),
    ("doCommand", 1),
    ("runCommand", 1),
    ("cmdAppDocument", 1),
    ("cmdGuiDocument", 1),
    ("cmdAppObject", 1),
    ("cmdGuiObject", 1),
    ("cmdAppDocumentArgs", 1),
    ("cmdAppObjectArgs", 1),
    ("cmdGuiObjectArgs", 1),
    ("doCommandT", 1),
    ("_FCMD_DOC_CMD", 2),
    ("FCMD_DOC_CMD", 1),
    ("_FCMD_OBJ_DOC_CMD", 2),
    ("FCMD_OBJ_DOC_CMD", 1),
    ("FCMD_VOBJ_DOC_CMD", 1),
    ("_FCMD_OBJ_CMD", 3),
    ("FCMD_OBJ_CMD", 1),
    ("FCMD_VOBJ_CMD", 1),
    ("FCMD_OBJ_CMD2", 0),
    ("FCMD_VOBJ_CMD2", 0),
)

_CPP_COMMAND_PREFIXES: dict[str, str] = {
    "Gui::cmdAppDocument": "App.getDocument().",
    "cmdAppDocument": "App.getDocument().",
    "Gui::cmdAppDocumentArgs": "App.getDocument().",
    "cmdAppDocumentArgs": "App.getDocument().",
    "Gui::cmdGuiDocument": "Gui.getDocument().",
    "cmdGuiDocument": "Gui.getDocument().",
    "Gui::cmdAppObject": "App.getDocument().getObject().",
    "cmdAppObject": "App.getDocument().getObject().",
    "Gui::cmdAppObjectArgs": "App.getDocument().getObject().",
    "cmdAppObjectArgs": "App.getDocument().getObject().",
    "Gui::cmdGuiObject": "Gui.getDocument().getObject().",
    "cmdGuiObject": "Gui.getDocument().getObject().",
    "Gui::cmdGuiObjectArgs": "Gui.getDocument().getObject().",
    "cmdGuiObjectArgs": "Gui.getDocument().getObject().",
    "_FCMD_DOC_CMD": "App.getDocument().",
    "FCMD_DOC_CMD": "App.getDocument().",
    "_FCMD_OBJ_DOC_CMD": "App.getDocument().",
    "FCMD_OBJ_DOC_CMD": "App.getDocument().",
    "FCMD_VOBJ_DOC_CMD": "Gui.getDocument().",
    "_FCMD_OBJ_CMD": "App.getDocument().getObject().",
    "FCMD_OBJ_CMD": "App.getDocument().getObject().",
    "FCMD_VOBJ_CMD": "Gui.getDocument().getObject().",
    "FCMD_OBJ_CMD2": "App.getDocument().getObject().",
    "FCMD_VOBJ_CMD2": "Gui.getDocument().getObject().",
}
_CPP_GUI_WRAPPER_NAMES = frozenset(
    name for name, _index in _CPP_COMMAND_ARGUMENTS if name.startswith("Gui::")
)
_CPP_ANONYMOUS_NAMESPACE = "<anonymous>"

_CPP_GUI_USING_PATTERN = re.compile(
    r"\busing\s+(?:(?:namespace\s+)?(?:::)?\s*Gui|(?:::)?\s*Gui\s*::\s*Command)\s*;"
)
_CPP_GLOBAL_PREFIX_KEYWORDS = frozenset(
    {
        "alignof",
        "co_await",
        "co_return",
        "decltype",
        "delete",
        "new",
        "noexcept",
        "return",
        "sizeof",
        "throw",
        "typeid",
    }
)


def _skip_cpp_trivia(source: str, index: int, end: int) -> int:
    while index < end:
        if source[index].isspace():
            index += 1
        elif source.startswith("//", index):
            newline = source.find("\n", index + 2, end)
            index = end if newline < 0 else newline
        elif source.startswith("/*", index):
            closing = source.find("*/", index + 2, end)
            if closing < 0:
                return end
            index = closing + 2
        else:
            break
    return index


def _cpp_string_literal_spans(source: str, start: int, end: int) -> list[tuple[int, int]]:
    """Return spans only when the complete expression is adjacent string literals."""
    spans: list[tuple[int, int]] = []
    index = _skip_cpp_trivia(source, start, end)
    prefixes = ("u8R", "uR", "UR", "LR", "R", "u8", "u", "U", "L", "")
    while index < end:
        prefix = next(
            (candidate for candidate in prefixes if source.startswith(candidate + '"', index)),
            None,
        )
        if prefix is None:
            return []
        quote_start = index + len(prefix)
        literal_start = quote_start - 1 if prefix.endswith("R") else quote_start
        if prefix.endswith("R"):
            literal_end = _raw_literal_end(source, literal_start)
        else:
            literal_end = _quoted_literal_end(source, quote_start)
        if literal_end is None or literal_end > end:
            return []
        spans.append((literal_start, literal_end))
        index = _skip_cpp_trivia(source, literal_end, end)
    return spans


def _decode_cpp_string(source: str, start: int, end: int) -> str | None:
    if source.startswith('R"', start):
        delimiter_start = start + 2
        opening_parenthesis = source.find("(", delimiter_start, end)
        if opening_parenthesis < 0:
            return None
        delimiter = source[delimiter_start:opening_parenthesis]
        return source[opening_parenthesis + 1 : end - len(delimiter) - 2]
    quote_start = source.find('"', start, end)
    if quote_start < 0 or end <= quote_start + 1:
        return None
    literal = source[quote_start:end]
    try:
        value = ast.literal_eval(literal)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        try:
            value = bytes(literal[1:-1], "utf-8").decode("unicode_escape")
        except (UnicodeDecodeError, ValueError):
            return None
    return value if isinstance(value, str) else None


def _cpp_call_end(masked: str, opening: int) -> int | None:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "(":
            depth += 1
        elif masked[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    return None


def _cpp_call_argument_ranges(masked: str, opening: int, closing: int) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    start = opening + 1
    depth = {"(": 0, "[": 0, "{": 0}
    pairs = {")": "(", "]": "[", "}": "{"}
    for index in range(opening + 1, closing):
        character = masked[index]
        if character in depth:
            depth[character] += 1
        elif character in pairs:
            depth[pairs[character]] -= 1
        elif character == "," and not any(depth.values()):
            ranges.append((start, index))
            start = index + 1
    if start < closing or masked[opening + 1 : closing].strip():
        ranges.append((start, closing))
    return ranges


def _cpp_namespace_ranges(
    masked: str, brace_ranges: list[tuple[int, int]] | None = None
) -> list[tuple[int, int, str]]:
    """Return namespace brace ranges, including anonymous namespaces."""
    if brace_ranges is None:
        brace_ranges = _cpp_brace_ranges(masked)
    closing_by_opening = dict(brace_ranges)
    ranges: list[tuple[int, int, str]] = []
    declaration = re.compile(r"\bnamespace(?:\s+([A-Za-z_]\w*(?:\s*::\s*[A-Za-z_]\w*)*))?\s*\{")
    for match in declaration.finditer(masked):
        opening = masked.find("{", match.start(), match.end())
        closing = closing_by_opening.get(opening, len(masked))
        name = (
            _CPP_ANONYMOUS_NAMESPACE
            if match.group(1) is None
            else re.sub(r"\s*::\s*", "::", match.group(1))
        )
        ranges.append((opening, closing, name))
    return ranges


def _cpp_namespace_at(namespace_ranges: list[tuple[int, int, str]], offset: int) -> str | None:
    active = [name for opening, closing, name in namespace_ranges if opening < offset < closing]
    return "::".join(active) if active else None


def _cpp_gui_name_is_global(
    context: str | None,
    namespace_ranges: list[tuple[int, int, str]] | None = None,
    offset: int | None = None,
    namespace_identities: dict[str, int] | None = None,
    gui_shadows: list[tuple[int, tuple[int, int] | None, str | None, int | None]] | None = None,
) -> bool:
    if offset is not None and gui_shadows is not None:
        for declaration, scope, namespace_scope, lifetime_end in gui_shadows:
            if declaration >= offset or (lifetime_end is not None and offset >= lifetime_end):
                continue
            if namespace_scope is not None:
                if namespace_scope and (
                    context is None
                    or not (
                        context == namespace_scope or context.startswith(namespace_scope + "::")
                    )
                ):
                    continue
                return False
            if scope is not None and scope[0] < offset < scope[1]:
                return False
    if context is None:
        return True
    parts = context.split("::")
    if "Gui" in parts[1:]:
        return False
    if namespace_ranges is None or offset is None:
        return True
    for length in range(len(parts), 0, -1):
        candidate = "::".join((*parts[:length], "Gui"))
        if _cpp_namespace_declared_before(
            namespace_ranges, candidate, offset, namespace_identities
        ):
            return False
    # Once nearer candidates have been checked, lookup may still resolve the
    # global namespace Gui (including from unrelated namespace contexts).
    return True


def _cpp_brace_ranges(masked: str) -> list[tuple[int, int]]:
    """Return balanced C++ brace ranges in already-masked source."""
    stack: list[int] = []
    ranges: list[tuple[int, int]] = []
    for index, character in enumerate(masked):
        if character == "{":
            stack.append(index)
        elif character == "}" and stack:
            ranges.append((stack.pop(), index))
    return ranges


def _cpp_innermost_brace(
    brace_ranges: list[tuple[int, int]], offset: int
) -> tuple[int, int] | None:
    enclosing = [
        (opening, closing) for opening, closing in brace_ranges if opening < offset < closing
    ]
    return min(enclosing, key=lambda item: item[1] - item[0], default=None)


def _cpp_namespace_scope(
    namespace_ranges: list[tuple[int, int, str]],
    brace_ranges: list[tuple[int, int]],
    offset: int,
) -> str | None:
    context = _cpp_namespace_at(namespace_ranges, offset)
    if context is None:
        return "" if _cpp_innermost_brace(brace_ranges, offset) is None else None
    active = [
        (opening, closing)
        for opening, closing, _name in namespace_ranges
        if opening < offset < closing
    ]
    innermost_brace = _cpp_innermost_brace(brace_ranges, offset)
    if not active or innermost_brace is None or innermost_brace[0] != active[-1][0]:
        return None
    return context


def _cpp_namespace_declared_before(
    namespace_ranges: list[tuple[int, int, str]],
    context: str,
    offset: int,
    namespace_identities: dict[str, int] | None = None,
) -> bool:
    if namespace_identities is not None:
        return namespace_identities.get(context, offset) < offset
    for opening, _closing, name in namespace_ranges:
        if opening >= offset:
            continue
        parents = sorted(
            (parent_opening, parent_name)
            for parent_opening, parent_closing, parent_name in namespace_ranges
            if parent_opening < opening < parent_closing
        )
        identity = "::".join((*[parent_name for _opening, parent_name in parents], name))
        if identity == context:
            return True
    return False


def _cpp_namespace_identities(
    namespace_ranges: list[tuple[int, int, str]],
) -> dict[str, int]:
    identities: dict[str, int] = {}
    active: list[tuple[int, int, str]] = []
    for opening, _closing, name in namespace_ranges:
        while active and active[-1][1] <= opening:
            active.pop()
        identity = "::".join((*[parent_name for _opening, _closing, parent_name in active], name))
        identities.setdefault(identity, opening)
        active.append((opening, _closing, name))
    return identities


def _cpp_gui_using_is_global(
    masked: str,
    match: re.Match[str],
    namespace_ranges: list[tuple[int, int, str]],
    namespace_context: str | None,
    namespace_identities: dict[str, int] | None = None,
    gui_shadows: list[tuple[int, tuple[int, int] | None, str | None, int | None]] | None = None,
) -> bool:
    declaration = masked[match.start() : match.end()]
    if re.search(r"\busing\s+(?:namespace\s+)?\s*::", declaration):
        return True
    if not _cpp_gui_name_is_global(
        namespace_context,
        namespace_ranges,
        match.start(),
        namespace_identities,
        gui_shadows,
    ):
        return False
    if namespace_context is None:
        return True
    parts = namespace_context.split("::")
    for length in range(len(parts), 0, -1):
        candidate = "::".join((*parts[:length], "Gui"))
        if _cpp_namespace_declared_before(
            namespace_ranges, candidate, match.start(), namespace_identities
        ):
            return False
    return True


def _cpp_gui_using_declarations(
    masked: str,
    brace_ranges: list[tuple[int, int]],
    namespace_ranges: list[tuple[int, int, str]],
    namespace_identities: dict[str, int],
    gui_shadows: list[tuple[int, tuple[int, int] | None, str | None, int | None]] | None = None,
) -> list[tuple[int, str | None, tuple[int, int] | None]]:
    declarations: list[tuple[int, str | None, tuple[int, int] | None]] = []
    for match in _CPP_GUI_USING_PATTERN.finditer(masked):
        namespace_context = _cpp_namespace_at(namespace_ranges, match.start())
        if not _cpp_gui_using_is_global(
            masked,
            match,
            namespace_ranges,
            namespace_context,
            namespace_identities,
            gui_shadows,
        ):
            continue
        declarations.append(
            (
                match.start(),
                _cpp_namespace_scope(namespace_ranges, brace_ranges, match.start()),
                _cpp_innermost_brace(brace_ranges, match.start()),
            )
        )
    return declarations


def _cpp_template_shadow_body_opening(
    masked: str, parameter_closing: int, closing_by_opening: dict[int, int]
) -> int | None:
    search_start = parameter_closing + 1
    while True:
        body_opening = masked.find("{", search_start)
        declaration_end = masked.find(";", search_start)
        if body_opening < 0 or (declaration_end >= 0 and declaration_end < body_opening):
            return None
        requires_matches = list(re.finditer(r"\brequires\b", masked[search_start:body_opening]))
        if requires_matches:
            requires_offset = search_start + requires_matches[-1].start()
            if not _cpp_requires_is_clause(masked, requires_offset):
                body_closing = closing_by_opening.get(body_opening)
                if body_closing is None:
                    return None
                search_start = body_closing + 1
                continue
        return body_opening


def _cpp_template_parameter_has_gui(masked: str, opening: int, closing: int) -> bool:
    parameters = masked[opening + 1 : closing]
    segments: list[str] = []
    start = 0
    angle_depth = paren_depth = bracket_depth = brace_depth = 0
    for index, character in enumerate(parameters):
        if character == "<":
            angle_depth += 1
        elif character == ">" and angle_depth:
            angle_depth -= 1
        elif character == "(":
            paren_depth += 1
        elif character == ")" and paren_depth:
            paren_depth -= 1
        elif character == "[":
            bracket_depth += 1
        elif character == "]" and bracket_depth:
            bracket_depth -= 1
        elif character == "{":
            brace_depth += 1
        elif character == "}" and brace_depth:
            brace_depth -= 1
        elif character == "," and not any((angle_depth, paren_depth, bracket_depth, brace_depth)):
            segments.append(parameters[start:index])
            start = index + 1
    segments.append(parameters[start:])

    for segment in segments:
        angle_depth = paren_depth = bracket_depth = brace_depth = 0
        declaration_end = len(segment)
        for index, character in enumerate(segment):
            if character == "<":
                angle_depth += 1
            elif character == ">" and angle_depth:
                angle_depth -= 1
            elif character == "(":
                paren_depth += 1
            elif character == ")" and paren_depth:
                paren_depth -= 1
            elif character == "[":
                bracket_depth += 1
            elif character == "]" and bracket_depth:
                bracket_depth -= 1
            elif character == "{":
                brace_depth += 1
            elif character == "}" and brace_depth:
                brace_depth -= 1
            elif character == "=" and not any(
                (angle_depth, paren_depth, bracket_depth, brace_depth)
            ):
                declaration_end = index
                break
        declaration = segment[:declaration_end]
        names: list[str] = []
        angle_depth = paren_depth = bracket_depth = brace_depth = 0
        for token in re.finditer(r"[A-Za-z_]\w*", declaration):
            before = declaration[: token.start()]
            angle_depth = before.count("<") - before.count(">")
            paren_depth = before.count("(") - before.count(")")
            bracket_depth = before.count("[") - before.count("]")
            brace_depth = before.count("{") - before.count("}")
            if not any((angle_depth, paren_depth, bracket_depth, brace_depth)):
                names.append(token.group())
        if names and names[-1] == "Gui":
            return True
    return False


def _cpp_gui_shadow_declarations(
    masked: str,
    brace_ranges: list[tuple[int, int]],
    namespace_ranges: list[tuple[int, int, str]],
) -> list[tuple[int, tuple[int, int] | None, str | None, int | None]]:
    """Return ordered lexical declarations which hide the global ``Gui`` namespace."""
    shadows: list[tuple[int, tuple[int, int] | None, str | None, int | None]] = []
    closing_by_opening = dict(brace_ranges)

    def add_shadow(offset: int, lifetime_end: int | None = None) -> None:
        namespace_scope = _cpp_namespace_scope(namespace_ranges, brace_ranges, offset)
        scope = _cpp_innermost_brace(brace_ranges, offset)
        if namespace_scope is not None and scope is not None:
            active = [
                opening
                for opening, closing, _name in namespace_ranges
                if opening < offset < closing
            ]
            if active and scope[0] == active[-1]:
                scope = None
        shadows.append((offset, scope, namespace_scope, lifetime_end))

    for match in re.finditer(r"\b(?:struct|class|union)\s+Gui\b(?=\s*(?:[:{;]))", masked):
        add_shadow(match.start())
    for match in re.finditer(r"\busing\s+Gui\s*=", masked):
        add_shadow(match.start())
    for match in re.finditer(r"\btypedef\b[^;{}]*\bGui\b\s*(?=;)", masked):
        add_shadow(match.start())
    for match in re.finditer(r"\bnamespace\s+Gui\s*=", masked):
        add_shadow(match.start())

    def add_template_shadow(offset: int, opening: int) -> None:
        depth = 0
        closing = None
        for index in range(opening, len(masked)):
            if masked[index] == "<":
                depth += 1
            elif masked[index] == ">":
                depth -= 1
                if depth == 0:
                    closing = index
                    break
        if closing is None or not _cpp_template_parameter_has_gui(masked, opening, closing):
            return
        body_opening = _cpp_template_shadow_body_opening(masked, closing, closing_by_opening)
        if body_opening is not None:
            add_shadow(offset, closing_by_opening.get(body_opening))

    for match in re.finditer(r"\btemplate\s*<", masked):
        add_template_shadow(match.start(), masked.find("<", match.start(), match.end()))
    for match in re.finditer(r"\]\s*<", masked):
        add_template_shadow(match.start(), masked.find("<", match.start(), match.end()))

    shadows.sort(key=lambda item: item[0])
    return shadows


def _cpp_has_gui_using(
    offset: int,
    brace_ranges: list[tuple[int, int]],
    namespace_ranges: list[tuple[int, int, str]],
    declarations: list[tuple[int, str | None, tuple[int, int] | None]],
) -> bool:
    call_scope = _cpp_innermost_brace(brace_ranges, offset)
    call_namespace = _cpp_namespace_at(namespace_ranges, offset)
    for using_offset, namespace_scope, using_scope in declarations:
        if using_offset >= offset:
            break
        if namespace_scope is not None:
            if namespace_scope and not (
                call_namespace == namespace_scope
                or (call_namespace or "").startswith(namespace_scope + "::")
            ):
                continue
            if not namespace_scope:
                return True
            return True
        if using_scope is None or (
            using_scope[0] < offset < using_scope[1]
            and (call_scope is None or using_scope[0] <= call_scope[0])
        ):
            return True
    return False


def _cpp_qualifier_before(masked: str, offset: int) -> str | None:
    qualifier = re.search(r"((?:[A-Za-z_]\w*\s*::\s*)+)\s*$", masked[:offset])
    if not qualifier:
        return None
    normalized = re.sub(r"\s*::\s*", "::", qualifier.group(1)).strip()
    return normalized.removesuffix("::")


def _cpp_global_prefix_is_qualified(masked: str, offset: int) -> bool:
    prefix = masked[:offset].rstrip()
    if prefix.endswith(("::", ".", "->")):
        return True
    previous = re.search(r"([A-Za-z_]\w*)$", prefix)
    return bool(previous and previous.group(1) not in _CPP_GLOBAL_PREFIX_KEYWORDS)


def _cpp_member_access_before(masked: str, offset: int) -> bool:
    """Return whether a qualified call is selected through an object member."""
    return bool(re.search(r"(?:\.|->)\s*$", masked[:offset]))


def _cpp_sizeof_wrapper_operand_end(
    masked: str, start: int, wrapper_call: re.Pattern[str]
) -> int | None:
    index = _skip_cpp_trivia(masked, start, len(masked))
    for prefix in ("++", "--", "*", "+", "-", "!", "~", "&"):
        if masked.startswith(prefix, index):
            return _cpp_sizeof_wrapper_operand_end(masked, index + len(prefix), wrapper_call)
    index = _skip_cpp_trivia(masked, index, len(masked))
    wrapper_match = wrapper_call.match(masked, index)
    if wrapper_match is not None:
        opening = masked.find("(", index, wrapper_match.end())
        closing = _cpp_call_end(masked, opening)
        return None if closing is None else closing + 1
    cast_match = re.match(
        r"(?:static_cast|const_cast|reinterpret_cast|dynamic_cast)\s*<", masked[index:]
    )
    if cast_match is not None:
        angle_opening = index + cast_match.end() - 1
        angle_depth = 0
        angle_closing = None
        for angle_index in range(angle_opening, len(masked)):
            if masked[angle_index] == "<":
                angle_depth += 1
            elif masked[angle_index] == ">":
                angle_depth -= 1
                if angle_depth == 0:
                    angle_closing = angle_index
                    break
        if angle_closing is None:
            return None
        cast_opening = _skip_cpp_trivia(masked, angle_closing + 1, len(masked))
        if cast_opening >= len(masked) or masked[cast_opening] != "(":
            return None
        cast_closing = _cpp_call_end(masked, cast_opening)
        if cast_closing is None:
            return None
        inner_end = _cpp_sizeof_wrapper_operand_end(masked, cast_opening + 1, wrapper_call)
        if (
            inner_end is not None
            and _skip_cpp_trivia(masked, inner_end, cast_closing) == cast_closing
        ):
            return cast_closing + 1
        return None
    if index >= len(masked) or masked[index] != "(":
        return None
    outer_closing = _cpp_call_end(masked, index)
    if outer_closing is None:
        return None
    inner_end = _cpp_sizeof_wrapper_operand_end(masked, index + 1, wrapper_call)
    if (
        inner_end is not None
        and _skip_cpp_trivia(masked, inner_end, outer_closing) == outer_closing
    ):
        return outer_closing + 1
    cast = masked[index + 1 : outer_closing].strip()
    if re.fullmatch(r"(?:const\s+)?[A-Za-z_]\w*(?:\s*[A-Za-z_]\w*)?(?:\s*[&*])?", cast):
        return _cpp_sizeof_wrapper_operand_end(masked, outer_closing + 1, wrapper_call)
    return None


def _cpp_unevaluated_ranges(
    masked: str, brace_ranges: list[tuple[int, int]]
) -> list[tuple[int, int]]:
    """Return source ranges whose expressions are not evaluated at runtime.

    The ranges are deliberately limited to standard unevaluated operands. In
    particular, ``typeid`` is omitted because a polymorphic glvalue operand
    may be evaluated, while ``return``/``throw``/``co_return`` remain ordinary
    evaluated expressions.
    """
    ranges: list[tuple[int, int]] = []
    for match in re.finditer(r"\b(?:alignof|decltype|noexcept|sizeof)\s*\(", masked):
        opening = masked.find("(", match.start(), match.end())
        closing = _cpp_call_end(masked, opening)
        if closing is not None:
            ranges.append((opening, closing + 1))

    wrapper_names = "|".join(
        re.escape(name).replace(r"::", r"\s*::\s*") for name, _index in _CPP_COMMAND_ARGUMENTS
    )
    wrapper_call = re.compile(rf"(?:::)?\s*(?:{wrapper_names})\s*\(")

    for match in re.finditer(r"\b(?:alignof|sizeof)\b(?!\s*\()", masked):
        operand_start = _skip_cpp_trivia(masked, match.end(), len(masked))
        operand_end = _cpp_sizeof_wrapper_operand_end(masked, operand_start, wrapper_call)
        if operand_end is not None:
            ranges.append((operand_start, operand_end))

    closing_by_opening = dict(brace_ranges)
    prefix_starts = _cpp_requires_prefix_starts(masked)
    for match in re.finditer(r"\brequires\b", masked):
        index = _skip_cpp_trivia(masked, match.end(), len(masked))
        if index < len(masked) and masked[index] == "(":
            closing = _cpp_call_end(masked, index)
            if closing is None:
                continue
            ranges.append((index, closing + 1))
            index = _skip_cpp_trivia(masked, closing + 1, len(masked))
        if (
            index < len(masked)
            and masked[index] == "{"
            and not _cpp_requires_is_clause(masked, match.start(), prefix_starts.get(match.start()))
        ):
            closing = closing_by_opening.get(index)
            if closing is not None:
                ranges.append((index, closing + 1))
    return ranges


def _cpp_requires_is_clause(masked: str, offset: int, prefix_start: int | None = None) -> bool:
    """Recognize a trailing function/lambda requires-clause before its body."""
    # Qualifiers and the function declarator are immediately adjacent to the
    # requires keyword.  Keep the ordinary path bounded, but recover a generic
    # lambda head from its own capture introducer: comments between the head
    # and ``requires`` are legal and must not be cut off by an arbitrary byte
    # limit.
    if prefix_start is None:
        prefix_start = _cpp_requires_prefix_start(masked, offset)
    prefix = masked[prefix_start:offset]
    if _cpp_requires_generic_lambda_invocation_expression(prefix):
        return False
    if _cpp_requires_ref_qualifier_is_declarator(prefix):
        return True
    if _cpp_requires_generic_lambda_template_head_is_clause(masked, offset, prefix_start):
        return True
    for opening, has_ref_qualifier in _cpp_requires_declarator_candidates(prefix):
        declarator = prefix[:opening]
        if _cpp_requires_generic_lambda_invocation_prefix(declarator):
            continue
        if (
            _cpp_requires_generic_lambda_parameter_prefix(declarator)
            and re.search(r"}\s*(?:\(|\.)", prefix[opening:])
            and "requires requires" not in prefix[opening:]
        ):
            continue
        if not has_ref_qualifier and _cpp_requires_declarator_is_declarator(prefix[:opening]):
            return True
    return False


def _cpp_requires_prefix_start(masked: str, offset: int) -> int:
    """Find the start of the enclosing declaration/statement before ``requires``."""
    depths = {"}": 0, ")": 0, "]": 0}
    matching = {"{": "}", "(": ")", "[": "]"}
    for index in range(offset - 1, -1, -1):
        character = masked[index]
        if character in depths:
            depths[character] += 1
        elif character in matching:
            closing = matching[character]
            if depths[closing]:
                depths[closing] -= 1
        elif character == ";" and not any(depths.values()):
            return index + 1
    return 0


def _cpp_requires_prefix_starts(masked: str) -> dict[int, int]:
    """Return statement starts for every ``requires`` keyword in one pass."""
    starts: dict[int, int] = {}
    stack: list[str] = []
    matching = {"(": ")", "[": "]", "{": "}"}
    statement_start = 0
    for index, character in enumerate(masked):
        if character in matching:
            stack.append(matching[character])
        elif stack and character == stack[-1]:
            stack.pop()
        elif character == ";" and not stack:
            statement_start = index + 1
        elif not stack and masked.startswith("requires", index):
            before = masked[index - 1] if index else " "
            after_index = index + len("requires")
            after = masked[after_index] if after_index < len(masked) else " "
            if not (before.isalnum() or before == "_") and not (after.isalnum() or after == "_"):
                starts[index] = statement_start
    return starts


def _cpp_requires_suffix(suffix: str) -> tuple[bool, bool] | None:
    """Parse function declarator suffixes immediately before ``requires``."""
    index = 0
    phase = 0
    has_ref_qualifier = False
    end = len(suffix)
    while True:
        index = _skip_cpp_trivia(suffix, index, end)
        if index == end:
            return has_ref_qualifier, False
        if suffix.startswith("[[", index):
            if phase > 1:
                return None
            closing = suffix.find("]]", index + 2)
            if closing < 0:
                return None
            phase = 1
            index = closing + 2
            continue
        qualifier = re.match(
            r"(?:const|volatile|override|final|mutable|constexpr|consteval|static)\b",
            suffix[index:],
        )
        if qualifier is not None:
            if phase != 0 or has_ref_qualifier:
                return None
            index += qualifier.end()
            continue
        if suffix.startswith("&&", index) or suffix.startswith("&", index):
            if phase != 0 or has_ref_qualifier:
                return None
            has_ref_qualifier = True
            index += 2 if suffix.startswith("&&", index) else 1
            continue
        exception = re.match(r"(?:noexcept|throw)\b", suffix[index:])
        if exception is not None:
            if phase > 1:
                return None
            phase = 1
            index += exception.end()
            index = _skip_cpp_trivia(suffix, index, end)
            if index < end and suffix[index] == "(":
                closing = _cpp_call_end(suffix, index)
                if closing is None:
                    return None
                index = closing + 1
            continue
        if suffix.startswith("->", index):
            if phase > 1 or not suffix[index + 2 :].strip():
                return None
            return has_ref_qualifier, True
        return None


def _cpp_requires_declarator_candidates(prefix: str) -> list[tuple[int, bool]]:
    candidates: list[tuple[int, bool]] = []
    for closing in range(len(prefix) - 1, -1, -1):
        if prefix[closing] != ")":
            continue
        depth = 0
        opening = None
        for index in range(closing, -1, -1):
            if prefix[index] == ")":
                depth += 1
            elif prefix[index] == "(":
                depth -= 1
                if depth == 0:
                    opening = index
                    break
        if opening is None:
            continue
        parsed_suffix = _cpp_requires_suffix(prefix[closing + 1 :])
        if parsed_suffix is not None:
            candidates.append((opening, parsed_suffix[0]))
    return candidates


def _cpp_requires_declarator_is_declarator(declarator: str) -> bool:
    declarator = declarator.rstrip()
    # A requires-expression in a generic lambda's template head may contain a
    # semicolon and braces before the parameter list.  Check that assignment
    # before trimming ordinary statement/body prefixes below, which would
    # otherwise discard that valid lambda RHS along with its constraint body.
    assignments = [match.start() for match in re.finditer("=", declarator)]
    for assignment in reversed(assignments):
        assignment_rhs = declarator[assignment + 1 :]
        if ".template operator" in assignment_rhs:
            return False
        if _cpp_requires_lambda_assignment_rhs(assignment_rhs):
            return True
    boundary = max(declarator.rfind(";"), declarator.rfind("{"), declarator.rfind("}"))
    declarator = declarator[boundary + 1 :]
    angle_depth = bracket_depth = 0
    for index, character in enumerate(declarator):
        if character == "<":
            angle_depth += 1
        elif character == ">" and angle_depth:
            angle_depth -= 1
        elif character == "[":
            bracket_depth += 1
        elif character == "]" and bracket_depth:
            bracket_depth -= 1
        elif character == "=" and angle_depth == 0 and bracket_depth == 0:
            operator_start = declarator.rfind("operator", 0, index)
            if operator_start < 0 and not _cpp_requires_lambda_assignment_rhs(
                declarator[index + 1 :]
            ):
                return False
            if operator_start >= 0 and not re.fullmatch(
                r"\s*[^A-Za-z0-9_]*",
                declarator[operator_start + len("operator") : index],
            ):
                return False
    if re.search(r"\b(?:return|co_return|throw|static_assert)\b", declarator):
        return False
    if re.search(
        r"\b(?:auto|void|bool|char|short|int|long|float|double|template|operator)\b",
        declarator,
    ):
        return True
    return bool(
        re.search(
            r"(?:^|\s)[A-Za-z_]\w*(?:::\w+)*(?:\s*[&*]+)?(?:\s*<[^<>]*>)?\s+[A-Za-z_]\w*(?:\s*<[^<>]*>)?\s*$",
            declarator,
        )
    )


def _cpp_requires_lambda_assignment_rhs(
    rhs: str, *, require_generic_template_head: bool = False
) -> bool:
    """Recognize a lambda assignment RHS, optionally requiring a generic head."""
    index = _skip_cpp_trivia(rhs, 0, len(rhs))
    if index >= len(rhs) or rhs[index] != "[":
        return False
    capture = _cpp_requires_lambda_capture(rhs, index)
    if capture is None:
        return False
    _capture_opening, closing = capture
    index = _skip_cpp_trivia(rhs, closing + 1, len(rhs))
    has_template_head = False
    if index < len(rhs) and rhs[index] == "<":
        has_template_head = True
        angle_depth = 0
        template_closing = None
        for angle_index in range(index, len(rhs)):
            if rhs[angle_index] == "<":
                angle_depth += 1
            elif rhs[angle_index] == ">" and angle_depth:
                angle_depth -= 1
                if angle_depth == 0:
                    template_closing = angle_index
                    break
        if template_closing is None:
            return False
        index = template_closing + 1
    if require_generic_template_head and not has_template_head:
        return False
    if require_generic_template_head:
        # This shortcut is only for the requires keyword immediately after a
        # parameterless generic lambda's template head.  Do not reinterpret a
        # template-head constraint, lambda body, or invocation as that clause.
        return not rhs[index:].strip()
    index = _skip_cpp_trivia(rhs, index, len(rhs))
    if index == len(rhs):
        return True
    if not re.match(r"requires\b", rhs[index:]):
        return False

    # A generic lambda may constrain its template head before the parameter
    # list.  This deliberately only checks the small prefix needed here; the
    # parameter list and trailing lambda requires-clause are handled by the
    # caller's declarator candidates.
    constraint = rhs[index + len("requires") :].strip()
    if not constraint:
        return False
    openings = "([{"
    matching = {
        ")": "(",
        "]": "[",
        "}": "{",
    }
    stack: list[str] = []
    for character in constraint:
        if character in openings:
            stack.append(character)
        elif character in matching:
            if not stack or stack.pop() != matching[character]:
                return False
        elif character == ";" and "{" not in stack:
            return False
    return not stack


def _cpp_requires_matching_opening(
    source: str, closing: int, opening_character: str, closing_character: str
) -> int | None:
    depth = 0
    for index in range(closing, -1, -1):
        if source[index] == closing_character:
            depth += 1
        elif source[index] == opening_character:
            depth -= 1
            if depth == 0:
                return index
    return None


def _cpp_requires_lambda_capture(source: str, opening: int) -> tuple[int, int] | None:
    """Return a balanced lambda capture range beginning at ``opening``."""
    if opening >= len(source) or source[opening] != "[":
        return None
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "[":
            depth += 1
        elif source[index] == "]":
            depth -= 1
            if depth == 0:
                return opening, index
    return None


def _cpp_requires_generic_lambda_invocation_expression(prefix: str) -> bool:
    """Return whether a prefix ends in a lambda invocation before ``&&``/``||``."""
    for closing in range(len(prefix) - 1, -1, -1):
        if prefix[closing] != ")":
            continue
        opening = _cpp_requires_matching_opening(prefix, closing, "(", ")")
        if opening is None:
            continue
        if not re.fullmatch(r"\s*(?:&&|\|\|)\s*", prefix[closing + 1 :]):
            continue
        if _cpp_requires_generic_lambda_invocation_prefix(prefix[:opening]):
            return True
    return False


def _cpp_requires_generic_lambda_template_head_is_clause(
    source: str, offset: int, declaration_start: int = 0
) -> bool:
    """Recognize an assigned generic lambda with an implicit parameter list."""
    for capture_opening in range(declaration_start, offset):
        if source[capture_opening] != "[":
            continue
        capture = _cpp_requires_lambda_capture(source, capture_opening)
        if capture is None:
            continue
        opening, _capture_closing = capture
        if opening >= offset:
            continue
        for assignment in (
            match.start() + declaration_start
            for match in re.finditer("=", source[declaration_start:opening])
        ):
            rhs = source[assignment + 1 : offset]
            if not rhs.lstrip().startswith("["):
                continue
            if _cpp_requires_lambda_assignment_rhs(rhs, require_generic_template_head=True):
                return True
    return False


def _cpp_requires_ref_qualifier_is_declarator(prefix: str) -> bool:
    for opening, has_ref_qualifier in _cpp_requires_declarator_candidates(prefix):
        if not has_ref_qualifier:
            continue
        declarator = prefix[:opening]
        if _cpp_requires_generic_lambda_invocation_prefix(declarator):
            continue
        if _cpp_requires_declarator_is_declarator(declarator):
            return True
    return False


def _cpp_requires_generic_lambda_invocation_prefix(declarator: str) -> bool:
    """Reject a completed generic-lambda invocation as a function declarator.

    The input ends immediately before the invocation's argument-list opening
    parenthesis.  Parse the two supported spellings rather than recognizing a
    loose textual suffix: ``lambda()`` and
    ``lambda.template operator()<args>()``.  Delimiter matching makes nested
    explicit template arguments and masked comments unambiguous, while the
    required lambda capture/template head prevents ref-qualified declarations
    from being reclassified.
    """
    end = len(declarator)
    while end and declarator[end - 1].isspace():
        end -= 1
    if not end:
        return False

    body_closing = end - 1
    if declarator[body_closing] != "}":
        template_closing = body_closing
        angle_depth = 0
        while template_closing >= 0:
            character = declarator[template_closing]
            if character == ">":
                angle_depth += 1
            elif character == "<":
                angle_depth -= 1
                if angle_depth == 0:
                    break
            template_closing -= 1
        if template_closing < 0:
            return False
        operator_end = template_closing
        operator_start = declarator.rfind("operator", 0, operator_end)
        if operator_start < 0:
            return False
        before_operator = declarator[:operator_start].rstrip()
        if not re.search(r"\.\s*(?:template\s*)?$", before_operator):
            return False
        operator_text = declarator[operator_start:template_closing]
        if not re.fullmatch(r"operator\s*\(\)\s*", operator_text):
            return False
        body_prefix = re.sub(r"\.\s*(?:template\s*)?$", "", before_operator).rstrip()
        body_closing = len(body_prefix) - 1
        if body_closing < 0 or declarator[body_closing] != "}":
            return False
    body_opening = body_closing
    brace_depth = 0
    while body_opening >= 0:
        character = declarator[body_opening]
        if character == "}":
            brace_depth += 1
        elif character == "{":
            brace_depth -= 1
            if brace_depth == 0:
                break
        body_opening -= 1
    if body_opening < 0:
        return False

    head = declarator[:body_opening]
    capture_opening = capture_closing = None
    for candidate in range(len(head)):
        if head[candidate] != "[":
            continue
        capture = _cpp_requires_lambda_capture(head, candidate)
        if capture is None:
            continue
        candidate_closing = capture[1]
        template_opening = _skip_cpp_trivia(head, candidate_closing + 1, len(head))
        if template_opening < len(head) and head[template_opening] == "<":
            capture_opening, capture_closing = candidate, candidate_closing
            break
    if capture_opening is None or capture_closing is None:
        return False
    template_opening = _skip_cpp_trivia(head, capture_closing + 1, len(head))
    if template_opening >= len(head) or head[template_opening] != "<":
        return False
    template_depth = 0
    template_closing = None
    for index in range(template_opening, len(head)):
        if head[index] == "<":
            template_depth += 1
        elif head[index] == ">":
            template_depth -= 1
            if template_depth == 0:
                template_closing = index
                break
    if template_closing is None:
        return False
    return _cpp_requires_generic_lambda_head(head[capture_opening:])


def _cpp_requires_generic_lambda_parameter_prefix(declarator: str) -> bool:
    """Reject a generic lambda's template/parameter prefix as a declaration."""
    return any(
        _cpp_requires_generic_lambda_head(declarator[assignment + 1 :])
        for assignment in (match.start() for match in re.finditer("=", declarator))
    )


def _cpp_requires_generic_lambda_head(head: str) -> bool:
    """Recognize a generic lambda head, including its declarator suffixes."""
    index = _skip_cpp_trivia(head, 0, len(head))
    if index >= len(head) or head[index] != "[":
        return False
    capture = _cpp_requires_lambda_capture(head, index)
    if capture is None:
        return False
    _capture_opening, capture_closing = capture
    template_opening = _skip_cpp_trivia(head, capture_closing + 1, len(head))
    if template_opening >= len(head) or head[template_opening] != "<":
        return False
    angle_depth = 0
    template_closing = None
    for index in range(template_opening, len(head)):
        if head[index] == "<":
            angle_depth += 1
        elif head[index] == ">":
            angle_depth -= 1
            if angle_depth == 0:
                template_closing = index
                break
    if template_closing is None:
        return False

    suffix = head[template_closing + 1 :]
    stripped = suffix.lstrip()
    if re.fullmatch(r"requires\s+requires\s*\([^{}]*\)\s*", stripped):
        return False
    if stripped and not re.match(
        r"(?:\(|requires\b|mutable\b|constexpr\b|consteval\b|static\b|"
        r"noexcept\b|throw\b|\[\[|->)",
        stripped,
    ):
        return False
    stack: list[str] = []
    matching = {")": "(", "]": "[", "}": "{"}
    for character in suffix:
        if character in "([{":
            stack.append(character)
        elif character in matching:
            if not stack or stack.pop() != matching[character]:
                return False
        elif character == ";" and not stack:
            return False
    return not stack


def _cpp_offset_in_ranges(offset: int, ranges: list[tuple[int, int]]) -> bool:
    return any(start <= offset < end for start, end in ranges)


def _decoded_cpp_command_literals(source: str) -> list[tuple[str, list[int]]]:
    """Decode literals passed to known executable GUI command wrappers."""
    masked = mask_cpp_non_code(source)
    brace_ranges = _cpp_brace_ranges(masked)
    unevaluated_ranges = _cpp_unevaluated_ranges(masked, brace_ranges)
    namespace_ranges = _cpp_namespace_ranges(masked, brace_ranges)
    namespace_identities = _cpp_namespace_identities(namespace_ranges)
    gui_shadows = _cpp_gui_shadow_declarations(masked, brace_ranges, namespace_ranges)
    using_declarations = _cpp_gui_using_declarations(
        masked,
        brace_ranges,
        namespace_ranges,
        namespace_identities,
        gui_shadows,
    )
    names = "|".join(
        re.escape(name).replace(r"::", r"\s*::\s*") for name, _index in _CPP_COMMAND_ARGUMENTS
    )
    pattern = re.compile(rf"(?<![\w:])(?:::)?\s*(?:{names})\s*\(")
    decoded: list[tuple[str, list[int]]] = []
    for match in pattern.finditer(masked):
        if _cpp_offset_in_ranges(match.start(), unevaluated_ranges):
            continue
        opening = masked.find("(", match.start(), match.end())
        if opening < 0:
            continue
        closing = _cpp_call_end(masked, opening)
        if closing is None:
            continue
        raw_function_name = match.group(0).split("(", 1)[0].strip()
        function_name = re.sub(r"\s*::\s*", "::", raw_function_name)
        globally_qualified = function_name.startswith("::")
        lookup_name = function_name.removeprefix("::")
        qualifier = _cpp_qualifier_before(masked, match.start())
        context = _cpp_namespace_at(namespace_ranges, match.start())
        if globally_qualified and _cpp_global_prefix_is_qualified(masked, match.start()):
            continue
        if _cpp_member_access_before(masked, match.start()):
            continue
        if "::" not in lookup_name:
            if re.search(r"(?:\.|->|::)\s*$", masked[: match.start()]):
                continue
        elif lookup_name in _CPP_GUI_WRAPPER_NAMES:
            if qualifier is not None:
                continue
            if not globally_qualified and not _cpp_gui_name_is_global(
                context,
                namespace_ranges,
                match.start(),
                namespace_identities,
                gui_shadows,
            ):
                continue
        elif lookup_name.startswith("Command::"):
            if globally_qualified or (qualifier is not None and qualifier != "Gui"):
                continue
            if qualifier == "Gui":
                if context is not None and context.split("::", 1)[0] != "Gui":
                    continue
            elif (context is None or context.split("::", 1)[0] != "Gui") and not _cpp_has_gui_using(
                match.start(),
                brace_ranges,
                namespace_ranges,
                using_declarations,
            ):
                continue
        if globally_qualified and lookup_name not in _CPP_GUI_WRAPPER_NAMES:
            continue
        argument_index = next(
            index for name, index in _CPP_COMMAND_ARGUMENTS if lookup_name.endswith(name)
        )
        arguments = _cpp_call_argument_ranges(masked, opening, closing)
        if argument_index >= len(arguments):
            continue
        literal_spans = _cpp_string_literal_spans(source, *arguments[argument_index])
        values: list[str] = []
        line_map: list[int] = []
        for literal_start, literal_end in literal_spans:
            value = _decode_cpp_string(source, literal_start, literal_end)
            if value is None:
                values = []
                break
            values.append(value)
            line_map.extend(_decoded_line_map(value, source, literal_start, literal_end))
        if values:
            prefix = _CPP_COMMAND_PREFIXES.get(lookup_name, "")
            payload = "".join(values)
            if not payload:
                continue
            command = prefix + payload
            if not command:
                continue
            if not line_map:
                anchor = source.count("\n", 0, literal_spans[0][0]) + 1
                prefix_map = [anchor] * len(prefix)
            else:
                prefix_map = [line_map[0]] * len(prefix)
            decoded.append((command, prefix_map + line_map))
    return decoded


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


def _py_string_end(source: str, index: int) -> int:
    """Return the index one past the Python string literal starting at ``index``.

    ``index`` points at the opening quote; an ``f``/``r``/``b``/``u`` prefix is an
    inert letter immediately before it. Handles triple-quoted literals and
    backslash escapes.
    """
    char = source[index]
    if source.startswith(char * 3, index):
        return _triple_quoted_end(source, index, char)
    return _quoted_literal_end(source, index)


def _line_offsets(source: str) -> list[int]:
    """Return absolute offsets for the starts of source lines."""
    offsets = [0]
    for index, character in enumerate(source):
        if character == "\n":
            offsets.append(index + 1)
    return offsets


def _ast_offset(offsets: list[int], source_lines: list[str], line: int, column: int) -> int:
    """Convert an AST's UTF-8 byte column to a Python string offset."""
    line_text = source_lines[line - 1]
    byte_prefix = line_text.encode("utf-8")[:column]
    return offsets[line - 1] + len(byte_prefix.decode("utf-8", errors="ignore"))


def _token_offset(offsets: list[int], line: int, column: int) -> int:
    """Convert a tokenize character column to an absolute source offset."""
    return offsets[line - 1] + column


@lru_cache(maxsize=4)
def _all_literal_token_spans(source: str) -> tuple[tuple[int, int], ...]:
    """Tokenize one source string and return all decoded-string token spans."""
    offsets = _line_offsets(source)
    try:
        tokens = tokenize.generate_tokens(io.StringIO(source).readline)
        spans: list[tuple[int, int]] = []
        for token in tokens:
            if token.type != tokenize.STRING:
                continue
            token_start = _token_offset(offsets, token.start[0], token.start[1])
            token_end = _token_offset(offsets, token.end[0], token.end[1])
            quote_offset = next(
                (
                    index
                    for index, character in enumerate(source[token_start:token_end])
                    if character in "\"'"
                ),
                0,
            )
            spans.append((token_start + quote_offset, token_end))
        return tuple(spans)
    except (IndentationError, SyntaxError, tokenize.TokenError):
        return ()


def _literal_token_spans(source: str, start: int, end: int) -> list[tuple[int, int]]:
    """Return string-token spans in a source range (including implicit joins)."""
    return [
        (quote_start, literal_end)
        for quote_start, literal_end in _all_literal_token_spans(source)
        if quote_start >= start and literal_end <= end
    ]


def _string_expression_spans(
    node: ast.AST, offsets: list[int], source_lines: list[str], source: str
) -> list[tuple[int, int]]:
    """Return executable string-expression spans below a doCommand argument.

    AST ownership is important here: a string in a nested function call is an
    inert value, while a literal, concatenation, parenthesised literal, or
    f-string that forms the first command argument is executable later by
    FreeCAD. The returned spans are subsequently re-masked so nested command
    strings/comments remain inert.
    """
    if isinstance(node, (ast.Constant, ast.JoinedStr)):
        if isinstance(node, ast.Constant) and not isinstance(node.value, str):
            return []
        if not hasattr(node, "lineno") or not hasattr(node, "end_lineno"):
            return []
        start = _ast_offset(offsets, source_lines, node.lineno, node.col_offset)
        end = _ast_offset(offsets, source_lines, node.end_lineno, node.end_col_offset)
        if isinstance(node, ast.Constant):
            literal_spans = _literal_token_spans(source, start, end)
            if literal_spans:
                return literal_spans
        # AST ranges include an f/r/b/u prefix, but the masker visits the
        # opening quote itself. Keep the prefix visible and use the quote range
        # as the re-masking key.
        quote_start = next((index for index in range(start, end) if source[index] in "\"'"), start)
        return [(quote_start, end)]
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        return _string_expression_spans(
            node.left, offsets, source_lines, source
        ) + _string_expression_spans(node.right, offsets, source_lines, source)
    return []


def _literal_source(source: str, quote_start: int, end: int) -> str:
    """Return a literal source span, including any prefix before its quote."""
    start = quote_start
    while start and source[start - 1] in "rRuUbBfF":
        start -= 1
    return source[start:end]


def _decoded_line_map(value: str, source: str, quote_start: int, literal_end: int) -> list[int]:
    """Map decoded characters to conservative physical source lines.

    Escaped newlines do not consume a repository line, while real newlines in
    triple-quoted literals do. A source-line escape therefore stays anchored to
    the literal's line; physical newlines advance the mapping one line at a
    time. This is intentionally conservative for unusual mixtures of escaped
    and physical newlines and keeps evidence on a real source line.
    """
    line = source.count("\n", 0, quote_start) + 1
    physical_newlines = source.count("\n", quote_start, literal_end)
    mapping: list[int] = []
    for character in value:
        mapping.append(line)
        if character == "\n" and physical_newlines:
            physical_newlines -= 1
            line += 1
    return mapping


def _decoded_string_expression(
    node: ast.AST, source: str, offsets: list[int], source_lines: list[str]
) -> tuple[str, list[int]] | None:
    """Decode a literal/implicit string expression and its source-line map."""
    if isinstance(node, ast.JoinedStr):
        if not node.values:
            return None
        start = _ast_offset(offsets, source_lines, node.lineno, node.col_offset)
        end = _ast_offset(offsets, source_lines, node.end_lineno, node.end_col_offset)
        # Python 3.12 exposes f-strings as FSTRING_* tokens rather than a
        # STRING token, so token-based discovery is not available here. The
        # literal segments are already decoded by the AST. Unknown formatted
        # values become a NUL separator so a regex cannot bridge an
        # interpolation (for example ``FreeCAD{doc}.ActiveDocument``).
        decoded_parts: list[str] = []
        line_map: list[int] = []
        for value in node.values:
            if isinstance(value, ast.Constant) and isinstance(value.value, str):
                segment_start = _ast_offset(offsets, source_lines, value.lineno, value.col_offset)
                segment_end = _ast_offset(
                    offsets, source_lines, value.end_lineno, value.end_col_offset
                )
                decoded_parts.append(value.value)
                line_map.extend(_decoded_line_map(value.value, source, segment_start, segment_end))
            elif isinstance(value, ast.FormattedValue):
                decoded_parts.append("\x00")
                line_map.append(value.lineno)
            else:
                return None
        return "".join(decoded_parts), line_map
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left = _decoded_string_expression(node.left, source, offsets, source_lines)
        right = _decoded_string_expression(node.right, source, offsets, source_lines)
        if left is None or right is None:
            return None
        return left[0] + right[0], left[1] + right[1]
    if not isinstance(node, ast.Constant) or not isinstance(node.value, str):
        return None
    if not hasattr(node, "lineno") or not hasattr(node, "end_lineno"):
        return None
    start = _ast_offset(offsets, source_lines, node.lineno, node.col_offset)
    end = _ast_offset(offsets, source_lines, node.end_lineno, node.end_col_offset)
    values: list[str] = []
    literal_spans = _literal_token_spans(source, start, end)
    if not literal_spans:
        return None
    for quote_start, literal_end in literal_spans:
        try:
            value = ast.literal_eval(_literal_source(source, quote_start, literal_end))
        except (SyntaxError, ValueError, TypeError, MemoryError):
            return None
        if not isinstance(value, str):
            return None
        values.append(value)
    mapping: list[int] = []
    for (quote_start, literal_end), value in zip(literal_spans, values):
        mapping.extend(_decoded_line_map(value, source, quote_start, literal_end))
    return "".join(values), mapping


def _decoded_do_command_literals(source: str) -> list[tuple[str, list[int]]]:
    """Return decoded command strings and their physical source-line maps."""
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []
    offsets = _line_offsets(source)
    source_lines = source.splitlines()
    decoded: list[tuple[str, list[int]]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not node.args:
            continue
        function = node.func
        if not isinstance(function, ast.Attribute) or function.attr != "doCommand":
            continue
        if not isinstance(function.value, ast.Name) or function.value.id not in {
            "FreeCADGui",
            "Gui",
        }:
            continue
        value = _decoded_string_expression(node.args[0], source, offsets, source_lines)
        if value is not None:
            decoded.append(value)
    return decoded


def _python_update_data_provider_matches(source: str, relative_path: str) -> list[tuple[int, str]]:
    """Return AST-classified Python ViewProvider ``updateData`` methods.

    Generic Qt model callbacks commonly use the same method name with
    ``topLeft``/``bottomRight`` parameters. Provider classification therefore
    requires a provider-named class or a reviewed provider module path instead
    of treating every ``def updateData`` as a presentation callback.
    """
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []
    path_lower = relative_path.lower()
    provider_path = "viewprovider" in path_lower or "viewproviders" in path_lower
    matches: list[tuple[int, str]] = []
    source_lines = source.splitlines()
    for class_node in (node for node in ast.walk(tree) if isinstance(node, ast.ClassDef)):
        class_lower = class_node.name.lower()
        provider_class = class_lower.startswith(
            ("viewprovider", "_viewprovider", "vp")
        ) or class_lower.endswith("viewprovider")
        if not (provider_class or provider_path):
            continue
        for method in class_node.body:
            if not isinstance(method, (ast.FunctionDef, ast.AsyncFunctionDef)):
                continue
            if method.name != "updateData":
                continue
            if method.lineno <= len(source_lines):
                matches.append((method.lineno, source_lines[method.lineno - 1].strip()))
    return matches


def _python_thread_join_matches(source: str) -> list[tuple[int, str]]:
    """Return structural Python ``join()`` waits on thread-like receivers.

    A bare ``.join()`` regex would classify string, path, and collection joins.
    The receiver-name/type hints below intentionally cover worker abstractions
    used by GUI-facing solver code while leaving ordinary ``str.join`` and
    ``os.path.join`` calls inert.
    """
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []
    source_lines = source.splitlines()
    receiver_names = {
        "machine",
        "process",
        "proc",
        "task",
        "thread",
        "worker",
        "future",
        "executor",
    }
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Assign, ast.AnnAssign)):
            continue
        value = node.value
        if not isinstance(value, ast.Call):
            continue
        function = value.func
        if isinstance(function, ast.Name):
            function_name = function.id
        elif isinstance(function, ast.Attribute):
            function_name = function.attr
        else:
            function_name = ""
        if not any(
            hint in function_name.lower()
            for hint in ("machine", "process", "thread", "task", "worker", "future")
        ):
            continue
        targets = node.targets if isinstance(node, ast.Assign) else [node.target]
        receiver_names.update(
            target.id.lower() for target in targets if isinstance(target, ast.Name)
        )
    matches: list[tuple[int, str]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not isinstance(node.func, ast.Attribute):
            continue
        if node.func.attr != "join" or node.args or node.keywords:
            continue
        receiver = node.func.value
        if isinstance(receiver, ast.Name):
            receiver_name = receiver.id
        elif isinstance(receiver, ast.Attribute):
            receiver_name = receiver.attr
        else:
            receiver_name = ""
        receiver_lower = receiver_name.lower()
        if receiver_lower not in receiver_names and not any(
            receiver_lower.endswith(suffix)
            for suffix in ("thread", "task", "process", "worker", "future")
        ):
            continue
        if node.lineno <= len(source_lines):
            matches.append((node.lineno, source_lines[node.lineno - 1].strip()))
    return matches


def _do_command_string_spans(source: str) -> list[tuple[int, int]]:
    """Return executable string spans passed to FreeCADGui/Gui.doCommand.

    Parsing the source rather than searching raw text prevents comments and
    inert literals from becoming pseudo-calls. AST ranges also naturally cover
    parenthesised, concatenated, and nested f-string arguments. A syntax error
    leaves the source conservatively masked; malformed Python cannot be safely
    classified as executable command text.
    """
    try:
        tree = ast.parse(source)
    except (SyntaxError, ValueError, TypeError, MemoryError):
        return []
    offsets = _line_offsets(source)
    source_lines = source.splitlines()
    spans: list[tuple[int, int]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call) or not node.args:
            continue
        function = node.func
        if not isinstance(function, ast.Attribute) or function.attr != "doCommand":
            continue
        if not isinstance(function.value, ast.Name) or function.value.id not in {
            "FreeCADGui",
            "Gui",
        }:
            continue
        spans.extend(_string_expression_spans(node.args[0], offsets, source_lines, source))
    return spans


def mask_py_non_code(source: str, expand_do_command: bool = False) -> str:
    """Replace Python comments and string literals with spaces, keeping newlines.

    Handles ``#`` line comments and single-/double-/triple-quoted string literals
    (including ``f``/``r``/``b``/``u`` prefixes, which are inert letters and left
    unmasked). A ``#`` inside a string is consumed by the string handler first, so
    it is never misread as a comment.

    When ``expand_do_command`` is true, a string literal that is a top-level
    argument of a ``doCommand(...)`` call is re-masked as *code* instead of
    blanked: its nested string literals and comments are blanked but the code
    text is left visible, because those strings are compiled and executed at
    runtime. The expansion preserves length and newline positions, so line
    numbers stay aligned with the original source.
    """
    masked = list(source)
    do_command_spans = set(_do_command_string_spans(source)) if expand_do_command else set()
    index = 0
    while index < len(source):
        char = source[index]
        if char == "#":
            end = source.find("\n", index + 1)
            end = len(source) if end < 0 else end
            _mask_range(masked, source, index, end)
            index = end
        elif char in "\"'":
            triple = source.startswith(char * 3, index)
            end = _py_string_end(source, index)
            if (index, end) in do_command_spans:
                _mask_range(masked, source, index, end)
                open_len = 3 if triple else 1
                content = source[index + open_len : end - open_len]
                re_masked = mask_py_non_code(content, expand_do_command=False)
                for offset, re_char in enumerate(re_masked):
                    masked[index + open_len + offset] = re_char
            else:
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
        return mask_py_non_code(source, expand_do_command=True)
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


def _production_init_gui_files(repository_root: Path) -> list[Path]:
    """Return top-level production workbench GUI entry points."""
    mod_root = repository_root / "src" / "Mod"
    if not mod_root.is_dir():
        return []
    return [
        path
        for path in sorted(mod_root.glob("*/InitGui.py"))
        if path.relative_to(mod_root).parts[0] not in rules.EXCLUDED_WORKBENCHES
    ]


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

    files.extend(_production_init_gui_files(repository_root))

    for extra_dir in rules.EXTRA_GUI_DIRS:
        extra_path = repository_root / extra_dir
        if extra_path.is_dir():
            files.extend(extra_path.rglob("*"))

    for extra_file in rules.EXTRA_GUI_FILES:
        extra_path = repository_root / extra_file
        if extra_path.is_file():
            files.append(extra_path)

    unique: dict[str, Path] = {}
    for path in files:
        if not path.is_file():
            continue
        if path.suffix not in rules.SOURCE_SUFFIXES:
            continue
        if _is_excluded_dir(path, repository_root):
            continue
        if _is_excluded_file(path.relative_to(repository_root).as_posix()):
            continue
        relative = path.relative_to(repository_root).as_posix()
        unique[relative] = path
    return [unique[relative] for relative in sorted(unique)]


def evidence_for(source: str, source_lines: list[str], start: int, end: int) -> str:
    """Return whitespace-normalised evidence spanning the matched line(s)."""
    start_line = source.count("\n", 0, start)
    end_line = source.count("\n", 0, max(start, end - 1))
    parts = [source_lines[line].strip() for line in range(start_line, end_line + 1)]
    return " ".join(parts)


def evidence_for_line_map(
    source_lines: list[str], line_map: list[int], start: int, end: int
) -> str:
    """Return normalized source evidence for a decoded-string match span."""
    start_index = min(start, len(line_map) - 1)
    end_index = min(max(end - 1, start), len(line_map) - 1)
    start_line = max(1, min(line_map[start_index], len(source_lines)))
    end_line = max(start_line, min(line_map[end_index], len(source_lines)))
    return " ".join(source_lines[line - 1].strip() for line in range(start_line, end_line + 1))


def scan_source(source: str, suffix: str, relative_path: str) -> list[Finding]:
    """Scan one in-memory source string and return its findings (unsorted)."""
    masked = mask_source(source, suffix)
    source_lines = source.splitlines()
    decoded_command_literals = (
        _decoded_cpp_command_literals(source) if suffix in rules.CPP_SUFFIXES else []
    )
    subsystem = subsystem_for(relative_path)
    language = _language_for(suffix)
    findings: list[Finding] = []
    provider_matches = (
        _python_update_data_provider_matches(source, relative_path) if suffix == ".py" else []
    )
    thread_join_matches = _python_thread_join_matches(source) if suffix == ".py" else []
    for category in rules.CATEGORIES:
        if category.key == "update-data-provider" and suffix == ".py":
            for line, evidence in provider_matches:
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
            continue
        if category.key == "thread-waits" and suffix == ".py":
            for line, evidence in thread_join_matches:
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
        if suffix == ".py":
            decoded_literals = _decoded_do_command_literals(source)
            decoded_compiled = compiled
        else:
            decoded_literals = decoded_command_literals
            decoded_compiled = _COMPILED[(category.key, "py")]
        if decoded_compiled is None:
            continue
        for decoded, line_map in decoded_literals:
            decoded_masked = mask_py_non_code(decoded)
            for match in decoded_compiled.finditer(decoded_masked):
                line = line_map[min(match.start(), len(line_map) - 1)]
                evidence = evidence_for_line_map(source_lines, line_map, match.start(), match.end())
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
    unique: list[Finding] = []
    seen: set[tuple[str, int, str]] = set()
    for finding in findings:
        if finding.key() in seen:
            continue
        seen.add(finding.key())
        unique.append(finding)
    return unique


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


def scope_entries(repository_root: Path) -> list[str]:
    """Return the ordered, relative GUI directories/files the scanner covers."""
    entries: list[str] = []
    gui_root = repository_root / "src" / "Gui"
    if gui_root.is_dir():
        entries.append("src/Gui")
    mod_root = repository_root / "src" / "Mod"
    if mod_root.is_dir():
        entries.extend(
            f"src/Mod/{path.relative_to(mod_root).as_posix()}"
            for path in sorted(mod_root.rglob("Gui"))
            if path.relative_to(mod_root).parts[0] not in rules.EXCLUDED_WORKBENCHES
        )
    entries.extend(
        path.relative_to(repository_root).as_posix()
        for path in _production_init_gui_files(repository_root)
    )
    entries.extend(
        extra_dir for extra_dir in rules.EXTRA_GUI_DIRS if (repository_root / extra_dir).is_dir()
    )
    entries.extend(
        extra_file
        for extra_file in rules.EXTRA_GUI_FILES
        if (repository_root / extra_file).is_file()
    )
    return entries


def build_payload(repository_root: Path) -> dict[str, object]:
    """Return the complete ordered payload that ``inventory.json`` stores."""
    findings = scan(repository_root)
    exclusions = load_exclusions()
    curated = apply_exclusions(findings, exclusions)
    return {
        "generator": "tests/architecture/gui_blocking_live_model/scanner.py",
        "scope": scope_entries(repository_root),
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
