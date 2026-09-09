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


def test_private_feature_execution_is_limited_to_compatibility_and_worker_kernels() -> None:
    matches: list[str] = []
    for path in (REPO_ROOT / "src").rglob("*.cpp"):
        source = _suppress_cpp(_read(path))
        for match in re.finditer(r"\b_recomputeFeature\s*\(", source):
            line = source.count("\n", 0, match.start()) + 1
            matches.append(f"{path.relative_to(REPO_ROOT).as_posix()}:{line}")
    owners = [entry.rsplit(":", 1)[0] for entry in matches]
    # Document.cpp contributes the private definition plus the full-document
    # and one-feature synchronous compatibility calls.
    assert owners.count(DOCUMENT_SOURCE) == 3, matches
    assert owners.count(GENERIC_SOURCE) == 1, matches
    assert len(matches) == 4, (
        "_recomputeFeature has an unclassified live caller: " + ", ".join(matches)
    )

    document = _read(DOCUMENT_SOURCE)
    full = _compact(_body(document, "Document::recompute"))
    facade = _compact(_body(document, "Document::recomputeFeature"))
    generic_source = _read(GENERIC_SOURCE)
    friend = _compact(_body(generic_source, "execute"))
    derived = full.find("if(collaborationDerivedRecomputeGranted()){")
    async_call = full.find("recomputeAsync(objs,force,options)", derived)
    live_call = full.find("_recomputeFeature(object)", async_call)
    assert 0 <= derived < async_call < live_call
    direct_feature = facade.find("if(!collaborationDerivedRecomputeGranted()){")
    direct_feature_call = facade.find("_recomputeFeature(feature)", direct_feature)
    coordinator = facade.find("recomputeCoordinator()", direct_feature_call)
    assert 0 <= direct_feature < direct_feature_call < coordinator
    # Only the internal derived branch enters the coordinator. Ordinary
    # synchronous feature calls keep the native compatibility behavior.
    assert "makeGenericIsolatedRecomputeRequest(" in facade
    assert "*this,*feature,recursive" in facade
    assert "owner_thread_execution" not in facade

    temp_document = friend.find("document.testStatus(Document::TempDoc)")
    ownership = friend.find("feature.getDocument()!=&document", temp_document)
    attached = friend.find("!feature.isAttachedToDocument()", ownership)
    private_call = friend.find("returndocument._recomputeFeature(&feature);", attached)
    assert 0 <= temp_document < ownership < attached < private_call
    assert "executeAuthoritative" not in generic_source

    legacy_matches: list[str] = []
    for path in (REPO_ROOT / "src").rglob("*"):
        if path.suffix not in {".cpp", ".h", ".hpp"}:
            continue
        source = _read(path)
        for match in re.finditer(r"\brecomputeLegacy\s*\(", source):
            line = source.count("\n", 0, match.start()) + 1
            legacy_matches.append(
                f"{path.relative_to(REPO_ROOT).as_posix()}:{line}"
            )
    assert not legacy_matches, (
        "recomputeLegacy remains in production source: " + ", ".join(legacy_matches)
    )

    document_rollback = _compact(
        _body(document, "Document::rollbackCollaborationTransaction")
    )
    assert "recompute(" not in document_rollback
    assert "recomputeAsync(" not in document_rollback

    commit_source = _read(COMMIT_SOURCE)
    coordinator = _compact(
        _body(
            commit_source,
            "DocumentCommitCoordinator::commitOnDocumentThreadWithOptions",
        )
    )
    eager = coordinator.find(
        "if(recomputePolicy==CollaborationCompatibilityRecomputePolicy::Eager){"
    )
    assert eager >= 0
    eager_opening = coordinator.find("{", eager)
    eager_closing = _matching(coordinator, eager_opening, "{", "}")
    assert eager_closing is not None
    eager_stage = coordinator[eager_opening + 1:eager_closing]
    scoped_targets = eager_stage.find(
        "autorecomputeTargets=pendingTransactionRecomputeTargets(_document);"
    )
    empty = eager_stage.find("if(recomputeTargets.empty()){", scoped_targets)
    empty_finalize = eager_stage.find(
        "_document.finalizeEmptyDetachedRecompute();", empty
    )
    derived_grant = eager_stage.find(
        "autoderivedRecompute=_document.openCollaborationDerivedRecomputeGrant();"
    )
    detached_recompute = eager_stage.find(
        "static_cast<void>(_document.recompute("
        "recomputeTargets,true,&recomputeHasError));",
        derived_grant,
    )
    assert 0 <= scoped_targets < empty < empty_finalize < derived_grant < detached_recompute
    assert eager_stage.count("openCollaborationDerivedRecomputeGrant()") == 1
    assert eager_stage.count("_document.recompute(") == 1
    assert "structuralCompatibility" not in eager_stage
    assert "openCollaborationStructuralRecomputeGrant(" not in eager_stage
    assert "recomputeLegacy(" not in eager_stage
    assert "trustedStructural" not in eager_stage

    rollback = _compact(
        _body(
            commit_source,
            "DocumentCommitCoordinator::rollbackNativeCommitTransaction",
        )
    )
    rollback_literals = _compact(
        _suppress_cpp(
            _body(
                commit_source,
                "DocumentCommitCoordinator::rollbackNativeCommitTransaction",
                raw=True,
            ),
            literals=False,
        )
    )
    initial_rollback = rollback.find(
        "autorollback=_document.rollbackCollaborationTransaction();"
    )
    recovery_lambda = rollback.find(
        "constautorestoreFailedStabilization=[&]()noexcept{",
        initial_rollback,
    )
    recovery_opening = rollback.find("{", recovery_lambda)
    recovery_closing = _matching(rollback, recovery_opening, "{", "}")
    assert recovery_closing is not None
    recovery_stage = rollback[recovery_opening + 1:recovery_closing]
    assert (
        recovery_stage.count("_document.rollbackCollaborationTransaction()")
        == 1
    )
    open_stabilization = rollback.find(
        "openNativeCommitTransaction(,false)",
        recovery_closing,
    )
    derived_grant = rollback.find(
        "_document.openCollaborationDerivedRecomputeGrant()",
        open_stabilization,
    )
    detached_recompute = rollback.find(
        "_document.recompute(recomputeTargets,true,&recomputeHasError)",
        derived_grant,
    )
    failed_recompute = rollback.find("if(recomputeHasError){", detached_recompute)
    recovery_call = rollback.find(
        "if(restoreFailedStabilization()){",
        failed_recompute,
    )
    assert (
        0
        <= initial_rollback
        < recovery_lambda
        < recovery_closing
        < open_stabilization
        < derived_grant
        < detached_recompute
        < failed_recompute
        < recovery_call
    )
    assert rollback.count("_document.rollbackCollaborationTransaction()") == 2
    assert rollback.find(
        "_document.rollbackCollaborationTransaction()", initial_rollback
    ) == initial_rollback + len("autorollback=")
    assert (
        'openNativeCommitTransaction("Detachedrollbackstabilization",false)'
        in rollback_literals
    )


