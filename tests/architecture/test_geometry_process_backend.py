# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP08 gate for the isolated FreeCADCmd geometry boundary."""

from __future__ import annotations

from pathlib import Path
import re
from typing import Mapping, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]

APPLICATION_SOURCE = "src/App/Application.cpp"
APP_CMAKE = "src/App/CMakeLists.txt"
MANAGER_HEADER = "src/App/GeometryJobManager.h"
MANAGER_SOURCE = "src/App/GeometryJobManager.cpp"
BACKEND_HEADER = "src/App/GeometryProcessBackend.h"
BACKEND_SOURCE = "src/App/GeometryProcessBackend.cpp"
WORKER_HEADER = "src/App/GeometryWorkerMain.h"
WORKER_SOURCE = "src/App/GeometryWorkerMain.cpp"
MAIN_CMD_SOURCE = "src/Main/MainCmd.cpp"
TEST_APP_CMAKE = "tests/src/App/CMakeLists.txt"
MANAGER_TEST = "tests/src/App/GeometryJobManager.cpp"
BACKEND_TEST = "tests/src/App/GeometryProcessBackend.cpp"
TEST_WORKER_SOURCE = "tests/src/App/GeometryProcessTestWorker.cpp"
GENERIC_RECOMPUTE_SOURCE = "src/App/GenericIsolatedRecompute.cpp"

BOUNDARY_FILES = (
    MANAGER_HEADER,
    MANAGER_SOURCE,
    BACKEND_HEADER,
    BACKEND_SOURCE,
    WORKER_HEADER,
    WORKER_SOURCE,
    MAIN_CMD_SOURCE,
)

MANAGER_CONTRACTS = (
    "GeometryJobRequest",
    "GeometryJobProgress",
    "GeometryJobStatus",
    "GeometryJobResult",
    "GeometryJobDispatch",
)
BACKEND_CONTRACTS = ("GeometryProcessBackendOptions",)

LIVE_AUTHORITY_TOKENS = (
    "Document",
    "DocumentObject",
    "DocumentCommitCoordinator",
    "DocumentCollaborationService",
    "PreparedEdit",
    "Transaction",
    "std::function",
    "shared_ptr",
    "weak_ptr",
    "unique_ptr",
    "reference_wrapper",
)

HEAVY_OR_FALLBACK_RE = re.compile(
    r"(?P<token>\bTopoDS(?:_[A-Za-z0-9_]+)?\b|"
    r"\bBRep[A-Za-z0-9_]*\b|"
    r"\bOpenCASCADE\b|"
    r"\bOCC(?:_[A-Za-z0-9_]+)?\b|"
    r"\bPart\s*::|"
    r"\bGeom(?:2d)?_[A-Za-z0-9_]+\b|"
    r"\bgp_[A-Za-z0-9_]+\b|"
    r"\brecompute(?:Feature)?\s*\(|"
    r"\bPreparedEditExecutor\b|"
    r"\bQProcess\s*::\s*execute\s*\(|"
    r"\bstd\s*::\s*system\s*\(|"
    r"\bpopen\s*\()"
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
    if any(character.isspace() or character in "()\\" for character in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    closing = source.find(terminator, opening + 1)
    return len(source) if closing < 0 else closing + len(terminator)


def _quoted_literal_end(source: str, start: int) -> int:
    quote = source[start]
    end = start + 1
    escaped = False
    while end < len(source):
        character = source[end]
        end += 1
        if escaped:
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == quote:
            break
    return end


def _is_numeric_separator(source: str, index: int) -> bool:
    return (
        source[index] == "'"
        and index > 0
        and index + 1 < len(source)
        and source[index - 1].isdigit()
        and source[index + 1].isdigit()
    )


def _blank_non_newlines(characters: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if characters[index] not in "\r\n":
            characters[index] = " "


def _suppress_cpp_non_code(source: str) -> str:
    """Blank comments and literals while preserving source offsets and lines."""

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
        if _is_numeric_separator(source, index):
            index += 1
            continue
        if source[index] in {'"', "'"}:
            end = _quoted_literal_end(source, index)
            _blank_non_newlines(result, index, end)
            index = end
            continue
        index += 1
    return "".join(result)


def _suppress_cpp_comments(source: str) -> str:
    """Blank comments while retaining trusted string-literal protocol values."""

    result = list(source)
    index = 0
    while index < len(source):
        raw_end = _raw_literal_end(source, index)
        if raw_end is not None:
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
        if _is_numeric_separator(source, index):
            index += 1
            continue
        if source[index] in {'"', "'"}:
            index = _quoted_literal_end(source, index)
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


def _is_constructor_name(name: str) -> bool:
    parts = name.split("::")
    return len(parts) >= 2 and parts[-1] == parts[-2]


def _is_definition_suffix(name: str, suffix: str) -> bool:
    if _is_constructor_name(name) and suffix.lstrip().startswith(":"):
        return True
    remainder = re.sub(r"\b(?:const|volatile|override|final)\b", "", suffix)
    remainder = re.sub(r"\bnoexcept(?:\s*\([^;{}]*\))?", "", remainder)
    return not remainder.strip()


def _function_bodies(source: str, name: str) -> list[tuple[str, str, str]]:
    stripped = _suppress_cpp_non_code(source)
    pattern = re.compile(r"(?<![A-Za-z0-9_:])" + re.escape(name) + r"\s*\(")
    bodies: list[tuple[str, str, str]] = []
    for match in pattern.finditer(stripped):
        opening_parenthesis = stripped.find("(", match.start())
        closing_parenthesis = _matching_delimiter(
            stripped, opening_parenthesis, "(", ")"
        )
        if closing_parenthesis is None:
            continue
        semicolon = stripped.find(";", closing_parenthesis + 1)
        opening_brace = stripped.find("{", closing_parenthesis + 1)
        if opening_brace < 0 or (semicolon >= 0 and semicolon < opening_brace):
            continue
        suffix = stripped[closing_parenthesis + 1:opening_brace]
        if not _is_definition_suffix(name, suffix):
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
            item
            for item in candidates
            if fragment in re.sub(r"\s+", "", item[0])
        ]
    assert len(candidates) == 1, (
        f"expected one body for {name}"
        + (f" containing {signature_contains}" if signature_contains else "")
        + f", found {len(candidates)}"
    )
    return candidates[0][2 if raw else 1]


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


def _compact(source: str) -> str:
    return re.sub(r"\s+", "", source)


def _field_declarator(statement: str) -> str:
    brace = statement.find("{")
    equals = statement.find("=")
    cutoffs = [offset for offset in (brace, equals) if offset >= 0]
    return statement[:min(cutoffs)] if cutoffs else statement


def _contract_violations(
    header: str, contracts: Sequence[str]
) -> list[str]:
    violations: list[str] = []
    for name in contracts:
        try:
            body = _type_body(header, "struct", name)
        except AssertionError as error:
            violations.append(str(error))
            continue
        for statement in body.split(";"):
            field = statement.strip()
            if not field or "(" in field:
                continue
            declarator = _field_declarator(field)
            for token in LIVE_AUTHORITY_TOKENS:
                if re.search(
                    r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])",
                    declarator,
                ):
                    violations.append(f"{name}: live/shared authority field contains {token}")
            if "*" in declarator:
                violations.append(f"{name}: raw pointer field")
            if "&" in declarator:
                violations.append(f"{name}: reference authority field")
    return violations


