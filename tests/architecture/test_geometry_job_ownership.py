# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP06 ownership gate for detached geometry jobs."""

from __future__ import annotations

from pathlib import Path
import re
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]

APPLICATION_HEADER = "src/App/Application.h"
APPLICATION_SOURCE = "src/App/Application.cpp"
APP_CMAKE = "src/App/CMakeLists.txt"
POLICY_HEADER = "src/App/CollaborativeOperationRegistry.h"
DCS_SOURCE = "src/App/DocumentCollaborationService.cpp"
MANAGER_HEADER = "src/App/GeometryJobManager.h"
MANAGER_SOURCE = "src/App/GeometryJobManager.cpp"
EXECUTOR_HEADER = "src/App/PreparedEditExecutor.h"
EXECUTOR_SOURCE = "src/App/PreparedEditExecutor.cpp"
TEST_APP_CMAKE = "tests/src/App/CMakeLists.txt"

PRODUCTION_OWNERSHIP_SOURCES = (
    APPLICATION_HEADER,
    APPLICATION_SOURCE,
    POLICY_HEADER,
    DCS_SOURCE,
    MANAGER_HEADER,
    MANAGER_SOURCE,
    EXECUTOR_HEADER,
    EXECUTOR_SOURCE,
)

POINTER_FREE_CONTRACTS = (
    "GeometryJobRequest",
    "GeometryJobProgress",
    "GeometryJobStatus",
    "GeometryJobResult",
    "GeometryJobDispatch",
)

AUTHORITY_TOKENS = (
    "Document",
    "DocumentObject",
    "DocumentCommitCoordinator",
    "PreparedEdit",
    "CollaborativeOperation",
    "DocumentRevision",
    "DocumentIdentity",
    "std::function",
    "shared_ptr",
    "weak_ptr",
    "unique_ptr",
    "reference_wrapper",
)

HEAVY_GEOMETRY_RE = re.compile(
    r"(?P<token>\bTopoDS(?:_[A-Za-z0-9_]+)?\b|"
    r"\bBRep[A-Za-z0-9_]*\b|"
    r"\bOpenCASCADE\b|"
    r"\bOCC(?:_[A-Za-z0-9_]+)?\b|"
    r"\bPart\s*::|"
    r"\bGeom(?:2d)?_[A-Za-z0-9_]+\b|"
    r"\bgp_[A-Za-z0-9_]+\b)"
)


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


def _suppress_cpp_non_code(source: str) -> str:
    """Blank comments and literals while preserving offsets and line positions."""

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


def _type_body(source: str, keyword: str, name: str) -> str:
    stripped = _suppress_cpp_non_code(source)
    match = re.search(
        r"\b" + re.escape(keyword) + r"\b[^;{}]*\b" + re.escape(name) + r"\b[^;{}]*\{",
        stripped,
    )
    assert match, f"missing {keyword} {name}"
    opening = stripped.find("{", match.start())
    closing = _matching_delimiter(stripped, opening, "{", "}")
    assert closing is not None, f"unterminated {keyword} {name}"
    return stripped[opening + 1:closing]


def _function_bodies(source: str, name: str) -> list[tuple[str, str, str]]:
    stripped = _suppress_cpp_non_code(source)
    pattern = re.compile(r"(?<![A-Za-z0-9_:])" + re.escape(name) + r"\s*\(")
    bodies: list[tuple[str, str, str]] = []
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
                    source[opening_brace + 1:closing_brace],
                )
            )
    return bodies


def _body_for(
    source: str,
    name: str,
    *,
    signature_contains: str | None = None,
    raw: bool = False,
) -> str:
    candidates = _function_bodies(source, name)
    if signature_contains is not None:
        fragment = re.sub(r"\s+", "", signature_contains)
        candidates = [
            item for item in candidates if fragment in re.sub(r"\s+", "", item[0])
        ]
    assert len(candidates) == 1, (
        f"expected one body for {name}"
        + (f" containing {signature_contains}" if signature_contains else "")
        + f", found {len(candidates)}"
    )
    return candidates[0][2 if raw else 1]


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def _has_destructor_definition(source: str, class_name: str) -> bool:
    qualified_destructor = f"{class_name}::~{class_name}"
    if _function_bodies(source, qualified_destructor):
        return True
    compact = _compact(_suppress_cpp_non_code(source))
    return f"{qualified_destructor}()=default;" in compact


