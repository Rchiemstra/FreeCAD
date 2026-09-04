// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include "PreparationPolicy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace App
{

class Document;
class DocumentObject;
struct GeometryArchive;

namespace Internal
{
class GeometryJobManagerTestAccess;
class GeometryJobProcessBackend;
class GeometryProcessBackendTestAccess;
struct GeometryProcessBackendOptions;
}

using GeometryJobId = std::uint64_t;

enum class GeometryJobState
{
    Queued,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    DeadlineExceeded,
    WorkerCrashed,
    WorkerOutOfMemory,
    Failed
};

enum class GeometryJobCoalescing
{
    None,
    JoinIdentical,
    LatestWins
};

/** Pointer-free request captured by a trusted parent-side geometry adapter. */
struct AppExport GeometryJobRequest
{
    std::string operationType;
    PreparationPolicy policy {PreparationPolicy::IsolatedProcess};
    std::string coalescingKey;
    std::string inputDigest;
    GeometryJobCoalescing coalescing {GeometryJobCoalescing::LatestWins};
    std::chrono::steady_clock::time_point deadline;
};

struct AppExport GeometryJobProgress
{
    double fraction {0.0};
    std::string phase;
};

struct AppExport GeometryJobStatus
{
    GeometryJobId id {0};
    GeometryJobState state {GeometryJobState::Queued};
    GeometryJobProgress progress;
    std::chrono::steady_clock::time_point deadline;
    bool cancellationRequested {false};
    std::string diagnostic;
};

/** Pointer-free terminal worker outcome. It never carries live commit authority. */
struct AppExport GeometryJobResult
{
    GeometryJobId id {0};
    GeometryJobState state {GeometryJobState::Failed};
    std::filesystem::path resultArtifact;
    std::string resultDigest;
    std::string diagnostic;
    std::string operationType {};
    std::string buildFingerprint {};
    std::string inputDigest {};
};

/** Immutable dispatch packet consumed only by the trusted process backend. */
struct AppExport GeometryJobDispatch
{
    GeometryJobId id {0};
    GeometryJobRequest request;
};

/**
 * App-owned bounded lifecycle manager for isolated geometry work.
 *
 * Submission and observation are pointer-free. Trusted process backends use
 * the private dispatch/reporting surface; workers never receive Document or
 * DocumentObject pointers and results never commit themselves.
 */
class AppExport GeometryJobManager
{
public:
    explicit GeometryJobManager(std::size_t activeCapacity = 2,
                                std::size_t queueCapacity = 64);
    ~GeometryJobManager();

    GeometryJobManager(const GeometryJobManager&) = delete;
    GeometryJobManager& operator=(const GeometryJobManager&) = delete;
    GeometryJobManager(GeometryJobManager&&) = delete;
    GeometryJobManager& operator=(GeometryJobManager&&) = delete;

    [[nodiscard]] GeometryJobId submit(GeometryJobRequest request);
    [[nodiscard]] GeometryJobId submit(GeometryJobRequest request,
                                       GeometryArchive inputArchive);
    [[nodiscard]] bool cancel(GeometryJobId id);
    [[nodiscard]] std::optional<GeometryJobStatus> status(GeometryJobId id) const;
    [[nodiscard]] std::optional<GeometryJobResult> takeResult(GeometryJobId id);

    [[nodiscard]] std::size_t activeCapacity() const noexcept;
    [[nodiscard]] std::size_t queueCapacity() const noexcept;
    [[nodiscard]] std::size_t activeCount() const;
    [[nodiscard]] std::size_t queuedCount() const;

private:
    friend class Internal::GeometryJobManagerTestAccess;
    friend class Internal::GeometryJobProcessBackend;
    friend class Internal::GeometryProcessBackendTestAccess;
    friend class Application;

    [[nodiscard]] std::optional<GeometryJobDispatch> takeNext();
    [[nodiscard]] std::optional<GeometryJobDispatch> takeNext(
        GeometryArchive& inputArchive);
    [[nodiscard]] bool reportProgress(GeometryJobId id, GeometryJobProgress progress);
    [[nodiscard]] bool finish(GeometryJobResult result);
    void startProcessBackend(Internal::GeometryProcessBackendOptions options);

    class Impl;
    std::unique_ptr<Impl> _impl;
    std::unique_ptr<Internal::GeometryJobProcessBackend> _processBackend;
};

}  // namespace App
