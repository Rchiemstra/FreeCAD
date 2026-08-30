"""CC-WP10 architecture gates for dependency-ordered document recompute."""

from __future__ import annotations

import re
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]

HEADER = Path("src/App/DocumentRecomputeCoordinator.h")
SOURCE = Path("src/App/DocumentRecomputeCoordinator.cpp")
DOCUMENT_HEADER = Path("src/App/Document.h")
DOCUMENT_SOURCE = Path("src/App/Document.cpp")
DOCUMENT_PRIVATE = Path("src/App/private/DocumentP.h")
APP_CMAKE = Path("src/App/CMakeLists.txt")
NATIVE_TEST = Path("tests/src/App/DocumentRecomputeCoordinator.cpp")
TEST_CMAKE = Path("tests/src/App/CMakeLists.txt")


def _read_source(path: Path) -> str:
    return (REPOSITORY / path).read_text(
        encoding="utf-8", errors="surrogateescape"
    )


def _blank(character: str) -> str:
    return "\n" if character == "\n" else " "


def _raw_literal_end(source: str, start: int) -> int | None:
    match = re.match(r'(?:u8|u|U|L)?R"([^\s()\\]{0,16})\(', source[start:])
    if not match:
        return None
    terminator = ")" + match.group(1) + '"'
    closing = source.find(terminator, start + match.end())
    return len(source) if closing < 0 else closing + len(terminator)


def _suppress_cpp(source: str, *, literals: bool = True) -> str:
    """Suppress comments/literals without moving source offsets or newlines."""

    output = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            for cursor in range(index, end):
                output[cursor] = _blank(source[cursor])
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = len(source) if end < 0 else end + 2
            for cursor in range(index, end):
                output[cursor] = _blank(source[cursor])
            index = end
            continue
        raw_end = _raw_literal_end(source, index)
        if raw_end is not None:
            if literals:
                for cursor in range(index, raw_end):
                    output[cursor] = _blank(source[cursor])
            index = raw_end
            continue
        if (
            source[index] == "'"
            and index > 0
            and index + 1 < len(source)
            and source[index - 1].isalnum()
            and source[index + 1].isalnum()
        ):
            index += 1
            continue
        quote = re.match(r"(?:u8|u|U|L)?([\"'])", source[index:])
        if quote:
            delimiter = quote.group(1)
            end = index + quote.end()
            escaped = False
            while end < len(source):
                character = source[end]
                end += 1
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == delimiter:
                    break
            if literals:
                for cursor in range(index, end):
                    output[cursor] = _blank(source[cursor])
            index = end
            continue
        index += 1
    return "".join(output)


def _matching(source: str, opening: int, left: str, right: str) -> int:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == left:
            depth += 1
        elif source[index] == right:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unclosed {left!r} at offset {opening}")


def _type_body(source: str, kind: str, name: str) -> str:
    clean = _suppress_cpp(source)
    match = re.search(
        rf"\b{re.escape(kind)}\b[^;{{}}]*\b{re.escape(name)}\b[^;{{}}]*\{{",
        clean,
    )
    assert match, f"missing {kind} {name}"
    opening = clean.find("{", match.start())
    closing = _matching(clean, opening, "{", "}")
    return source[opening + 1 : closing]


def _function_bodies(source: str, name: str) -> list[str]:
    clean = _suppress_cpp(source)
    bodies: list[str] = []
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", clean):
        opening = clean.find("(", match.start())
        closing = _matching(clean, opening, "(", ")")
        cursor = closing + 1
        qualifier = re.compile(
            r"\s*(?:(?:const|noexcept|override|final)\b(?:\s*\([^)]*\))?|"
            r"->\s*[^\{;=]+)"
        )
        while next_part := qualifier.match(clean, cursor):
            cursor = next_part.end()
        cursor += len(clean[cursor:]) - len(clean[cursor:].lstrip())
        if cursor < len(clean) and clean[cursor] == ":":
            # Qualified constructors may have a parenthesized initializer list.
            opening_body = clean.find("{", cursor + 1)
            semicolon = clean.find(";", cursor + 1)
            if opening_body < 0 or (semicolon >= 0 and semicolon < opening_body):
                continue
            cursor = opening_body
        if cursor >= len(clean) or clean[cursor] != "{":
            continue
        end = _matching(clean, cursor, "{", "}")
        bodies.append(source[cursor + 1 : end])
    return bodies


def _one_body(source: str, name: str) -> str:
    bodies = _function_bodies(source, name)
    assert len(bodies) == 1, f"expected one {name} definition, found {len(bodies)}"
    return bodies[0]


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", _suppress_cpp(source, literals=False))


