# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP11 gate for generic isolated feature recompute."""

from __future__ import annotations

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]

INVENTORY = "doc/document-collaboration-ingress-inventory.md"
DOCUMENT_HEADER = "src/App/Document.h"
DOCUMENT_SOURCE = "src/App/Document.cpp"
OBJECT_SOURCE = "src/App/DocumentObject.cpp"
PYTHON_SOURCE = "src/App/DocumentObjectPyImp.cpp"
GUI_SOURCE = "src/Gui/Document.cpp"
GENERIC_HEADER = "src/App/GenericIsolatedRecompute.h"
GENERIC_SOURCE = "src/App/GenericIsolatedRecompute.cpp"
PYTHON_FEATURE_HEADER = "src/App/FeaturePython.h"
WORKER_REGISTRY_HEADER = "src/App/GeometryWorkerOperationRegistry.h"
WORKER_SOURCE = "src/App/GeometryWorkerMain.cpp"
RECOMPUTE_SOURCE = "src/App/DocumentRecomputeCoordinator.cpp"
SERVICE_HEADER = "src/App/DocumentCollaborationService.h"
SERVICE_SOURCE = "src/App/DocumentCollaborationService.cpp"
COMMIT_SOURCE = "src/App/DocumentCommitCoordinator.cpp"
APP_CMAKE = "src/App/CMakeLists.txt"
APP_TEST_CMAKE = "tests/src/App/CMakeLists.txt"
NATIVE_TEST = "tests/src/App/GenericIsolatedRecompute.cpp"


