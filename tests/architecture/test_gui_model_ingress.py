# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP04 gate for GUI model-intent ingress."""

from __future__ import annotations

from dataclasses import dataclass, replace
import io
from pathlib import Path
import re
import tokenize
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
INVENTORY_PATH = REPO_ROOT / "doc" / "document-collaboration-ingress-inventory.md"

COMMAND_HEADER_SOURCE = "src/Gui/Command.h"
COMMAND_SOURCE = "src/Gui/Command.cpp"
COMMIT_COORDINATOR_SOURCE = "src/App/DocumentCommitCoordinator.cpp"
GUI_DOCUMENT_SOURCE = "src/Gui/Document.cpp"
GUI_APPLICATION_PY_SOURCE = "src/Gui/ApplicationPy.cpp"
PROPERTY_ITEM_SOURCE = "src/Gui/propertyeditor/PropertyItem.cpp"
PYTHON_EDITOR_SOURCE = "src/Gui/PythonEditor.cpp"

INVENTORY_HEADER = (
    "domain",
    "file",
    "symbol/caller",
    "operation",
    "current owner",
    "target owner/route",
    "classification",
    "migration WP",
    "exception rationale",
)

PRIVATE_NATIVE_CONTROLS = frozenset(
    {
        "_openTransaction",
        "_commitTransaction",
        "_abortTransaction",
        "openCollaborationCommitTransaction",
        "commitCollaborationCommitTransaction",
        "rollbackCollaborationTransaction",
        "rollbackCollaborationTransactionPreservingPendingRecompute",
    }
)
PRIVATE_CONTROL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(
        sorted(PRIVATE_NATIVE_CONTROLS, key=len, reverse=True)
    ) + r")\s*\("
)

EXPECTED_TYPED_ADAPTERS = frozenset(
    {
        (
            "src/Gui/ApplicationPy.cpp",
            "ApplicationPy::{sDoCommand,sDoCommandGui,sDoCommandEval}",
            "caller-supplied Python execution bridge",
        ),
        ("src/Gui/Command.cpp", "Gui::Command::_doCommand", "command/macro bridge"),
        (
            "src/Gui/PythonEditor.cpp",
            "PythonEditor::onExecuteInConsole",
            "command/macro bridge",
        ),
        (
            "src/Tools/embedded/PySide/mainwindow.py",
            "MainWindow.on_actionCube_triggered",
            "full recompute",
        ),
        (
            "src/Tools/embedded/PySide/mainwindow3.py",
            "MainWindow.on_actionCube_triggered",
            "full recompute",
        ),
    }
)

PUBLIC_OPERATION_PATTERNS = {
    "open": re.compile(r"(?<![A-Za-z0-9_])(?:openTransaction|openCommand|setActiveTransaction)\s*\("),
    "commit": re.compile(r"(?<![A-Za-z0-9_])(?:commitTransaction|commitCommand|closeActiveTransaction)\s*\("),
    "abort": re.compile(r"(?<![A-Za-z0-9_])(?:abortTransaction|abortCommand|closeActiveTransaction)\s*\("),
    "undo": re.compile(r"(?<![A-Za-z0-9_])undo\s*\("),
    "redo": re.compile(r"(?<![A-Za-z0-9_])redo\s*\("),
}
PUBLIC_RECOMPUTE_RE = re.compile(
    r"(?<![A-Za-z0-9_])(?:recomputeFeature|recompute)\s*\("
)


@dataclass(frozen=True)
class InventoryRow:
    number: int
    domain: str
    file: str
    symbol: str
    operation: str
    current_owner: str
    target_route: str
    classification: str
    migration_wp: str
    rationale: str

    @classmethod
    def from_cells(cls, number: int, cells: Sequence[str]) -> "InventoryRow":
        return cls(number, *cells)

    @property
    def source_path(self) -> str:
        return self.file.strip("`")

    @property
    def stable_symbol(self) -> str:
        symbol = re.sub(
            r"\s+\((?:candidate )?lines?\b.*\)$", "", self.symbol
        ).strip()
        return symbol.strip("`")

    def diagnostic(self, message: str) -> str:
        return f"{self.source_path}: inventory row {self.number} {self.symbol}: {message}"