def _executable_resolution_violations(
    source: str, worker_source: str | None = None
) -> list[str]:
    violations: list[str] = []
    try:
        resolver = _compact(
            _suppress_cpp_comments(_body_for(source, "installedWorkerExecutable", raw=True))
        )
    except AssertionError as error:
        return [str(error)]
    required = {
        "default worker is not resolved relative to the current executable": (
            "Internal::currentExecutablePath().parent_path()"
        ),
        "missing Windows sibling FreeCADCmd.exe": 'executable/="FreeCADCmd.exe";',
        "missing Unix sibling FreeCADCmd": 'executable/="FreeCADCmd";',
    }
    for message, fragment in required.items():
        if fragment not in resolver:
            violations.append(message)

    code = _compact(_suppress_cpp_comments(source))
    if "if(options.executable.empty()){options.executable=installedWorkerExecutable();}" not in code:
        violations.append("empty executable does not select the trusted sibling default")
    if "process.setProgram(toQStringPath(options.executable));" not in code:
        violations.append("QProcess does not receive the resolved filesystem path")
    forbidden = (
        'return"FreeCADCmd";',
        'return"FreeCADCmd.exe";',
        'process.setProgram("FreeCADCmd")',
        'process.setProgram("FreeCADCmd.exe")',
        "QStandardPaths::findExecutable(",
        'std::getenv("PATH")',
        'qEnvironmentVariable("PATH")',
    )
    for token in forbidden:
        if token in code:
            violations.append(f"PATH/bare-program worker resolution is forbidden: {token}")

    if worker_source is None:
        violations.append("missing shared OS-native current-executable resolver source")
        return violations

    try:
        os_resolver = _compact(
            _suppress_cpp_comments(
                _body_for(
                    worker_source,
                    "App::Internal::currentExecutablePath",
                    raw=True,
                )
            )
        )
    except AssertionError as error:
        violations.append(str(error))
        return violations
    resolver_requirements = {
        "Windows executable resolver must use GetModuleFileNameW": "GetModuleFileNameW(",
        "Linux executable resolver must use /proc/self/exe readlink": (
            '::readlink("/proc/self/exe",'
        ),
        "macOS executable resolver must use _NSGetExecutablePath": "_NSGetExecutablePath(",
        "macOS executable result must be canonicalized": "std::filesystem::weakly_canonical(",
    }
    for message, fragment in resolver_requirements.items():
        if fragment not in os_resolver:
            violations.append(message)
    combined = _compact(_suppress_cpp_comments(source + "\n" + worker_source))
    for qcore_path in (
        "QCoreApplication::applicationDirPath()",
        "QCoreApplication::applicationFilePath()",
    ):
        if qcore_path in combined:
            violations.append(
                f"headless executable resolution must not depend on {qcore_path}"
            )
    return violations


def _build_fingerprint_violations(
    worker_header: str,
    worker_source: str,
    backend_source: str,
    native_test: str,
) -> list[str]:
    violations: list[str] = []
    try:
        runtime = _compact(
            _suppress_cpp_comments(
                _body_for(worker_source, "workerRuntimeLibrary", raw=True)
            )
        )
        add_file = _compact(_body_for(worker_source, "addFingerprintFile"))
        fingerprint = _compact(
            _suppress_cpp_comments(
                _body_for(
                    worker_source,
                    "App::Internal::geometryWorkerBuildFingerprint",
                    raw=True,
                )
            )
        )
        worker_main = _compact(
            _body_for(worker_source, "App::Internal::runGeometryWorkerMain")
        )
        parent = _compact(_body_for(backend_source, "runChecked"))
    except AssertionError as error:
        return [str(error)]

    header = _compact(_suppress_cpp_non_code(worker_header))
    if (
        "AppExportstd::stringgeometryWorkerBuildFingerprint("
        "conststd::filesystem::path&workerExecutable);"
    ) not in header:
        violations.append("shared geometry worker build fingerprint is not declared")

    runtime_candidates = (
        'directory/"FreeCADApp.dll"',
        'directory/"libFreeCADApp.dylib"',
        'directory.parent_path()/"lib"/"libFreeCADApp.dylib"',
        'directory/"libFreeCADApp.so"',
        'directory.parent_path()/"lib"/"libFreeCADApp.so"',
        'directory.parent_path()/"lib64"/"libFreeCADApp.so"',
    )
    for candidate in runtime_candidates:
        if candidate not in runtime:
            violations.append(f"missing exact FreeCADApp runtime candidate: {candidate}")
    if "std::filesystem::is_regular_file(candidate,error)" not in runtime:
        violations.append("FreeCADApp runtime candidates are not validated as exact files")

    if "QFilefile(toQStringPath(path))" not in add_file:
        violations.append("fingerprint input does not read the exact native file path")
    label = add_file.find("hash.addData(label)")
    bytes_read = add_file.find("while(!file.atEnd())")
    payload = add_file.find("hash.addData(chunk)")
    if label < 0 or bytes_read < 0 or payload < 0 or not label < bytes_read < payload:
        violations.append("fingerprint input is not deterministic label then exact file bytes")

    launcher_label = fingerprint.find('QByteArray("FreeCADCmd\\0",11)')
    runtime_label = fingerprint.find('QByteArray("FreeCADApp\\0",11)')
    runtime_input = fingerprint.find("runtimeLibrary", runtime_label)
    if fingerprint.count("addFingerprintFile(hash,") != 2:
        violations.append("composite build fingerprint does not include exactly two files")
    if launcher_label < 0:
        violations.append("composite build fingerprint lacks the labeled FreeCADCmd bytes")
    if runtime_label < 0 or runtime_input < runtime_label:
        violations.append("composite build fingerprint lacks the labeled FreeCADApp bytes")
    if launcher_label >= 0 and runtime_label >= 0 and launcher_label > runtime_label:
        violations.append("composite build fingerprint label order is not deterministic")
    for fragment in (
        "constautoruntimeLibrary=workerRuntimeLibrary(workerExecutable)",
        "workerExecutable.empty()||runtimeLibrary.empty()",
        "QCryptographicHashhash(QCryptographicHash::Sha256)",
    ):
        if fragment not in fingerprint:
            violations.append(f"composite build fingerprint step is missing: {fragment}")

    if (
        "Internal::geometryWorkerBuildFingerprint(options.executable)"
        not in parent
    ):
        violations.append("parent does not use the shared composite build fingerprint")
    if (
        "Internal::geometryWorkerBuildFingerprint("
        "Internal::currentExecutablePath())"
        not in worker_main
    ):
        violations.append("worker self-check does not use the shared composite fingerprint")
    native = _compact(_suppress_cpp_non_code(native_test))
    if "expected.buildFingerprint=Internal::geometryWorkerBuildFingerprint(worker)" not in native:
        violations.append("native result validation does not use the shared composite fingerprint")

    combined = _suppress_cpp_non_code(worker_source + "\n" + backend_source)
    for launcher_only in ("executableSha256(", "fileSha256("):
        if launcher_only in combined:
            violations.append(f"launcher-only worker hashing is forbidden: {launcher_only}")
    return violations


