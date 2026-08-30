# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP12 gate for the public recompute compatibility facades."""

from __future__ import annotations

import ast
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]

INVENTORY = Path("doc/document-collaboration-ingress-inventory.md")
APPLICATION_SOURCE = Path("src/App/Application.cpp")
DOCUMENT_HEADER = Path("src/App/Document.h")
DOCUMENT_SOURCE = Path("src/App/Document.cpp")
DOCUMENT_PYTHON = Path("src/App/DocumentPyImp.cpp")
DOCUMENT_STUB = Path("src/App/Document.pyi")
COORDINATOR_HEADER = Path("src/App/DocumentRecomputeCoordinator.h")
COMMIT_SOURCE = Path("src/App/DocumentCommitCoordinator.cpp")
SERVICE_SOURCE = Path("src/App/DocumentCollaborationService.cpp")
HANDLE_HEADER = Path("src/App/RecomputeHandle.h")
HANDLE_SOURCE = Path("src/App/RecomputeHandle.cpp")
HANDLE_PYTHON = Path("src/App/RecomputeHandlePyImp.cpp")
HANDLE_STUB = Path("src/App/RecomputeHandle.pyi")
APP_CMAKE = Path("src/App/CMakeLists.txt")
TEST_CMAKE = Path("tests/src/App/CMakeLists.txt")
NATIVE_TEST = Path("tests/src/App/RecomputeHandle.cpp")


def _read(path: str | Path) -> str:
    location = Path(path)
    if not location.is_absolute():
        location = REPO_ROOT / location
    return location.read_text(encoding="utf-8", errors="surrogateescape")


def _raw_literal_end(source: str, start: int) -> int | None:
    prefixes = ('u8R"', 'uR"', 'UR"', 'LR"', 'R"')
    prefix = next((item for item in prefixes if source.startswith(item, start)), None)
    if prefix is None or (
        start and (source[start - 1].isalnum() or source[start - 1] == "_")
    ):
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
    """Blank comments and optionally literals without moving source offsets."""

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


def _function_bodies(source: str, name: str, *, raw: bool = False) -> list[str]:
    stripped = _suppress_cpp(source)
    pattern = re.compile(r"(?<![A-Za-z0-9_:])" + re.escape(name) + r"\s*\(")
    result: list[str] = []
    for match in pattern.finditer(stripped):
        opening_parenthesis = stripped.find("(", match.start())
        closing_parenthesis = _matching(stripped, opening_parenthesis, "(", ")")
        if closing_parenthesis is None:
            continue
        semicolon = stripped.find(";", closing_parenthesis + 1)
        opening_brace = stripped.find("{", closing_parenthesis + 1)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue
        suffix = stripped[closing_parenthesis + 1 : opening_brace]
        if not _is_definition_suffix(name, suffix):
            continue
        closing_brace = _matching(stripped, opening_brace, "{", "}")
        if closing_brace is not None:
            selected = source if raw else stripped
            result.append(selected[opening_brace + 1 : closing_brace])
    return result


def _body(path: str | Path, name: str, *, raw: bool = False) -> str:
    bodies = _function_bodies(_read(path), name, raw=raw)
    assert len(bodies) == 1, f"expected one definition for {name}, found {len(bodies)}"
    return bodies[0]


def _class_body(path: str | Path, name: str) -> str:
    source = _suppress_cpp(_read(path))
    declaration = re.search(
        r"\bclass\s+(?:[A-Za-z_]\w*\s+)*" + re.escape(name) + r"\s*\{",
        source,
    )
    assert declaration is not None, f"expected class definition for {name}"
    opening = source.find("{", declaration.start())
    closing = _matching(source, opening, "{", "}")
    assert closing is not None, f"unterminated class definition for {name}"
    return source[opening + 1 : closing]