def _parse_main_inventory(text: str) -> list[InventoryRow]:
    rows: list[InventoryRow] = []
    in_main_table = False
    for number, line in enumerate(text.splitlines(), 1):
        if not line.startswith("|"):
            if in_main_table and rows:
                break
            continue
        cells = tuple(cell.strip() for cell in line.strip("|").split("|"))
        if cells == INVENTORY_HEADER:
            in_main_table = True
            continue
        if not in_main_table or len(cells) != len(INVENTORY_HEADER):
            continue
        if all(re.fullmatch(r":?-+:?", cell) for cell in cells):
            continue
        rows.append(InventoryRow.from_cells(number, cells))
    return rows


def _cc_wp04_rows(text: str | None = None) -> list[InventoryRow]:
    if text is None:
        text = INVENTORY_PATH.read_text(encoding="utf-8")
    return [row for row in _parse_main_inventory(text) if row.migration_wp == "CC-WP04"]


def _inventory_row_violations(rows: Sequence[InventoryRow], root: Path) -> list[str]:
    violations: list[str] = []
    field_names = InventoryRow.__dataclass_fields__
    for row in rows:
        for field_name in field_names:
            if field_name == "number":
                continue
            if not getattr(row, field_name).strip():
                violations.append(row.diagnostic(f"blank required field {field_name}"))
        if not (root / row.source_path).is_file():
            violations.append(row.diagnostic("named source file does not exist"))
    return violations


def _symbol_anchor_tokens(row: InventoryRow) -> tuple[str, ...]:
    symbol = row.stable_symbol
    grouped = re.search(r"\{([^{}]+)\}$", symbol)
    if grouped:
        tokens: list[str] = []
        for member in grouped.group(1).split(","):
            identifiers = re.findall(r"[A-Za-z_]\w*", member)
            if identifiers:
                tokens.append(identifiers[-1])
        return tuple(tokens)

    tail = re.split(r"::|\.", symbol)[-1]
    identifiers = re.findall(r"[A-Za-z_]\w*", tail)
    return (identifiers[-1],) if identifiers else ()


def _symbol_anchor_violations(
    rows: Sequence[InventoryRow], sources: Mapping[str, str]
) -> list[str]:
    violations: list[str] = []
    suppressed: dict[str, str] = {}
    for row in rows:
        source = sources.get(row.source_path)
        if source is None:
            continue
        searchable = suppressed.setdefault(
            row.source_path, _suppress_non_code(row.source_path, source)
        )
        tokens = _symbol_anchor_tokens(row)
        if not tokens:
            violations.append(row.diagnostic("cannot derive a symbol anchor token"))
            continue
        for token in tokens:
            pattern = re.compile(
                r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])"
            )
            if not pattern.search(searchable):
                violations.append(row.diagnostic(f"missing symbol anchor token {token!r}"))
    return violations


def _typed_adapter_key(row: InventoryRow) -> tuple[str, str, str]:
    return (row.source_path, row.stable_symbol, row.operation)


def _typed_adapter_violations(rows: Sequence[InventoryRow]) -> list[str]:
    typed = [row for row in rows if row.classification == "typed adapter required"]
    actual = {_typed_adapter_key(row) for row in typed}
    violations: list[str] = []
    for row in typed:
        key = _typed_adapter_key(row)
        if key not in EXPECTED_TYPED_ADAPTERS:
            violations.append(row.diagnostic(f"unexpected sixth typed-adapter row {key!r}"))
    for missing in sorted(EXPECTED_TYPED_ADAPTERS - actual):
        violations.append(f"<inventory>: row missing {missing[1]}: missing typed-adapter row {missing!r}")
    if len(typed) != len(EXPECTED_TYPED_ADAPTERS):
        violations.append(
            f"<inventory>: row count typed-adapter: expected 5, found {len(typed)}"
        )
    return violations


