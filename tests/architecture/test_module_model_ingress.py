# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP05 gate for workbench/module model ingress."""

from __future__ import annotations

from dataclasses import dataclass
import io
from pathlib import Path
import re
import tokenize
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
INVENTORY_PATH = REPO_ROOT / "doc" / "document-collaboration-ingress-inventory.md"

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
EXPECTED_ROW_COUNT = 908
EXPECTED_PATH_COUNT = 372
CANDIDATE_DRIFT_LINES = 50

PRIVATE_CALL_NAMES = frozenset(
    {
        "_openTransaction",
        "_commitTransaction",
        "_abortTransaction",
        "_clearUndos",
        "_clearRedos",
        "openCollaborationCommitTransaction",
        "commitCollaborationCommitTransaction",
        "rollbackCollaborationTransaction",
        "rollbackCollaborationTransactionPreservingPendingRecompute",
        "openMutationTransaction",
        "openNativeCommitTransaction",
        "commitNativeCommitTransaction",
        "rollbackNativeCommitTransaction",
        "setActiveCompatibilityTransaction",
        "commitCompatibilityTransaction",
        "abortCompatibilityTransaction",
        "undoCompatibilityTransaction",
        "redoCompatibilityTransaction",
        "clearCompatibilityTransactionHistory",
        "setActiveCompatibilityTransactionImpl",
        "commitCompatibilityTransactionImpl",
        "abortCompatibilityTransactionImpl",
        "undoCompatibilityTransactionImpl",
        "redoCompatibilityTransactionImpl",
        "clearCompatibilityTransactionHistoryImpl",
    }
)
PRIVATE_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(
        sorted(PRIVATE_CALL_NAMES, key=len, reverse=True)
    ) + r")\s*\("
)
DIRECT_COORDINATOR_RE = re.compile(
    r"(?P<token>\bDocumentCommitCoordinator\b|"
    r"(?<![A-Za-z0-9_])collaborationService\s*\(|"
    r"(?<![A-Za-z0-9_])_coordinator\s*[.\-])"
)

UNTYPED_GLOBAL_PATTERNS = (
    (
        "openGlobalTransaction",
        re.compile(r"(?<![A-Za-z0-9_])openGlobalTransaction\s*\("),
    ),
)

PUBLIC_OPERATION_PATTERNS = {
    "open transaction": re.compile(
        r"(?<![A-Za-z0-9_])(?:openTransaction|openCommand)\s*\("
    ),
    "commit transaction": re.compile(
        r"(?<![A-Za-z0-9_])(?:commitTransaction|commitCommand)\s*\("
    ),
    "abort transaction": re.compile(
        r"(?<![A-Za-z0-9_])(?:abortTransaction|abortCommand)\s*\("
    ),
    "active transaction control": re.compile(
        r"(?<![A-Za-z0-9_])setActiveTransaction\s*\("
    ),
    "undo/history control": re.compile(
        r"(?<![A-Za-z0-9_])(?:undo|clearUndo|clearUndos)\s*\("
    ),
    "redo/history control": re.compile(r"(?<![A-Za-z0-9_])redo\s*\("),
    "full recompute": re.compile(r"(?<![A-Za-z0-9_])recompute\s*\("),
    "feature recompute": re.compile(
        r"(?<![A-Za-z0-9_])(?:recomputeFeature|recompute)\s*\("
    ),
    "command/macro bridge": re.compile(
        r"(?<![A-Za-z0-9_])(?:doCommand|doCommandT|runCommand|runPythonCommand|"
        r"FCMD_OBJ_CMD|FCMD_DOC_CMD)\s*\("
    ),
}


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
        symbol = symbol.strip("`")
        return re.sub(r"\s+module initialization$", "", symbol)

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


def _cc_wp05_rows(text: str | None = None) -> list[InventoryRow]:
    if text is None:
        text = INVENTORY_PATH.read_text(encoding="utf-8")
    return [row for row in _parse_main_inventory(text) if row.migration_wp == "CC-WP05"]


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
    offsets: list[int] = []
    offset = 0
    for line in source.splitlines(keepends=True):
        offsets.append(offset)
        offset += len(line)
    if not offsets:
        offsets.append(0)
    for token in tokenize.generate_tokens(io.StringIO(source).readline):
        if token.type != tokenize.COMMENT and (keep_literals or token.type != tokenize.STRING):
            continue
        start = offsets[token.start[0] - 1] + token.start[1]
        end = offsets[token.end[0] - 1] + token.end[1]
        _blank_non_newlines(result, start, end)
    return "".join(result)


