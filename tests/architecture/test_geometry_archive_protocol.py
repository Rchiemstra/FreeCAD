# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static CC-WP07 gate for the trusted FCG/1 geometry archive protocol."""

from __future__ import annotations

from pathlib import Path
import re
from typing import Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]

APP_CMAKE = "src/App/CMakeLists.txt"
ARCHIVE_HEADER = "src/App/GeometryArchive.h"
ARCHIVE_SOURCE = "src/App/GeometryArchive.cpp"
TEST_APP_CMAKE = "tests/src/App/CMakeLists.txt"
ARCHIVE_TEST = "tests/src/App/GeometryArchive.cpp"

POINTER_FREE_CONTRACTS = (
    "GeometryArchiveLimits",
    "GeometryArchiveMetadata",
    "GeometryArchiveSection",
    "GeometryArchive",
    "GeometryArchiveExpectation",
    "GeometryArchiveError",
    "GeometryArchiveWriteResult",
    "GeometryArchiveReadResult",
    "GeometryElementMapping",
    "GeometryElementHistory",
)

LIVE_AUTHORITY_TOKENS = (
    "Document",
    "DocumentObject",
    "DocumentCommitCoordinator",
    "DocumentCollaborationService",
    "GeometryJobManager",
    "PreparedEdit",
    "CollaborativeOperation",
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
    if any(character.isspace() or character in "()\\" for character in delimiter):
        return None
    terminator = ")" + delimiter + '"'
    closing = source.find(terminator, opening + 1)
    return len(source) if closing < 0 else closing + len(terminator)


def _blank_non_newlines(characters: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if characters[index] not in "\r\n":
            characters[index] = " "


def _suppress_cpp_non_code(source: str) -> str:
    """Blank C++ comments and literals while preserving offsets and newlines."""

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
                character = source[end]
                end += 1
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
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


def _is_definition_suffix(suffix: str) -> bool:
    """Accept qualifiers used by these definitions, not outer call syntax."""

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
        if not _is_definition_suffix(stripped[closing_parenthesis + 1:opening_brace]):
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
    """Return a data-member declarator without its brace/equal initializer."""

    brace = statement.find("{")
    equals = statement.find("=")
    cutoffs = [offset for offset in (brace, equals) if offset >= 0]
    return statement[:min(cutoffs)] if cutoffs else statement


def _contract_authority_violations(
    header: str, contracts: Sequence[str] = POINTER_FREE_CONTRACTS
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


def _whole_digest_violations(source: str) -> list[str]:
    violations: list[str] = []
    try:
        encode = _compact(_body_for(source, "encodeArchive"))
        read = _compact(_body_for(source, "GeometryArchiveCodec::readValidated"))
    except AssertionError as error:
        return [str(error)]

    encode_requirements = {
        "missing whole-archive SHA-256 calculation on encode": (
            "constautoarchiveDigest=sha256(writer.data().data(),writer.data().size());"
        ),
        "missing whole-archive hexadecimal digest publication": (
            "digestHex=toHex(archiveDigest);"
        ),
        "missing whole-archive digest trailer": (
            "writer.bytes(archiveDigest.data(),archiveDigest.size());"
        ),
    }
    for message, fragment in encode_requirements.items():
        if fragment not in encode:
            violations.append(message)

    read_requirements = {
        "missing fixed SHA-256 trailer boundary on read": (
            "conststd::size_tpayloadLimit=encoded.size()-Sha256Bytes;"
        ),
        "missing whole-archive SHA-256 calculation on read": (
            "constautoactualArchiveDigest=sha256(encoded.data(),payloadLimit);"
        ),
        "missing whole-archive digest comparison": "std::equal(actualArchiveDigest.begin()",
        "missing validated digest result": "archive.archiveDigest=toHex(actualArchiveDigest);",
    }
    for message, fragment in read_requirements.items():
        if fragment not in read:
            violations.append(message)
    digest_check = read.find("std::equal(actualArchiveDigest.begin()")
    parser_start = read.find("ByteReaderreader(encoded,payloadLimit);")
    if digest_check < 0 or parser_start < 0 or digest_check > parser_start:
        violations.append("whole-archive digest must be checked before parsing payload")
    return violations


def _expectation_violations(source: str) -> list[str]:
    try:
        body = _compact(_body_for(source, "validateExpectation"))
    except AssertionError as error:
        return [str(error)]
    required = {
        "missing nonzero trusted job expectation": "expectation.jobId==0",
        "missing operation expectation completeness check": "expectation.operationType.empty()",
        "missing build expectation completeness check": "expectation.buildFingerprint.empty()",
        "missing input digest expectation completeness check": "!isSha256Hex(expectation.inputDigest)",
        "missing exact kind expectation": "metadata.kind!=expectation.kind",
        "missing exact job expectation": "metadata.jobId!=expectation.jobId",
        "missing exact operation expectation": "metadata.operationType!=expectation.operationType",
        "missing exact build expectation": "metadata.buildFingerprint!=expectation.buildFingerprint",
        "missing exact input digest expectation": "metadata.inputDigest!=expectation.inputDigest",
    }
    return [message for message, fragment in required.items() if fragment not in body]


def _publication_violations(source: str) -> list[str]:
    try:
        body = _compact(_body_for(source, "GeometryArchiveCodec::writeAtomic"))
    except AssertionError as error:
        return [str(error)]
    required = {
        "missing create-only target preflight": "std::filesystem::exists(target,filesystemError)",
        "temporary archive is not derived in the target directory": "autotemporary=target;",
        "missing temporary artifact cleanup": "std::filesystem::remove(path,ignored);",
        "missing prepublication interruption hook": "_prePublishTestHook.load(std::memory_order_acquire)",
        "missing create-only hard-link publication": (
            "std::filesystem::create_hard_link(temporary,target,filesystemError);"
        ),
    }
    violations = [message for message, fragment in required.items() if fragment not in body]
    forbidden = (
        "std::filesystem::rename(",
        "std::filesystem::copy_file(",
        "ReplaceFile(",
        "MoveFileEx(",
        "renameat(",
    )
    for token in forbidden:
        if token in body:
            violations.append(f"unsafe replace/rename-style publication: {token}")

    stream = body.find("std::ofstreamstream(temporary,")
    hook = body.find("_prePublishTestHook.load(")
    publish = body.find("std::filesystem::create_hard_link(")
    if stream < 0 or hook < 0 or publish < 0 or not stream < hook < publish:
        violations.append("interruption hook must run after temporary write and before publication")
    return violations


def _heavy_geometry_violations(source: str, path: str) -> list[str]:
    code = _suppress_cpp_non_code(source)
    violations: list[str] = []
    for match in HEAVY_GEOMETRY_RE.finditer(code):
        line = code.count("\n", 0, match.start("token")) + 1
        violations.append(f"{path}:{line}: geometry execution token {match.group('token')!r}")
    return violations


def test_fcg_protocol_and_public_contracts_are_fixed_and_pointer_free() -> None:
    header = _read_source(REPO_ROOT / ARCHIVE_HEADER)
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    compact_header = _compact(_suppress_cpp_non_code(header))

    assert "inlineconstexprstd::uint32_tGeometryArchiveProtocolVersion=1;" in compact_header
    kind = _compact(_type_body(header, "enum class", "GeometryArchiveKind"))
    assert kind == "Request=1,Result=2", f"unexpected FCG archive kinds: {kind}"
    assert "GeometryArchiveKindkind{GeometryArchiveKind::Request};" in compact_header
    assert "PreparationPolicypolicy{PreparationPolicy::IsolatedProcess};" in compact_header
    assert "GeometryJobIdjobId{0};" in compact_header
    assert "std::int64_tdeadlineEpochMilliseconds{0};" in compact_header
    for field in ("operationType", "buildFingerprint", "inputDigest"):
        assert f"std::string{field};" in compact_header

    assert "ArchiveMagic {'F', 'C', 'G', 'A', 'R', 'C', 'H', '1'}" in source
    violations = _contract_authority_violations(header)
    assert not violations, "archive contract authority violations:\n" + "\n".join(violations)


def test_encode_and_read_validate_all_metadata_and_bounds_before_payload_use() -> None:
    header = _read_source(REPO_ROOT / ARCHIVE_HEADER)
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    compact_header = _compact(_type_body(header, "struct", "GeometryArchiveLimits"))
    for limit in (
        "maxArchiveBytes",
        "maxSectionBytes",
        "maxTotalSectionBytes",
        "maxSections",
        "maxElementMappings",
        "maxElementNameBytes",
    ):
        assert re.search(r"\b" + limit + r"\s*\{\s*[1-9]", _type_body(header, "struct", "GeometryArchiveLimits")), (
            f"{limit} must have a nonzero default bound"
        )
    assert compact_header

    metadata = _compact(_body_for(source, "validateMetadata"))
    for fragment in (
        "metadata.protocolVersion!=GeometryArchiveProtocolVersion",
        "metadata.kind!=GeometryArchiveKind::Request",
        "metadata.kind!=GeometryArchiveKind::Result",
        "metadata.jobId==0",
        "metadata.policy!=PreparationPolicy::IsolatedProcess",
        "metadata.deadlineEpochMilliseconds<=0",
        "metadata.operationType.empty()",
        "metadata.operationType.size()>MaxOperationTypeBytes",
        "!isPrintableAscii(metadata.operationType)",
        "metadata.buildFingerprint.empty()",
        "metadata.buildFingerprint.size()>MaxBuildFingerprintBytes",
        "!isPrintableAscii(metadata.buildFingerprint)",
        "!isSha256Hex(metadata.inputDigest)",
    ):
        assert fragment in metadata, f"missing metadata validation: {fragment}"

    encode = _compact(_body_for(source, "encodeArchive"))
    assert encode.find("validateMetadata(archive.metadata,error)") < encode.find("ByteWriterwriter;")
    for fragment in (
        "archive.sections.size()>limits.maxSections",
        "!isSafeSectionName(section.name)",
        "!names.insert(section.name).second",
        "section.bytes.size()>limits.maxSectionBytes",
        "!checkedAdd(totalSectionBytes,section.bytes.size())",
        "totalSectionBytes>limits.maxTotalSectionBytes",
        "encoded.size()>limits.maxArchiveBytes",
    ):
        assert fragment in encode, f"missing encode bound: {fragment}"

    read = _compact(_body_for(source, "GeometryArchiveCodec::readValidated"))
    archive_bound = read.find("fileSize>limits.maxArchiveBytes")
    archive_allocation = read.find("std::vector<std::uint8_t>encoded(")
    assert 0 <= archive_bound < archive_allocation, "archive size must be bounded before allocation"
    section_bound = read.find("sectionCount>limits.maxSections")
    table_allocation = read.find("table.reserve(sectionCount)")
    assert 0 <= section_bound < table_allocation, "section count must be bounded before reserve"
    for fragment in (
        "reader.string16(archive.metadata.operationType,MaxOperationTypeBytes)",
        "reader.string16(archive.metadata.buildFingerprint,MaxBuildFingerprintBytes)",
        "reader.string16(archive.metadata.inputDigest,Sha256HexBytes)",
        "reader.string16(entry.name,MaxSectionNameBytes)",
        "entry.size>limits.maxSectionBytes",
        "!checkedAdd(totalSectionBytes,entry.size)",
        "totalSectionBytes>limits.maxTotalSectionBytes",
        "totalSectionBytes!=reader.remaining()",
    ):
        assert fragment in read, f"missing read bound: {fragment}"
    assert read.find("totalSectionBytes!=reader.remaining()") < read.find(
        "archive.sections.reserve(table.size())"
    )

    vector_reader = _compact(
        _body_for(source, "vector", signature_contains="std::vector<std::uint8_t>& destination")
    )
    assert vector_reader.find("!available(size)") < vector_reader.find("destination.assign(")
    string_reader = _compact(_body_for(source, "string16", signature_contains="maximum"))
    assert string_reader.find("size>maximum") < string_reader.find("value.assign(")
    assert string_reader.find("!available(size)") < string_reader.find("value.assign(")


def test_section_names_are_bounded_unique_and_traversal_safe_on_both_paths() -> None:
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    safe_name = _compact(_body_for(source, "isSafeSectionName", raw=True))
    for fragment in (
        "name.empty()",
        "name.size()>MaxSectionNameBytes",
        'name.find("..")!=std::string::npos',
        "std::all_of(name.begin(),name.end()",
        "character>='a'&&character<='z'",
        "character>='A'&&character<='Z'",
        "character>='0'&&character<='9'",
        "character=='.'",
        "character=='_'",
        "character=='-'",
    ):
        assert fragment in safe_name, f"safe section-name policy is incomplete: {fragment}"
    assert "'/'" not in safe_name and "'\\\\'" not in safe_name

    encode = _compact(_body_for(source, "encodeArchive"))
    read = _compact(_body_for(source, "GeometryArchiveCodec::readValidated"))
    assert "!isSafeSectionName(section.name)" in encode
    assert "!names.insert(section.name).second" in encode
    assert "!isSafeSectionName(entry.name)" in read
    assert "!names.insert(entry.name).second" in read


def test_archive_and_section_sha256_and_trusted_expectations_are_exact() -> None:
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    digest_implementation = _compact(_body_for(source, "sha256"))
    assert "QCryptographicHashhash(QCryptographicHash::Sha256);" in digest_implementation
    compact_source = _compact(_suppress_cpp_non_code(source))
    assert "constexprstd::size_tSha256Bytes=32;" in compact_source
    assert "constexprstd::size_tSha256HexBytes=64;" in compact_source
    whole_digest = _whole_digest_violations(source)
    assert not whole_digest, "whole archive digest violations:\n" + "\n".join(whole_digest)
    expectations = _expectation_violations(source)
    assert not expectations, "trusted expectation violations:\n" + "\n".join(expectations)

    encode = _compact(_body_for(source, "encodeArchive"))
    read = _compact(_body_for(source, "GeometryArchiveCodec::readValidated"))
    assert "digests.push_back(sha256(section.bytes.data(),section.bytes.size()))" in encode
    assert "writer.bytes(digests[index].data(),digests[index].size())" in encode
    assert "reader.bytes(entry.digest.data(),entry.digest.size())" in read
    assert "sha256(section.bytes.data(),section.bytes.size())!=entry.digest" in read


def test_atomic_publication_is_same_directory_create_only_and_interruptible() -> None:
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    test_source = _read_source(REPO_ROOT / ARCHIVE_TEST)
    violations = _publication_violations(source)
    assert not violations, "atomic publication violations:\n" + "\n".join(violations)

    body = _compact(_body_for(source, "GeometryArchiveCodec::writeAtomic"))
    assert "temporary+=\".tmp.\"" in _compact(
        _body_for(source, "GeometryArchiveCodec::writeAtomic", raw=True)
    )
    assert body.find("std::filesystem::exists(target,filesystemError)") < body.find(
        "std::ofstreamstream(temporary,"
    )
    assert body.find("stream.close()") < body.find("_prePublishTestHook.load(")
    assert "atomicallyRoundTripsRequestAndResultArchives" in test_source
    assert 'duplicate.error.code, "TargetExists"' in test_source
    assert "interruptionLeavesNoPublishedOrTemporaryArtifact" in test_source
    assert "std::filesystem::is_empty(directory.path)" in test_source


def test_element_history_codec_is_exact_bounded_and_candidate_published() -> None:
    header = _read_source(REPO_ROOT / ARCHIVE_HEADER)
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    test_source = _read_source(REPO_ROOT / ARCHIVE_TEST)

    history = _compact(_type_body(header, "struct", "GeometryElementHistory"))
    generated = "std::vector<GeometryElementMapping>generated;"
    modified = "std::vector<GeometryElementMapping>modified;"
    deleted = "std::vector<std::string>deleted;"
    assert generated in history and modified in history and deleted in history
    assert history.find(generated) < history.find(modified) < history.find(deleted)
    assert "HistoryMagic {'F', 'C', 'G', 'H', 'M', 'A', 'P', '1'}" in source

    validation = _compact(_body_for(source, "validateHistory"))
    for fragment in (
        "checkedAdd(count,history.modified.size())",
        "checkedAdd(count,history.deleted.size())",
        "count>limits.maxElementMappings",
        "count>std::numeric_limits<std::uint32_t>::max()",
        "!validElementName(mapping.inputElement,limits)",
        "!validElementName(mapping.outputElement,limits)",
        "!unique.emplace(mapping.inputElement,mapping.outputElement).second",
        "!deleted.insert(element).second",
    ):
        assert fragment in validation, f"missing element-history validation: {fragment}"

    name_validation = _compact(_body_for(source, "validElementName"))
    assert "name.size()<=limits.maxElementNameBytes" in name_validation
    assert "isPrintableAscii(name)" in name_validation

    encode = _compact(_body_for(source, "GeometryArchiveCodec::encodeElementHistory"))
    assert encode.find("validateHistory(history,limits,error)") < encode.find("ByteWriterwriter;")
    for fragment in (
        "writeMappings(history.generated)",
        "writeMappings(history.modified)",
        "writer.u32(static_cast<std::uint32_t>(history.deleted.size()))",
        "section.name=\"element-history\"",
        "section.bytes.size()>limits.maxSectionBytes",
    ):
        assert fragment in _compact(
            _body_for(source, "GeometryArchiveCodec::encodeElementHistory", raw=True)
        ), f"missing element-history encode step: {fragment}"

    decode = _compact(_body_for(source, "GeometryArchiveCodec::decodeElementHistory"))
    assert "GeometryElementHistorycandidate;" in decode
    assert "section.bytes.size()>limits.maxSectionBytes" in decode
    assert decode.find("totalMappings>limits.maxElementMappings") < decode.find(
        "mappings.reserve(count)"
    )
    deleted_bound = decode.rfind("totalMappings>limits.maxElementMappings")
    assert 0 <= deleted_bound < decode.find("candidate.deleted.reserve(deletedCount)")
    assert "reader.string16(mapping.inputElement,limits.maxElementNameBytes)" in decode
    assert "reader.string16(mapping.outputElement,limits.maxElementNameBytes)" in decode
    assert "reader.string16(element,limits.maxElementNameBytes)" in decode
    validation_offset = decode.rfind("validateHistory(candidate,limits,error)")
    publication_offset = decode.rfind("history=std::move(candidate)")
    assert 0 <= validation_offset < publication_offset
    assert "history.generated" not in decode and "history.modified" not in decode
    assert "history.deleted" not in decode

    assert "exactElementHistoryRoundTripsAndRejectsMalformedData" in test_source
    assert "duplicate.generated.push_back" in test_source
    assert 'EXPECT_EQ(decoded, sentinel)' in test_source


def test_geometry_archive_is_registered_and_contains_no_geometry_execution() -> None:
    app_cmake = _read_source(REPO_ROOT / APP_CMAKE)
    test_cmake = _read_source(REPO_ROOT / TEST_APP_CMAKE)
    for name in ("GeometryArchive.cpp", "GeometryArchive.h"):
        assert re.search(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])", app_cmake)
    assert re.search(r"(?<![A-Za-z0-9_])GeometryArchive\.cpp(?![A-Za-z0-9_])", test_cmake)

    violations: list[str] = []
    for path in (ARCHIVE_HEADER, ARCHIVE_SOURCE):
        violations.extend(_heavy_geometry_violations(_read_source(REPO_ROOT / path), path))
    assert not violations, "archive codec contains geometry execution:\n" + "\n".join(violations)


def test_digest_gate_rejects_missing_whole_archive_trailer() -> None:
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    broken = source.replace(
        "        writer.bytes(archiveDigest.data(), archiveDigest.size());\n", ""
    )
    assert broken != source, "negative fixture did not remove the digest trailer"
    violations = _whole_digest_violations(broken)
    assert "missing whole-archive digest trailer" in violations


def test_publication_gate_rejects_replace_style_rename() -> None:
    source = _read_source(REPO_ROOT / ARCHIVE_SOURCE)
    broken = source.replace(
        "std::filesystem::create_hard_link(temporary, target, filesystemError);",
        "std::filesystem::rename(temporary, target, filesystemError);",
    )
    assert broken != source, "negative fixture did not replace hard-link publication"
    violations = _publication_violations(broken)
    assert "missing create-only hard-link publication" in violations
    assert any("unsafe replace/rename-style publication" in item for item in violations)


def test_expectation_gate_rejects_missing_job_and_build_binding() -> None:
    source = """
        bool validateExpectation(const Metadata& metadata, const Expectation& expectation)
        {
            if (expectation.jobId == 0 || expectation.operationType.empty()
                || expectation.buildFingerprint.empty()
                || !isSha256Hex(expectation.inputDigest)) return false;
            if (metadata.kind != expectation.kind) return false;
            if (metadata.operationType != expectation.operationType) return false;
            if (metadata.inputDigest != expectation.inputDigest) return false;
            return true;
        }
    """
    violations = _expectation_violations(source)
    assert "missing exact job expectation" in violations
    assert "missing exact build expectation" in violations


def test_contract_gate_rejects_live_document_pointer_authority() -> None:
    header = """
        struct GeometryArchiveMetadata
        {
            GeometryJobId jobId {0};
            Document* document {nullptr};
        };
    """
    violations = _contract_authority_violations(header, ("GeometryArchiveMetadata",))
    assert "GeometryArchiveMetadata: live/shared authority field contains Document" in violations
    assert "GeometryArchiveMetadata: raw pointer field" in violations


def test_contract_gate_accepts_multiplication_in_bounded_default() -> None:
    header = """
        struct GeometryArchiveLimits
        {
            std::uint64_t maxArchiveBytes {256ULL * 1024ULL * 1024ULL};
        };
    """
    assert not _contract_authority_violations(header, ("GeometryArchiveLimits",))


def test_function_body_parser_excludes_calls_in_control_expressions() -> None:
    source = """
        bool validateMetadata(const Metadata& metadata) noexcept
        {
            return metadata.valid;
        }

        void caller(const Metadata& metadata)
        {
            if (!validateMetadata(metadata)) {
                return;
            }
            while (validateMetadata(metadata)) {
                break;
            }
        }
    """
    bodies = _function_bodies(source, "validateMetadata")
    assert len(bodies) == 1
    assert "return metadata.valid" in bodies[0][1]


def test_reader_is_lossless_and_scanners_ignore_comments_and_literals(tmp_path: Path) -> None:
    path = tmp_path / "Archive.cpp"
    path.write_bytes(
        b"// TopoDS_Shape Document* std::filesystem::rename()\n"
        b'const char* text = "BRepAlgoAPI_Fuse GeometryJobManager";\n'
        b'const char* raw = R"gate(Part::Feature OCC_VERSION)gate";\n'
        b"// non-UTF-8: \xff\n"
    )
    source = _read_source(path)
    assert "\udcff" in source
    assert not _heavy_geometry_violations(source, "Archive.cpp")
    code = _suppress_cpp_non_code(source)
    assert "std::filesystem::rename" not in code
    assert "Document" not in code