def _read_source(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="surrogateescape")


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


def _suppress_cpp_non_code(source: str, *, keep_literals: bool = False) -> str:
    result = list(source)
    index = 0
    while index < len(source):
        raw_end = _raw_literal_end(source, index)
        if raw_end is not None:
            if not keep_literals:
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
            if not keep_literals:
                _blank_non_newlines(result, index, end)
            index = end
            continue
        index += 1
    return "".join(result)


def _python_token_suppressed(source: str, *, keep_literals: bool) -> str:
    result = list(source)
    line_offsets: list[int] = []
    offset = 0
    for line in source.splitlines(keepends=True):
        line_offsets.append(offset)
        offset += len(line)
    if not line_offsets:
        line_offsets.append(0)
    tokens = tokenize.generate_tokens(io.StringIO(source).readline)
    for token in tokens:
        if token.type != tokenize.COMMENT and (keep_literals or token.type != tokenize.STRING):
            continue
        start = line_offsets[token.start[0] - 1] + token.start[1]
        end = line_offsets[token.end[0] - 1] + token.end[1]
        _blank_non_newlines(result, start, end)
    return "".join(result)


def _suppress_non_code(path: str, source: str, *, keep_literals: bool = False) -> str:
    if Path(path).suffix.lower() == ".py":
        return _python_token_suppressed(source, keep_literals=keep_literals)
    return _suppress_cpp_non_code(source, keep_literals=keep_literals)


def _matching_delimiter(source: str, opening: int, left: str, right: str) -> int | None:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == left:
            depth += 1
        elif source[index] == right:
            depth -= 1
            if depth == 0:
                return index
    return None


def _function_bodies(source: str, qualified_name: str) -> list[tuple[str, str]]:
    stripped = _suppress_cpp_non_code(source)
    pattern = re.compile(r"(?<![A-Za-z0-9_:])" + re.escape(qualified_name) + r"\s*\(")
    bodies: list[tuple[str, str]] = []
    for match in pattern.finditer(stripped):
        opening_parenthesis = stripped.find("(", match.start())
        closing_parenthesis = _matching_delimiter(stripped, opening_parenthesis, "(", ")")
        if closing_parenthesis is None:
            continue
        semicolon = stripped.find(";", closing_parenthesis + 1)
        opening_brace = stripped.find("{", closing_parenthesis + 1)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue
        closing_brace = _matching_delimiter(stripped, opening_brace, "{", "}")
        if closing_brace is not None:
            bodies.append(
                (
                    stripped[match.start():opening_brace],
                    stripped[opening_brace + 1:closing_brace],
                )
            )
    return bodies


def _body_for(
    source: str,
    qualified_name: str,
    *,
    signature_contains: str | None = None,
) -> str:
    candidates = _function_bodies(source, qualified_name)
    if signature_contains is not None:
        fragment = re.sub(r"\s+", "", signature_contains)
        candidates = [
            item for item in candidates if fragment in re.sub(r"\s+", "", item[0])
        ]
    assert len(candidates) == 1, (
        f"expected one body for {qualified_name}"
        + (f" containing {signature_contains}" if signature_contains else "")
        + f", found {len(candidates)}"
    )
    return candidates[0][1]


def _assert_body_calls(body: str, expression: str, owner: str) -> None:
    actual = re.sub(r"\s+", "", body)
    expected = re.sub(r"\s+", "", expression)
    assert expected in actual, f"{owner}: expected call {expression}"