def _suppress_non_code(path: str, source: str, *, keep_literals: bool = False) -> str:
    if Path(path).suffix.lower() in {".py", ".fcmacro"}:
        return _python_token_suppressed(source, keep_literals=keep_literals)
    return _suppress_cpp_non_code(source, keep_literals=keep_literals)


def _symbol_anchor_tokens(row: InventoryRow) -> tuple[str, ...]:
    grouped = re.search(r"\{([^{}]+)\}$", row.stable_symbol)
    if grouped:
        tokens: list[str] = []
        for member in grouped.group(1).split(","):
            identifiers = re.findall(r"[A-Za-z_]\w*", member)
            if identifiers:
                tokens.append(identifiers[-1])
        return tuple(tokens)
    tail = re.split(r"::|\.", row.stable_symbol)[-1]
    identifiers = re.findall(r"[A-Za-z_]\w*", tail)
    return (identifiers[0],) if identifiers else ()


def _candidate_ranges(row: InventoryRow) -> tuple[tuple[int, int], ...]:
    annotation = re.search(
        r"\((?:candidate )?lines?\s+(?P<locations>[^)]+)\)$", row.symbol
    )
    if not annotation:
        return ()
    ranges: list[tuple[int, int]] = []
    for match in re.finditer(r"(?<!\d)(\d+)(?:\s*-\s*(\d+))?", annotation.group("locations")):
        start = int(match.group(1))
        end = int(match.group(2) or match.group(1))
        ranges.append((start, end))
    return tuple(ranges)


def _inventory_shape_violations(rows: Sequence[InventoryRow], root: Path) -> list[str]:
    violations: list[str] = []
    for row in rows:
        for field_name in InventoryRow.__dataclass_fields__:
            if field_name == "number":
                continue
            if not getattr(row, field_name).strip():
                violations.append(row.diagnostic(f"blank required field {field_name}"))
        if row.classification != "migrate":
            violations.append(row.diagnostic(f"classification is {row.classification!r}, not 'migrate'"))
        if row.target_route != "Coordinator workbench route":
            violations.append(row.diagnostic(f"unexpected target route {row.target_route!r}"))
        if not (root / row.source_path).is_file():
            violations.append(row.diagnostic("named source file does not exist"))
    return violations


def _source_anchor_violations(
    rows: Sequence[InventoryRow], sources: Mapping[str, str]
) -> list[str]:
    violations: list[str] = []
    code_cache: dict[str, str] = {}
    public_cache: dict[str, str] = {}
    for row in rows:
        source = sources.get(row.source_path)
        if source is None:
            continue
        code = code_cache.setdefault(
            row.source_path, _suppress_non_code(row.source_path, source)
        )
        public_source = public_cache.setdefault(
            row.source_path,
            _suppress_non_code(row.source_path, source, keep_literals=True),
        )

        tokens = _symbol_anchor_tokens(row)
        if not tokens:
            violations.append(row.diagnostic("cannot derive a symbol/caller anchor token"))
        for token in tokens:
            if not re.search(
                r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])", code
            ):
                violations.append(row.diagnostic(f"missing symbol/caller token {token!r}"))

        ranges = _candidate_ranges(row)
        if not ranges:
            violations.append(row.diagnostic("missing parseable candidate location"))
            continue
        lines = public_source.splitlines()
        valid_ranges = [
            (start, end)
            for start, end in ranges
            if start > 0
            and end >= start
            and start <= len(lines) + CANDIDATE_DRIFT_LINES
            and end <= len(lines) + CANDIDATE_DRIFT_LINES
        ]
        if not valid_ranges:
            violations.append(
                row.diagnostic(
                    f"candidate locations {ranges!r} are outside a {len(lines)}-line source"
                )
            )
            continue
        neighborhoods = "\n".join(
            "\n".join(
                lines[
                    max(0, start - 1 - CANDIDATE_DRIFT_LINES):
                    min(len(lines), end + CANDIDATE_DRIFT_LINES)
                ]
            )
            for start, end in valid_ranges
        )
        for operation, pattern in PUBLIC_OPERATION_PATTERNS.items():
            if operation in row.operation.lower() and not pattern.search(neighborhoods):
                suffix = (
                    "; detached recompute execution remains CC-WP12 scope"
                    if "recompute" in operation
                    else ""
                )
                violations.append(
                    row.diagnostic(
                        f"candidate neighborhood lacks recognizable public {operation} API{suffix}"
                    )
                )
    return violations