def _read(path: str | Path) -> str:
    location = Path(path)
    if not location.is_absolute():
        location = REPO_ROOT / location
    return location.read_text(encoding="utf-8", errors="surrogateescape")


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
    if any(character.isspace() or character in "()\\" for character in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    closing = source.find(terminator, opening + 1)
    return len(source) if closing < 0 else closing + len(terminator)


def _quoted_literal_end(source: str, start: int) -> int:
    quote = source[start]
    cursor = start + 1
    escaped = False
    while cursor < len(source):
        character = source[cursor]
        cursor += 1
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == quote:
            break
    return cursor


def _blank(result: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if result[index] not in "\r\n":
            result[index] = " "


def _is_numeric_separator(source: str, index: int) -> bool:
    return (
        source[index] == "'"
        and index > 0
        and index + 1 < len(source)
        and source[index - 1].isdigit()
        and source[index + 1].isdigit()
    )


def _suppress_cpp(source: str, *, literals: bool = True) -> str:
    """Blank comments and optionally literals without moving line offsets."""

    result = list(source)
    cursor = 0
    while cursor < len(source):
        raw_end = _raw_literal_end(source, cursor)
        if raw_end is not None:
            if literals:
                _blank(result, cursor, raw_end)
            cursor = raw_end
            continue
        if source.startswith("//", cursor):
            end = source.find("\n", cursor + 2)
            end = len(source) if end < 0 else end
            _blank(result, cursor, end)
            cursor = end
            continue
        if source.startswith("/*", cursor):
            closing = source.find("*/", cursor + 2)
            end = len(source) if closing < 0 else closing + 2
            _blank(result, cursor, end)
            cursor = end
            continue
        if _is_numeric_separator(source, cursor):
            cursor += 1
            continue
        if source[cursor] in {'"', "'"}:
            end = _quoted_literal_end(source, cursor)
            if literals:
                _blank(result, cursor, end)
            cursor = end
            continue
        cursor += 1
    return "".join(result)


def _matching(source: str, opening: int, left: str, right: str) -> int | None:
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == left:
            depth += 1
        elif source[index] == right:
            depth -= 1
            if depth == 0:
                return index
    return None


def _is_definition_suffix(name: str, suffix: str) -> bool:
    parts = name.split("::")
    if len(parts) >= 2 and parts[-1] == parts[-2] and suffix.lstrip().startswith(":"):
        return True
    remainder = re.sub(r"\b(?:const|volatile|override|final)\b", "", suffix)
    remainder = re.sub(r"\bnoexcept(?:\s*\([^;{}]*\))?", "", remainder)
    return not remainder.strip()


def _function_bodies(source: str, name: str) -> list[tuple[str, str, str]]:
    stripped = _suppress_cpp(source)
    pattern = re.compile(r"(?<![A-Za-z0-9_:])" + re.escape(name) + r"\s*\(")
    result: list[tuple[str, str, str]] = []
    for match in pattern.finditer(stripped):
        opening_parenthesis = stripped.find("(", match.start())
        closing_parenthesis = _matching(stripped, opening_parenthesis, "(", ")")
        if closing_parenthesis is None:
            continue
        semicolon = stripped.find(";", closing_parenthesis + 1)
        opening_brace = stripped.find("{", closing_parenthesis + 1)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue
        suffix = stripped[closing_parenthesis + 1:opening_brace]
        if not _is_definition_suffix(name, suffix):
            continue
        closing_brace = _matching(stripped, opening_brace, "{", "}")
        if closing_brace is not None:
            result.append(
                (
                    stripped[match.start():opening_brace],
                    stripped[opening_brace + 1:closing_brace],
                    source[opening_brace + 1:closing_brace],
                )
            )
    return result


def _body(source: str, name: str, *, raw: bool = False) -> str:
    bodies = _function_bodies(source, name)
    assert len(bodies) == 1, f"expected one definition for {name}, found {len(bodies)}"
    return bodies[0][2 if raw else 1]


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def _wp11_inventory_rows() -> list[list[str]]:
    rows: list[list[str]] = []
    for line_number, line in enumerate(_read(INVENTORY).splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 9 and cells[7] == "CC-WP11":
            cells.append(str(line_number))
            rows.append(cells)
    return rows


def test_inventory_freezes_all_four_transitive_ingress_routes() -> None:
    rows = _wp11_inventory_rows()
    assert len(rows) == 4, f"expected four CC-WP11 inventory rows, found {len(rows)}"
    actual = {(row[1].strip("`"), row[2].split(" (candidate", 1)[0].strip("`")) for row in rows}
    assert actual == {
        (DOCUMENT_SOURCE, "Document::_recomputeFeature"),
        (OBJECT_SOURCE, "DocumentObject::recomputeFeature"),
        (PYTHON_SOURCE, "DocumentObjectPy::recompute"),
        (GUI_SOURCE, "Document::slotSkipRecompute"),
    }
    for row in rows:
        assert all(row[index] for index in range(9)), (
            f"inventory row {row[9]} has an empty required field"
        )
        assert (REPO_ROOT / row[1].strip("`")).is_file(), (
            f"inventory row {row[9]} names missing source {row[1]}"
        )
    classifications = {row[2].split(" (candidate", 1)[0].strip("`"): row[6] for row in rows}
    assert classifications["DocumentObjectPy::recompute"] == "typed adapter required"
    assert all(
        value == "migrate"
        for key, value in classifications.items()
        if key != "DocumentObjectPy::recompute"
    )


def test_private_feature_execution_has_only_full_recompute_and_detached_friend_callers() -> None:
    matches: list[str] = []
    for path in (REPO_ROOT / "src").rglob("*.cpp"):
        source = _suppress_cpp(_read(path))
        for match in re.finditer(r"\b_recomputeFeature\s*\(", source):
            line = source.count("\n", 0, match.start()) + 1
            matches.append(f"{path.relative_to(REPO_ROOT).as_posix()}:{line}")
    owners = [entry.rsplit(":", 1)[0] for entry in matches]
    assert owners.count(DOCUMENT_SOURCE) == 2, matches
    assert owners.count(GENERIC_SOURCE) == 1, matches
    assert len(matches) == 3, (
        "_recomputeFeature has an unclassified live caller: " + ", ".join(matches)
    )

    document = _read(DOCUMENT_SOURCE)
    full = _compact(_body(document, "Document::recompute"))
    facade = _compact(_body(document, "Document::recomputeFeature"))
    friend = _compact(_body(_read(GENERIC_SOURCE), "execute"))
    assert "_recomputeFeature(obj)" in full
    assert "_recomputeFeature(" not in facade
    assert "recomputeCoordinator()" in facade
    assert "makeGenericIsolatedRecomputeRequest(*this,*feature,recursive)" in facade
    assert "document.testStatus(Document::TempDoc)" in friend
    assert "feature.getDocument()!=&document" in friend
    assert "returndocument._recomputeFeature(&feature);" in friend


def test_documentobject_python_and_gui_delegate_to_the_isolated_document_facade() -> None:
    object_body = _compact(_body(_read(OBJECT_SOURCE), "DocumentObject::recomputeFeature"))
    assert "doc->recomputeFeature(this,recursive)" in object_body
    assert "_recomputeFeature" not in object_body

    python_body = _compact(_body(_read(PYTHON_SOURCE), "DocumentObjectPy::recompute"))
    assert "getDocumentObjectPtr()->recomputeFeature(Base::asBoolean(recursive))" in python_body
    assert "_recomputeFeature" not in python_body

    gui_body = _compact(_body(_read(GUI_SOURCE), "Document::slotSkipRecompute"))
    assert "obj->recomputeFeature(true)" in gui_body
    assert "_recomputeFeature" not in gui_body


def test_archive_protocol_is_bounded_schema_exact_and_fail_closed() -> None:
    source = _read(GENERIC_SOURCE)
    literals = _compact(_suppress_cpp(source, literals=False))
    for fragment in (
        "constexprstd::uint32_tProtocolMagic=0x31524947U;",
        "constexprstd::uint32_tProtocolVersion=1;",
        "constexprstd::size_tMaxObjects=10'000;",
        "constexprstd::size_tMaxProperties=1'000'000;",
        "constexprstd::size_tMaxFieldBytes=1U<<20;",
        "constexprstd::size_tMaxPayloadBytes=128U<<20;",
    ):
        assert fragment in literals, f"missing generic protocol bound: {fragment}"

    prepare = _compact(_suppress_cpp(_body(source, "prepareGenericRecompute", raw=True), literals=False))
    assert 'intent.arguments.size()!=1||!intent.arguments.contains("feature")' in prepare
    assert 'input.sections.push_back({"document.fcstd"' in prepare
    assert 'input.sections.push_back({"recompute.params",encodeParameters(' in prepare
    assert "PropertyLinkBase" in prepare
    assert "PropertyPythonObject" in prepare
    assert "GeometryJobRequestrequest" in prepare
    assert "request.policy=App::PreparationPolicy::IsolatedProcess" in prepare
    assert "IsolatedTaskisolated" in prepare

    execute = _compact(_suppress_cpp(_body(source, "executeGenericRecompute", raw=True), literals=False))
    document_section = execute.find('requireSection(input,"document.fcstd",2)')
    parameter_section = execute.find('requireSection(input,"recompute.params",2)')
    decode = execute.find("decodeParameters(parameterSection.bytes)")
    first_schema = execute.find("validateDetachedSchema(*detached,manifests)")
    baseline = execute.find("capturePropertySnapshots(*detached,manifests)")
    run = execute.find("GenericIsolatedRecomputeAccess::execute(*detached,*target)")
    second_schema = execute.find("validateDetachedSchema(*detached,manifests)", first_schema + 1)
    side_effect = execute.find('"genericrecomputeproducedanundeclaredpropertysideeffect:')
    publication = execute.find('{"recompute.outputs",encodeOutputs(')
    assert 0 <= document_section < parameter_section < decode < first_schema < baseline < run
    assert run < second_schema < side_effect < publication
    assert execute.count("validateDetachedSchema(*detached,manifests)") == 2

    decoder = _compact(_suppress_cpp(_body(source, "decodeResult", raw=True), literals=False))
    for fragment in (
        'requireSection(archive,"recompute.outputs",1)',
        "count>expectedOutputs.size()",
        "expected==expectedOutputs.end()||expected->second!=type",
        "!seen.insert(name).second",
        "reader.finish()",
    ):
        assert fragment in decoder, f"result decoder is not fail closed: {fragment}"


def test_worker_boundary_has_no_parent_authority_and_featurepython_is_explicit_opt_in() -> None:
    generic = _read(GENERIC_SOURCE)
    signature = _function_bodies(generic, "executeGenericRecompute")[0][0]
    assert "GeometryArchive" in signature and "stop_token" in signature
    assert "Document&" not in signature and "DocumentObject" not in signature

    worker_registry = _compact(_suppress_cpp(_read(WORKER_REGISTRY_HEADER)))
    assert (
        "usingGeometryWorkerOperation=std::function<GeometryArchive("
        "constGeometryArchive&,std::stop_token)>;"
    ) in worker_registry
    assert "Document*" not in worker_registry
    assert "DocumentObject*" not in worker_registry

    closure = _compact(_body(generic, "collectClosure"))
    assert "object->getDocument()!=&document" in closure
    assert "dependency->getDocument()!=&document" in closure
    assert "!object->canRecomputeOnWorker()" in closure

    python_opt_in = _compact(_body(_read(PYTHON_FEATURE_HEADER), "canRecomputeOnWorker"))
    assert "!FeatureT::canRecomputeOnWorker()" in python_opt_in
    assert "imp->supportsAsyncRecompute()==FeaturePythonImp::Accepted" in python_opt_in


def test_recompute_commit_is_private_and_uses_the_deferred_dcc_policy() -> None:
    service_header = _read(SERVICE_HEADER)
    private = service_header.split("private:", 1)[1]
    assert "friend class DocumentRecomputeCoordinator;" in private
    assert "commitRecomputeEdit(" in private
    assert service_header.find("private:") < service_header.find("commitRecomputeEdit(")

    recompute = _compact(_body(_read(RECOMPUTE_SOURCE), "DocumentRecomputeCoordinator::poll"))
    assert "_service.commitRecomputeEdit(sessionId,*terminal->preparedEdit)" in recompute
    assert "_service.commitEdit(" not in recompute

    service = _compact(_body(
        _read(SERVICE_SOURCE),
        "DocumentCollaborationService::commitRecomputeEditOnDocumentThread",
    ))
    assert "_coordinator.commitWithPreparationPolicyAndRecompute(" in service
    assert "CollaborationCompatibilityRecomputePolicy::Deferred" in service

    commit = _compact(_body(
        _read(COMMIT_SOURCE),
        "DocumentCommitCoordinator::commitOnDocumentThreadWithOptions",
    ))
    deferred = commit.find(
        "recomputePolicy==CollaborationCompatibilityRecomputePolicy::Deferred"
    )
    fence = commit.find("_document.openCollaborationDeferredRecomputeFence()", deferred)
    apply = commit.find("operation.apply(_document)", fence)
    assert 0 <= deferred < fence < apply


def test_recompute_capture_allows_only_touched_state_beyond_normal_capture() -> None:
    document = _compact(_body(
        _read(DOCUMENT_SOURCE), "Document::collaborationRecomputeCaptureBlocked"
    ))
    normal = _compact(_body(
        _read(DOCUMENT_SOURCE), "Document::collaborationStableReadBlocked"
    ))
    assert "mustExecute()" in normal
    assert "mustExecute()" not in document
    for boundary in (
        "collaborationCommitNotificationBarrier",
        "collaborationReplayingNotifications",
        "hasPendingTransaction()",
        "transacting()",
        "getBookedTransactionID()!=0",
        "isTransactionLocked()",
        "collaborationRecomputeTeardownDepth",
        "pendingRemovalProcessing",
        "!d->pendingRemove.empty()",
    ):
        assert boundary in document

    service = _compact(_body(
        _read(SERVICE_SOURCE),
        "DocumentCollaborationService::prepareEditAsyncOnDocumentThread",
    ))
    assert "intent.operationType==GenericIsolatedRecomputeOperationType" in service
    assert "collaborationRecomputeCaptureBlocked()" in service

    recompute = _compact(_body(_read(RECOMPUTE_SOURCE), "DocumentRecomputeCoordinator::poll"))
    assert "_service.takeRecomputePreparedEdit(" in recompute
    assert "_service.takePreparedEdit(" not in recompute


def test_worker_registers_app_before_optional_part_and_cmake_wires_native_coverage() -> None:
    worker = _compact(_suppress_cpp(_body(
        _read(WORKER_SOURCE), "App::Internal::runGeometryWorkerMain", raw=True
    ), literals=False))
    registry = worker.find("GeometryWorkerOperationRegistry::instance()")
    app = worker.find("Internal::ensureGenericIsolatedRecomputeRegistered()", registry)
    first_check = worker.find("if(!registry.contains(operation))", app)
    part = worker.find('Base::Interpreter().runString("importPart")', first_check)
    second_check = worker.find("if(!registry.contains(operation))", part)
    rejection = worker.find("return13", second_check)
    execute = worker.find("registry.execute(operation,*input.archive", rejection)
    assert 0 <= registry < app < first_check < part < second_check < rejection < execute
    assert worker.count("registry.execute(") == 1

    app_cmake = _read(APP_CMAKE)
    test_cmake = _read(APP_TEST_CMAKE)
    assert "GenericIsolatedRecompute.cpp" in app_cmake
    assert "GenericIsolatedRecompute.h" in app_cmake
    assert "GenericIsolatedRecompute.cpp" in test_cmake
    assert (REPO_ROOT / NATIVE_TEST).is_file()


def test_scanners_ignore_comments_literals_and_read_non_utf8_losslessly(tmp_path: Path) -> None:
    source = r'''
        // document._recomputeFeature(&feature);
        const char* text = "Document::_recomputeFeature(";
        const char* raw = R"gate(recomputeFeature() document.fcstd)gate";
        int Document::recomputeFeature(DocumentObject* feature, bool recursive)
        {
            return feature && recursive;
        }
    '''
    assert len(_function_bodies(source, "Document::recomputeFeature")) == 1
    assert "_recomputeFeature" not in _suppress_cpp(source)

    path = tmp_path / "Generic.cpp"
    path.write_bytes(b"// non-UTF-8: \xff\nint safe() { return 1; }\n")
    decoded = _read(path)
    assert "\udcff" in decoded
    assert len(_function_bodies(decoded, "safe")) == 1


def test_low_level_scanner_rejects_an_injected_live_caller(tmp_path: Path) -> None:
    injected = tmp_path / "Bypass.cpp"
    injected.write_text(
        "void bypass(App::Document& document, App::DocumentObject* object) "
        "{ document._recomputeFeature(object); }",
        encoding="utf-8",
    )
    source = _suppress_cpp(_read(injected))
    matches = list(re.finditer(r"\b_recomputeFeature\s*\(", source))
    assert len(matches) == 1