def _unicode_path_violations(
    manager_header: str, backend_source: str, worker_source: str
) -> list[str]:
    violations: list[str] = []
    try:
        result = _compact(_type_body(manager_header, "struct", "GeometryJobResult"))
        backend_from = _compact(_body_for(backend_source, "fromQStringPath"))
        backend_to = _compact(_body_for(backend_source, "toQStringPath"))
        worker_from = _compact(_body_for(worker_source, "pathEnvironment"))
        worker_to = _compact(_body_for(worker_source, "toQStringPath"))
        parent = _compact(
            _suppress_cpp_comments(_body_for(backend_source, "runChecked", raw=True))
        )
        child = _compact(
            _suppress_cpp_comments(
                _body_for(
                    worker_source,
                    "App::Internal::runGeometryWorkerMain",
                    raw=True,
                )
            )
        )
    except AssertionError as error:
        return [str(error)]

    if "std::filesystem::pathresultArtifact;" not in result:
        violations.append("GeometryJobResult.resultArtifact is not a native filesystem path")
    conversion_requirements = {
        "backend Windows QString path decode is not wide": (
            backend_from,
            "std::filesystem::path(value.toStdWString())",
        ),
        "backend Unix QString path decode is not UTF-8 native": (
            backend_from,
            "value.toUtf8()",
        ),
        "backend Unix QString path decode does not publish UTF-8 bytes": (
            backend_from,
            "std::filesystem::path(encoded.constData())",
        ),
        "backend Windows path encode is not wide": (
            backend_to,
            "QString::fromStdWString(value.wstring())",
        ),
        "backend Unix path encode is not UTF-8 native": (
            backend_to,
            "QString::fromUtf8(value.native())",
        ),
        "worker Windows environment path decode is not wide": (
            worker_from,
            "std::filesystem::path(value.toStdWString())",
        ),
        "worker Unix environment path decode is not UTF-8 native": (
            worker_from,
            "value.toUtf8()",
        ),
        "worker Unix environment path decode does not publish UTF-8 bytes": (
            worker_from,
            "std::filesystem::path(encoded.constData())",
        ),
        "worker Windows path encode is not wide": (
            worker_to,
            "QString::fromStdWString(value.wstring())",
        ),
        "worker Unix path encode is not UTF-8 native": (
            worker_to,
            "QString::fromUtf8(value.native())",
        ),
    }
    for message, (body, fragment) in conversion_requirements.items():
        if fragment not in body:
            violations.append(message)

    parent_path_uses = (
        "process.setProgram(toQStringPath(options.executable))",
        "process.setWorkingDirectory(toQStringPath(workspace))",
        "process.setStandardOutputFile(toQStringPath(stdoutPath)",
        "process.setStandardErrorFile(toQStringPath(stderrPath)",
        "environment.insert(RequestEnvironment,toQStringPath(requestPath))",
        "environment.insert(ResultEnvironment,toQStringPath(workerResultPath))",
        "environment.insert(HeartbeatEnvironment,toQStringPath(heartbeatPath))",
        "environment.insert(StartGateEnvironment,toQStringPath(startGatePath))",
        "environment.insert(CancelEnvironment,toQStringPath(cancelPath))",
        "GeometryJobState::Completed,completedPath,published.archiveDigest",
    )
    for fragment in parent_path_uses:
        if fragment not in parent:
            violations.append(f"parent transport path bypasses native conversion: {fragment}")

    for path_name, environment_name in (
        ("requestPath", "RequestEnvironment"),
        ("resultPath", "ResultEnvironment"),
        ("heartbeatPath", "HeartbeatEnvironment"),
        ("startGatePath", "StartGateEnvironment"),
        ("cancelPath", "CancelEnvironment"),
    ):
        required = (
            f"conststd::filesystem::path{path_name}="
            f"pathEnvironment({environment_name})"
        )
        if required not in child:
            violations.append(f"worker transport path is not decoded natively: {path_name}")

    narrow_patterns = (
        "completedPath.string()",
        "requestPath.string()",
        "workerResultPath.string()",
        "heartbeatPath.string()",
        "startGatePath.string()",
        "cancelPath.string()",
        "workspace.string()",
        "options.executable.string()",
        "requestPath.toStdString()",
        "resultPath.toStdString()",
        "heartbeatPath.toStdString()",
        "startGatePath.toStdString()",
        "cancelPath.toStdString()",
    )
    transport_code = _compact(_suppress_cpp_non_code(backend_source + "\n" + worker_source))
    for pattern in narrow_patterns:
        if pattern in transport_code:
            violations.append(f"narrow transport artifact path conversion is forbidden: {pattern}")
    return violations


def _windows_containment_violations(source: str) -> list[str]:
    code = _compact(_suppress_cpp_non_code(source))
    violations: list[str] = []
    for message, fragment in {
        "missing KILL_ON_JOB_CLOSE containment": "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE",
        "missing Job Object limit configuration": "SetInformationJobObject(handle,",
        "missing worker Job Object assignment": "AssignProcessToJobObject(handle,process)",
        "missing whole-Job termination": "TerminateJobObject(handle,70)",
    }.items():
        if fragment not in code:
            violations.append(message)
    try:
        run = _compact(_body_for(source, "runChecked"))
    except AssertionError as error:
        violations.append(str(error))
        return violations
    assignment = run.find("windowsJob.assign(process.processId())")
    gate = run.find("std::ofstreamstart(startGatePath")
    if assignment < 0 or gate < 0 or assignment > gate:
        violations.append("Windows Job Object assignment must precede start-gate release")
    return violations


def _unix_tree_violations(source: str) -> list[str]:
    code = _compact(_suppress_cpp_non_code(source))
    violations: list[str] = []
    if "QProcess::UnixProcessFlag::CreateNewSession" not in code:
        violations.append("worker does not start in a new Unix session/process group")
    try:
        terminate = _compact(_body_for(source, "terminateProcessTree"))
    except AssertionError as error:
        violations.append(str(error))
        return violations
    term = terminate.find("::kill(-processId,SIGTERM)")
    wait = terminate.find("process.waitForFinished(")
    kill = terminate.find("::kill(-processId,SIGKILL)")
    if term < 0:
        violations.append("Unix termination does not signal the worker process group with TERM")
    if kill < 0:
        violations.append("Unix termination does not escalate the worker process group with KILL")
    if term < 0 or wait < 0 or kill < 0 or not term < wait < kill:
        violations.append("Unix process-group TERM/grace/KILL ordering is missing")
    if "::kill(processId,SIGTERM)" in terminate or "::kill(processId,SIGKILL)" in terminate:
        violations.append("Unix termination targets only the worker leader")
    return violations