def test_documentobject_python_and_gui_delegate_to_the_public_sync_facade() -> None:
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
        "constexprstd::uint32_tProtocolVersion=2;",
        "constexprstd::size_tMaxObjects=10'000;",
        "constexprstd::size_tMaxProperties=1'000'000;",
        "constexprstd::size_tMaxFieldBytes=1U<<20;",
        "constexprstd::size_tMaxPayloadBytes=128U<<20;",
    ):
        assert fragment in literals, f"missing generic protocol bound: {fragment}"

    prepare = _compact(
        _suppress_cpp(
            _body(source, "prepareGenericRecompute", raw=True), literals=False
        )
    )
    assert (
        'constautolegacyMode=intent.arguments.find("legacy_revision_semantics")'
        in prepare
    )
    assert 'constautoforceMode=intent.arguments.find("force_execution")' in prepare
    assert (
        'constautoidentityArgument=intent.arguments.find("stable_object_identity")'
        in prepare
    )
    assert "intent.arguments.empty()||intent.arguments.size()>4" in prepare
    assert '!intent.arguments.contains("feature")' in prepare
    assert "identityArgument==intent.arguments.end()" in prepare
    assert "std::ranges::any_of(intent.arguments" in prepare
    assert 'argument.first!="legacy_revision_semantics"' in prepare
    assert 'argument.first!="force_execution"' in prepare
    assert 'argument.first!="stable_object_identity"' in prepare
    assert (
        "constboolpreserveLegacyRevisionSemantics="
        "legacyMode!=intent.arguments.end()" in prepare
    )
    assert 'preserveLegacyRevisionSemantics&&legacyMode->second!="1"' in prepare
    assert "constboolforceExecution=forceMode!=intent.arguments.end()" in prepare
    assert 'forceExecution&&forceMode->second!="1"' in prepare
    schema_rejection = prepare.find("throwstd::invalid_argument(")
    target_lookup = prepare.find('intent.arguments.at("feature")')
    assert 0 <= schema_rejection < target_lookup
    assert 'input.sections.push_back({"document.fcstd"' in prepare
    assert 'input.sections.push_back({"recompute.params",encodeParameters(' in prepare
    assert "PropertyLinkBase" in prepare
    assert "PropertyPythonObject" in prepare
    assert "GeometryJobRequestrequest" in prepare
    assert "request.policy=App::PreparationPolicy::IsolatedProcess" in prepare
    assert "IsolatedTaskisolated" in prepare

    execute = _compact(
        _suppress_cpp(
            _body(source, "executeGenericRecompute", raw=True), literals=False
        )
    )
    document_section = execute.find('requireSection(input,"document.fcstd",2)')
    parameter_section = execute.find('requireSection(input,"recompute.params",2)')
    decode = execute.find("decodeParameters(parameterSection.bytes)")
    first_schema = execute.find("validateDetachedSchema(*detached,manifests)")
    closure_contract = execute.find(
        "hasAuditedArchiveContract(*object,*target)", first_schema
    )
    target_contract = execute.find(
        "!hasAuditedCompleteWorkerResultContract(*target)", closure_contract
    )
    baseline = execute.find("capturePropertySnapshots(*detached,manifests)")
    run = execute.find("GenericIsolatedRecomputeAccess::execute(*detached,*target)")
    second_schema = execute.find("validateDetachedSchema(*detached,manifests)", first_schema + 1)
    failure_branch = execute.find("if(result!=0){", second_schema)
    failure_publication = execute.find(
        '{"recompute.outputs",encodeFailure(targetName,failureDiagnostic)}',
        failure_branch,
    )
    side_effect = execute.find('"genericrecomputeproducedanundeclaredpropertysideeffect:')
    publication = execute.find('{"recompute.outputs",encodeOutputs(')
    assert (
        0
        <= document_section
        < parameter_section
        < decode
        < first_schema
        < closure_contract
        < target_contract
        < baseline
        < run
    )
    assert run < second_schema < failure_branch < failure_publication < side_effect < publication
    assert execute.count("validateDetachedSchema(*detached,manifests)") == 2

    success_encoder = _compact(
        _suppress_cpp(_body(source, "encodeOutputs", raw=True), literals=False)
    )
    success_magic = success_encoder.find("appendU32(result,ProtocolMagic)")
    success_version = success_encoder.find(
        "appendU32(result,ProtocolVersion)", success_magic
    )
    success_target = success_encoder.find("appendString(result,targetName)", success_version)
    success_status = success_encoder.find("appendU32(result,1)", success_target)
    success_count = success_encoder.find(
        "appendU32(result,static_cast<std::uint32_t>(changed.size()))",
        success_status,
    )
    assert 0 <= success_magic < success_version < success_target < success_status < success_count

    failure_encoder = _compact(
        _suppress_cpp(_body(source, "encodeFailure", raw=True), literals=False)
    )
    assert 'if(diagnostic.empty()){diagnostic="detachedfeaturerecomputefailed";}' in failure_encoder
    failure_magic = failure_encoder.find("appendU32(result,ProtocolMagic)")
    failure_version = failure_encoder.find(
        "appendU32(result,ProtocolVersion)", failure_magic
    )
    failure_target = failure_encoder.find("appendString(result,targetName)", failure_version)
    failure_status = failure_encoder.find("appendU32(result,0)", failure_target)
    failure_diagnostic = failure_encoder.find(
        "appendString(result,diagnostic)", failure_status
    )
    assert 0 <= failure_magic < failure_version < failure_target < failure_status < failure_diagnostic

    decoder = _compact(
        _suppress_cpp(_body(source, "decodeResult", raw=True), literals=False)
    )
    for fragment in (
        'requireSection(archive,"recompute.outputs",1)',
        "constautosucceeded=reader.u32()",
        "succeeded>1",
        "if(succeeded==0)",
        "diagnostic.empty()",
        "count>expectedOutputs.size()",
        "expected==expectedOutputs.end()||expected->second!=type",
        "!seen.insert(name).second",
        "reader.finish()",
    ):
        assert fragment in decoder, f"result decoder is not fail closed: {fragment}"

    effects_decoder = _compact(
        _suppress_cpp(
            _body(source, "decodeLegacyPublicationEffects", raw=True),
            literals=False,
        )
    )
    failure_status = effects_decoder.find("if(succeeded==0){")
    failure_filter = effects_decoder.find("std::erase_if(effects", failure_status)
    model_only = effects_decoder.find(
        "effect.key!=App::DocumentRevisionKey::objectModel(target)",
        failure_filter,
    )
    unknown_only = effects_decoder.find(
        "App::DocumentRevisionKind::UnknownModelMutation", model_only
    )
    unit_delta = effects_decoder.find("effect.revisionDelta=1", unknown_only)
    assert 0 <= failure_status < failure_filter < model_only < unknown_only < unit_delta

    generic_operation = source.split("class GenericRecomputeOperation", 1)[1]
    generic_operation = generic_operation.split(
        "class GenericRecomputeBookkeepingOperation", 1
    )[0]
    failure_apply = _compact(_body(generic_operation, "apply"))
    assert (
        "if(_failureDiagnostic){"
        "App::Internal::GenericIsolatedRecomputeAccess::applyFailure("
        "document,*target,*_failureDiagnostic);return;}" in failure_apply
    )
    # The isolated result records how detached execute() went before owner-
    # thread application. It must not report a raising feature as a success.
    outcomes = [
        _compact(body[1])
        for body in _function_bodies(source, "recomputeOutcomeSucceeded")
    ]
    assert len(outcomes) == 1, outcomes
    for outcome in outcomes:
        assert "return!_failureDiagnostic.has_value();" in outcome


