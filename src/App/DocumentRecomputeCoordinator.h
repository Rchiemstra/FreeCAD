// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperationRegistry.h"
#include "PreparedEditExecutor.h"

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace App
{

class DocumentCollaborationService;
class RecomputeHandle;

using DocumentRecomputeId = std::uint64_t;

enum class DocumentRecomputeState
{
    Running,
    Cancelling,
    Completed,
    PartialFailure,
    Cancelled
};

enum class DocumentRecomputeFeatureState
{
    Waiting,
    Preparing,
    Committing,
    Committed,
    Stale,
    Failed,
    Blocked,
    Cancelling,
    Cancelled
};

/** One pointer-free feature operation in a detached recompute plan. */
struct AppExport DocumentRecomputeFeatureRequest
{
    std::string featureId;
    std::vector<std::string> dependencies;
    std::string operationId;
    CollaborativeOperationIntent intent;
    std::string provenance;
};

/**
 * Immutable dependency graph submitted to one document-owned coordinator.
 *
 * A non-empty coalescing key identifies an identical active plan. Reusing a
 * key for a different active plan is rejected instead of silently discarding
 * intent.
 */
struct AppExport DocumentRecomputeRequest
{
    std::vector<DocumentRecomputeFeatureRequest> features;
    std::string coalescingKey;
};

struct AppExport DocumentRecomputeFeatureSnapshot
{
    std::string featureId;
    DocumentRecomputeFeatureState state {DocumentRecomputeFeatureState::Waiting};
    std::string diagnostic;
};

/** Copyable, pointer-free observation of a recompute plan. */
struct AppExport DocumentRecomputeSnapshot
{
    DocumentRecomputeId id {0};
    DocumentRecomputeState state {DocumentRecomputeState::Running};
    std::size_t completedFeatures {0};
    std::size_t failedFeatures {0};
    std::size_t totalFeatures {0};
    double progress {0.0};
    std::string diagnostic;
    std::vector<DocumentRecomputeFeatureSnapshot> features;

    [[nodiscard]] bool terminal() const noexcept
    {
        return state == DocumentRecomputeState::Completed
            || state == DocumentRecomputeState::PartialFailure
            || state == DocumentRecomputeState::Cancelled;
    }
};

/**
 * Per-document scheduler for dependency-ordered detached recompute work.
 *
 * The coordinator owns no live Document or DocumentObject pointer. Trusted
 * DCS adapters capture immutable inputs, detached executors produce operation
 * payloads, and DCS/DCC alone revalidate and commit on the document thread.
 * poll() advances ready work and is intentionally owner-thread driven; the
 * compatibility and asynchronous facades that drive it are added in CC-WP12.
 */
class AppExport DocumentRecomputeCoordinator
{
public:
    explicit DocumentRecomputeCoordinator(DocumentCollaborationService& service);
    ~DocumentRecomputeCoordinator();

    DocumentRecomputeCoordinator(const DocumentRecomputeCoordinator&) = delete;
    DocumentRecomputeCoordinator& operator=(const DocumentRecomputeCoordinator&) = delete;

    [[nodiscard]] DocumentRecomputeId submit(DocumentRecomputeRequest request);
    [[nodiscard]] bool poll(DocumentRecomputeId id);
    [[nodiscard]] bool cancel(DocumentRecomputeId id,
                              std::string reason = "recompute cancelled by caller");
    [[nodiscard]] std::optional<DocumentRecomputeSnapshot> status(
        DocumentRecomputeId id) const;
    [[nodiscard]] bool hasPendingWork() const;

private:
    friend class RecomputeHandle;

    struct Job;

    void scheduleReady(DocumentRecomputeId id);
    void finalizeIfTerminal(DocumentRecomputeId id);
    [[nodiscard]] std::optional<DocumentRecomputeSnapshot> statusLocked(
        DocumentRecomputeId id) const;
    [[nodiscard]] bool claimPresentationFinalization(DocumentRecomputeId id);

    DocumentCollaborationService& _service;
    mutable std::recursive_mutex _operationMutex;
    mutable std::mutex _stateMutex;
    std::map<DocumentRecomputeId, std::unique_ptr<Job>> _jobs;
    DocumentRecomputeId _nextId {1};
    bool _operationActive {false};
};

}  // namespace App