def _access_before(class_body: str, offset: int) -> str | None:
    accesses = list(re.finditer(r"\b(public|protected|private)\s*:", class_body[:offset]))
    return accesses[-1].group(1) if accesses else None


def _singleton_violations(sources: Mapping[str, str]) -> list[str]:
    violations: list[str] = []
    for path, source in sorted(sources.items()):
        code = _suppress_cpp_non_code(source)
        for label, pattern in (
            ("GeometryJobManager::instance", re.compile(r"\bGeometryJobManager\s*::\s*instance\s*\(")),
            (
                "static GeometryJobManager ownership",
                re.compile(
                    r"\bstatic\b[^;{}\n]*\b(?:unique_ptr\s*<\s*)?"
                    r"GeometryJobManager\b[^;{}\n]*[;=]"
                ),
            ),
        ):
            for match in pattern.finditer(code):
                line = code.count("\n", 0, match.start()) + 1
                violations.append(f"{path}:{line}: {label}")

        depth = 0
        for line_number, line in enumerate(code.splitlines(), 1):
            at_namespace_scope = depth <= 1
            stripped_line = line.strip()
            if (
                at_namespace_scope
                and "GeometryJobManager" in stripped_line
                and ";" in stripped_line
                and "(" not in stripped_line
                and not stripped_line.startswith(("class ", "using ", "friend "))
            ):
                violations.append(
                    f"{path}:{line_number}: namespace/global GeometryJobManager object"
                )
            depth += line.count("{") - line.count("}")
    return violations


def _pointer_contract_violations(
    header: str, contracts: Sequence[str] = POINTER_FREE_CONTRACTS
) -> list[str]:
    violations: list[str] = []
    for name in contracts:
        try:
            body = _type_body(header, "struct", name)
        except AssertionError as error:
            violations.append(str(error))
            continue
        for token in AUTHORITY_TOKENS:
            if re.search(r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])", body):
                violations.append(f"{name}: live/shared authority token {token}")
        if "*" in body:
            violations.append(f"{name}: raw pointer field/token")
        if "&" in body:
            violations.append(f"{name}: reference authority field/token")
    alias = re.search(r"\busing\s+GeometryJobId\s*=\s*([^;]+);", _suppress_cpp_non_code(header))
    if not alias:
        violations.append("GeometryJobId: missing value-type alias")
    elif any(token in alias.group(1) for token in ("*", "&", "Document", "shared_ptr")):
        violations.append(f"GeometryJobId: pointer/authority alias {alias.group(1).strip()}")
    return violations


def _policy_members(header: str) -> tuple[str, ...]:
    body = _type_body(header, "enum class", "PreparationPolicy")
    return tuple(re.findall(r"\b[A-Za-z_]\w*\b", body))


def _lightweight_policy_violations(header: str, source: str) -> list[str]:
    violations: list[str] = []
    compact_header = _compact(_suppress_cpp_non_code(header))
    required_declaration = (
        "submit(CollaborativeOperationPreparation::DetachedTasktask,"
        "PreparationPolicypolicy);"
    )
    if required_declaration not in compact_header:
        violations.append("PreparedEditExecutor::submit must require an explicit PreparationPolicy")
    if "PreparationPolicypolicy=" in compact_header:
        violations.append("PreparedEditExecutor::submit policy must not have a default")
    try:
        body = _body_for(source, "submit", signature_contains="DetachedTask task")
    except AssertionError as error:
        violations.append(str(error))
        return violations
    compact_body = _compact(body)
    if "if(policy!=PreparationPolicy::DetachedInProcess)" not in compact_body:
        violations.append("PreparedEditExecutor submit lacks DetachedInProcess-only guard")
    if "throwstd::invalid_argument(" not in compact_body:
        violations.append("PreparedEditExecutor policy guard does not reject invalid work")
    return violations


def _heavy_geometry_violations(sources: Mapping[str, str]) -> list[str]:
    violations: list[str] = []
    for path, source in sorted(sources.items()):
        code = _suppress_cpp_non_code(source)
        for match in HEAVY_GEOMETRY_RE.finditer(code):
            line = code.count("\n", 0, match.start("token")) + 1
            violations.append(f"{path}:{line}: heavy geometry token {match.group('token')!r}")
    return violations