def _rows_by_source(rows: Sequence[InventoryRow]) -> dict[str, list[InventoryRow]]:
    result: dict[str, list[InventoryRow]] = {}
    for row in rows:
        result.setdefault(row.source_path, []).append(row)
    return result


def _source_context(path: str, rows_by_source: Mapping[str, Sequence[InventoryRow]]) -> str:
    rows = rows_by_source.get(path, ())
    if not rows:
        return "row <unclassified> symbol <unknown>"
    return ", ".join(f"row {row.number} {row.symbol}" for row in rows)


def _private_ingress_violations(
    sources: Mapping[str, str], rows: Sequence[InventoryRow]
) -> list[str]:
    violations: list[str] = []
    by_source = _rows_by_source(rows)
    for path, source in sorted(sources.items()):
        code = _suppress_non_code(path, source)
        for match in PRIVATE_CALL_RE.finditer(code):
            line = code.count("\n", 0, match.start(1)) + 1
            violations.append(
                f"{path}:{line}: private control {match.group(1)}; inventory "
                f"{_source_context(path, by_source)}"
            )
        for match in DIRECT_COORDINATOR_RE.finditer(code):
            line = code.count("\n", 0, match.start("token")) + 1
            violations.append(
                f"{path}:{line}: direct coordinator internal {match.group('token')!r}; inventory "
                f"{_source_context(path, by_source)}"
            )
    return violations


def _untyped_global_violations(
    sources: Mapping[str, str], rows: Sequence[InventoryRow]
) -> list[str]:
    violations: list[str] = []
    by_source = _rows_by_source(rows)
    for path, source in sorted(sources.items()):
        code = _suppress_non_code(path, source)
        for label, pattern in UNTYPED_GLOBAL_PATTERNS:
            for match in pattern.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                violations.append(
                    f"{path}:{line}: untyped cross-document/global transaction primitive "
                    f"{label}; inventory {_source_context(path, by_source)}"
                )
    return violations


def _sources_for_rows(rows: Sequence[InventoryRow], root: Path = REPO_ROOT) -> dict[str, str]:
    return {
        path: _read_source(root / path)
        for path in sorted({row.source_path for row in rows})
        if (root / path).is_file()
    }


def test_cc_wp05_inventory_shape_paths_and_anchors() -> None:
    rows = _cc_wp05_rows()
    assert len(rows) == EXPECTED_ROW_COUNT, (
        f"CC-WP05 inventory rows: expected {EXPECTED_ROW_COUNT}, found {len(rows)}"
    )
    paths = {row.source_path for row in rows}
    assert len(paths) == EXPECTED_PATH_COUNT, (
        f"CC-WP05 inventory paths: expected {EXPECTED_PATH_COUNT}, found {len(paths)}"
    )
    violations = _inventory_shape_violations(rows, REPO_ROOT)
    sources = _sources_for_rows(rows)
    violations.extend(_source_anchor_violations(rows, sources))
    assert not violations, "CC-WP05 inventory/source anchor violations:\n" + "\n".join(violations)


def test_cc_wp05_module_sources_use_public_ingress_only() -> None:
    """Validate facade ingress only; this does not credit CC-WP12 recompute execution."""

    rows = _cc_wp05_rows()
    sources = _sources_for_rows(rows)
    violations = _private_ingress_violations(sources, rows)
    violations.extend(_untyped_global_violations(sources, rows))
    assert not violations, "CC-WP05 module ingress violations:\n" + "\n".join(violations)


def test_missing_source_is_rejected_with_row_and_symbol(tmp_path: Path) -> None:
    row = InventoryRow(
        7001,
        "Module injected",
        "`src/Mod/Absent.py`",
        "`Absent.run` (candidate line 1)",
        "full recompute",
        "module caller",
        "Coordinator workbench route",
        "migrate",
        "CC-WP05",
        "negative fixture",
    )
    violations = _inventory_shape_violations([row], tmp_path)
    assert violations == [row.diagnostic("named source file does not exist")]