def _request_authority_violations(header: str) -> list[str]:
    violations: list[str] = []
    for name in ("DocumentRecomputeFeatureRequest", "DocumentRecomputeRequest"):
        body = _suppress_cpp(_type_body(header, "struct", name))
        for token in ("Document", "DocumentObject", "DocumentCommitCoordinator"):
            if re.search(rf"\b{token}\b", body):
                violations.append(f"{name}: live authority token {token}")
        if "*" in body:
            violations.append(f"{name}: raw pointer field")
        if re.search(r"\b(?:shared_ptr|weak_ptr|unique_ptr|reference_wrapper)\b", body):
            violations.append(f"{name}: shared authority field")
    return violations


def _committed_dependency_guard(source: str) -> bool:
    try:
        body = _compact(_one_body(source, "scheduleReady"))
    except AssertionError:
        return False
    pattern = re.compile(
        r"node\.state==DocumentRecomputeFeatureState::Waiting"
        r"&&std::ranges::all_of\(node\.request\.dependencies,"
        r"\[&job\]\(conststd::string&dependency\)\{"
        r"returnjob\.nodes\.at\(dependency\)\.state"
        r"==DocumentRecomputeFeatureState::Committed;\}\)"
    )
    return bool(pattern.search(body))


def test_request_and_snapshot_contracts_are_pointer_free_values() -> None:
    header = _read_source(HEADER)
    violations = _request_authority_violations(header)
    assert not violations, "recompute request authority violations:\n" + "\n".join(
        violations
    )

    feature = _compact(_type_body(header, "struct", "DocumentRecomputeFeatureRequest"))
    for field in (
        "std::stringfeatureId;",
        "std::vector<std::string>dependencies;",
        "std::stringoperationId;",
        "CollaborativeOperationIntentintent;",
        "std::stringprovenance;",
    ):
        assert field in feature
    request = _compact(_type_body(header, "struct", "DocumentRecomputeRequest"))
    assert "std::vector<DocumentRecomputeFeatureRequest>features;" in request
    assert "std::stringcoalescingKey;" in request
    assert "boolrefreshRevisionFenceAfterEachCommit{false};" in request

    feature_states = set(
        re.findall(
            r"\b[A-Za-z_]\w*\b",
            _type_body(header, "enum class", "DocumentRecomputeFeatureState"),
        )
    )
    assert feature_states == {
        "Waiting",
        "Preparing",
        "Committing",
        "Committed",
        "Stale",
        "Failed",
        "Blocked",
        "Cancelling",
        "Cancelled",
    }
    job_states = set(
        re.findall(
            r"\b[A-Za-z_]\w*\b",
            _type_body(header, "enum class", "DocumentRecomputeState"),
        )
    )
    assert job_states == {
        "Running",
        "Cancelling",
        "Completed",
        "PartialFailure",
        "Cancelled",
    }


def test_document_exclusively_owns_one_recompute_coordinator() -> None:
    private = _compact(_read_source(DOCUMENT_PRIVATE))
    document = _read_source(DOCUMENT_SOURCE)
    header = _compact(_read_source(DOCUMENT_HEADER))
    assert private.count(
        "std::unique_ptr<DocumentRecomputeCoordinator>recomputeCoordinator;"
    ) == 1
    constructor = _compact(_one_body(document, "Document::Document"))
    assert (
        "d->recomputeCoordinator.reset(newDocumentRecomputeCoordinator("
        "*d->collaborationService));" in constructor
    )
    assert "DocumentRecomputeCoordinator&recomputeCoordinator();" in header
    assert "constDocumentRecomputeCoordinator&recomputeCoordinator()const;" in header
    accessors = _function_bodies(document, "Document::recomputeCoordinator")
    assert len(accessors) == 2
    assert all("return*d->recomputeCoordinator;" in _compact(body) for body in accessors)

    clean = _suppress_cpp(document)
    assert "DocumentRecomputeCoordinator::instance" not in clean
    assert not re.search(r"\bstatic\s+DocumentRecomputeCoordinator\b", clean)


def test_coordinator_has_no_live_document_execution_boundary() -> None:
    source = _read_source(SOURCE)
    code = _suppress_cpp(source)
    includes = re.findall(r"(?m)^\s*#\s*include\s*[<\"]([^>\"]+)[>\"]", source)
    assert "Document.h" not in includes
    for forbidden in (
        r"\bDocumentObject\b",
        r"\bDocument::",
        r"\bgetDocument\s*\(",
        r"(?:\.|->)recompute(?:Feature)?\s*\(",
        r"\b_document\b",
    ):
        assert not re.search(forbidden, code), f"live document access matched {forbidden}"

    header = _compact(_read_source(HEADER))
    assert "DocumentCollaborationService&_service;" in header
    assert "Document&_document" not in header
    assert "DocumentObject" not in _suppress_cpp(_read_source(HEADER))