def test_worker_boundary_has_no_parent_authority_and_featurepython_is_rejected() -> None:
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
    contract = closure.find("!hasAuditedArchiveContract(*object,target)")
    dependencies = closure.find("object->getOutList()", contract)
    assert 0 <= contract < dependencies

    python_boundary = _compact(
        _body(_read(PYTHON_FEATURE_HEADER), "canRecomputeOnWorker")
    )
    assert python_boundary == "returnfalse;"
    assert "supportsAsyncRecompute" not in python_boundary


def test_worker_admission_freezes_exact_types_properties_and_schema() -> None:
    source = _read(GENERIC_SOURCE)

    def string_literals(symbol: str) -> list[str]:
        body = _suppress_cpp(_body(source, symbol, raw=True), literals=False)
        return re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', body)

    assert string_literals("hasAuditedWorkerResultTypeId") == [
        "App::FeatureTest",
        "App::FeatureTestException",
        "App::FeatureTestColumn",
        "App::FeatureTestRow",
        "App::FeatureTestAbsAddress",
        "App::FeatureTestPlacement",
    ]
    audited_types = _compact(_body(source, "hasAuditedWorkerResultTypeId"))
    assert "object.getTypeId()==type" in audited_types
    assert "isDerivedFrom" not in audited_types
    # These exact types may settle proven no-execute bookkeeping. App::Link is
    # deliberately absent: it is admissible only as a closure input.
    assert string_literals("hasKnownInertBookkeepingTypeId") == [
        "App::DocumentObject",
        "App::FeaturePython",
    ]
    inert_types = _compact(_body(source, "hasKnownInertBookkeepingTypeId"))
    assert "object.getTypeId()==type" in inert_types
    assert "isDerivedFrom" not in inert_types
    assert string_literals("hasAuditedArchivePropertyTypeId") == [
        "App::PropertyString",
        "App::PropertyExpressionEngine",
        "App::PropertyBool",
        "App::PropertyInteger",
        "App::PropertyFloat",
        "App::PropertyBoolList",
        "App::PropertyPath",
        "App::PropertyStringList",
        "App::PropertyEnumeration",
        "App::PropertyIntegerConstraint",
        "App::PropertyFloatConstraint",
        "App::PropertyColor",
        "App::PropertyColorList",
        "App::PropertyMaterial",
        "App::PropertyMaterialList",
        "App::PropertyDistance",
        "App::PropertyAngle",
        "App::PropertyIntegerList",
        "App::PropertyFloatList",
        "App::PropertyLink",
        "App::PropertyLinkSub",
        "App::PropertyLinkList",
        "App::PropertyLinkSubList",
        "App::PropertyVector",
        "App::PropertyVectorList",
        "App::PropertyMatrix",
        "App::PropertyPlacement",
        "App::PropertyQuantity",
        # Exact App::Link closure inputs have three additional built-in
        # structural value types.  The object type discriminator itself is
        # part of the same closed admission function.
        "App::Link",
        "App::PropertyXLink",
        "App::PropertyLinkSubHidden",
        "App::PropertyPlacementList",
    ]

    extensions = _compact(_body(source, "hasCanonicalExtensionSet"))
    for fragment in (
        "App::Extension::getExtensionClassTypeId()",
        "object.getExtensionsDerivedFrom(extensionType)",
        "canonical.getExtensionsDerivedFrom(extensionType)",
        "actualExtensions.size()!=canonicalExtensions.size()",
        "for(constauto*canonicalExtension:canonicalExtensions)",
        "object.getExtension(type,false,true)",
        "typeid(*actualExtension)!=typeid(*canonicalExtension)",
    ):
        assert fragment in extensions, f"canonical extension gate omitted {fragment}"
    assert extensions.count("getExtensionsDerivedFrom(extensionType)") == 2
    assert extensions.count("object.getExtension(type,false,true)") == 1

    property_types = _compact(_body(source, "hasAuditedArchivePropertyTypeId"))
    assert "property.getTypeId()==type" in property_types
    assert "object.getTypeId()!=linkType" in property_types
    assert "typeid(object)!=typeid(App::Link)" in property_types
    exact_runtime = _compact(_body(source, "hasExactRegisteredRuntimeType"))
    assert "typeid(*registeredType)==typeid(object)" in exact_runtime

    schema = _compact(_body(source, "hasCanonicalAuditedArchiveSchema"))
    for fragment in (
        "typeid(*registered)!=typeid(object)",
        "!hasCanonicalExtensionSet(object,*registered)",
        "actual.size()!=canonical.size()",
        "actualName!=canonicalName",
        "actualProperty->testStatus(App::Property::PropDynamic)",
        "!hasAuditedArchivePropertyTypeId(object,*actualProperty)",
        "actualProperty->getTypeId()!=canonicalProperty->getTypeId()",
        "actualProperty->getType()!=canonicalProperty->getType()",
        "(1UL<<App::Property::Touched)|(1UL<<App::Property::Busy)",
        "(actualProperty->getStatus()&~volatileStatusMask)!="
        "(canonicalProperty->getStatus()&~volatileStatusMask)",
        "actualLink->getScope()!=canonicalLink->getScope()",
        "for(intflag=App::PropertyLinkBase::LinkAllowExternal;"
        "flag<=App::PropertyLinkBase::LinkSilentRestore;++flag)",
        "actualLink->testFlag(flag)!=canonicalLink->testFlag(flag)",
        "App::Prop_Transient|App::Prop_NoPersist",
        "!sameSerializedProperty(*actualProperty,*canonicalProperty)",
    ):
        assert fragment in schema, f"canonical archive schema omitted {fragment}"

    prepare = _compact(_body(source, "prepareGenericRecompute"))
    audited = prepare.find(
        "constboolauditedWorkerType=hasAuditedWorkerResultTypeId(*target)"
    )
    inert = prepare.find(
        "constboolknownInertType=hasKnownInertBookkeepingTypeId(*target)"
    )
    unknown_rejection = prepare.find("if(!auditedWorkerType&&!knownInertType){", inert)
    exact_runtime = prepare.find(
        "if(!hasExactRegisteredRuntimeType(*target)){", unknown_rejection
    )
    archive_contract = prepare.find(
        "!hasCanonicalAuditedArchiveSchema(*target)", exact_runtime
    )
    closure = prepare.find("collectClosure(document,*target)", archive_contract)
    assert 0 <= audited < inert < unknown_rejection < exact_runtime < archive_contract < closure