def _cooperative_cancel_violations(
    backend_source: str, worker_source: str
) -> list[str]:
    violations: list[str] = []
    try:
        run = _compact(
            _suppress_cpp_comments(_body_for(backend_source, "runChecked", raw=True))
        )
        worker = _compact(
            _suppress_cpp_comments(
                _body_for(
                    worker_source,
                    "App::Internal::runGeometryWorkerMain",
                    raw=True,
                )
            )
        )
    except AssertionError as error:
        return [str(error)]

    backend_code = _compact(_suppress_cpp_comments(backend_source))
    worker_code = _compact(_suppress_cpp_comments(worker_source))
    for message, fragment in {
        "backend lacks the FCG cancel environment binding": (
            'constexprautoCancelEnvironment="FREECAD_GEOMETRY_CANCEL";'
        ),
        "cancel signal is not scoped to the per-job workspace": (
            'constautocancelPath=workspace/"cancel";'
        ),
        "cancel path is not passed to the worker": (
            "environment.insert(CancelEnvironment,toQStringPath(cancelPath));"
        ),
    }.items():
        target = backend_code if "environment binding" in message else run
        if fragment not in target:
            violations.append(message)

    signal = run.find("std::ofstreamcancel(cancelPath,")
    signalled_at = run.find(
        "cancellationSignalledAt=std::chrono::steady_clock::now()", signal
    )
    grace = run.find(
        "std::chrono::steady_clock::now()-cancellationSignalledAt"
        ">=options.terminationGrace"
    )
    force = run.find("terminateProcessTree(process,", grace)
    if signal < 0 or signalled_at < 0:
        violations.append("manager cancellation does not publish the workspace cancel signal")
    if (
        signal < 0
        or signalled_at < 0
        or grace < 0
        or force < 0
        or not signal < signalled_at < grace < force
    ):
        violations.append(
            "manager cancellation does not signal, wait configured grace, then force"
        )
    if "status->cancellationRequested&&!cancellationSignalled" not in run:
        violations.append("cancel signal is not emitted exactly once after manager cancellation")
    if (
        "isManagerTerminal(status->state)&&"
        "status->state!=GeometryJobState::Cancelled"
    ) not in run:
        violations.append("generic forced termination does not exempt cooperative cancellation")
    cooperative = run.find("if(cancellationSignalled)")
    cancelled = run.find("GeometryJobState::Cancelled", cooperative)
    diagnostic = run.find('"isolatedgeometryworkercancelledcooperatively"', cooperative)
    if cooperative < 0 or cancelled < cooperative or diagnostic < cancelled:
        violations.append("cooperative worker exit is not published as Cancelled with a diagnostic")

    for message, fragment in {
        "worker lacks the FCG cancel environment binding": (
            'constexprautoCancelEnvironment="FREECAD_GEOMETRY_CANCEL";'
        ),
        "worker does not read its cancel path": (
            "conststd::filesystem::pathcancelPath=pathEnvironment(CancelEnvironment);"
        ),
        "worker does not validate the cancel file inside its workspace": (
            '!isWorkspaceFile(cancelPath,"cancel")'
        ),
    }.items():
        target = worker_code if "environment binding" in message else worker
        if fragment not in target:
            violations.append(message)
    return violations


def _worker_operation_violations(source: str) -> list[str]:
    try:
        body = _compact(
            _suppress_cpp_comments(
                _body_for(source, "App::Internal::runGeometryWorkerMain", raw=True)
            )
        )
    except AssertionError as error:
        return [str(error)]
    violations: list[str] = []
    probe_guard = body.find('if(operation=="FreeCAD.Internal.GeometryProbe"){')
    if probe_guard < 0:
        violations.append("worker lacks the exact internal transport-probe allowlist")
    else:
        probe_open = body.find("{", probe_guard)
        probe_close = _matching_delimiter(body, probe_open, "{", "}")
        if probe_close is None:
            violations.append("transport-probe special case has no bounded body")
        else:
            probe_body = body[probe_open + 1:probe_close]
            if probe_body != "output=*input.archive;":
                violations.append(
                    "transport probe must only round-trip the validated input archive"
                )

            else_start = body.find("else{", probe_close)
            if else_start != probe_close + 1:
                violations.append(
                    "all non-probe operations must enter one explicit registry branch"
                )
            else:
                else_open = body.find("{", else_start)
                else_close = _matching_delimiter(body, else_open, "{", "}")
                if else_close is None:
                    violations.append("native worker registry branch has no bounded body")
                else:
                    native = body[else_open + 1:else_close]
                    registry = native.find(
                        "GeometryWorkerOperationRegistry::instance()"
                    )
                    app_registration = native.find(
                        "Internal::ensureGenericIsolatedRecomputeRegistered()"
                    )
                    contains_guards = [
                        match.start()
                        for match in re.finditer(
                            re.escape("if(!registry.contains(operation)){"), native
                        )
                    ]
                    import_guard = contains_guards[0] if len(contains_guards) == 2 else -1
                    unsupported = contains_guards[1] if len(contains_guards) == 2 else -1
                    import_close = None
                    unsupported_close = None
                    if import_guard >= 0:
                        import_open = native.find("{", import_guard)
                        import_close = _matching_delimiter(
                            native, import_open, "{", "}"
                        )
                    if unsupported >= 0:
                        unsupported_open = native.find("{", unsupported)
                        unsupported_close = _matching_delimiter(
                            native, unsupported_open, "{", "}"
                        )
                    execute = native.find(
                        "registry.execute(operation,*input.archive,cancellation.token())"
                    )
                    if registry < 0 or app_registration <= registry:
                        violations.append(
                            "App generic worker registration is not after registry acquisition"
                        )
                    if len(contains_guards) != 2:
                        violations.append(
                            "worker must use exactly one conditional Part import and one fail-closed contains guard"
                        )
                    if import_guard >= 0 and import_close is not None:
                        import_body = native[import_guard + len(
                            "if(!registry.contains(operation)){"
                        ):import_close]
                        if import_body != 'Base::Interpreter().runString("importPart");':
                            violations.append(
                                "conditional module registration must only import Part"
                            )
                    if not (
                        registry >= 0
                        and app_registration > registry
                        and import_guard > app_registration
                        and import_close is not None
                        and unsupported > import_close
                    ):
                        violations.append(
                            "worker registration order is not App generic then optional Part then final validation"
                        )
                    if (
                        unsupported < 0
                        or unsupported_close is None
                        or "return13;"
                        not in native[unsupported_open + 1:unsupported_close]
                    ):
                        violations.append(
                            "unsupported worker operation does not fail closed"
                        )
                    if (
                        execute < 0
                        or unsupported_close is None
                        or execute <= unsupported_close
                        or native.count("registry.execute(") != 1
                    ):
                        violations.append(
                            "native worker execute is not after registry contains validation"
                        )
                    publication = body.find("output.metadata=input.archive->metadata;", else_close)
                    if publication <= else_close:
                        violations.append(
                            "worker output publication is not after probe/native dispatch"
                        )
    code = _suppress_cpp_non_code(source)
    for token in ("executeFallback", "runGenericGeometry", "recompute(", "recomputeFeature("):
        if token in code:
            violations.append(f"unsupported operation fallback is forbidden: {token}")
    return violations