def test_application_exclusively_owns_geometry_job_manager() -> None:
    header = _read_source(REPO_ROOT / APPLICATION_HEADER)
    source = _read_source(REPO_ROOT / APPLICATION_SOURCE)
    app_body = _type_body(header, "class", "Application")
    compact_body = _compact(app_body)

    owner = "std::unique_ptr<GeometryJobManager>_geometryJobManager;"
    assert owner in compact_body, "Application must own GeometryJobManager by unique_ptr"
    owner_offset = compact_body.index(owner)
    compact_access_prefix = compact_body[:owner_offset]
    assert compact_access_prefix.rfind("private:") > compact_access_prefix.rfind("public:")

    accessor_pattern = re.compile(r"\bGeometryJobManager\s*&\s*geometryJobManager\s*\(")
    const_accessor_pattern = re.compile(
        r"\bconst\s+GeometryJobManager\s*&\s*geometryJobManager\s*\(\s*\)\s*const"
    )
    accessor = accessor_pattern.search(app_body)
    const_accessor = const_accessor_pattern.search(app_body)
    assert accessor and _access_before(app_body, accessor.start()) == "public"
    assert const_accessor and _access_before(app_body, const_accessor.start()) == "public"

    constructor = _compact(_body_for(source, "Application::Application"))
    assert "_geometryJobManager=std::make_unique<GeometryJobManager>();" in constructor
    destructor = _body_for(source, "Application::~Application")
    assert "_geometryJobManager.release(" not in destructor
    bodies = _function_bodies(source, "Application::geometryJobManager")
    assert len(bodies) == 2
    for _, body, _ in bodies:
        assert "return *_geometryJobManager" in body

    manager_source = _read_source(REPO_ROOT / MANAGER_SOURCE)
    assert _function_bodies(manager_source, "GeometryJobManager::GeometryJobManager")
    assert _has_destructor_definition(manager_source, "GeometryJobManager")

    sources = {path: _read_source(REPO_ROOT / path) for path in PRODUCTION_OWNERSHIP_SOURCES}
    violations = _singleton_violations(sources)
    assert not violations, "GeometryJobManager singleton/global violations:\n" + "\n".join(violations)


def test_geometry_job_contracts_are_pointer_free_values() -> None:
    header = _read_source(REPO_ROOT / MANAGER_HEADER)
    violations = _pointer_contract_violations(header)
    assert not violations, "geometry job contract authority violations:\n" + "\n".join(violations)


def test_geometry_and_lightweight_lanes_enforce_preparation_policy() -> None:
    policy_header = _read_source(REPO_ROOT / POLICY_HEADER)
    manager_header = _read_source(REPO_ROOT / MANAGER_HEADER)
    manager_source = _read_source(REPO_ROOT / MANAGER_SOURCE)
    executor_header = _read_source(REPO_ROOT / EXECUTOR_HEADER)
    executor_source = _read_source(REPO_ROOT / EXECUTOR_SOURCE)
    dcs_source = _read_source(REPO_ROOT / DCS_SOURCE)

    assert _policy_members(policy_header) == (
        "Inline",
        "DetachedInProcess",
        "IsolatedProcess",
    )
    request_body = _compact(_type_body(manager_header, "struct", "GeometryJobRequest"))
    assert "PreparationPolicypolicy{PreparationPolicy::IsolatedProcess};" in request_body

    validation = _compact(_body_for(manager_source, "validateRequest"))
    assert "if(request.policy!=PreparationPolicy::IsolatedProcess)" in validation
    assert "throwstd::invalid_argument(" in validation

    lightweight = _lightweight_policy_violations(executor_header, executor_source)
    assert not lightweight, "lightweight executor policy violations:\n" + "\n".join(lightweight)

    dcs_body = _body_for(dcs_source, "DocumentCollaborationService::prepareEditAsyncOnDocumentThread")
    dcs_raw_body = _body_for(
        dcs_source, "DocumentCollaborationService::prepareEditAsyncOnDocumentThread", raw=True
    )
    compact_dcs = _compact(dcs_body)
    assert "preparationPolicy=preparation.policy;" in compact_dcs
    assert "executor.submit(std::move(detachedTask),preparationPolicy)" in compact_dcs
    assert "if(preparation.policy!=PreparationPolicy::DetachedInProcess)" in compact_dcs
    assert "isolated geometry preparation requires a GeometryJobManager adapter" in dcs_raw_body