def test_archive_contract_rejects_extensions_expressions_python_and_dynamic_schema() -> None:
    source = _read(GENERIC_SOURCE)
    contract = _compact(_body(source, "hasAuditedArchiveContract"))
    for fragment in (
        "constboolauditedExecutable=hasAuditedWorkerResultTypeId(object)",
        "constboolauditedInputLink=&object!=&target",
        "if(!auditedExecutable&&!auditedInputLink){returnfalse;}",
        "if(!hasExactRegisteredRuntimeType(object)){returnfalse;}",
        "!object.canRecomputeOnWorker()",
        "object.hasExtensions()",
        "hasPythonObjectProperty(object)",
        "!object.ExpressionEngine.getExpressions().empty()",
        "!isSafePlainAppLinkArchiveInput(object)",
        "returnhasCanonicalAuditedArchiveSchema(object);",
    ):
        assert fragment in contract, f"archive contract omitted {fragment}"

    safe_link = _compact(_body(source, "isSafePlainAppLinkArchiveInput"))
    assert "typeid(object)!=typeid(App::Link)" in safe_link
    assert "!hasCanonicalExtensionSet(object,*canonical)" in safe_link
    assert "object.mustExecute()!=0" in safe_link
    assert "link->ElementCount.getValue()!=0" in safe_link
    assert "App::LinkBaseExtension::CopyOnChangeDisabled" in safe_link
    assert "!object.ExpressionEngine.getExpressions().empty()" in safe_link
    assert "entry.second->testStatus(App::Property::PropDynamic)" in safe_link
    assert "linked->getPropertyByName(" in safe_link
    assert "App::PropertyPythonObject" in safe_link

    declared = _compact(_body(source, "isDeclaredRecomputeOutput"))
    assert declared.endswith("returncompatibleOutput;")
    assert "hasExtension" not in declared


