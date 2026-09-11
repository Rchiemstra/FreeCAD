# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static ownership gate for native document transaction controls."""

from __future__ import annotations

from dataclasses import dataclass
import re
from pathlib import Path
from typing import Mapping


REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOT = REPO_ROOT / "src"

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
DOCUMENT_PY_SOURCE = "src/App/DocumentPyImp.cpp"
APPLICATION_PY_SOURCE = "src/App/ApplicationPy.cpp"
PROPERTY_STANDARD_SOURCE = "src/App/PropertyStandard.cpp"


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
                if occurrence.declaration:
                    violations.append(
                        occurrence.diagnostic(
                            "the coordinator may invoke private native controls but may not declare them"
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
        normalized_fragment = re.sub(r"\s+", "", signature_contains)
        candidates = [
            item
            for item in candidates
            if normalized_fragment in re.sub(r"\s+", "", item[0])
        ]
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


def _assert_no_internal_transaction_route(body: str, owner: str) -> None:
    forbidden = (
        "collaborationService(",
        "_coordinator.",
        "CompatibilityTransaction",
        "ApplicationTransactionThroughCoordinator",
    )
    compact_body = re.sub(r"\s+", "", body)
    leaked = [token for token in forbidden if token in compact_body]
    leaked.extend(
        occurrence.symbol
        for occurrence in _control_occurrences(f"<{owner}>", body)
    )
    assert not leaked, f"{owner} bypasses its public facade via {sorted(set(leaked))}"


def _assert_exact_public_transaction_calls(
    body: str,
    expected: set[str],
    owner: str,
) -> None:
    public_facades = (
        "openTransaction",
        "commitTransaction",
        "abortTransaction",
        "setActiveTransaction",
        "closeActiveTransaction",
        "undo",
        "redo",
        "clearUndos",
    )
    pattern = re.compile(
        r"(?<![A-Za-z0-9_])(" + "|".join(public_facades) + r")\s*\("
    )
    actual = {match.group(1) for match in pattern.finditer(_suppress_cpp_non_code(body))}
    assert actual == expected, f"{owner} public transaction calls: expected {expected}, got {actual}"


def test_private_native_controls_have_only_classified_owners() -> None:
    violations = _ownership_violations(_production_cpp_sources())
    assert not violations, "private transaction ownership violations:\n" + "\n".join(violations)


def test_collaboration_service_has_no_direct_private_control() -> None:
    source = _read_cpp_source(REPO_ROOT / COLLABORATION_SERVICE_SOURCE)
    occurrences = _control_occurrences(COLLABORATION_SERVICE_SOURCE, source)
    assert not occurrences, "DCS private-control bypasses:\n" + "\n".join(
        occurrence.diagnostic("DCS must delegate to DCC") for occurrence in occurrences
    )


def test_application_close_routes_document_bridges_to_dcc_terminals() -> None:
    auto_transaction = _read_cpp_source(REPO_ROOT / AUTO_TRANSACTION_SOURCE)
    document = _read_cpp_source(REPO_ROOT / DOCUMENT_SOURCE)
    dcs = _read_cpp_source(REPO_ROOT / COLLABORATION_SERVICE_SOURCE)
    dcc = _read_cpp_source(REPO_ROOT / COORDINATOR_SOURCE)

    close_body = _body_for(auto_transaction, "Application::closeActiveTransaction")
    _assert_body_calls(
        close_body,
        "doc->commitApplicationTransactionThroughCoordinator()",
        "Application::closeActiveTransaction",
    )
    _assert_body_calls(
        close_body,
        "doc->abortApplicationTransactionThroughCoordinator()",
        "Application::closeActiveTransaction",
    )

    application_routes = (
        ("commit", "_commitTransaction("),
        ("abort", "_abortTransaction("),
    )
    for operation, terminal_call in application_routes:
        bridge = f"{operation}ApplicationTransactionThroughCoordinator"
        dcs_method = f"{operation}ApplicationTransaction"
        _assert_body_calls(
            _body_for(document, f"Document::{bridge}"),
            f"collaborationService().{dcs_method}(",
            f"Document::{bridge}",
        )
        _assert_body_calls(
            _body_for(dcs, f"DocumentCollaborationService::{dcs_method}"),
            f"_coordinator.{dcs_method}(",
            f"DocumentCollaborationService::{dcs_method}",
        )
        _assert_body_calls(
            _body_for(dcc, f"DocumentCommitCoordinator::{dcs_method}"),
            f"_document.{terminal_call}",
            f"DocumentCommitCoordinator::{dcs_method}",
        )


def test_public_compatibility_transactions_follow_dcs_to_dcc_route() -> None:
    document = _read_cpp_source(REPO_ROOT / DOCUMENT_SOURCE)
    dcs = _read_cpp_source(REPO_ROOT / COLLABORATION_SERVICE_SOURCE)
    dcc = _read_cpp_source(REPO_ROOT / COORDINATOR_SOURCE)

    routes = (
        (
            "openTransaction",
            "TransactionName",
            "openCompatibilityTransaction",
            "openCompatibilityTransactionImpl",
        ),
        (
            "setActiveTransaction",
            None,
            "setActiveCompatibilityTransaction",
            "setActiveCompatibilityTransactionImpl",
        ),
        (
            "commitTransaction",
            None,
            "commitCompatibilityTransaction",
            "commitCompatibilityTransactionImpl",
        ),
        (
            "abortTransaction",
            None,
            "abortCompatibilityTransaction",
            "abortCompatibilityTransactionImpl",
        ),
        (
            "undo",
            None,
            "undoCompatibilityTransaction",
            "undoCompatibilityTransactionImpl",
        ),
        (
            "redo",
            None,
            "redoCompatibilityTransaction",
            "redoCompatibilityTransactionImpl",
        ),
        (
            "clearUndos",
            None,
            "clearCompatibilityTransactionHistory",
            "clearCompatibilityTransactionHistoryImpl",
        ),
    )
    for document_method, signature, service_method, impl_method in routes:
        owner = f"Document::{document_method}"
        body = _body_for(document, owner, signature_contains=signature)
        _assert_body_calls(body, f"collaborationService().{service_method}(", owner)
        _assert_body_calls(
            _body_for(dcs, f"DocumentCollaborationService::{service_method}"),
            f"_coordinator.{service_method}(",
            f"DocumentCollaborationService::{service_method}",
        )
        _assert_body_calls(
            _body_for(dcc, f"DocumentCommitCoordinator::{service_method}"),
            f"_document.{impl_method}(",
            f"DocumentCommitCoordinator::{service_method}",
        )


def test_lazy_mutation_opening_routes_through_dcs_and_dcc() -> None:
    document = _read_cpp_source(REPO_ROOT / DOCUMENT_SOURCE)
    dcs = _read_cpp_source(REPO_ROOT / COLLABORATION_SERVICE_SOURCE)
    dcc = _read_cpp_source(REPO_ROOT / COORDINATOR_SOURCE)

    for document_method in ("_checkTransaction", "changePropertyOfObject"):
        _assert_body_calls(
            _body_for(document, f"Document::{document_method}"),
            "collaborationService().openMutationTransaction(",
            f"Document::{document_method}",
        )
    _assert_body_calls(
        _body_for(dcs, "DocumentCollaborationService::openMutationTransaction"),
        "_coordinator.openMutationTransaction(",
        "DocumentCollaborationService::openMutationTransaction",
    )
    _assert_body_calls(
        _body_for(dcc, "DocumentCommitCoordinator::openMutationTransaction"),
        "_document._openTransaction(",
        "DocumentCommitCoordinator::openMutationTransaction",
    )
    _assert_body_calls(
        _body_for(document, "Document::_openTransaction"),
        "transactionInitiator->collaborationService().openMutationTransaction(",
        "Document::_openTransaction cross-document recursion",
    )


def test_language_bindings_and_property_string_use_public_facades_only() -> None:
    document_py = _read_cpp_source(REPO_ROOT / DOCUMENT_PY_SOURCE)
    application_py = _read_cpp_source(REPO_ROOT / APPLICATION_PY_SOURCE)
    property_standard = _read_cpp_source(REPO_ROOT / PROPERTY_STANDARD_SOURCE)

    for method in ("openTransaction", "commitTransaction", "abortTransaction", "undo", "redo", "clearUndos"):
        owner = f"DocumentPy::{method}"
        body = _body_for(document_py, owner)
        _assert_body_calls(body, f"getDocumentPtr()->{method}(", owner)
        _assert_exact_public_transaction_calls(body, {method}, owner)
        _assert_no_internal_transaction_route(body, owner)

    application_routes = (
        ("sSetActiveTransaction", "GetApplication().setActiveTransaction("),
        ("sCloseActiveTransaction", "GetApplication().closeActiveTransaction("),
    )
    for method, expression in application_routes:
        owner = f"ApplicationPy::{method}"
        body = _body_for(application_py, owner)
        _assert_body_calls(body, expression, owner)
        _assert_exact_public_transaction_calls(
            body,
            {"setActiveTransaction" if method == "sSetActiveTransaction" else "closeActiveTransaction"},
            owner,
        )
        _assert_no_internal_transaction_route(body, owner)

    property_owner = "PropertyString::setValue(const char*)"
    property_body = _body_for(
        property_standard,
        "PropertyString::setValue",
        signature_contains="const char*",
    )
    _assert_body_calls(property_body, "obj->getDocument()->openTransaction(", property_owner)
    _assert_body_calls(property_body, "obj->getDocument()->commitTransaction(", property_owner)
    _assert_exact_public_transaction_calls(
        property_body,
        {"openTransaction", "commitTransaction"},
        property_owner,
    )
    _assert_no_internal_transaction_route(property_body, property_owner)


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


def test_scanner_rejects_private_control_in_auto_transaction() -> None:
    violations = _ownership_violations(
        {AUTO_TRANSACTION_SOURCE: 'void widen(Document& doc) { doc._openTransaction("bad"); }'}
    )
    assert len(violations) == 1
    assert violations[0].startswith(
        f"{AUTO_TRANSACTION_SOURCE}:1: _openTransaction: unclassified private native "
        "transaction control outside the sole-owner boundary"
    )