def _private_bypass_violations(
    sources: Mapping[str, str],
    rows: Sequence[InventoryRow],
) -> list[str]:
    rows_by_file: dict[str, list[InventoryRow]] = {}
    for row in rows:
        rows_by_file.setdefault(row.source_path, []).append(row)
    violations: list[str] = []
    for path, source in sorted(sources.items()):
        stripped = _suppress_non_code(path, source)
        for match in PRIVATE_CONTROL_RE.finditer(stripped):
            line = stripped.count("\n", 0, match.start(1)) + 1
            owners = rows_by_file.get(path, [])
            if owners:
                inventory = ", ".join(
                    f"row {row.number} {row.symbol}" for row in owners
                )
            else:
                inventory = "row <unclassified> symbol <unknown>"
            violations.append(
                f"{path}:{line}: {match.group(1)} private GUI bypass; inventory {inventory}"
            )
    return violations


def _sources_for_rows(rows: Sequence[InventoryRow]) -> dict[str, str]:
    return {
        path: _read_source(REPO_ROOT / path)
        for path in sorted({row.source_path for row in rows})
    }


def _operation_route_violations(
    rows: Sequence[InventoryRow], sources: Mapping[str, str]
) -> list[str]:
    violations: list[str] = []
    for row in rows:
        searchable = _suppress_non_code(
            row.source_path, sources[row.source_path], keep_literals=True
        )
        operation = row.operation.lower()
        for verb, pattern in PUBLIC_OPERATION_PATTERNS.items():
            relevant = f"{verb} transaction" in operation or f"{verb}/history" in operation
            if relevant and not pattern.search(searchable):
                violations.append(row.diagnostic(f"no public {verb} facade token in named source"))
        if "recompute" in operation and not PUBLIC_RECOMPUTE_RE.search(searchable):
            violations.append(
                row.diagnostic(
                    "recompute ingress is not a public recompute/recomputeFeature call; "
                    "CC-WP12 execution migration is not credited here"
                )
            )
    return violations


def test_cc_wp04_inventory_shape_and_files() -> None:
    rows = _cc_wp04_rows()
    assert len(rows) == 364
    assert len({row.source_path for row in rows}) == 107
    classifications = {
        name: sum(row.classification == name for row in rows)
        for name in {row.classification for row in rows}
    }
    assert classifications == {"migrate": 359, "typed adapter required": 5}
    violations = _inventory_row_violations(rows, REPO_ROOT)
    existing_sources = {
        path: _read_source(REPO_ROOT / path)
        for path in sorted({row.source_path for row in rows})
        if (REPO_ROOT / path).is_file()
    }
    violations.extend(_symbol_anchor_violations(rows, existing_sources))
    assert not violations, "CC-WP04 inventory violations:\n" + "\n".join(violations)


def test_cc_wp04_has_exact_five_typed_adapters() -> None:
    violations = _typed_adapter_violations(_cc_wp04_rows())
    assert not violations, "typed-adapter inventory violations:\n" + "\n".join(violations)


def test_cc_wp04_sources_have_no_private_app_transaction_control() -> None:
    rows = _cc_wp04_rows()
    violations = _private_bypass_violations(_sources_for_rows(rows), rows)
    assert not violations, "private GUI transaction bypasses:\n" + "\n".join(violations)


def test_central_gui_transaction_routes_use_public_app_facades() -> None:
    command = _read_source(REPO_ROOT / COMMAND_SOURCE)
    gui_document = _read_source(REPO_ROOT / GUI_DOCUMENT_SOURCE)

    _assert_body_calls(
        _body_for(
            command,
            "Command::openActiveDocumentCommand",
            signature_contains="App::TransactionName",
        ),
        "getDocument()->setActiveTransaction(",
        "Gui::Command::openActiveDocumentCommand",
    )
    for method in ("commit", "abort"):
        owner = f"Command::{method}Command(int)"
        body = _body_for(command, f"Command::{method}Command", signature_contains="int tid")
        _assert_body_calls(body, f"App::GetApplication().{method}Transaction(", owner)

    gui_routes = (
        ("openCommand", "openTransaction("),
        ("commitCommand", "commitTransaction("),
        ("abortCommand", "abortTransaction("),
        ("undo", "undo("),
        ("redo", "redo("),
    )
    for method, public_call in gui_routes:
        owner = f"Gui::Document::{method}"
        _assert_body_calls(
            _body_for(gui_document, f"Document::{method}"),
            f"getDocument()->{public_call}",
            owner,
        )