def _execution_boundary_violations(sources: Mapping[str, str]) -> list[str]:
    violations: list[str] = []
    for path, source in sorted(sources.items()):
        code = _suppress_cpp_non_code(source)
        for match in HEAVY_OR_FALLBACK_RE.finditer(code):
            line = code.count("\n", 0, match.start("token")) + 1
            violations.append(f"{path}:{line}: forbidden geometry/fallback token {match.group('token')!r}")
    return violations


def test_application_owns_and_starts_process_backend_only_outside_worker() -> None:
    application = _read_source(REPO_ROOT / APPLICATION_SOURCE)
    constructor = _compact(_body_for(application, "Application::Application"))
    assert "_geometryJobManager=std::make_unique<GeometryJobManager>();" in constructor
    route = (
        "if(!Internal::geometryWorkerRequested()){"
        "_geometryJobManager->startProcessBackend("
        "Internal::GeometryProcessBackendOptions{});"
        "}"
    )
    assert route in constructor, "Application must not recursively start a backend inside a worker"
    destructor = _compact(_body_for(application, "Application::~Application"))
    assert "_geometryJobManager.reset();" in destructor

    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    violations = _executable_resolution_violations(backend, worker)
    assert not violations, "worker executable resolution violations:\n" + "\n".join(violations)
    worker_header = _compact(_suppress_cpp_non_code(_read_source(REPO_ROOT / WORKER_HEADER)))
    assert "AppExportstd::filesystem::pathcurrentExecutablePath();" in worker_header


def test_fcg_request_result_and_parent_worker_bindings_are_exact() -> None:
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    backend_literals = _compact(_suppress_cpp_comments(backend))
    worker_literals = _compact(_suppress_cpp_comments(worker))
    assert 'constexprautoWorkerProtocolValue="FCG/1";' in backend_literals
    assert 'constexprautoWorkerProtocolValue="FCG/1";' in worker_literals

    parent = _compact(_body_for(backend, "runChecked"))
    for fragment in (
        "input.metadata.protocolVersion=GeometryArchiveProtocolVersion",
        "input.metadata.kind=GeometryArchiveKind::Request",
        "input.metadata.jobId=dispatchPacket.id",
        "input.metadata.policy=PreparationPolicy::IsolatedProcess",
        "input.metadata.deadlineEpochMilliseconds=deadlineEpochMilliseconds(",
        "input.metadata.operationType=dispatchPacket.request.operationType",
        "input.metadata.buildFingerprint=buildFingerprint",
        "input.metadata.inputDigest=dispatchPacket.request.inputDigest",
        "GeometryArchiveCodec::writeAtomic(requestPath,input)",
        "expectation.kind=GeometryArchiveKind::Result",
        "expectation.jobId=dispatchPacket.id",
        "expectation.operationType=dispatchPacket.request.operationType",
        "expectation.buildFingerprint=buildFingerprint",
        "expectation.inputDigest=dispatchPacket.request.inputDigest",
        "GeometryArchiveCodec::readValidated(workerResultPath,expectation)",
    ):
        assert fragment in parent, f"parent-side FCG binding is missing: {fragment}"

    child = _compact(_body_for(worker, "App::Internal::runGeometryWorkerMain"))
    for fragment in (
        "!parseJobId(environment(JobIdEnvironment),jobId)",
        "operation.empty()",
        "expectedBuild.empty()",
        "inputDigest.empty()",
        "conststd::stringactualBuild=Internal::geometryWorkerBuildFingerprint(",
        "Internal::currentExecutablePath()",
        "actualBuild!=expectedBuild",
        "expectation.kind=GeometryArchiveKind::Request",
        "expectation.jobId=jobId",
        "expectation.operationType=operation",
        "expectation.buildFingerprint=expectedBuild",
        "expectation.inputDigest=inputDigest",
        "GeometryArchiveCodec::readValidated(requestPath,expectation)",
        "output.metadata.kind=GeometryArchiveKind::Result",
        "GeometryArchiveCodec::writeAtomic(resultPath,output)",
    ):
        assert fragment in child, f"worker-side FCG binding is missing: {fragment}"


def test_build_identity_binds_labeled_launcher_and_freecadapp_runtime() -> None:
    worker_header = _read_source(REPO_ROOT / WORKER_HEADER)
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    native = _read_source(REPO_ROOT / BACKEND_TEST)
    violations = _build_fingerprint_violations(
        worker_header, worker, backend, native
    )
    assert not violations, "build fingerprint violations:\n" + "\n".join(violations)


def test_process_submission_and_public_contracts_are_pointer_free() -> None:
    manager_header = _read_source(REPO_ROOT / MANAGER_HEADER)
    backend_header = _read_source(REPO_ROOT / BACKEND_HEADER)
    violations = _contract_violations(manager_header, MANAGER_CONTRACTS)
    violations.extend(_contract_violations(backend_header, BACKEND_CONTRACTS))
    assert not violations, "process contract authority violations:\n" + "\n".join(violations)

    compact_header = _compact(_suppress_cpp_non_code(manager_header))
    assert "submit(GeometryJobRequestrequest,GeometryArchiveinputArchive);" in compact_header
    public_manager = _type_body(manager_header, "class", "GeometryJobManager").split("private:", 1)[0]
    assert "GeometryArchive*" not in _compact(public_manager)
    assert "GeometryArchive&" not in _compact(public_manager)
    manager_source = _compact(_suppress_cpp_non_code(_read_source(REPO_ROOT / MANAGER_SOURCE)))
    assert "std::optional<GeometryArchive>inputArchive;" in manager_source
    assert "job.inputArchive=std::move(inputArchive);" in manager_source


def test_transport_artifact_paths_are_native_and_unicode_safe() -> None:
    manager_header = _read_source(REPO_ROOT / MANAGER_HEADER)
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    violations = _unicode_path_violations(manager_header, backend, worker)
    assert not violations, "transport path violations:\n" + "\n".join(violations)

    native = _read_source(REPO_ROOT / BACKEND_TEST)
    assert "installedFreeCADCmdRoundTripsAValidatedProbe" in native
    assert 'std::filesystem::u8path("géométrie-路径")' in native
    assert "std::filesystem::create_directories(unicodeRoot)" in native
    assert "start(manager, worker, unicodeRoot)" in native
    assert "readValidated(result->resultArtifact, expected)" in native
    assert "waitForNoJobDirectories(unicodeRoot)" in native