def test_worker_import_is_a_state_transfer_that_skips_schema_migrations() -> None:
    """The worker archive is written by this build from already-migrated state.

    It is a same-version transfer, not a request to reinterpret an old file.
    Freeze the import scope and the shared predicate used by restore hooks. The
    presence of this defense does not admit any otherwise unsupported type.
    """
    execute = _compact(_body(_read(GENERIC_SOURCE), "executeGenericRecompute"))
    assert "App::Document::CurrentSchemaTransfer,detached" in execute
    # The locker has to cover the import itself, not merely exist in the function.
    transfer = execute.split("App::Document::CurrentSchemaTransfer,detached", 1)[1]
    assert transfer.startswith(");static_cast<void>(importer.importObjects(archiveStream));")

    document_header = _compact(_suppress_cpp(_read(DOCUMENT_HEADER)))
    assert "CurrentSchemaTransfer=15" in document_header

    predicate = _compact(_body(_read(OBJECT_SOURCE), "DocumentObject::isRestoringDeprecatedSchema"))
    assert "doc->testStatus(Document::CurrentSchemaTransfer)" in predicate


def test_unconditional_executors_are_never_bookkeeping_only_recompute_targets() -> None:
    """mustExecute() does not answer whether execute() is a no-op.

    It answers whether dependents must re-run.  The in-process recompute never
    conflated the two -- it executes every touched object -- so a class whose
    execute() does unconditional work is invisible to the mustRecompute()
    shortcut, and short-circuiting it purges the touch and drops the work.
    AssemblyObject::execute() is one proven case: it runs the joint solver even
    though mustExecute() does not declare that work.
    """
    generic = _read(GENERIC_SOURCE)
    bookkeeping = _compact(_body(generic, "isBookkeepingOnlyTarget"))
    assert bookkeeping.startswith("if(owesUnconditionalExecuteWork(object)){returnfalse;}")
    assert "object.mustRecompute()==0" in bookkeeping

    literal = _compact(_body(generic, "owesUnconditionalExecuteWork", raw=True))
    assert 'Base::Type::fromName("Assembly::AssemblyObject")' in literal
    predicate = _compact(_body(generic, "owesUnconditionalExecuteWork"))
    assert "object.getTypeId().isDerivedFrom(assemblyType)" in predicate

    # The premise: the assembly really does solve from execute() without
    # declaring that work through mustExecute().
    assembly = _read("src/Mod/Assembly/App/AssemblyObject.cpp")
    assert "solve(false);" in _compact(_body(assembly, "AssemblyObject::execute"))
    assert "AssemblyObject::mustExecute" not in assembly