def test_property_item_interpreter_is_confined_to_compatibility_callback() -> None:
    source = _read_source(REPO_ROOT / PROPERTY_ITEM_SOURCE)
    body = _body_for(
        source, "PropertyItem::setPropertyValue", signature_contains="const std::string&"
    )
    stripped = _suppress_cpp_non_code(body)
    call_name = "executeCompatibilityMutation"
    spans: list[tuple[int, int]] = []
    for match in re.finditer(r"(?<![A-Za-z0-9_])" + call_name + r"\s*\(", stripped):
        opening = stripped.find("(", match.start())
        closing = _matching_delimiter(stripped, opening, "(", ")")
        if closing is not None:
            spans.append((match.start(), closing + 1))
    assert spans, "PropertyItem::setPropertyValue: no executeCompatibilityMutation callback"

    interpreter_calls = list(
        re.finditer(r"Base::Interpreter\s*\(\s*\)\s*\.\s*runString\s*\(", stripped)
    )
    assert interpreter_calls, "PropertyItem::setPropertyValue: missing callback interpreter"
    outside = [
        call.start()
        for call in interpreter_calls
        if not any(start <= call.start() < end for start, end in spans)
    ]
    assert not outside, (
        "PropertyItem::setPropertyValue: direct interpreter execution outside its "
        f"executeCompatibilityMutation callback at body offsets {outside}"
    )