def test_workspace_is_randomized_owner_marked_and_janitor_scope_is_bounded() -> None:
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    backend_header = _compact(
        _suppress_cpp_comments(_read_source(REPO_ROOT / BACKEND_HEADER))
    )
    create = _compact(_suppress_cpp_comments(_body_for(backend, "createWorkspace", raw=True)))
    for fragment in (
        "for(intattempt=0;attempt<32;++attempt)",
        "QRandomGenerator::system()->generate64()",
        '"job-"+std::to_string(currentProcessId())',
        "+std::to_string(id)",
        "options.workspaceRoot/name",
        "std::filesystem::create_directory(path,error)",
        "std::filesystem::perms::owner_all",
        "path/OwnerMarker",
        "owner<<currentProcessId()",
    ):
        assert fragment in create, f"workspace isolation step is missing: {fragment}"

    cleanup = _compact(_body_for(backend, "~WorkspaceCleanup"))
    assert "removeAll(workspace)" in cleanup
    janitor = _compact(_suppress_cpp_comments(_body_for(backend, "runStartupJanitor", raw=True)))
    for fragment in (
        "std::filesystem::directory_iteratoriterator(options.workspaceRoot,error)",
        'name.starts_with("job-")',
        "constautoowner=readOwner(entry.path())",
        "!owner||!processIsAlive(*owner)",
        "removeAll(entry.path())",
        "name.starts_with(ResultPrefix)",
        "now-written>options.completedArtifactRetention",
    ):
        assert fragment in janitor, f"bounded startup janitor step is missing: {fragment}"
    assert "removeAll(options.workspaceRoot)" not in janitor
    assert "completedArtifactRetention{24}" in backend_header
    assert "options.completedArtifactRetention<=std::chrono::hours::zero()" in _compact(
        _suppress_cpp_non_code(backend)
    )


def test_heartbeat_is_50ms_and_startup_and_gap_detection_are_bounded() -> None:
    header = _compact(_suppress_cpp_non_code(_read_source(REPO_ROOT / BACKEND_HEADER)))
    for fragment in (
        "pollInterval{10}",
        "startupHeartbeatTimeout{30'000}",
        "heartbeatTimeout{2'000}",
        "terminationGrace{500}",
    ):
        assert fragment in header, f"missing bounded backend timing default: {fragment}"

    worker = _compact(_suppress_cpp_non_code(_read_source(REPO_ROOT / WORKER_SOURCE)))
    assert "std::this_thread::sleep_for(50ms)" in worker
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    run = _compact(_body_for(backend, "runChecked"))
    for fragment in (
        "lastHeartbeatSeen=processStarted",
        "heartbeatObserved=false",
        "lastHeartbeatSeen=std::chrono::steady_clock::now()",
        "!heartbeatObserved&&now-processStarted>options.startupHeartbeatTimeout",
        "heartbeatObserved&&now-lastHeartbeatSeen>options.heartbeatTimeout",
        "GeometryJobState::WorkerCrashed",
    ):
        assert fragment in run, f"heartbeat detection step is missing: {fragment}"
    test_source = _compact(_suppress_cpp_non_code(_read_source(REPO_ROOT / BACKEND_TEST)))
    assert "waitForProgress(manager,id,250ms)" in test_source
    assert "Test.NoHeartbeat" not in test_source  # literal is suppressed; structure cannot be spoofed
    test_literals = _compact(_suppress_cpp_comments(_read_source(REPO_ROOT / BACKEND_TEST)))
    assert 'request("Test.NoHeartbeat",5s)' in test_literals


def test_platform_process_containment_terminates_complete_worker_tree() -> None:
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    windows = _windows_containment_violations(backend)
    unix = _unix_tree_violations(backend)
    assert not windows, "Windows worker containment violations:\n" + "\n".join(windows)
    assert not unix, "Unix worker containment violations:\n" + "\n".join(unix)


def test_cancellation_signals_then_waits_bounded_grace_before_tree_termination() -> None:
    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    violations = _cooperative_cancel_violations(backend, worker)
    assert not violations, "cooperative cancellation violations:\n" + "\n".join(violations)

    fixture = _compact(
        _suppress_cpp_comments(_read_source(REPO_ROOT / TEST_WORKER_SOURCE))
    )
    for fragment in (
        'operation=="Test.CooperativeCancel"',
        'environment("FREECAD_GEOMETRY_CANCEL")',
        "std::filesystem::is_regular_file(cancel,error)",
        "return85",
    ):
        assert fragment in fixture, f"cooperative-cancel helper step is missing: {fragment}"
    native = _compact(_suppress_cpp_comments(_read_source(REPO_ROOT / BACKEND_TEST)))
    assert "cancellationOffersABoundedCooperativeExitFirst" in native
    assert 'request("Test.CooperativeCancel")' in native
    assert 'diagnostic.find("cooperatively")' in native


def test_cancel_deadline_crash_oom_and_generic_failure_are_distinct() -> None:
    manager_header = _read_source(REPO_ROOT / MANAGER_HEADER)
    states = set(re.findall(r"\b[A-Za-z_]\w*\b", _type_body(
        manager_header, "enum class", "GeometryJobState"
    )))
    for state in (
        "Cancelled",
        "DeadlineExceeded",
        "WorkerCrashed",
        "WorkerOutOfMemory",
        "Failed",
    ):
        assert state in states

    backend = _read_source(REPO_ROOT / BACKEND_SOURCE)
    classify = _compact(_body_for(backend, "classifyExit"))
    assert "exitCode==WorkerOutOfMemoryExitCode" in classify
    assert "QProcess::CrashExit" in classify
    assert "returnGeometryJobState::Failed" in classify
    run = _compact(_body_for(backend, "run"))
    assert "catch(conststd::bad_alloc&)" in run
    assert "GeometryJobState::WorkerOutOfMemory" in run
    assert run.count("GeometryJobState::Failed") >= 2
    checked = _compact(_body_for(backend, "runChecked"))
    assert "status->cancellationRequested" in checked
    assert "GeometryJobState::Cancelled" in checked

    manager = _read_source(REPO_ROOT / MANAGER_SOURCE)
    expiry = _compact(_body_for(manager, "expireDeadlines"))
    assert "GeometryJobState::DeadlineExceeded" in expiry
    assert "job.cancellationRequested=true" in expiry
    tests = _read_source(REPO_ROOT / BACKEND_TEST)
    assert "classifiesCrashOomTimeoutAndLostHeartbeat" in tests
    assert "cancellationTerminatesTheCompleteWorkerTree" in tests
    manager_tests = _read_source(REPO_ROOT / MANAGER_TEST)
    assert "preservesStructuredWorkerFailureStates" in manager_tests