def test_assembly_solving_is_not_reproducible_in_the_worker() -> None:
    """solve() writes its whole product onto objects other than the target.

    ensureIdentityPlacements() normalises member link groups and
    setNewPlacements() moves every jointed part.  The worker publishes only the
    recomputed target's own declared outputs, so a solve that runs detached
    converges and is then discarded, leaving the parts where they were.  The
    assembly therefore remains outside the exact audited worker type list. Its
    historical worker-affinity opt-out is retained as defense in depth, not as
    archive admission authority.
    """
    header = _compact(_suppress_cpp(_read("src/Mod/Assembly/App/AssemblyObject.h")))
    assert "boolcanRecomputeOnWorker()constoverride{returnfalse;}" in header

    # The premise: solve() really does write through to other objects.
    assembly = _read("src/Mod/Assembly/App/AssemblyObject.cpp")
    solve = _compact(_body(assembly, "AssemblyObject::solve"))
    assert "ensureIdentityPlacements();" in solve
    assert "setNewPlacements();" in solve
    identity = _compact(_body(assembly, "AssemblyObject::ensureIdentityPlacements"))
    assert "pPlc->setValue(Base::Placement());" in identity

    # And the worker really does publish only the target's own outputs.
    generic = _compact(_body(_read(GENERIC_SOURCE), "executeGenericRecompute"))
    assert "encodeOutputs(targetName,*target,*targetManifest,baseline)" in generic