def test_missing_symbol_is_rejected_in_existing_source(tmp_path: Path) -> None:
    source_path = tmp_path / "src" / "Mod" / "Present.py"
    source_path.parent.mkdir(parents=True)
    source_path.write_text("def other():\n    App.ActiveDocument.recompute()\n", encoding="utf-8")
    row = InventoryRow(
        7002,
        "Module injected",
        "`src/Mod/Present.py`",
        "`Present.missing` (candidate line 2)",
        "full recompute",
        "module caller",
        "Coordinator workbench route",
        "migrate",
        "CC-WP05",
        "negative fixture",
    )
    assert not _inventory_shape_violations([row], tmp_path)
    violations = _source_anchor_violations(
        [row], {row.source_path: _read_source(source_path)}
    )
    assert violations == [row.diagnostic("missing symbol/caller token 'missing'")]


def test_surrogateescape_source_still_detects_private_ascii_bypass(tmp_path: Path) -> None:
    source_path = tmp_path / "NonUtf8.cpp"
    source_path.write_bytes(
        b"// vendored byte: \xff\nvoid bypass(App::Document& doc) { doc._commitTransaction(); }\n"
    )
    source = _read_source(source_path)
    assert "\udcff" in source
    row = InventoryRow(
        7003,
        "Module injected",
        "`src/Mod/NonUtf8.cpp`",
        "`bypass` (candidate line 2)",
        "commit transaction",
        "module caller",
        "Coordinator workbench route",
        "migrate",
        "CC-WP05",
        "negative fixture",
    )
    violations = _private_ingress_violations({row.source_path: source}, [row])
    assert len(violations) == 1
    assert "src/Mod/NonUtf8.cpp:2: private control _commitTransaction" in violations[0]
    assert "row 7003 `bypass`" in violations[0]


def test_injected_private_coordinator_bypass_is_rejected() -> None:
    row = InventoryRow(
        7004,
        "Module injected",
        "`src/Mod/Bypass.cpp`",
        "`Bypass::run` (candidate line 1)",
        "commit transaction",
        "module caller",
        "Coordinator workbench route",
        "migrate",
        "CC-WP05",
        "negative fixture",
    )
    source = "void Bypass::run(App::Document& doc) { doc.collaborationService().commitCompatibilityTransaction(); }"
    violations = _private_ingress_violations({row.source_path: source}, [row])
    assert any("private control commitCompatibilityTransaction" in item for item in violations)
    assert any("direct coordinator internal 'collaborationService('" in item for item in violations)
    assert all("row 7004 `Bypass::run`" in item for item in violations)


def test_injected_untyped_global_transaction_bypass_is_rejected() -> None:
    row = InventoryRow(
        7005,
        "Module injected",
        "`src/Mod/GlobalBypass.cpp`",
        "`GlobalBypass::run` (candidate line 1)",
        "open transaction",
        "module caller",
        "Coordinator workbench route",
        "migrate",
        "CC-WP05",
        "negative fixture",
    )
    source = 'void GlobalBypass::run() { App::GetApplication().openGlobalTransaction("bad"); }'
    violations = _untyped_global_violations({row.source_path: source}, [row])
    assert len(violations) == 1
    assert "openGlobalTransaction" in violations[0]
    assert "row 7005 `GlobalBypass::run`" in violations[0]


def test_public_application_compatibility_facades_are_accepted() -> None:
    cpp_source = """
        void publicCompatibilityFacades(int id) {
            App::GetApplication().setActiveTransaction(App::TransactionName {"public"});
            App::GetApplication().commitTransaction(id);
            App::GetApplication().abortTransaction(id);
            App::GetApplication().closeActiveTransaction(App::TransactionCloseMode::Commit, id);
        }
    """
    python_source = """
App.setActiveTransaction("public")
App.closeActiveTransaction()
"""
    violations = _untyped_global_violations(
        {
            "src/Mod/PublicCompatibility.cpp": cpp_source,
            "src/Mod/public_compatibility.py": python_source,
        },
        [],
    )
    assert not violations


def test_public_history_and_command_wrapper_variants_are_recognized() -> None:
    history = PUBLIC_OPERATION_PATTERNS["undo/history control"]
    for expression in (
        "AssemblyObjectPy::clearUndo()",
        "self.assembly.clearUndo()",
        "doc.clearUndos()",
        "doc.undo()",
    ):
        assert history.search(expression), f"public history facade not recognized: {expression}"

    command = PUBLIC_OPERATION_PATTERNS["command/macro bridge"]
    for expression in (
        'FCMD_OBJ_CMD(object, "setValue()")',
        'FCMD_DOC_CMD(document, "recompute()")',
    ):
        assert command.search(expression), f"public command wrapper not recognized: {expression}"