def test_worker_fails_closed_and_maincmd_dispatches_private_worker_mode() -> None:
    worker = _read_source(REPO_ROOT / WORKER_SOURCE)
    violations = _worker_operation_violations(worker)
    assert not violations, "worker operation violations:\n" + "\n".join(violations)
    worker_body = _compact(_body_for(worker, "App::Internal::runGeometryWorkerMain"))
    assert worker_body.find("waitForStartGate(startGatePath)") < worker_body.find(
        "GeometryArchiveCodec::readValidated(requestPath,expectation)"
    )

    main = _compact(_body_for(_read_source(REPO_ROOT / MAIN_CMD_SOURCE), "main"))
    requested = main.find("App::Internal::geometryWorkerRequested()")
    dispatch = main.find("App::Internal::runGeometryWorkerMain()")
    destruct = main.find("Application::destruct()", dispatch)
    worker_return = main.find("returnworkerExitCode", destruct)
    normal_run = main.find("Application::runApplication()")
    assert 0 <= requested < dispatch < destruct < worker_return < normal_run


def test_production_and_native_process_fixtures_are_wired() -> None:
    app_cmake = _read_source(REPO_ROOT / APP_CMAKE)
    for name in (
        "GeometryJobManager.cpp",
        "GeometryJobManager.h",
        "GeometryProcessBackend.cpp",
        "GeometryProcessBackend.h",
        "GeometryWorkerMain.cpp",
        "GeometryWorkerMain.h",
    ):
        assert re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", app_cmake)

    test_cmake = _compact(_suppress_cpp_comments(_read_source(REPO_ROOT / TEST_APP_CMAKE)))
    for fragment in (
        "GeometryJobManager.cpp",
        "GeometryProcessBackend.cpp",
        "add_executable(GeometryProcessTestWorkerGeometryProcessTestWorker.cpp)",
        "add_dependencies(App_tests_runGeometryProcessTestWorkerFreeCADMainCmd)",
        'GEOMETRY_PROCESS_TEST_WORKER_PATH="$<TARGET_FILE:GeometryProcessTestWorker>"',
        'GEOMETRY_INSTALLED_WORKER_PATH="$<TARGET_FILE:FreeCADMainCmd>"',
        'if(NOTWIN32)add_custom_command(TARGETApp_tests_runPOST_BUILDCOMMAND${CMAKE_COMMAND}-Ecopy_if_different"$<TARGET_FILE:FreeCADMainCmd>""$<TARGET_FILE_DIR:App_tests_run>/$<TARGET_FILE_NAME:FreeCADMainCmd>")endif()',
    ):
        assert fragment in test_cmake, f"missing native fixture wiring: {fragment}"

    backend_tests = _read_source(REPO_ROOT / BACKEND_TEST)
    for test_name in (
        "installedFreeCADCmdRoundTripsAValidatedProbe",
        "installedWorkerDoesNotLoadTheLiveGuiUserProfile",
        "janitorRemovesOnlyDeadOwnedWorkspaces",
        "classifiesCrashOomTimeoutAndLostHeartbeat",
        "cancellationTerminatesTheCompleteWorkerTree",
        "cancellationOffersABoundedCooperativeExitFirst",
        "missingTrustedWorkerFailsClosedWithoutLeak",
    ):
        assert test_name in backend_tests
    fixture = _read_source(REPO_ROOT / TEST_WORKER_SOURCE)
    for operation in (
        "Test.Crash",
        "Test.OutOfMemory",
        "Test.NoHeartbeat",
        "Test.CooperativeCancel",
        "Test.ChildHang",
        "Test.Hang",
    ):
        assert operation in fixture


def test_process_boundary_contains_no_occ_or_synchronous_geometry_fallback() -> None:
    sources = {path: _read_source(REPO_ROOT / path) for path in BOUNDARY_FILES}
    violations = _execution_boundary_violations(sources)
    assert not violations, "process boundary execution violations:\n" + "\n".join(violations)


def test_part_shape_recompute_compatibility_is_narrow_and_non_abi() -> None:
    generic = _suppress_cpp_comments(
        _read_source(REPO_ROOT / GENERIC_RECOMPUTE_SOURCE)
    )
    body = _compact(
        _body_for(generic, "isPartFeatureShapeRecomputeOutput", raw=True)
    )
    assert 'Base::Type::fromName("Part::Feature")' in body
    assert "object.getTypeId().isDerivedFrom(partFeatureType)" in body
    assert 'object.getPropertyByName("Shape")==&property' in body

    document_object = _read_source(REPO_ROOT / "src/App/DocumentObject.h")
    assert "isRecomputeOutputProperty" not in document_object


def test_executable_gate_rejects_bare_path_lookup_program() -> None:
    source = """
        std::filesystem::path installedWorkerExecutable()
        {
            return "FreeCADCmd";
        }
        void run()
        {
            if (options.executable.empty()) { options.executable = installedWorkerExecutable(); }
            process.setProgram("FreeCADCmd");
        }
    """
    violations = _executable_resolution_violations(source)
    assert any("PATH/bare-program" in item for item in violations)


def test_executable_gate_rejects_qcore_only_and_empty_os_resolvers() -> None:
    backend = """
        std::filesystem::path installedWorkerExecutable()
        {
            auto executable = Internal::currentExecutablePath().parent_path();
            executable /= "FreeCADCmd.exe";
            executable /= "FreeCADCmd";
            return executable;
        }
        void configure()
        {
            if (options.executable.empty()) { options.executable = installedWorkerExecutable(); }
            process.setProgram(toQStringPath(options.executable));
        }
    """
    qcore_worker = """
        std::filesystem::path App::Internal::currentExecutablePath()
        {
            return std::filesystem::path(QCoreApplication::applicationFilePath().toStdString());
        }
        std::string executableSha256()
        {
            const auto path = Internal::currentExecutablePath();
            return path.string();
        }
    """
    qcore = _executable_resolution_violations(backend, qcore_worker)
    assert any("applicationFilePath" in item for item in qcore)
    assert any("GetModuleFileNameW" in item for item in qcore)

    empty_worker = """
        std::filesystem::path App::Internal::currentExecutablePath() { return {}; }
        std::string executableSha256()
        {
            const auto path = Internal::currentExecutablePath();
            return path.string();
        }
    """
    empty = _executable_resolution_violations(backend, empty_worker)
    assert any("GetModuleFileNameW" in item for item in empty)
    assert any("/proc/self/exe" in item for item in empty)
    assert any("_NSGetExecutablePath" in item for item in empty)