def test_unaudited_types_have_no_implicit_async_live_fallback() -> None:
    """Thread-affinity hooks never grant generic archive or commit authority."""
    generic = _read(GENERIC_SOURCE)
    prepare = _compact(_body(generic, "prepareGenericRecompute"))
    unknown = "if(!auditedWorkerType&&!knownInertType){"
    exact_runtime = "if(!hasExactRegisteredRuntimeType(*target)){"
    inert_contract = (
        "constboolprovenInertBookkeepingContract=knownInertType&&"
        "(isSafePlainDocumentObjectBookkeepingTarget(*target)||"
        "isNullProxyExternalLinkHolderBookkeepingTarget(*target));"
    )
    assert unknown in prepare
    assert exact_runtime in prepare
    assert inert_contract in prepare
    assert prepare.index(unknown) < prepare.index(exact_runtime)
    assert prepare.index(inert_contract) < prepare.index("isBookkeepingOnlyTarget(*target)")
    assert prepare.index(inert_contract) < prepare.index("collectClosure(document,*target)")
    assert "owner_thread_execution" not in generic
    assert "GenericRecomputeInProcessOperation" not in generic
    assert "executeAuthoritative" not in generic

    bookkeeping = _compact(_body(generic, "isBookkeepingOnlyTarget"))
    assert "isSafePlainDocumentObjectBookkeepingTarget(object)" in bookkeeping
    assert "isNullProxyExternalLinkHolderBookkeepingTarget(object)" in bookkeeping
    assert "isSafePlainAppLinkArchiveInput(object)" not in bookkeeping
    null_proxy = _compact(_body(generic, "isNullProxyExternalLinkHolderBookkeepingTarget"))
    assert "!proxy->getValue().isNone()" in null_proxy
    assert "isSafePlainAppLinkArchiveInput(*dependency)" in null_proxy

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
    assert "return_coordinator.commitRecompute(edit);" in service
    assert service.count("_coordinator.commitRecompute(edit)") == 1

    routing = _compact(_body(
        _read(COMMIT_SOURCE),
        "DocumentCommitCoordinator::commitRecompute",
    ))
    grant = routing.find("if(_document.collaborationDerivedRecomputeGranted()){")
    grant_open = routing.find("{", grant)
    grant_close = _matching(routing, grant_open, "{", "}")
    assert grant_close is not None
    derived = routing.find(
        "returncommitDerivedRecomputeInActiveTransaction(edit);", grant_open
    )
    ordinary = routing.find("returncommitWithPreparationPolicyAndOptions(", grant_close)
    assert ordinary >= 0
    ordinary_open = routing.find("(", ordinary)
    ordinary_close = _matching(routing, ordinary_open, "(", ")")
    assert ordinary_close is not None
    ordinary_arguments = routing[ordinary_open + 1:ordinary_close].split(",")
    assert ordinary_arguments == [
        "edit",
        "false",
        "false",
        "CollaborationCompatibilityRecomputePolicy::Deferred",
        "false",
    ]
    assert 0 <= grant < grant_open < derived < grant_close < ordinary
    assert ordinary < ordinary_open < ordinary_close
    assert routing.count("commitDerivedRecomputeInActiveTransaction(edit)") == 1
    assert routing.count("commitWithPreparationPolicyAndOptions(") == 1
    assert "commitWithPreparationPolicyAndRecompute(" not in routing

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


def test_safe_touch_bookkeeping_stays_in_the_lightweight_commit_lane() -> None:
    source = _read(GENERIC_SOURCE)
    generic = _compact(source)
    assert (
        "!forceExecution&&target->isTouched()&&isBookkeepingOnlyTarget(*target)"
        in generic
    )
    bookkeeping = _compact(_body(source, "isBookkeepingOnlyTarget"))
    assert "object.mustRecompute()==0" in bookkeeping
    assert "isSafePlainDocumentObjectBookkeepingTarget(object)" in bookkeeping
    assert "isNullProxyExternalLinkHolderBookkeepingTarget(object)" in bookkeeping
    assert "isSafePlainAppLinkArchiveInput(object)" not in bookkeeping

    # The broad mustRecompute()==0 shortcut is reachable only after the target
    # passed the exact audited/inert type and runtime-contract gates.
    prepare = _compact(_body(source, "prepareGenericRecompute"))
    known = prepare.find(
        "constboolknownInertType=hasKnownInertBookkeepingTypeId(*target)"
    )
    exact = prepare.find("if(!hasExactRegisteredRuntimeType(*target)){", known)
    inert_contract = prepare.find("constboolprovenInertBookkeepingContract=", exact)
    rejected = prepare.find(
        "if(!auditedWorkerType&&!provenInertBookkeepingContract){",
        inert_contract,
    )
    shortcut = prepare.find("isBookkeepingOnlyTarget(*target)", rejected)
    assert 0 <= known < exact < inert_contract < rejected < shortcut

    assert "PreparationPolicy::DetachedInProcess" in generic
    operation = generic.split("classGenericRecomputeBookkeepingOperation", 1)[1]
    operation = operation.split("std::unique_ptr<constApp::CollaborativeOperation>decodeResult", 1)[0]
    assert "target->mustRecompute()" in operation
    assert "document.settleRecomputedFeature(*target)" in operation
    assert "executeGenericRecompute" not in operation