def test_coordinator_routes_capture_collection_and_commit_only_through_dcs() -> None:
    source = _read_source(SOURCE)
    submit = _compact(_one_body(source, "submit"))
    schedule = _compact(_one_body(source, "scheduleReady"))
    poll = _compact(_one_body(source, "poll"))
    cancel = _compact(_one_body(source, "cancel"))

    assert "_service.beginEditSession(" in submit
    assert "_service.prepareEditAsync(" in schedule
    status = poll.find("_service.preparedEditStatus(")
    take = poll.find("_service.takeRecomputePreparedEdit(", status)
    commit = poll.find("_service.commitRecomputeEdit(", take)
    assert 0 <= status < take < commit
    assert "_service.takePreparedEdit(" not in poll
    assert "_service.commitEdit(" not in poll
    assert "_service.cancelPreparedEdit(" in cancel
    assert "_service.cancelEdit(" in _compact(source)


def test_mutating_reentrancy_is_recursive_and_fails_fast_while_status_observes() -> None:
    header = _compact(_read_source(HEADER))
    source = _read_source(SOURCE)
    assert "mutablestd::recursive_mutex_operationMutex;" in header
    assert "bool_operationActive{false};" in header

    admission_bodies = [
        _compact(body) for body in _function_bodies(source, "OperationAdmission")
    ]
    assert len(admission_bodies) == 2
    constructor_bodies = [
        body
        for body in admission_bodies
        if "_active=true;" in body
        or "reentrantdocumentrecomputemutationisnotsupported" in body
    ]
    assert len(constructor_bodies) == 1
    admission = constructor_bodies[0]
    assert "if(_active){" in admission
    assert (
        'throwstd::runtime_error("reentrantdocumentrecomputemutationisnotsupported")'
        in admission
    )
    assert admission.find("if(_active){") < admission.find("_active=true;")
    admission_type = _compact(_type_body(source, "class", "OperationAdmission"))
    assert "~OperationAdmission(){_active=false;}" in admission_type

    for method in ("submit", "poll", "cancel"):
        body = _compact(_one_body(source, method))
        lock = body.find("std::lock_guardoperationLock(_operationMutex);")
        guard = body.find("OperationAdmissionoperationAdmission(_operationActive);")
        assert 0 <= lock < guard, f"{method} lacks recursive lock plus fail-fast admission"

    status = _compact(_one_body(source, "status"))
    assert "OperationAdmission" not in status
    assert "std::lock_guardstateLock(_stateMutex);" in status


def test_downstream_work_requires_every_dependency_to_be_committed() -> None:
    source = _read_source(SOURCE)
    assert _committed_dependency_guard(source)
    schedule = _compact(_one_body(source, "scheduleReady"))
    assert "featureFailed(job.nodes.at(dependency).state)" in schedule
    assert "DocumentRecomputeFeatureState::Blocked" in schedule


def test_production_and_native_sources_are_registered_in_cmake() -> None:
    app_cmake = _read_source(APP_CMAKE)
    test_cmake = _read_source(TEST_CMAKE)
    for name in ("DocumentRecomputeCoordinator.cpp", "DocumentRecomputeCoordinator.h"):
        assert re.search(rf"(?m)^\s*{re.escape(name)}\s*$", app_cmake)
    assert re.search(
        r"(?m)^\s*DocumentRecomputeCoordinator\.cpp\s*$", test_cmake
    )
    native = _read_source(NATIVE_TEST)
    for contract in (
        "childCaptureBeginsOnlyAfterUpstreamCommit",
        "staleRevisionIsRejectedWithoutOverwritingLiveState",
        "cancellationDrainsToTerminalState",
        "partialFailurePreservesCommitsRunsIndependentAndBlocksDescendants",
        "malformedDependencyPlansAreRejectedBeforeAnyWork",
        "commitObserverRejectsReentrantMutationsWithoutBlockingOuterCommit",
    ):
        assert contract in native


def test_request_scanner_rejects_live_pointer_authority() -> None:
    unsafe = """
        struct DocumentRecomputeFeatureRequest {
            DocumentObject* object;
        };
        struct DocumentRecomputeRequest {
            Document* document;
        };
    """
    violations = _request_authority_violations(unsafe)
    assert "DocumentRecomputeFeatureRequest: live authority token DocumentObject" in violations
    assert "DocumentRecomputeFeatureRequest: raw pointer field" in violations
    assert "DocumentRecomputeRequest: live authority token Document" in violations


def test_dependency_guard_rejects_ready_or_merely_terminal_dependencies() -> None:
    unsafe = """
        void scheduleReady()
        {
            const auto ready = std::ranges::find_if(job.nodes, [&job](const auto& entry) {
                const auto& node = entry.second;
                return node.state == DocumentRecomputeFeatureState::Waiting
                    && std::ranges::all_of(node.request.dependencies,
                        [&job](const std::string& dependency) {
                            return featureTerminal(job.nodes.at(dependency).state);
                        });
            });
        }
    """
    assert not _committed_dependency_guard(unsafe)


def test_source_reader_preserves_non_utf8_bytes(tmp_path: Path) -> None:
    source = tmp_path / "Recompute.cpp"
    source.write_bytes(
        b"// vendor byte \xff\n"
        b"DocumentRecomputeRequest request;\n"
    )
    decoded = _read_source(source)
    assert "\udcff" in decoded
    assert "DocumentRecomputeRequest" in decoded