def test_gui_python_execution_converges_on_command_run_bridge() -> None:
    application_py = _read_source(REPO_ROOT / GUI_APPLICATION_PY_SOURCE)
    command_header = _read_source(REPO_ROOT / COMMAND_HEADER_SOURCE)
    command = _read_source(REPO_ROOT / COMMAND_SOURCE)
    python_editor = _read_source(REPO_ROOT / PYTHON_EDITOR_SOURCE)

    stripped_header = _suppress_cpp_non_code(command_header)
    enum_match = re.search(
        r"enum\s+class\s+PythonCommandMode\s*\{(?P<body>[^{}]*)\}",
        stripped_header,
    )
    assert enum_match, "Gui::Command: missing typed PythonCommandMode enum"
    enum_members = {
        match.group(0)
        for match in re.finditer(r"[A-Za-z_]\w*", enum_match.group("body"))
    }
    assert enum_members == {"File", "Eval"}, (
        "Gui::Command::PythonCommandMode must contain exactly File and Eval; "
        f"found {sorted(enum_members)}"
    )
    compact_header = re.sub(r"\s+", "", stripped_header)
    declaration = "runPythonCommand(constchar*sCmd,PythonCommandModemode);"
    assert declaration in compact_header, "Gui::Command: missing typed runPythonCommand declaration"
    assert compact_header.count("runPythonCommand(") == 1, (
        "Gui::Command: runPythonCommand must have one public declaration"
    )

    application_routes = (
        ("sDoCommand", "App", "File"),
        ("sDoCommandGui", "Gui", "File"),
        ("sDoCommandEval", None, "Eval"),
    )
    for method, macro_type, mode in application_routes:
        owner = f"ApplicationPy::{method}"
        body = _body_for(application_py, owner)
        _assert_body_calls(
            body,
            "Gui::Command::runPythonCommand("
            f"sCmd, Gui::Command::PythonCommandMode::{mode})",
            owner,
        )
        assert body.count("Gui::Command::runPythonCommand(") == 1, (
            f"{owner}: must make exactly one typed bridge call"
        )
        if macro_type is None:
            assert "macroManager" not in body and "MacroManager::" not in body, (
                f"{owner}: Eval mode must not record a macro line"
            )
        else:
            _assert_body_calls(
                body,
                f"macroManager()->addLine(MacroManager::{macro_type}, sCmd)",
                f"{owner} macro recording",
            )
        assert "PyRun_String" not in body, f"{owner}: retains a direct interpreter bypass"
        assert "Base::Interpreter" not in body, f"{owner}: retains an interpreter bypass"

    _assert_body_calls(
        _body_for(command, "Command::_doCommand"),
        "_runCommand(",
        "Gui::Command::_doCommand",
    )
    run_command_body = _body_for(
        command, "Command::_runCommand", signature_contains="const char* sCmd"
    )
    _assert_body_calls(
        run_command_body,
        "runPythonCommand(sCmd, PythonCommandMode::File)",
        "Gui::Command::_runCommand typed bridge",
    )
    _assert_body_calls(
        run_command_body,
        "macroManager()->addLine(MacroManager::Gui, sCmd)",
        "Gui::Command::_runCommand Gui macro recording",
    )
    _assert_body_calls(
        run_command_body,
        "macroManager()->addLine(MacroManager::App, sCmd)",
        "Gui::Command::_runCommand App macro recording",
    )
    assert "PyRun_String" not in run_command_body
    assert "Base::Interpreter" not in run_command_body
    _assert_body_calls(
        _body_for(command, "Command::_runCommand", signature_contains="const QByteArray&"),
        "_runCommand(file, line, eType, sCmd.constData())",
        "Gui::Command::_runCommand(QByteArray)",
    )

    interpreter_body = _body_for(command, "Command::runPythonCommand")
    _assert_body_calls(
        interpreter_body,
        "mode == PythonCommandMode::Eval ? Py_eval_input : Py_file_input",
        "Gui::Command::runPythonCommand input mode",
    )
    _assert_body_calls(
        interpreter_body,
        "return PyRun_String(sCmd, inputMode, dict, dict)",
        "Gui::Command::runPythonCommand interpreter terminal",
    )
    assert interpreter_body.count("PyRun_String(") == 1
    assert "Base::Interpreter" not in interpreter_body
    assert "MacroManager" not in interpreter_body and "macroManager" not in interpreter_body
    assert "DoCmd_Type" not in interpreter_body
    assert not re.search(r"(?<![A-Za-z0-9_])(?:App|Gui)(?![A-Za-z0-9_])", interpreter_body)

    central_pyrun_counts = {
        COMMAND_SOURCE: _suppress_cpp_non_code(command).count("PyRun_String("),
        GUI_APPLICATION_PY_SOURCE: _suppress_cpp_non_code(application_py).count(
            "PyRun_String("
        ),
        PYTHON_EDITOR_SOURCE: _suppress_cpp_non_code(python_editor).count("PyRun_String("),
    }
    assert central_pyrun_counts == {
        COMMAND_SOURCE: 1,
        GUI_APPLICATION_PY_SOURCE: 0,
        PYTHON_EDITOR_SOURCE: 0,
    }, f"PyRun_String must be owned only by Command::runPythonCommand: {central_pyrun_counts}"

    editor_body = _body_for(python_editor, "PythonEditor::onExecuteInConsole")
    _assert_body_calls(
        editor_body,
        "Gui::Command::doCommand(",
        "PythonEditor::onExecuteInConsole",
    )
    assert "PyRun_String" not in editor_body
    assert "Base::Interpreter" not in editor_body


def test_long_lived_gui_transaction_is_rejected_as_busy_without_qt_bootstrap() -> None:
    command = _read_source(REPO_ROOT / COMMAND_SOURCE)
    coordinator = _read_source(REPO_ROOT / COMMIT_COORDINATOR_SOURCE)

    _assert_body_calls(
        _body_for(
            command,
            "Command::openActiveDocumentCommand",
            signature_contains="App::TransactionName",
        ),
        "getDocument()->setActiveTransaction(",
        "Gui::Command::openActiveDocumentCommand long-lived transaction",
    )

    coordinator_without_comments = _suppress_cpp_non_code(coordinator, keep_literals=True)
    compact = re.sub(r"\s+", "", coordinator_without_comments)
    busy_admission = (
        "if(_document.hasPendingTransaction()||_document.transacting()||"
        "_document.getBookedTransactionID()!=0||_document.isTransactionLocked()){"
        "returnmakeResult(DocumentCommitStatus::Busy,edit,"
        '"documentalreadyhasanativetransactioninprogress");}'
    )
    assert busy_admission in compact, (
        "DocumentCommitCoordinator admission must couple pending/transacting/booked/locked "
        "native transaction state to DocumentCommitStatus::Busy and the native-transaction-"
        "in-progress diagnostic"
    )