def _command_literal_calls_public_recompute(
    raw_body: str,
    *,
    allow_unqualified_do_command: bool = False,
) -> bool:
    """Recognize only recompute literals passed to the audited GUI command bridges."""

    source = _suppress_cpp(raw_body, literals=False)
    bridges = [re.compile(r"\b(?:Command::doCommand|FCMD_DOC_CMD)\s*\(")]
    if allow_unqualified_do_command:
        bridges.append(re.compile(r"(?<![:A-Za-z0-9_])doCommand\s*\("))
    string_literal = re.compile(r'"(?:\\.|[^"\\])*"')
    for bridge in bridges:
        for match in bridge.finditer(source):
            opening = source.find("(", match.start())
            closing = _matching(source, opening, "(", ")")
            if closing is None:
                continue
            arguments = source[opening + 1 : closing]
            if any(
                re.search(r"\.recompute\s*\(", literal)
                for literal in string_literal.findall(arguments)
            ):
                return True
    return False


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def _wp12_inventory_rows() -> list[list[str]]:
    rows: list[list[str]] = []
    for line_number, line in enumerate(_read(INVENTORY).splitlines(), 1):
        if not line.startswith("|"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if len(cells) == 9 and cells[7] == "CC-WP12":
            cells.append(str(line_number))
            rows.append(cells)
    return rows


def _inventory_symbol(row: list[str]) -> str:
    return row[2].split(" (candidate", 1)[0].strip("`")


def _inventory_path(row: list[str]) -> Path:
    return Path(row[1].strip("`"))


def _python_definitions(
    path: Path,
) -> dict[str, ast.FunctionDef | ast.AsyncFunctionDef]:
    tree = ast.parse(_read(path), filename=path.as_posix())
    definitions: dict[str, ast.FunctionDef | ast.AsyncFunctionDef] = {}

    def collect(statements: list[ast.stmt], prefix: tuple[str, ...] = ()) -> None:
        for statement in statements:
            if isinstance(statement, ast.ClassDef):
                collect(statement.body, (*prefix, statement.name))
            elif isinstance(statement, (ast.FunctionDef, ast.AsyncFunctionDef)):
                qualified = ".".join((*prefix, statement.name))
                definitions[qualified] = statement
                collect(statement.body, (*prefix, statement.name))

    collect(tree.body)
    return definitions


def _called_name(call: ast.Call) -> tuple[str, str | None]:
    if isinstance(call.func, ast.Name):
        return call.func.id, None
    if isinstance(call.func, ast.Attribute):
        owner = call.func.value.id if isinstance(call.func.value, ast.Name) else None
        return call.func.attr, owner
    return "", None


@dataclass(frozen=True)
class _PythonDefinition:
    path: Path
    qualified_name: str
    node: ast.FunctionDef | ast.AsyncFunctionDef


def _python_route_index(
    paths: set[Path],
) -> tuple[
    dict[tuple[Path, str], _PythonDefinition], dict[str, list[_PythonDefinition]]
]:
    by_key: dict[tuple[Path, str], _PythonDefinition] = {}
    by_leaf: dict[str, list[_PythonDefinition]] = defaultdict(list)
    for path in paths:
        for qualified_name, node in _python_definitions(path).items():
            definition = _PythonDefinition(path, qualified_name, node)
            by_key[(path, qualified_name)] = definition
            by_leaf[qualified_name.rsplit(".", 1)[-1]].append(definition)
    return by_key, by_leaf


class _CallCollector(ast.NodeVisitor):
    def __init__(self) -> None:
        self.calls: list[ast.Call] = []

    def visit_Call(self, node: ast.Call) -> None:  # noqa: N802
        self.calls.append(node)
        self.generic_visit(node)

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:  # noqa: N802
        return

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:  # noqa: N802
        return


def _calls(definition: _PythonDefinition) -> list[ast.Call]:
    collector = _CallCollector()
    for statement in definition.node.body:
        collector.visit(statement)
    return collector.calls


PUBLIC_RECOMPUTE_FACADES = {
    "abortTransaction",
    "commitTransaction",
    "openTransaction",
    "queueRecomputeRequest",
    "recompute",
    "recomputeAsync",
    "recomputeFeature",
    "redo",
    "undo",
}

COMMAND_LITERAL_RECOMPUTE_ROUTES = {
    (Path("src/Gui/Command.cpp"), "Command::updateActive"),
    (Path("src/Gui/Document.cpp"), "Document::saveAll"),
    (Path("src/Gui/Document.cpp"), "Document::save"),
}
UNQUALIFIED_DO_COMMAND_ROUTE = (
    Path("src/Gui/Command.cpp"),
    "Command::updateActive",
)


def _python_reaches_public_facade(
    start: _PythonDefinition,
    by_key: dict[tuple[Path, str], _PythonDefinition],
    by_leaf: dict[str, list[_PythonDefinition]],
) -> bool:
    visited: set[tuple[Path, str]] = set()

    def visit(definition: _PythonDefinition) -> bool:
        key = (definition.path, definition.qualified_name)
        if key in visited:
            return False
        visited.add(key)
        owner = definition.qualified_name.rsplit(".", 1)[0]
        for call in _calls(definition):
            name, receiver = _called_name(call)
            if name in PUBLIC_RECOMPUTE_FACADES:
                return True

            candidates: list[_PythonDefinition] = []
            if receiver in {"self", "cls"}:
                candidate = by_key.get((definition.path, f"{owner}.{name}"))
                if candidate:
                    candidates.append(candidate)
            if receiver is None:
                candidate = by_key.get((definition.path, name))
                if candidate:
                    candidates.append(candidate)
            if not candidates and name and len(by_leaf.get(name, [])) == 1:
                candidates.extend(by_leaf[name])
            if any(visit(candidate) for candidate in candidates):
                return True
        return False

    return visit(start)


def test_sync_async_and_feature_facades_share_the_coordinator_generic_plan() -> None:
    asynchronous = _compact(_body(DOCUMENT_SOURCE, "Document::recomputeAsync"))
    synchronous = _compact(_body(DOCUMENT_SOURCE, "Document::recompute"))
    feature = _compact(_body(DOCUMENT_SOURCE, "Document::recomputeFeature"))

    registration = asynchronous.find(
        "Internal::ensureGenericIsolatedRecomputeRegistered()"
    )
    plan = asynchronous.find(
        "Internal::makeGenericIsolatedRecomputeRequest(", registration
    )
    submission = asynchronous.find("recomputeCoordinator().submit(", plan)
    handle = asynchronous.find(
        "std::make_unique<RecomputeHandle>(*this,id)", submission
    )
    assert 0 <= registration < plan < submission < handle

    async_call = synchronous.find("recomputeAsync(objs,force,options)")
    wait = synchronous.find("handle->wait(", async_call)
    assert 0 <= async_call < wait
    assert "recomputeLegacy(" not in synchronous

    feature_registration = feature.find(
        "Internal::ensureGenericIsolatedRecomputeRegistered()"
    )
    feature_plan = feature.find(
        "Internal::makeGenericIsolatedRecomputeRequest(*this,*feature,recursive)",
        feature_registration,
    )
    feature_submit = feature.find("coordinator.submit(", feature_plan)
    assert 0 <= feature_registration < feature_plan < feature_submit
    assert "recomputeLegacy(" not in feature


def test_python_binding_returns_the_exported_handle_and_declares_its_surface() -> None:
    setup = _compact(
        _suppress_cpp(
            _body(APPLICATION_SOURCE, "Application::setupPythonTypes", raw=True),
            literals=False,
        )
    )
    binding = _compact(_body(DOCUMENT_PYTHON, "DocumentPy::recomputeAsync"))
    document_stub = ast.parse(_read(DOCUMENT_STUB), filename=DOCUMENT_STUB.as_posix())
    handle_stub = ast.parse(_read(HANDLE_STUB), filename=HANDLE_STUB.as_posix())

    assert 'addType(&RecomputeHandlePy::Type,pAppModule,"RecomputeHandle")' in setup
    native_call = binding.find("getDocumentPtr()->recomputeAsync(")
    wrapper = binding.find("newRecomputeHandlePy(handle.release())", native_call)
    assert 0 <= native_call < wrapper

    imported = {
        alias.name
        for node in document_stub.body
        if isinstance(node, ast.ImportFrom) and node.module == "RecomputeHandle"
        for alias in node.names
    }
    assert imported == {"RecomputeHandle"}
    declarations = _python_definitions(DOCUMENT_STUB)
    async_stub = declarations["Document.recomputeAsync"]
    assert isinstance(async_stub.returns, ast.Name)
    assert async_stub.returns.id == "RecomputeHandle"

    handle_class = next(
        node
        for node in handle_stub.body
        if isinstance(node, ast.ClassDef) and node.name == "RecomputeHandle"
    )
    methods = {
        node.name: node
        for node in handle_class.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    assert set(methods) == {"id", "status", "progress", "done", "cancel", "wait"}


def test_handle_is_pointer_safe_after_close_and_python_timeout_is_bounded() -> None:
    coordinator = _compact(_read(COORDINATOR_HEADER))
    header = _compact(_read(HANDLE_HEADER))
    handle_class = _class_body(HANDLE_HEADER, "RecomputeHandle")
    status = _compact(_body(HANDLE_SOURCE, "RecomputeHandle::status"))
    closed = _compact(_body(HANDLE_SOURCE, "RecomputeHandle::closedDocumentSnapshot"))
    wait = _compact(_body(HANDLE_SOURCE, "RecomputeHandle::wait"))
    python_id = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::id"))
    python_status = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::status"))
    python_progress = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::progress"))
    python_done = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::done"))
    python_cancel = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::cancel"))
    python_wait = _compact(_body(HANDLE_PYTHON, "RecomputeHandlePy::wait"))

    assert "usingDocumentRecomputeId=std::uint64_t;" in coordinator
    assert "DocumentRecomputeIdid()constnoexcept;" in header
    public = handle_class.split("public:", 1)[1].split("private:", 1)[0]
    for method in ("id", "status", "poll", "cancel", "wait"):
        assert re.search(rf"\b{method}\(", public), (
            f"missing public handle method {method}"
        )
    assert "std::unique_ptr<DocumentWeakPtrT>_document;" in header
    assert "Document*" not in public

    null_check = status.find("if(!owner)")
    closed_snapshot = status.find("returnclosedDocumentSnapshot()", null_check)
    assert 0 <= null_check < closed_snapshot
    assert "snapshot.id=_id" in closed
    assert "snapshot.state=DocumentRecomputeState::Cancelled" in closed
    terminal = _compact(_body(COORDINATOR_HEADER, "terminal"))
    for state in ("Completed", "PartialFailure", "Cancelled"):
        assert f"state==DocumentRecomputeState::{state}" in terminal
    assert "constautoboundedTimeout=std::max(timeout,0ms)" in wait

    assert "PyLong_FromUnsignedLongLong(getRecomputeHandlePtr()->id())" in python_id
    assert "snapshotToPython(getRecomputeHandlePtr()->status())" in python_status
    assert "getRecomputeHandlePtr()->status().progress" in python_progress
    assert "getRecomputeHandlePtr()->status().terminal()" in python_done
    assert "getRecomputeHandlePtr()->cancel(reason)" in python_cancel

    finite = python_wait.find("!std::isfinite(timeoutSeconds)")
    lower = python_wait.find("timeoutSeconds<0.0", finite)
    upper = python_wait.find("timeoutSeconds>86'400.0", lower)
    error = python_wait.find("PyErr_SetString(", upper)
    conversion = python_wait.find("timeoutSeconds*1000.0", error)
    native_wait = python_wait.find("getRecomputeHandlePtr()->wait(", conversion)
    assert 0 <= finite < lower < upper < error < conversion < native_wait


def test_application_disables_the_old_worker_and_routes_requests_to_public_facades() -> (
    None
):
    constructor = _compact(_body(APPLICATION_SOURCE, "Application::Application"))
    queue = _compact(_body(APPLICATION_SOURCE, "Application::queueRecomputeRequest"))
    serialized = _compact(
        _body(APPLICATION_SOURCE, "Application::processRecomputeRequestSerialized")
    )
    public_route = _compact(
        _body(APPLICATION_SOURCE, "processRecomputeRequestUnserialized")
    )

    assert "_stopRecomputeThread=true" in constructor
    assert "recomputeWorker(" not in constructor
    assert not re.search(r"_recomputeThread(?:=|\{|\.swap\(|\.detach\()", constructor)

    assert "processRecomputeRequestSerialized(*request)" in queue
    assert "notifyRecomputeWorker(" not in queue
    assert not re.search(
        r"_recomputeRequests\.(?:push_back|push_front|emplace_back|emplace_front|insert)\(",
        queue,
    )
    assert "processRecomputeRequestUnserialized(request)" in serialized
    assert (
        "document->recompute({},request.force,&recomputeHasError,request.options)"
        in public_route
    )
    assert "if(recomputeHasError)" in public_route
    assert (
        "documentObject&&!documentObject->recomputeFeature(request.recursive)"
        in public_route
    )

    document_failure = public_route.find("if(recomputeHasError){")
    assert document_failure >= 0
    document_failure_open = public_route.find("{", document_failure)
    document_failure_end = _matching(
        public_route, document_failure_open, "{", "}"
    )
    assert document_failure_end is not None
    document_failure_branch = public_route[document_failure:document_failure_end]
    document_success = document_failure_branch.find("result.success=false;")
    document_kind = document_failure_branch.find(
        "result.failure=RecomputeFailure::Exception;", document_success
    )
    document_exception = document_failure_branch.find(
        "result.exception=std::make_unique<Base::RuntimeError>(", document_kind
    )
    assert 0 <= document_success < document_kind < document_exception

    feature_failure = public_route.find(
        "if(documentObject&&!documentObject->recomputeFeature(request.recursive)){"
    )
    assert feature_failure >= 0
    feature_failure_open = public_route.find("{", feature_failure)
    feature_failure_end = _matching(
        public_route, feature_failure_open, "{", "}"
    )
    assert feature_failure_end is not None
    feature_failure_branch = public_route[feature_failure:feature_failure_end]
    feature_success = feature_failure_branch.find("result.success=false;")
    feature_kind = feature_failure_branch.find(
        "result.failure=RecomputeFailure::Exception;", feature_success
    )
    feature_exception_guard = feature_failure_branch.find(
        "if(!result.exception){", feature_kind
    )
    feature_exception = feature_failure_branch.find(
        "result.exception=std::make_unique<Base::RuntimeError>(",
        feature_exception_guard,
    )
    assert 0 <= feature_success < feature_kind < feature_exception_guard < feature_exception
    assert "recomputeLegacy(" not in public_route


def test_recompute_commit_routing_uses_the_derived_grant_only_for_the_eager_stage() -> None:
    service = _compact(
        _body(
            SERVICE_SOURCE,
            "DocumentCollaborationService::commitRecomputeEditOnDocumentThread",
        )
    )
    service_validation = service.find(
        "CollaborativeOperationRegistry::instance().matches("
    )
    service_delegate = service.find("return_coordinator.commitRecompute(edit)")
    assert 0 <= service_validation < service_delegate
    assert service.count("_coordinator.commitRecompute(edit)") == 1

    routing = _compact(
        _body(COMMIT_SOURCE, "DocumentCommitCoordinator::commitRecompute")
    )
    grant_check = routing.find(
        "if(_document.collaborationDerivedRecomputeGranted()){"
    )
    nested = routing.find(
        "returncommitDerivedRecomputeInActiveTransaction(edit)", grant_check
    )
    ordinary = routing.find("returncommitWithPreparationPolicyAndOptions(", nested)
    assert ordinary >= 0
    ordinary_open = routing.find("(", ordinary)
    ordinary_close = _matching(routing, ordinary_open, "(", ")")
    assert ordinary_close is not None
    ordinary_arguments = routing[ordinary_open + 1 : ordinary_close].split(",")
    assert ordinary_arguments == [
        "edit",
        "false",
        "false",
        "CollaborationCompatibilityRecomputePolicy::Deferred",
        "false",
        "false",
    ]
    assert 0 <= grant_check < nested < ordinary < ordinary_open < ordinary_close
    assert routing.count("commitDerivedRecomputeInActiveTransaction(edit)") == 1
    assert routing.count("commitWithPreparationPolicyAndOptions(") == 1
    assert "commitWithPreparationPolicyAndRecompute(" not in routing

    commit = _compact(
        _body(
            COMMIT_SOURCE,
            "DocumentCommitCoordinator::commitOnDocumentThreadWithOptions",
        )
    )
    commit_raw = _body(
        COMMIT_SOURCE,
        "DocumentCommitCoordinator::commitOnDocumentThreadWithOptions",
        raw=True,
    )
    eager = commit.find(
        "if(recomputePolicy==CollaborationCompatibilityRecomputePolicy::Eager){"
    )
    assert eager >= 0
    eager_opening = commit.find("{", eager)
    eager_closing = _matching(commit, eager_opening, "{", "}")
    assert eager_closing is not None
    eager_stage = commit[eager_opening + 1 : eager_closing]
    grant = eager_stage.find(
        "autoderivedRecompute=_document.openCollaborationDerivedRecomputeGrant();"
    )
    structural = eager_stage.find("if(structuralCompatibility){", grant)
    structural_opening = eager_stage.find("{", structural)
    structural_closing = _matching(eager_stage, structural_opening, "{", "}")
    assert structural_closing is not None
    structural_stage = eager_stage[structural_opening + 1 : structural_closing]
    structural_grant = structural_stage.find(
        "autogrant=_document.openCollaborationStructuralRecomputeGrant("
        "trustedStructural);"
    )
    legacy = structural_stage.find(
        "_document.recomputeLegacy({},true,&recomputeHasError,0)",
        structural_grant,
    )
    assert 0 <= grant < structural < structural_opening
    assert 0 <= structural_grant < legacy
    assert eager_stage.count("openCollaborationDerivedRecomputeGrant()") == 1
    assert eager_stage.count("openCollaborationStructuralRecomputeGrant(") == 1
    assert structural_stage.count("_document.recomputeLegacy(") == 1
    assert "_document.recompute(" not in structural_stage

    ordinary_else = eager_stage.find("else{", structural_closing)
    ordinary_opening = eager_stage.find("{", ordinary_else)
    ordinary_closing = _matching(eager_stage, ordinary_opening, "{", "}")
    assert ordinary_closing is not None
    assert structural_closing < ordinary_else < ordinary_opening
    ordinary_stage = eager_stage[ordinary_opening + 1 : ordinary_closing]
    assert "_document.recompute({},true,&recomputeHasError)" in ordinary_stage
    assert "recomputeLegacy(" not in ordinary_stage

    first_recompute = eager_stage.find("_document.recompute(", grant)
    assert 0 <= grant < first_recompute
    assert eager_stage.count("_document.recompute(") == 1
    assert eager_stage.count("_document.recomputeLegacy(") == 1
    assert commit.count("openCollaborationDerivedRecomputeGrant()") == 1
    assert commit.count("_document.recompute(") == eager_stage.count(
        "_document.recompute("
    )
    assert commit.count("_document.recomputeLegacy(") == 1

    raw_legacy = commit_raw.find("_document.recomputeLegacy(")
    assert raw_legacy >= 0
    assert commit_raw.find("_document.recomputeLegacy(", raw_legacy + 1) < 0
    removal_note = commit_raw[max(0, raw_legacy - 450) : raw_legacy]
    assert re.search(
        r"//[^\n]*CC-WP13\s*\n\s*//\s*removes this live compatibility path",
        removal_note,
    )
    operation_apply = commit.find("operation.apply(_document)")
    postcondition = commit.find("operation.checkPostcondition(_document)", eager_closing)
    assert 0 <= operation_apply < eager < eager_closing < postcondition


def test_derived_recompute_commit_revalidates_and_folds_effects_without_a_transaction() -> None:
    derived = _compact(
        _body(
            COMMIT_SOURCE,
            "DocumentCommitCoordinator::commitDerivedRecomputeInActiveTransaction",
        )
    )

    owner = derived.find("!_document.isCollaborationOwnerThread()")
    active_grant = derived.find(
        "!_document.collaborationDerivedRecomputeGranted()", owner
    )
    active_transaction = derived.find(
        "!_document.hasPendingTransaction()", active_grant
    )
    suppressed = derived.find(
        "!_document.collaborationRevisionPublicationSuppressed()", active_transaction
    )
    identity = derived.find("identity=_document.collaborationIdentity()", suppressed)
    lifecycle = derived.find("identity->lifecycleEpoch!=edit.lifecycleEpoch()", identity)
    declared = derived.find(
        "constauto&declaredEffects=edit.publicationEffects()", lifecycle
    )
    exact_writes = derived.find(
        "effectsExactlyCoverWrites(declaredEffects,edit.writeSet())", declared
    )
    revisions = derived.find(
        "_document.collaborationRevisions().validate(edit.expectedRevisions())",
        exact_writes,
    )
    apply = derived.find("operation.apply(_document)", revisions)
    transaction_recheck = derived.find("if(!_document.hasPendingTransaction())", apply)
    audit = derived.find(
        "_document.beginCollaborationPreparedReadOnlyPostconditionAudit()",
        transaction_recheck,
    )
    postcondition = derived.find("operation.checkPostcondition(_document)", audit)
    postcondition_satisfied = derived.find(
        "if(!postcondition.satisfied){",
        postcondition,
    )
    pending_removal = derived.find(
        "_document.getMutationReadiness().pendingRemoval",
        postcondition_satisfied,
    )
    committed = derived.find(
        "returnmakeResult(DocumentCommitStatus::Committed,edit,",
        pending_removal,
    )
    assert (
        0
        <= owner
        < active_grant
        < active_transaction
        < suppressed
        < identity
        < lifecycle
        < declared
        < exact_writes
        < revisions
        < apply
        < transaction_recheck
        < audit
        < postcondition
        < postcondition_satisfied
        < pending_removal
        < committed
    )
    assert "identity->instanceId!=edit.documentInstanceId()" in derived
    assert "identity->state!=DocumentLifecycleState::Live" in derived
    assert "_document.endCollaborationPreparedAtomicPresentationAudit()" in derived
    assert "postconditionMutationAttempted" in derived
    assert "openNativeCommitTransaction(" not in derived
    assert "commitNativeCommitTransaction(" not in derived
    assert "commitCollaborationCommitTransaction(" not in derived
    assert "collaborationRevisions().publish(" not in derived
    assert "recordCollaborationObservedStructuralEffects(" not in derived


def test_derived_recompute_capture_grant_is_narrow_nonreentrant_and_raii() -> None:
    capture = _compact(
        _body(DOCUMENT_SOURCE, "Document::collaborationRecomputeCaptureBlocked")
    )
    foreign_boundary = (
        "constboolforeignMutationBoundary=d->collaborationCommitNotificationBarrier"
        "||hasPendingTransaction()||transacting()||getBookedTransactionID()!=0"
        "||isTransactionLocked()"
    )
    assert foreign_boundary in capture
    assert (
        "return(foreignMutationBoundary&&!d->collaborationDerivedRecomputeGranted)"
        in capture
    )
    assert capture.count("collaborationDerivedRecomputeGranted") == 1
    for invariant in (
        "d->collaborationReplayingNotifications",
        "d->collaborationRecomputeTeardownDepth.load(std::memory_order_acquire)!=0",
        "d->pendingRemovalProcessing.load(std::memory_order_acquire)",
        "!d->pendingRemove.empty()",
    ):
        assert invariant in capture

    constructor = _compact(
        _body(
            DOCUMENT_SOURCE,
            "Document::CollaborationDerivedRecomputeGrant::CollaborationDerivedRecomputeGrant",
        )
    )
    owner = constructor.find("!document.isCollaborationOwnerThread()")
    barrier = constructor.find("!state.collaborationCommitNotificationBarrier", owner)
    transaction = constructor.find("!state.activeUndoTransaction", barrier)
    suppression = constructor.find(
        "!state.suppressCollaborationRevisionPublication", transaction
    )
    reentrant = constructor.find(
        "if(state.collaborationDerivedRecomputeGranted)", suppression
    )
    set_grant = constructor.find(
        "state.collaborationDerivedRecomputeGranted=true", reentrant
    )
    assert 0 <= owner < barrier < transaction < suppression < reentrant < set_grant
    for blocked_state in (
        "state.collaborationAtomicPresentationAuditActive",
        "state.collaborationCommitPoisoned",
        "state.collaborationReplayingNotifications",
        "state.rollback",
    ):
        assert blocked_state in constructor

    destructor = _compact(
        _body(
            DOCUMENT_SOURCE,
            "Document::CollaborationDerivedRecomputeGrant::~CollaborationDerivedRecomputeGrant",
        )
    )
    assert destructor == "_document.d->collaborationDerivedRecomputeGranted=false;"
    opener = _compact(
        _body(DOCUMENT_SOURCE, "Document::openCollaborationDerivedRecomputeGrant")
    )
    assert opener == "returnCollaborationDerivedRecomputeGrant(*this);"


def test_all_58_inventory_callers_reach_a_public_recompute_facade() -> None:
    rows = _wp12_inventory_rows()
    assert len(rows) == 58, f"expected 58 CC-WP12 inventory rows, found {len(rows)}"
    keys = [(_inventory_path(row), _inventory_symbol(row)) for row in rows]
    assert len(keys) == len(set(keys)), (
        "CC-WP12 inventory contains duplicate path/symbol rows"
    )
    for row in rows:
        assert all(row[index] for index in range(9)), (
            f"inventory row {row[9]} has an empty required field"
        )
        assert (REPO_ROOT / _inventory_path(row)).is_file(), (
            f"inventory row {row[9]} names missing source {_inventory_path(row)}"
        )

    python_paths: set[Path] = set()
    for root in (
        REPO_ROOT / "tools/mcp/freecad-mcp/addon/FreeCADMCP",
        REPO_ROOT / "tools/mcp/freecad-mcp/src/freecad_mcp",
    ):
        python_paths.update(
            path for path in root.rglob("*.py") if "__pycache__" not in path.parts
        )
    relative_python_paths = {path.relative_to(REPO_ROOT) for path in python_paths}
    by_key, by_leaf = _python_route_index(relative_python_paths)

    failures: list[str] = []
    for row in rows:
        path = _inventory_path(row)
        symbol = _inventory_symbol(row)
        if path.suffix == ".py":
            definition = by_key.get((path, symbol))
            if definition is None:
                failures.append(f"{path}:{symbol} is not a Python definition")
            elif not _python_reaches_public_facade(definition, by_key, by_leaf):
                failures.append(
                    f"{path}:{symbol} does not reach a public recompute facade"
                )
            continue

        source = _read(path)
        bodies = _function_bodies(source, symbol)
        if len(bodies) != 1:
            failures.append(f"{path}:{symbol} has {len(bodies)} C++ definitions")
            continue
        body = bodies[0]
        if "recomputeLegacy(" in _compact(body):
            failures.append(f"{path}:{symbol} calls private recomputeLegacy")
            continue
        calls = {
            match.group(1)
            for match in re.finditer(
                r"\b(queueRecomputeRequest|recomputeAsync|recomputeFeature|recompute)\s*\(",
                body,
            )
        }
        if not calls:
            raw_bodies = _function_bodies(source, symbol, raw=True)
            route_key = (path, symbol)
            command_route = (
                route_key in COMMAND_LITERAL_RECOMPUTE_ROUTES
                and len(raw_bodies) == 1
                and _command_literal_calls_public_recompute(
                    raw_bodies[0],
                    allow_unqualified_do_command=(route_key == UNQUALIFIED_DO_COMMAND_ROUTE),
                )
            )
            if not command_route:
                failures.append(f"{path}:{symbol} does not call a public recompute facade")

    assert not failures, "\n".join(failures)


def test_wp12_legacy_recompute_only_scoped_coordinator_paths_and_handle_wiring() -> None:
    rollback_body = _body(
        DOCUMENT_SOURCE, "Document::rollbackCollaborationTransactionImpl"
    )
    rollback_raw_body = _body(
        DOCUMENT_SOURCE,
        "Document::rollbackCollaborationTransactionImpl",
        raw=True,
    )
    document_source = _suppress_cpp(_read(DOCUMENT_SOURCE))
    rollback_start = document_source.find(rollback_body)
    assert rollback_start >= 0
    assert document_source.find(rollback_body, rollback_start + 1) < 0
    rollback_end = rollback_start + len(rollback_body)

    coordinator_body = _body(
        COMMIT_SOURCE,
        "DocumentCommitCoordinator::commitOnDocumentThreadWithOptions",
    )
    coordinator_source = _suppress_cpp(_read(COMMIT_SOURCE))
    coordinator_start = coordinator_source.find(coordinator_body)
    assert coordinator_start >= 0
    assert coordinator_source.find(coordinator_body, coordinator_start + 1) < 0
    coordinator_end = coordinator_start + len(coordinator_body)

    header = _compact(_suppress_cpp(_read(DOCUMENT_HEADER)))
    assert header.count("recomputeLegacy(") == 1
    assert (
        "intrecomputeLegacy(conststd::vector<DocumentObject*>&objs,boolforce,"
        "bool*hasError,intoptions);" in header
    )

    rollback_call_sites: list[str] = []
    coordinator_call_sites: list[str] = []
    forbidden_call_sites: list[str] = []
    for path in (REPO_ROOT / "src").rglob("*"):
        if path.suffix not in {".cpp", ".h", ".hpp"}:
            continue
        source = _suppress_cpp(_read(path))
        for match in re.finditer(r"\brecomputeLegacy\s*\(", source):
            relative = path.relative_to(REPO_ROOT)
            if relative == DOCUMENT_HEADER:
                continue
            if relative == DOCUMENT_SOURCE:
                prefix = source[max(0, match.start() - 40) : match.start()]
                if re.search(r"Document::\s*$", prefix):
                    continue
            line = source.count("\n", 0, match.start()) + 1
            location = f"{relative.as_posix()}:{line}"
            if (
                relative == DOCUMENT_SOURCE
                and rollback_start <= match.start() < rollback_end
            ):
                rollback_call_sites.append(location)
            elif (
                relative == COMMIT_SOURCE
                and coordinator_start <= match.start() < coordinator_end
            ):
                coordinator_call_sites.append(location)
            else:
                forbidden_call_sites.append(location)
    assert len(rollback_call_sites) == 1, (
        "expected exactly one rollback stabilization recomputeLegacy call, found: "
        + ", ".join(rollback_call_sites)
    )
    assert len(coordinator_call_sites) == 1, (
        "expected exactly one structural coordinator recomputeLegacy call, found: "
        + ", ".join(coordinator_call_sites)
    )
    assert not forbidden_call_sites, (
        "forbidden recomputeLegacy callers remain: "
        + ", ".join(forbidden_call_sites)
    )

    legacy_call = re.search(r"\brecomputeLegacy\s*\(", rollback_body)
    assert legacy_call is not None
    enclosing_scopes = [
        (opening, closing)
        for opening in range(legacy_call.start())
        if rollback_body[opening] == "{"
        and (closing := _matching(rollback_body, opening, "{", "}")) is not None
        and closing > legacy_call.start()
    ]
    assert enclosing_scopes
    opening, closing = max(enclosing_scopes)
    call_scope = _compact(rollback_body[opening + 1 : closing])
    guard = call_scope.find(
        "Base::FlagToggler<bool>rollbackStabilizing("
        "d->collaborationRollbackStabilizing);"
    )
    call = call_scope.find(
        "static_cast<void>(recomputeLegacy({},true,&recomputeHasError,0));"
    )
    assert 0 <= guard < call
    assert _compact(rollback_body[: legacy_call.start()]).find(
        "boolrecomputeHasError=false;"
    ) >= 0

    raw_call = rollback_raw_body.find("recomputeLegacy(")
    assert raw_call >= 0
    removal_note = rollback_raw_body[max(0, raw_call - 400) : raw_call]
    assert re.search(
        r"//[^\n]*CC-WP13 removes\s*\n\s*//\s*the legacy body",
        removal_note,
    )

    app_cmake = _read(APP_CMAKE)
    test_cmake = _read(TEST_CMAKE)
    for name in (
        "RecomputeHandle.cpp",
        "RecomputeHandle.h",
        "RecomputeHandlePyImp.cpp",
    ):
        assert name in app_cmake
    assert re.search(r"(?m)^\s*RecomputeHandle\.cpp\s*$", test_cmake)
    assert (REPO_ROOT / NATIVE_TEST).is_file()
