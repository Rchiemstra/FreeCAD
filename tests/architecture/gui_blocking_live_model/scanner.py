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
            for decoded, line_map in _decoded_do_command_literals(source):
                decoded_masked = mask_py_non_code(decoded)
                for match in compiled.finditer(decoded_masked):
                    line = line_map[min(match.start(), len(line_map) - 1)]
                    evidence = evidence_for_line_map(
                        source_lines, line_map, match.start(), match.end()
                    )
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