def test_inventory_transaction_and_recompute_rows_use_public_facades() -> None:
    """Check public ingress only; detached recompute execution remains CC-WP12 scope."""

    rows = _cc_wp04_rows()
    sources = _sources_for_rows(rows)
    violations = _operation_route_violations(rows, sources)
    assert not violations, "public GUI ingress violations:\n" + "\n".join(violations)


def test_private_scanner_rejects_injected_gui_bypass_with_inventory_context() -> None:
    row = InventoryRow(
        77,
        "GUI injected",
        "`src/Gui/Injected.cpp`",
        "`Injected::bypass`",
        "commit transaction",
        "GUI caller",
        "Coordinator GUI route",
        "migrate",
        "CC-WP04",
        "negative fixture",
    )
    violations = _private_bypass_violations(
        {row.source_path: "void Injected::bypass(App::Document& doc) { doc._commitTransaction(); }"},
        [row],
    )
    assert len(violations) == 1
    assert "src/Gui/Injected.cpp:1: _commitTransaction" in violations[0]
    assert "row 77 `Injected::bypass`" in violations[0]


def test_private_scanner_ignores_comments_strings_chars_and_raw_strings() -> None:
    inert = r'''
        // doc._openTransaction("comment");
        /* doc._commitTransaction(); */
        const char* text = "doc._abortTransaction()";
        const char marker = '(';
        const char* raw = R"gate(doc.openCollaborationCommitTransaction())gate";
    '''
    assert not _private_bypass_violations({"src/Gui/Inert.cpp": inert}, [])


def test_typed_adapter_validator_rejects_a_sixth_row() -> None:
    rows = _cc_wp04_rows()
    migrate = next(row for row in rows if row.classification == "migrate")
    sixth = replace(
        migrate,
        number=9999,
        symbol="`Injected::sixthTypedAdapter`",
        classification="typed adapter required",
    )
    violations = _typed_adapter_violations([*rows, sixth])
    assert any("row 9999" in item and "Injected::sixthTypedAdapter" in item for item in violations)


def test_inventory_validator_rejects_missing_source_file() -> None:
    row = InventoryRow(
        1234,
        "GUI injected",
        "`src/Gui/DoesNotExist.cpp`",
        "`Injected::missing`",
        "open transaction",
        "GUI caller",
        "Coordinator GUI route",
        "migrate",
        "CC-WP04",
        "negative fixture",
    )
    violations = _inventory_row_violations([row], REPO_ROOT)
    assert violations == [row.diagnostic("named source file does not exist")]


def test_symbol_anchor_validator_rejects_absent_symbol_in_existing_file(
    tmp_path: Path,
) -> None:
    source_path = tmp_path / "src" / "Gui" / "Present.cpp"
    source_path.parent.mkdir(parents=True)
    source_path.write_text("void Present::otherSymbol() {}\n", encoding="utf-8")
    row = InventoryRow(
        4321,
        "GUI injected",
        "`src/Gui/Present.cpp`",
        "`Present::missingSymbol` (candidate line 1)",
        "open transaction",
        "GUI caller",
        "Coordinator GUI route",
        "migrate",
        "CC-WP04",
        "negative fixture",
    )

    assert not _inventory_row_violations([row], tmp_path)
    violations = _symbol_anchor_violations(
        [row], {row.source_path: _read_source(source_path)}
    )
    assert violations == [row.diagnostic("missing symbol anchor token 'missingSymbol'")]
