# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static ownership gate for native document transaction controls."""

from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import Path
from typing import Iterable, Mapping


REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = REPO_ROOT / "src"
INVENTORY_PATH = REPO_ROOT / "doc" / "document-collaboration-ingress-inventory.md"

CPP_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"})
EXCLUDED_PARTS = frozenset({"test", "tests", "build", "generated", "autogen", "cmakefiles"})

UNDERSCORE_CONTROLS = frozenset(
    {"_openTransaction", "_commitTransaction", "_abortTransaction"}
)
COLLABORATION_CONTROLS = frozenset(
    {
        "openCollaborationCommitTransaction",
        "commitCollaborationCommitTransaction",
        "rollbackCollaborationTransaction",
        "rollbackCollaborationTransactionPreservingPendingRecompute",
    }
)
PRIVATE_NATIVE_CONTROLS = UNDERSCORE_CONTROLS | COLLABORATION_CONTROLS
CONTROL_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(" + "|".join(
        sorted(PRIVATE_NATIVE_CONTROLS, key=len, reverse=True)
    ) + r")\s*\("
)

DOCUMENT_HEADER = "src/App/Document.h"
DOCUMENT_SOURCE = "src/App/Document.cpp"
COORDINATOR_SOURCE = "src/App/DocumentCommitCoordinator.cpp"
AUTO_TRANSACTION_SOURCE = "src/App/AutoTransaction.cpp"
COLLABORATION_SERVICE_SOURCE = "src/App/DocumentCollaborationService.cpp"


@dataclass(frozen=True)
class ControlOccurrence:
    path: str
    line: int
    symbol: str
    offset: int
    declaration: bool

    def diagnostic(self, reason: str) -> str:
        return f"{self.path}:{self.line}: {self.symbol}: {reason}"


def _raw_literal_end(source: str, start: int) -> int | None:
    """Return the exclusive end of a C++ raw string beginning at *start*."""

    prefixes = ("u8R\"", "uR\"", "UR\"", "LR\"", "R\"")
    prefix = next((candidate for candidate in prefixes if source.startswith(candidate, start)), None)
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
    """Blank comments and literals while preserving offsets and line breaks."""

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


def _is_declaration(source: str, match: re.Match[str]) -> bool:
    statement_start = max(
        source.rfind(";", 0, match.start(1)),
        source.rfind("{", 0, match.start(1)),
        source.rfind("}", 0, match.start(1)),
    ) + 1
    prefix = source[statement_start:match.start(1)].strip()
    if not prefix or prefix.endswith((".", "->", "::")):
        return False
    if re.search(r"\b(?:return|if|while|for|switch|case|throw|co_return)\b", prefix):
        return False
    if not re.search(r"[A-Za-z_]\w*", prefix):
        return False
    opening = source.find("(", match.start(1) + len(match.group(1)))
    closing = _matching_delimiter(source, opening, "(", ")")
    if closing is None:
        return False
    semicolon = source.find(";", closing + 1)
    brace = source.find("{", closing + 1)
    return semicolon >= 0 and (brace < 0 or semicolon < brace)


def _control_occurrences(path: str, source: str) -> list[ControlOccurrence]:
    stripped = _suppress_cpp_non_code(source)
    return [
        ControlOccurrence(
            path=path,
            line=stripped.count("\n", 0, match.start(1)) + 1,
            symbol=match.group(1),
            offset=match.start(1),
            declaration=_is_declaration(stripped, match),
        )
        for match in CONTROL_CALL_RE.finditer(stripped)
    ]


def _ownership_violations(sources: Mapping[str, str]) -> list[str]:
    violations: list[str] = []
    for path, source in sorted(sources.items()):
        for occurrence in _control_occurrences(path, source):
            if path == DOCUMENT_HEADER:
                if not occurrence.declaration:
                    violations.append(occurrence.diagnostic("Document.h may declare this control only"))
            elif path == DOCUMENT_SOURCE:
                # Document.cpp contains the primitive definitions and their internal mechanics.
                continue
            elif path == COORDINATOR_SOURCE:
                if occurrence.declaration or occurrence.symbol not in COLLABORATION_CONTROLS:
                    violations.append(
                        occurrence.diagnostic(
                            "the coordinator may invoke collaboration commit/rollback primitives only"
                        )
                    )
            elif path == AUTO_TRANSACTION_SOURCE:
                if occurrence.declaration or occurrence.symbol not in {
                    "_commitTransaction",
                    "_abortTransaction",
                }:
                    violations.append(
                        occurrence.diagnostic(
                            "AutoTransaction's temporary CC-WP03 classification covers _commitTransaction "
                            "and _abortTransaction only"
                        )
                    )
            else:
                violations.append(
                    occurrence.diagnostic(
                        "unclassified private native transaction control outside the sole-owner boundary"
                    )
                )
    return violations


def _read_cpp_source(path: Path) -> str:
    """Decode C++ source losslessly while leaving ASCII policy tokens searchable."""

    return path.read_text(encoding="utf-8", errors="surrogateescape")


