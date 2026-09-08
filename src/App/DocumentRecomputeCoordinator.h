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
#include <set>
#include <string>
#include <utility>
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
    /** Stable identity of the live object named by featureId, when applicable. */
    std::string stableObjectIdentity;
    /** Object-model revision captured when this live-object request was submitted. */
    std::optional<std::uint64_t> presentationObjectModelRevision;
    /** Exact semantic fence used to reject obsolete failure presentation. */
    std::vector<DocumentRevisionObservation> presentationRevisionFence;
    /** False when dependency capture never completed for this selector. */
    bool presentationRevisionFenceComplete {false};
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
    /** Capture only one ready node at a time so each node observes prior commits. */
    bool refreshRevisionFenceAfterEachCommit {false};
    /** Emit terminal recompute presentation when this job is first observed. */
    bool publishTerminalPresentation {true};
};

struct AppExport DocumentRecomputeFeatureSnapshot
{
    std::string featureId;
    DocumentRecomputeFeatureState state {DocumentRecomputeFeatureState::Waiting};
    std::string diagnostic;
    /** False when the node settled without running the feature's execute(). */
    bool executed {false};
    /** Identity captured for presentation; empty for non-object coordinator nodes. */
    std::string stableObjectIdentity;
    /** Exact live model revision against which terminal presentation is valid. */
    std::optional<std::uint64_t> presentationObjectModelRevision;
    /** Exact semantic fence used to reject obsolete failure presentation. */
    std::vector<DocumentRevisionObservation> presentationRevisionFence;
    /** False when dependency capture never completed for this selector. */
    bool presentationRevisionFenceComplete {false};
    /** True when the committed operation already applied this terminal outcome. */
    bool outcomeApplied {false};
    /** True when a successful live-object operation published its bound target. */
    bool targetPublicationConfirmed {false};
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
    bool publishTerminalPresentation {true};

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
    [[nodiscard]] bool hasUnresolvedWork() const;
    [[nodiscard]] bool hasUnresolvedExecutableWork() const;

private:
    friend class Document;
    friend class RecomputeHandle;

    struct Job;

    class SubmissionReservation
    {
    public:
        SubmissionReservation(SubmissionReservation&& other) noexcept;
        SubmissionReservation& operator=(SubmissionReservation&&) = delete;
        ~SubmissionReservation();

        SubmissionReservation(const SubmissionReservation&) = delete;
        SubmissionReservation& operator=(const SubmissionReservation&) = delete;

        [[nodiscard]] DocumentRecomputeId id() const noexcept;
        [[nodiscard]] bool created() const noexcept;

    private:
        friend class DocumentRecomputeCoordinator;

        SubmissionReservation(DocumentRecomputeCoordinator& coordinator,
                              std::unique_lock<std::recursive_mutex> operationLock,
                              DocumentRecomputeId id,
                              bool created) noexcept;

        DocumentRecomputeCoordinator* _coordinator {nullptr};
        std::unique_lock<std::recursive_mutex> _operationLock;
        DocumentRecomputeId _id {0};
        bool _created {false};
    };

    void scheduleReady(DocumentRecomputeId id);
    void finalizeIfTerminal(DocumentRecomputeId id);
    [[nodiscard]] std::optional<DocumentRecomputeSnapshot> statusLocked(
        DocumentRecomputeId id) const;
    [[nodiscard]] SubmissionReservation admitSubmission(
        DocumentRecomputeRequest request);
    void replaceSubmission(SubmissionReservation& reservation,
                           DocumentRecomputeRequest request);
    void failSubmission(SubmissionReservation& reservation,
                        std::string diagnostic) noexcept;
    void activateSubmission(SubmissionReservation reservation);
    [[nodiscard]] bool claimPresentationFinalization(DocumentRecomputeId id);
    void finishPresentationFinalization(DocumentRecomputeId id,
                                        bool completed) noexcept;
    void forgetUnresolvedFeature(DocumentRecomputeId id,
                                 const std::string& featureId,
                                 const std::string& stableObjectIdentity);
    void forgetAllUnresolvedFeature(const std::string& featureId,
                                    const std::string& stableObjectIdentity);

    struct UnresolvedLiveFeature
    {
        DocumentRecomputeId generation {0};
    };

    DocumentCollaborationService& _service;
    mutable std::recursive_mutex _operationMutex;
    mutable std::mutex _stateMutex;
    std::map<DocumentRecomputeId, std::unique_ptr<Job>> _jobs;
    std::map<std::pair<DocumentRecomputeId, std::string>, bool>
        _unresolvedSyntheticFeatures;
    std::map<std::pair<std::string, std::string>, UnresolvedLiveFeature>
        _unresolvedLiveFeatures;
    DocumentRecomputeId _nextId {1};
    bool _operationActive {false};
};

}  // namespace App