def test_prepared_edit_executor_contains_no_heavy_geometry() -> None:
    sources = {
        EXECUTOR_HEADER: _read_source(REPO_ROOT / EXECUTOR_HEADER),
        EXECUTOR_SOURCE: _read_source(REPO_ROOT / EXECUTOR_SOURCE),
    }
    violations = _heavy_geometry_violations(sources)
    assert not violations, "PreparedEditExecutor heavy geometry violations:\n" + "\n".join(violations)


def test_geometry_job_manager_is_listed_in_production_and_test_cmake() -> None:
    app_cmake = _suppress_cpp_non_code(_read_source(REPO_ROOT / APP_CMAKE))
    test_cmake = _suppress_cpp_non_code(_read_source(REPO_ROOT / TEST_APP_CMAKE))
    assert re.search(r"(?<![A-Za-z0-9_])GeometryJobManager\.cpp(?![A-Za-z0-9_])", app_cmake)
    assert re.search(r"(?<![A-Za-z0-9_])GeometryJobManager\.h(?![A-Za-z0-9_])", app_cmake)
    assert re.search(r"(?<![A-Za-z0-9_])GeometryJobManager\.cpp(?![A-Za-z0-9_])", test_cmake)


def test_singleton_scanner_rejects_static_instance_and_instance_accessor() -> None:
    source = """
        static GeometryJobManager processSingleton;
        GeometryJobManager& GeometryJobManager::instance() { return processSingleton; }
    """
    violations = _singleton_violations({"src/App/Injected.cpp": source})
    assert any("static GeometryJobManager ownership" in item for item in violations)
    assert any("GeometryJobManager::instance" in item for item in violations)


def test_destructor_gate_rejects_declaration_without_definition() -> None:
    source = "class GeometryJobManager { ~GeometryJobManager(); };"
    assert not _has_destructor_definition(source, "GeometryJobManager")


def test_contract_scanner_rejects_live_document_pointer_field() -> None:
    header = """
        using GeometryJobId = std::uint64_t;
        struct GeometryJobRequest { Document* document; };
    """
    violations = _pointer_contract_violations(header, ("GeometryJobRequest",))
    assert "GeometryJobRequest: live/shared authority token Document" in violations
    assert "GeometryJobRequest: raw pointer field/token" in violations


def test_lightweight_policy_gate_rejects_missing_guard() -> None:
    header = """
        class PreparedEditExecutor {
            PreparedEditExecutionId submit(
                CollaborativeOperationPreparation::DetachedTask task,
                PreparationPolicy policy);
        };
    """
    source = """
        PreparedEditExecutionId submit(
            CollaborativeOperationPreparation::DetachedTask task,
            PreparationPolicy policy)
        {
            return enqueue(std::move(task));
        }
    """
    violations = _lightweight_policy_violations(header, source)
    assert "PreparedEditExecutor submit lacks DetachedInProcess-only guard" in violations


def test_heavy_geometry_scanner_rejects_occ_types() -> None:
    source = "void prepare() { TopoDS_Shape shape; BRepAlgoAPI_Fuse fuse; }"
    violations = _heavy_geometry_violations({"src/App/InjectedExecutor.cpp": source})
    assert any("TopoDS_Shape" in item for item in violations)
    assert any("BRepAlgoAPI_Fuse" in item for item in violations)


def test_scanners_ignore_comments_and_literals_and_reader_preserves_bytes(tmp_path: Path) -> None:
    source_path = tmp_path / "Inert.cpp"
    source_path.write_bytes(
        b'// TopoDS_Shape GeometryJobManager::instance()\n'
        b'const char* text = "BRepAlgoAPI_Fuse Document*";\n'
        b'const char marker = \'*\';\n'
        b'const char* raw = R"gate(Part::Feature OCC_VERSION)gate";\n'
        b'// non-UTF-8: \xff\n'
    )
    source = _read_source(source_path)
    assert "\udcff" in source
    assert not _heavy_geometry_violations({"src/App/Inert.cpp": source})
    assert not _singleton_violations({"src/App/Inert.cpp": source})
