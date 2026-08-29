"""Architecture gates for the process-first native geometry adapters.

These tests deliberately inspect source instead of loading FreeCAD.  They keep
the trust boundary visible: live documents are read only by parent adapters,
workers receive archives, and only the parent reconstructs a PreparedEdit.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]

COLLABORATIVE_REGISTRY = Path("src/App/CollaborativeOperationRegistry.h")
DCS_SOURCE = Path("src/App/DocumentCollaborationService.cpp")
WORKER_MAIN = Path("src/App/GeometryWorkerMain.cpp")
WORKER_REGISTRY_HEADER = Path("src/App/GeometryWorkerOperationRegistry.h")
WORKER_REGISTRY_SOURCE = Path("src/App/GeometryWorkerOperationRegistry.cpp")
BOOLEAN_HEADER = Path("src/Mod/Part/App/CollaborativeBooleanOperation.h")
BOOLEAN_SOURCE = Path("src/Mod/Part/App/CollaborativeBooleanOperation.cpp")
SWEEP_FILLET_HEADER = Path("src/Mod/Part/App/CollaborativeSweepFilletOperations.h")
SWEEP_FILLET_SOURCE = Path("src/Mod/Part/App/CollaborativeSweepFilletOperations.cpp")
APP_CMAKE = Path("src/App/CMakeLists.txt")
PART_CMAKE = Path("src/Mod/Part/App/CMakeLists.txt")
PART_MODULE = Path("src/Mod/Part/App/AppPart.cpp")


def _read_source(path: Path) -> str:
    return (REPOSITORY / path).read_text(encoding="utf-8", errors="surrogateescape")


def _blank(character: str) -> str:
    return "\n" if character == "\n" else " "


def _raw_string_end(text: str, start: int) -> int | None:
    """Return the end of a C++ raw literal beginning at *start*, if any."""

    match = re.match(r'(?:u8|u|U|L)?R"([^\s()\\]{0,16})\(', text[start:])
    if not match:
        return None
    terminator = ")" + match.group(1) + '"'
    end = text.find(terminator, start + match.end())
    return len(text) if end < 0 else end + len(terminator)


def _suppress_cpp(text: str, *, suppress_literals: bool = True) -> str:
    """Suppress C++ comments and optionally literals while preserving offsets."""

    output = list(text)
    index = 0
    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            end = len(text) if end < 0 else end
            for cursor in range(index, end):
                output[cursor] = _blank(text[cursor])
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = len(text) if end < 0 else end + 2
            for cursor in range(index, end):
                output[cursor] = _blank(text[cursor])
            index = end
            continue

        raw_end = _raw_string_end(text, index)
        if raw_end is not None:
            if suppress_literals:
                for cursor in range(index, raw_end):
                    output[cursor] = _blank(text[cursor])
            index = raw_end
            continue

        if (
            text[index] == "'"
            and index > 0
            and index + 1 < len(text)
            and text[index - 1].isalnum()
            and text[index + 1].isalnum()
        ):
            # C++ digit separators are not character-literal delimiters.
            index += 1
            continue

        prefix = re.match(r"(?:u8|u|U|L)?([\"'])", text[index:])
        if prefix:
            quote = prefix.group(1)
            end = index + prefix.end()
            escaped = False
            while end < len(text):
                character = text[end]
                end += 1
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    break
            if suppress_literals:
                for cursor in range(index, end):
                    output[cursor] = _blank(text[cursor])
            index = end
            continue
        index += 1
    return "".join(output)


def _matching(text: str, opening: int, left: str, right: str) -> int:
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == left:
            depth += 1
        elif text[index] == right:
            depth -= 1
            if depth == 0:
                return index
    raise AssertionError(f"unclosed {left!r} at offset {opening}")


def _function_bodies(source: str, name: str) -> list[tuple[str, str]]:
    """Return ``(parameters, body)`` for definitions, excluding calls."""

    clean = _suppress_cpp(source)
    definitions: list[tuple[str, str]] = []
    for match in re.finditer(rf"\b{re.escape(name)}\s*\(", clean):
        before = clean[max(0, match.start() - 2) : match.start()]
        if before.endswith(".") or before.endswith("->"):
            continue
        opening = clean.find("(", match.start())
        closing = _matching(clean, opening, "(", ")")
        cursor = closing + 1
        while True:
            cursor_match = re.match(
                r"\s*(?:(?:const|noexcept|override|final)\b(?:\s*\([^)]*\))?|->\s*[^\{;=]+)",
                clean[cursor:],
            )
            if not cursor_match:
                break
            cursor += cursor_match.end()
        cursor += len(clean[cursor:]) - len(clean[cursor:].lstrip())
        if cursor >= len(clean) or clean[cursor] != "{":
            continue
        end = _matching(clean, cursor, "{", "}")
        definitions.append((source[opening + 1 : closing], source[cursor + 1 : end]))
    return definitions


def _one_function(source: str, name: str) -> tuple[str, str]:
    definitions = _function_bodies(source, name)
    assert len(definitions) == 1, (
        f"expected one definition of {name}, found {len(definitions)}"
    )
    return definitions[0]


def _type_body(source: str, kind: str, name: str) -> str:
    clean = _suppress_cpp(source)
    match = re.search(
        rf"\b{re.escape(kind)}\s+(?:[A-Za-z_]\w*\s+)*{re.escape(name)}\s*\{{",
        clean,
    )
    assert match, f"missing {kind} {name}"
    opening = clean.find("{", match.start())
    closing = _matching(clean, opening, "{", "}")
    return source[opening + 1 : closing]


def _compact(text: str) -> str:
    return re.sub(r"\s+", "", _suppress_cpp(text, suppress_literals=False))


def _contains_identifier(text: str, identifier: str) -> bool:
    return bool(re.search(rf"\b{re.escape(identifier)}\b", _suppress_cpp(text)))


def _assert_order(text: str, *needles: str) -> None:
    compact = _compact(text)
    positions = []
    for needle in needles:
        position = compact.find(_compact(needle))
        assert position >= 0, f"missing ordered source fragment: {needle}"
        positions.append(position)
    assert positions == sorted(positions), (
        f"source fragments are out of order: {list(zip(needles, positions))}"
    )


def _archive_handler_violations(source: str, function: str) -> list[str]:
    definitions = _function_bodies(source, function)
    if len(definitions) != 1:
        return [f"{function}: expected one definition, found {len(definitions)}"]
    parameters, body = definitions[0]
    compact_parameters = _compact(parameters)
    expected = "constApp::GeometryArchive&input,conststd::stop_tokenstopToken"
    violations: list[str] = []
    if compact_parameters != expected:
        violations.append(
            f"{function}: archive-only signature changed: {compact_parameters}"
        )
    clean_body = _suppress_cpp(body)
    for token in ("Document", "DocumentObject", "GetApplication"):
        if re.search(rf"\b{token}\b", clean_body):
            violations.append(f"{function}: live authority token {token}")
    if re.search(r"\b(?:getDocument|collaborationService)\s*\(", clean_body):
        violations.append(f"{function}: attempts to recover document authority")
    return violations


def _isolated_task_field_violations(source: str) -> list[str]:
    body = _type_body(source, "struct", "IsolatedTask")
    statements = [_compact(item) for item in body.split(";") if _compact(item)]
    expected = {
        "GeometryJobRequestrequest",
        "GeometryArchiveinputArchive",
        "IsolatedResultDecoderdecodeResult",
    }
    return [] if len(statements) == 3 and set(statements) == expected else statements


def test_collaborative_preparation_has_one_trusted_isolated_task_contract() -> None:
    source = _read_source(COLLABORATIVE_REGISTRY)
    compact = _compact(source)

    assert not _isolated_task_field_violations(source)
    assert (
        "usingIsolatedResultDecoder=std::function<std::unique_ptr<constCollaborativeOperation>"
        "(constGeometryArchive&)>;" in compact
    )
    assert compact.count("std::unique_ptr<IsolatedTask>isolatedTask;") == 1
    assert "std::vector<IsolatedTask>" not in compact
    assert "std::shared_ptr<IsolatedTask>" not in compact

    constructor_area = _compact(
        _type_body(source, "struct", "CollaborativeOperationPreparation")
    )
    assert constructor_area.count("IsolatedTaskisolatedTask") == 1
    assert constructor_area.count(
        "std::make_unique<IsolatedTask>(std::move(isolatedTask))"
    ) == 1
    assert constructor_area.count("policy(PreparationPolicy::IsolatedProcess)") == 1


def test_dcs_owns_isolated_submission_status_cancellation_and_collection() -> None:
    source = _read_source(DCS_SOURCE)
    _, prepare = _one_function(source, "prepareEditAsyncOnDocumentThread")
    compact_prepare = _compact(prepare)
    assert "preparation.policy==PreparationPolicy::IsolatedProcess" in compact_prepare
    assert "!preparation.isolatedTask" in compact_prepare
    assert "pending.isolatedResultDecoder=isolatedTask->decodeResult" in compact_prepare
    assert re.search(
        r"if\(isolatedTask\)\{.*geometryJobManager\(\)\.submit\(.*?\)"
        r";\}else\{.*preparedEditExecutor\(\)\.submit\(",
        compact_prepare,
        re.DOTALL,
    )

    _, status = _one_function(source, "preparedEditStatus")
    assert "geometryJobManager().status(geometryJobId(executionId))" in _compact(status)
    _, cancel = _one_function(source, "cancelPreparedEdit")
    assert "geometryJobManager().cancel(geometryJobId(executionId))" in _compact(cancel)

    _, take = _one_function(source, "takePreparedEditOnDocumentThread")
    compact_take = _compact(take)
    assert "geometryJobManager().takeResult(geometryJobId(executionId))" in compact_take
    for binding in (
        "expectation.kind=GeometryArchiveKind::Result",
        "expectation.jobId=geometryTerminal->id",
        "expectation.operationType=geometryTerminal->operationType",
        "expectation.buildFingerprint=geometryTerminal->buildFingerprint",
        "expectation.inputDigest=geometryTerminal->inputDigest",
    ):
        assert binding in compact_take
    _assert_order(
        take,
        "geometryJobManager().takeResult",
        "GeometryArchiveCodec::readValidated",
        "decoded.archive->archiveDigest != geometryTerminal->resultDigest",
        "pending.isolatedResultDecoder(*decoded.archive)",
        "new PreparedEdit",
    )


def test_worker_imports_part_and_dispatches_only_registered_archive_operations() -> None:
    worker_source = _read_source(WORKER_MAIN)
    _, worker = _one_function(worker_source, "runGeometryWorkerMain")
    worker_with_literals = _compact(worker)
    assert 'Base::Interpreter().runString("importPart")' in worker_with_literals
    _assert_order(
        worker,
        'Base::Interpreter().runString("import Part")',
        "GeometryWorkerOperationRegistry::instance()",
        "!registry.contains(operation)",
        "return 13",
        "registry.execute(operation, *input.archive, cancellation.token())",
    )

    registry_source = _read_source(WORKER_REGISTRY_SOURCE)
    _, execute = _one_function(registry_source, "execute")
    compact_execute = _compact(execute)
    assert "found==_operations.end()" in compact_execute
    assert 'throwstd::invalid_argument("unsupportedisolatedgeometryoperation")' in compact_execute
    _assert_order(
        execute,
        "found == _operations.end()",
        "throw",
        "operation = found->second",
        "return operation(input, stopToken)",
    )


def test_worker_registry_callback_is_archive_only_and_pointer_free() -> None:
    header = _read_source(WORKER_REGISTRY_HEADER)
    compact = _compact(header)
    assert (
        "usingGeometryWorkerOperation="
        "std::function<GeometryArchive(constGeometryArchive&,std::stop_token)>;" in compact
    )
    clean = _suppress_cpp(header)
    assert not re.search(r"\bDocument(?:Object)?\b", clean)
    alias = re.search(
        r"using\s+GeometryWorkerOperation\s*=\s*([^;]+);", clean, re.DOTALL
    )
    assert alias
    assert "*" not in alias.group(1)
    assert set(re.findall(r"\b(?:GeometryArchive|stop_token)\b", alias.group(1))) == {
        "GeometryArchive",
        "stop_token",
    }


@pytest.mark.parametrize(
    ("source_path", "preparer", "operation_type", "handler"),
    [
        (
            BOOLEAN_SOURCE,
            "prepareBooleanImpl",
            "CollaborativeBooleanOperationType",
            "executeBooleanArchive",
        ),
        (
            SWEEP_FILLET_SOURCE,
            "prepareSweep",
            "CollaborativeSweepOperationType",
            "executeSweep",
        ),
        (
            SWEEP_FILLET_SOURCE,
            "prepareFillet",
            "CollaborativeFilletOperationType",
            "executeFillet",
        ),
    ],
)
def test_part_adapters_construct_exact_isolated_tasks(
    source_path: Path, preparer: str, operation_type: str, handler: str
) -> None:
    source = _read_source(source_path)
    _, body = _one_function(source, preparer)
    compact = _compact(body)
    assert f"request.operationType=std::string(Part::{operation_type})" in compact
    assert "CollaborativeOperationPreparation::IsolatedTaskisolated{" in compact
    assert "std::move(request),std::move(" in compact
    assert "std::move(isolated)" in compact
    assert not _archive_handler_violations(source, handler)
    assert "PreparedEditExecutor" not in _suppress_cpp(source)
    assert "preparedEditExecutor" not in _suppress_cpp(source)


def test_boolean_test_probe_cannot_become_a_production_heavy_fallback() -> None:
    source = _read_source(BOOLEAN_SOURCE)
    _, production = _one_function(source, "prepareBoolean")
    assert "returnprepareBooleanImpl(document,intent,{})" in _compact(production)
    assert "DetachedTask" not in _suppress_cpp(production)

    _, implementation = _one_function(source, "prepareBooleanImpl")
    compact = _compact(implementation)
    assert compact.count("CollaborativeOperationPreparation::DetachedTasktask") == 1
    assert "if(probe){" in compact
    assert "else{App::GeometryArchiveinput" in compact
    assert "request.policy=App::PreparationPolicy::IsolatedProcess" in compact


def test_part_registration_binds_parent_adapters_and_worker_handlers() -> None:
    boolean = _read_source(BOOLEAN_SOURCE)
    _, boolean_registration = _one_function(
        boolean, "ensureCollaborativeBooleanOperationRegistered"
    )
    compact_boolean = _compact(boolean_registration)
    assert (
        "registerAdapter(std::string(CollaborativeBooleanOperationType),prepareBoolean)"
        in compact_boolean
    )
    assert (
        "registerOperation(std::string(CollaborativeBooleanOperationType),"
        "executeBooleanArchive)" in compact_boolean
    )

    sweep_fillet = _read_source(SWEEP_FILLET_SOURCE)
    _, registrations = _one_function(
        sweep_fillet, "ensureCollaborativeSweepFilletOperationsRegistered"
    )
    compact = _compact(registrations)
    for operation, preparer, handler in (
        ("CollaborativeSweepOperationType", "prepareSweep", "executeSweep"),
        ("CollaborativeFilletOperationType", "prepareFillet", "executeFillet"),
    ):
        assert f"registerAdapter(std::string({operation}),{preparer})" in compact
        assert f"registerOperation(std::string({operation}),{handler})" in compact

    module = _compact(_read_source(PART_MODULE))
    _assert_order(
        module,
        "Part::initModule()",
        "Part::ensureCollaborativeBooleanOperationRegistered()",
        "Part::ensureCollaborativeSweepFilletOperationsRegistered()",
    )


def test_part_worker_handlers_never_recover_live_document_authority() -> None:
    checks = (
        (BOOLEAN_SOURCE, "executeBooleanArchive"),
        (SWEEP_FILLET_SOURCE, "executeSweep"),
        (SWEEP_FILLET_SOURCE, "executeFillet"),
    )
    failures: list[str] = []
    for source_path, function in checks:
        failures.extend(
            f"{source_path}:{failure}"
            for failure in _archive_handler_violations(_read_source(source_path), function)
        )
    assert not failures, "\n".join(failures)


def test_native_adapter_sources_are_registered_in_cmake() -> None:
    app_cmake = _suppress_cpp(_read_source(APP_CMAKE), suppress_literals=False)
    part_cmake = _suppress_cpp(_read_source(PART_CMAKE), suppress_literals=False)
    for path in ("GeometryWorkerOperationRegistry.cpp", "GeometryWorkerOperationRegistry.h"):
        assert re.search(rf"(?m)^\s*{re.escape(path)}\s*$", app_cmake), path
    for path in (
        "CollaborativeBooleanOperation.cpp",
        "CollaborativeBooleanOperation.h",
        "CollaborativeSweepFilletOperations.cpp",
        "CollaborativeSweepFilletOperations.h",
    ):
        assert re.search(rf"(?m)^\s*{re.escape(path)}\s*$", part_cmake), path

    boolean_header = _compact(_read_source(BOOLEAN_HEADER))
    assert (
        'CollaborativeBooleanOperationType="Part.CollaborativeBoolean"'
        in boolean_header
    )
    sweep_header = _compact(_read_source(SWEEP_FILLET_HEADER))
    assert 'CollaborativeSweepOperationType="Part.CollaborativeSweep"' in sweep_header
    assert 'CollaborativeFilletOperationType="Part.CollaborativeFillet"' in sweep_header


def test_scanner_rejects_live_authority_but_ignores_comments_and_literals() -> None:
    safe = r'''
        App::GeometryArchive executeSweep(const App::GeometryArchive& input,
                                          const std::stop_token stopToken)
        {
            // App::DocumentObject is not executable source.
            const char* diagnostic = R"tag(App::Document *)tag";
            return input;
        }
    '''
    assert not _archive_handler_violations(safe, "executeSweep")

    unsafe = r'''
        App::GeometryArchive executeSweep(const App::GeometryArchive& input,
                                          const std::stop_token stopToken)
        {
            App::Document* document = App::GetApplication().activeDocument();
            return input;
        }
    '''
    violations = _archive_handler_violations(unsafe, "executeSweep")
    assert any("Document" in violation for violation in violations)
    assert any("GetApplication" in violation for violation in violations)


def test_isolated_task_scanner_rejects_a_second_authority_field() -> None:
    unsafe = """
        struct IsolatedTask {
            GeometryJobRequest request;
            GeometryArchive inputArchive;
            IsolatedResultDecoder decodeResult;
            void* liveAuthority;
        };
    """
    assert _isolated_task_field_violations(unsafe)


def test_function_parser_distinguishes_definitions_from_calls() -> None:
    source = """
        auto wrapper() { return executeFillet(input, token); }
        App::GeometryArchive executeFillet(
            const App::GeometryArchive& input,
            const std::stop_token stopToken) noexcept
        {
            if (ready(executeFillet(input, stopToken))) { return input; }
            return input;
        }
    """
    definitions = _function_bodies(source, "executeFillet")
    assert len(definitions) == 1
    assert "const App::GeometryArchive& input" in definitions[0][0]


def test_source_reader_preserves_ascii_scanning_with_non_utf8_bytes(tmp_path: Path) -> None:
    source = tmp_path / "worker.cpp"
    source.write_bytes(
        b"// vendor byte: \xff\n"
        b"GeometryArchive executeSweep(const GeometryArchive& input, std::stop_token token){}\n"
    )
    decoded = _read_source(source)
    assert "executeSweep" in decoded
    assert "\udcff" in decoded