def test_build_fingerprint_gate_rejects_launcher_only_hashing() -> None:
    header = """
        AppExport std::string geometryWorkerBuildFingerprint(
            const std::filesystem::path& workerExecutable);
    """
    worker = """
        std::filesystem::path workerRuntimeLibrary(
            const std::filesystem::path& executable)
        {
            const auto directory = executable.parent_path();
            const auto windows = directory / "FreeCADApp.dll";
            const auto mac = directory / "libFreeCADApp.dylib";
            const auto macLib = directory.parent_path() / "lib" / "libFreeCADApp.dylib";
            const auto unixLocal = directory / "libFreeCADApp.so";
            const auto unixLib = directory.parent_path() / "lib" / "libFreeCADApp.so";
            const auto unixLib64 = directory.parent_path() / "lib64" / "libFreeCADApp.so";
            if (std::filesystem::is_regular_file(candidate, error)) { return candidate; }
            return windows;
        }
        bool addFingerprintFile(
            QCryptographicHash& hash, const QByteArray& label,
            const std::filesystem::path& path)
        {
            QFile file(toQStringPath(path));
            hash.addData(label);
            while (!file.atEnd()) {
                const auto chunk = file.read(1024);
                hash.addData(chunk);
            }
            return true;
        }
        std::string App::Internal::geometryWorkerBuildFingerprint(
            const std::filesystem::path& workerExecutable)
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            addFingerprintFile(
                hash, QByteArray("FreeCADCmd\\0", 11), workerExecutable);
            return hash.result().toHex().toStdString();
        }
        int App::Internal::runGeometryWorkerMain() noexcept
        {
            const auto actual = Internal::geometryWorkerBuildFingerprint(
                Internal::currentExecutablePath());
            return actual.empty();
        }
    """
    backend = """
        void runChecked()
        {
            const auto buildFingerprint =
                Internal::geometryWorkerBuildFingerprint(options.executable);
        }
    """
    native = """
        void validate()
        {
            expected.buildFingerprint =
                Internal::geometryWorkerBuildFingerprint(worker);
        }
    """
    violations = _build_fingerprint_violations(header, worker, backend, native)
    assert "composite build fingerprint does not include exactly two files" in violations
    assert "composite build fingerprint lacks the labeled FreeCADApp bytes" in violations


def test_windows_gate_rejects_start_gate_before_job_assignment() -> None:
    source = """
        void runChecked()
        {
            std::ofstream start(startGatePath);
            windowsJob.assign(process.processId());
        }
    """
    violations = _windows_containment_violations(source)
    assert "Windows Job Object assignment must precede start-gate release" in violations


def test_unix_gate_rejects_killing_only_worker_leader() -> None:
    source = """
        void configure() { QProcess::UnixProcessFlag::CreateNewSession; }
        void terminateProcessTree(QProcess& process)
        {
            const auto processId = process.processId();
            ::kill(processId, SIGTERM);
            process.waitForFinished(500);
            ::kill(processId, SIGKILL);
        }
    """
    violations = _unix_tree_violations(source)
    assert "Unix termination targets only the worker leader" in violations


def test_cancellation_gate_rejects_immediate_force_only_path() -> None:
    backend = """
        constexpr auto CancelEnvironment = "FREECAD_GEOMETRY_CANCEL";
        void runChecked()
        {
            const auto cancelPath = workspace / "cancel";
            environment.insert(
                CancelEnvironment, toQStringPath(cancelPath));
            if (status->cancellationRequested) {
                terminateProcessTree(process, windowsJob, options.terminationGrace);
                publishTerminal(GeometryJobResult {
                    id, GeometryJobState::Cancelled, {}, {}, "forced"});
            }
        }
    """
    worker = """
        constexpr auto CancelEnvironment = "FREECAD_GEOMETRY_CANCEL";
        int App::Internal::runGeometryWorkerMain() noexcept
        {
            const std::filesystem::path cancelPath = pathEnvironment(CancelEnvironment);
            if (!isWorkspaceFile(cancelPath, "cancel")) { return 10; }
            return 0;
        }
    """
    violations = _cooperative_cancel_violations(backend, worker)
    assert "manager cancellation does not publish the workspace cancel signal" in violations
    assert (
        "manager cancellation does not signal, wait configured grace, then force"
        in violations
    )


def test_unicode_path_gate_rejects_narrow_artifact_transport() -> None:
    manager_header = """
        struct GeometryJobResult
        {
            std::string resultArtifact;
        };
    """
    backend = """
        std::filesystem::path fromQStringPath(const QString& value)
        {
            return std::filesystem::path(value.toStdString());
        }
        QString toQStringPath(const std::filesystem::path& value)
        {
            return QString::fromStdString(value.string());
        }
        void runChecked()
        {
            publish(GeometryJobResult {id, state, completedPath.string()});
        }
    """
    worker = """
        std::filesystem::path pathEnvironment(const char* name)
        {
            return std::filesystem::path(qEnvironmentVariable(name).toStdString());
        }
        QString toQStringPath(const std::filesystem::path& value)
        {
            return QString::fromStdString(value.string());
        }
        int App::Internal::runGeometryWorkerMain() noexcept { return 0; }
    """
    violations = _unicode_path_violations(manager_header, backend, worker)
    assert "GeometryJobResult.resultArtifact is not a native filesystem path" in violations
    assert any("completedPath.string()" in item for item in violations)


def test_contract_gate_rejects_live_document_field() -> None:
    header = """
        struct GeometryJobDispatch
        {
            GeometryJobId id {0};
            Document* document {nullptr};
        };
    """
    violations = _contract_violations(header, ("GeometryJobDispatch",))
    assert "GeometryJobDispatch: live/shared authority field contains Document" in violations
    assert "GeometryJobDispatch: raw pointer field" in violations


def test_worker_gate_rejects_unsupported_operation_fallback() -> None:
    source = """
        int App::Internal::runGeometryWorkerMain() noexcept
        {
            GeometryArchive output;
            if (operation == "FreeCAD.Internal.GeometryProbe") {
                output = *input.archive;
            }
            else {
                output = executeFallback(operation);
            }
            output.metadata = input.archive->metadata;
            return 0;
        }
    """
    violations = _worker_operation_violations(source)
    assert "unsupported worker operation does not fail closed" in violations
    assert any("executeFallback" in item for item in violations)


def test_parser_ignores_calls_comments_literals_and_reader_is_lossless(tmp_path: Path) -> None:
    source = """
        bool classifyExit(const Process& process) noexcept { return process.failed(); }
        void caller(const Process& process)
        {
            if (classifyExit(process)) { return; }
        }
    """
    assert len(_function_bodies(source, "classifyExit")) == 1

    path = tmp_path / "Boundary.cpp"
    path.write_bytes(
        b"// TopoDS_Shape recompute() QProcess::execute()\n"
        b'const char* text = "BRepAlgoAPI_Fuse Part::Feature";\n'
        b'const char* raw = R"gate(OCC_VERSION PreparedEditExecutor)gate";\n'
        b"// non-UTF-8: \xff\n"
    )
    decoded = _read_source(path)
    assert "\udcff" in decoded
    assert not _execution_boundary_violations({"Boundary.cpp": decoded})
