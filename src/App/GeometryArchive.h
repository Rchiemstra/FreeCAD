// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include "CollaborativeOperationRegistry.h"
#include "GeometryJobManager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App
{

inline constexpr std::uint32_t GeometryArchiveProtocolVersion = 1;

enum class GeometryArchiveKind : std::uint8_t
{
    Request = 1,
    Result = 2
};

struct AppExport GeometryArchiveLimits
{
    std::uint64_t maxArchiveBytes {256ULL * 1024ULL * 1024ULL};
    std::uint64_t maxSectionBytes {128ULL * 1024ULL * 1024ULL};
    std::uint64_t maxTotalSectionBytes {240ULL * 1024ULL * 1024ULL};
    std::uint32_t maxSections {64};
    std::uint32_t maxElementMappings {1'000'000};
    std::uint16_t maxElementNameBytes {256};
};

struct AppExport GeometryArchiveMetadata
{
    std::uint32_t protocolVersion {GeometryArchiveProtocolVersion};
    GeometryArchiveKind kind {GeometryArchiveKind::Request};
    GeometryJobId jobId {0};
    PreparationPolicy policy {PreparationPolicy::IsolatedProcess};
    std::int64_t deadlineEpochMilliseconds {0};
    std::string operationType;
    std::string buildFingerprint;
    std::string inputDigest;
};

struct AppExport GeometryArchiveSection
{
    std::string name;
    std::vector<std::uint8_t> bytes;

    bool operator==(const GeometryArchiveSection&) const = default;
};

struct AppExport GeometryArchive
{
    GeometryArchiveMetadata metadata;
    std::vector<GeometryArchiveSection> sections;
    std::string archiveDigest;
};

/** Exact identities supplied out-of-band by the trusted parent/worker launcher. */
struct AppExport GeometryArchiveExpectation
{
    GeometryArchiveKind kind {GeometryArchiveKind::Request};
    GeometryJobId jobId {0};
    std::string operationType;
    std::string buildFingerprint;
    std::string inputDigest;
};

struct AppExport GeometryArchiveError
{
    std::string code;
    std::string message;
};

struct AppExport GeometryArchiveWriteResult
{
    bool success {false};
    std::filesystem::path artifactPath;
    std::string archiveDigest;
    GeometryArchiveError error;
};

struct AppExport GeometryArchiveReadResult
{
    std::optional<GeometryArchive> archive;
    GeometryArchiveError error;

    [[nodiscard]] bool success() const noexcept
    {
        return archive.has_value();
    }
};

struct AppExport GeometryElementMapping
{
    std::string inputElement;
    std::string outputElement;

    bool operator==(const GeometryElementMapping&) const = default;
};

struct AppExport GeometryElementHistory
{
    std::vector<GeometryElementMapping> generated;
    std::vector<GeometryElementMapping> modified;
    std::vector<std::string> deleted;

    bool operator==(const GeometryElementHistory&) const = default;
};

namespace Internal
{
class GeometryArchiveTestAccess;
}

/** Trusted deterministic codec for the FCG/1 request/result envelope. */
class AppExport GeometryArchiveCodec
{
public:
    static GeometryArchiveWriteResult writeAtomic(
        const std::filesystem::path& target,
        const GeometryArchive& archive,
        const GeometryArchiveLimits& limits = {});

    static GeometryArchiveReadResult readValidated(
        const std::filesystem::path& source,
        const GeometryArchiveExpectation& expectation,
        const GeometryArchiveLimits& limits = {});

    static bool encodeElementHistory(
        const GeometryElementHistory& history,
        GeometryArchiveSection& section,
        GeometryArchiveError& error,
        const GeometryArchiveLimits& limits = {});

    static bool decodeElementHistory(
        const GeometryArchiveSection& section,
        GeometryElementHistory& history,
        GeometryArchiveError& error,
        const GeometryArchiveLimits& limits = {});

private:
    friend class Internal::GeometryArchiveTestAccess;
    using PrePublishTestHook = void (*)();
    static std::atomic<PrePublishTestHook> _prePublishTestHook;
};

}  // namespace App