def _production_cpp_sources() -> dict[str, str]:
    sources: dict[str, str] = {}
    for path in sorted(SOURCE_ROOT.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in CPP_SUFFIXES:
            continue
        relative = path.relative_to(REPO_ROOT)
        if any(part.lower() in EXCLUDED_PARTS for part in relative.parts):
            continue
        sources[relative.as_posix()] = _read_cpp_source(path)
    return sources


def _function_bodies(source: str, qualified_name: str) -> list[tuple[str, str]]:
    """Return ``(signature, body)`` pairs for definitions of a qualified function."""

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
        if closing_brace is None:
            continue
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
        candidates = [item for item in candidates if signature_contains in item[0]]
    assert len(candidates) == 1, (
        f"expected one definition of {qualified_name!r}"
        + (f" containing {signature_contains!r}" if signature_contains else "")
        + f", found {len(candidates)}"
    )
    return candidates[0][1]


def _assert_body_calls(body: str, expression: str, owner: str) -> None:
    compact_body = re.sub(r"\s+", "", body)
    compact_expression = re.sub(r"\s+", "", expression)
    assert compact_expression in compact_body, f"{owner} must call {expression}"


def _inventory_rows() -> Iterable[list[str]]:
    for line in INVENTORY_PATH.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if len(cells) == 9:
            yield cells


def test_private_native_controls_have_only_classified_owners() -> None:
    violations = _ownership_violations(_production_cpp_sources())
    assert not violations, "private transaction ownership violations:\n" + "\n".join(violations)


def test_collaboration_service_has_no_direct_private_control() -> None:
    source = (REPO_ROOT / COLLABORATION_SERVICE_SOURCE).read_text(encoding="utf-8")
    occurrences = _control_occurrences(COLLABORATION_SERVICE_SOURCE, source)
    assert not occurrences, "DCS private-control bypasses:\n" + "\n".join(
        occurrence.diagnostic("DCS must delegate to DCC") for occurrence in occurrences
    )


def test_auto_transaction_exception_is_concrete_and_owned_by_cc_wp03() -> None:
    matching_rows = [
        cells
        for cells in _inventory_rows()
        if cells[1] == f"`{AUTO_TRANSACTION_SOURCE}`"
        and "Application::closeActiveTransaction" in cells[2]
        and cells[6] == "migrate"
        and cells[7] == "CC-WP03"
        and "commit transaction" in cells[3]
        and "abort transaction" in cells[3]
    ]
    assert matching_rows, (
        f"{AUTO_TRANSACTION_SOURCE} _commitTransaction/_abortTransaction must have a concrete "
        "Application::closeActiveTransaction migration row assigned to CC-WP03"
    )


def test_public_compatibility_transactions_follow_dcs_to_dcc_route() -> None:
    document = (REPO_ROOT / DOCUMENT_SOURCE).read_text(encoding="utf-8")
    dcs = (REPO_ROOT / COLLABORATION_SERVICE_SOURCE).read_text(encoding="utf-8")
    dcc = (REPO_ROOT / COORDINATOR_SOURCE).read_text(encoding="utf-8")

    open_body = _body_for(
        document, "Document::openTransaction", signature_contains="TransactionName"
    )
    _assert_body_calls(
        open_body,
        "collaborationService().openCompatibilityTransaction(",
        "Document::openTransaction(TransactionName)",
    )
    _assert_body_calls(
        _body_for(document, "Document::commitTransaction"),
        "collaborationService().commitCompatibilityTransaction()",
        "Document::commitTransaction",
    )
    _assert_body_calls(
        _body_for(document, "Document::abortTransaction"),
        "collaborationService().abortCompatibilityTransaction()",
        "Document::abortTransaction",
    )

    for operation in ("open", "commit", "abort"):
        method = f"{operation}CompatibilityTransaction"
        _assert_body_calls(
            _body_for(dcs, f"DocumentCollaborationService::{method}"),
            f"_coordinator.{method}(",
            f"DocumentCollaborationService::{method}",
        )
        _assert_body_calls(
            _body_for(dcc, f"DocumentCommitCoordinator::{method}"),
            f"_document.{method}Impl(",
            f"DocumentCommitCoordinator::{method}",
        )


def test_scanner_rejects_unclassified_private_control() -> None:
    violations = _ownership_violations(
        {"src/Mod/Example/Bypass.cpp": 'void bypass(Document& doc) { doc._commitTransaction(); }'}
    )
    assert len(violations) == 1
    assert violations[0].startswith("src/Mod/Example/Bypass.cpp:1: _commitTransaction:")


def test_source_reader_preserves_non_utf8_bytes_and_scans_ascii_control(tmp_path: Path) -> None:
    source_path = tmp_path / "NonUtf8Bypass.cpp"
    source_path.write_bytes(
        b"// vendored byte: \xff\nvoid bypass(Document& doc) { doc._commitTransaction(); }\n"
    )

    source = _read_cpp_source(source_path)
    assert "\udcff" in source
    violations = _ownership_violations({"src/3rdParty/NonUtf8Bypass.cpp": source})
    assert violations == [
        "src/3rdParty/NonUtf8Bypass.cpp:2: _commitTransaction: unclassified private native "
        "transaction control outside the sole-owner boundary"
    ]


def test_scanner_ignores_comments_and_literals() -> None:
    source = r'''
        // doc._openTransaction("comment");
        /* doc._commitTransaction(); */
        const char* text = "doc._abortTransaction()";
        const char marker = '(';
        const char* raw = R"gate(doc.openCollaborationCommitTransaction())gate";
    '''
    assert not _control_occurrences("src/Mod/Example/Inert.cpp", source)
    assert not _ownership_violations({"src/Mod/Example/Inert.cpp": source})


def test_scanner_rejects_widening_auto_transaction_exception() -> None:
    violations = _ownership_violations(
        {AUTO_TRANSACTION_SOURCE: 'void widen(Document& doc) { doc._openTransaction("bad"); }'}
    )
    assert len(violations) == 1
    assert "covers _commitTransaction and _abortTransaction only" in violations[0]