def test_presentation_fences_are_cleared_recaptured_and_fail_closed() -> None:
    coordinator = _read(RECOMPUTE_SOURCE)
    activation = _compact(_body(
        coordinator, "DocumentRecomputeCoordinator::activateSubmission"
    ))
    object_reset = activation.find("node.presentationObjectModelRevision.reset()")
    fence_clear = activation.find("node.presentationRevisionFence.clear()", object_reset)
    incomplete = activation.find(
        "node.presentationRevisionFenceComplete=false", fence_clear
    )
    begin_session = activation.find("_service.beginEditSession(", incomplete)
    assert 0 <= object_reset < fence_clear < incomplete < begin_session

    service = _compact(_body(
        _read(SERVICE_SOURCE),
        "DocumentCollaborationService::prepareEditAsyncOnDocumentThread",
    ))
    initial_clear = service.find("presentationRevisionFence->clear()")
    initial_incomplete = service.find(
        "*presentationRevisionFenceComplete=false", initial_clear
    )
    provisional_capture = service.find(
        "captureGenericIsolatedRecomputePresentationFence(", initial_incomplete
    )
    provisional_complete = service.find(
        "*presentationRevisionFenceComplete=true", provisional_capture
    )
    preparation = service.find(
        "CollaborativeOperationRegistry::instance().prepare(", provisional_complete
    )
    canonical = service.find("validatePreparedEditMetadata(", preparation)
    replacement_incomplete = service.find(
        "*presentationRevisionFenceComplete=false", canonical
    )
    exact_fence = service.find(
        "std::vector<DocumentRevisionObservation>exactFence", replacement_incomplete
    )
    replace_fence = service.find(
        "*presentationRevisionFence=std::move(exactFence)", exact_fence
    )
    exact_complete = service.find(
        "*presentationRevisionFenceComplete=true", replace_fence
    )
    assert (
        0
        <= initial_clear
        < initial_incomplete
        < provisional_capture
        < provisional_complete
        < preparation
        < canonical
        < replacement_incomplete
        < exact_fence
        < replace_fence
        < exact_complete
    )

    finalize = _compact(_body(
        _read(DOCUMENT_SOURCE), "Document::finalizeDetachedRecompute"
    ))
    broad_failure = finalize.find("constboolrequiresBroadFailureFence=")
    semantic = finalize.find("constboolsemanticFenceMatches=", broad_failure)
    fence_complete = finalize.find(
        "node.presentationRevisionFenceComplete", semantic
    )
    validate = finalize.find(
        ".validate(node.presentationRevisionFence).empty()", fence_complete
    )
    apply = finalize.find("objectFenceMatches&&semanticFenceMatches", validate)
    assert 0 <= broad_failure < semantic < fence_complete < validate < apply

    schedule = _compact(_body(
        coordinator, "DocumentRecomputeCoordinator::scheduleReady"
    ))
    assert "boolfenceComplete=false" in schedule
    generic = schedule.find(
        "candidate.operationType==GenericIsolatedRecomputeOperationType"
    )
    capture = schedule.find(
        "captureGenericIsolatedRecomputePresentationFence(", generic
    )
    complete = schedule.find("fenceComplete=true", capture)
    publish = schedule.find(
        "presentationRevisionFenceComplete=fenceComplete", complete
    )
    assert 0 <= generic < capture < complete < publish
    assert schedule.count("fenceComplete=true") == 1


def test_unresolved_ledger_precedes_terminal_publication_and_is_generation_safe() -> None:
    coordinator = _read(RECOMPUTE_SOURCE)
    finalize = _compact(_body(
        coordinator, "DocumentRecomputeCoordinator::finalizeIfTerminal"
    ))
    synthetic = finalize.find("_unresolvedSyntheticFeatures.insert_or_assign(")
    generation = finalize.find("found->second.generation<=id", synthetic)
    live = finalize.find("_unresolvedLiveFeatures.insert_or_assign(", generation)
    session = finalize.find("foundJob->second->sessionFinalized=true", live)
    terminal = finalize.find("foundJob->second->state=terminalState", session)
    assert 0 <= synthetic < generation < live < session < terminal

    identity_filter = finalize.find("std::erase_if(liveFailures")
    lookup = finalize.find("document.getObject(candidate.featureId", identity_filter)
    identity = finalize.find(
        "document.collaborationObjectIdentity(*object)!=candidate.stableObjectIdentity",
        lookup,
    )
    finalize_state = finalize.find("finalizeState()", identity)
    assert 0 <= identity_filter < lookup < identity < finalize_state

    forget = _compact(_body(
        coordinator, "DocumentRecomputeCoordinator::forgetUnresolvedFeature"
    ))
    comparison = forget.find("found->second.generation<=id")
    erase = forget.find("_unresolvedLiveFeatures.erase(found)", comparison)
    assert 0 <= comparison < erase


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
    assert "collaborationStableReadBlocked()" in service

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
