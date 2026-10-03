// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"

#include <FCGlobal.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace App
{

enum class DocumentCommandState
{
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Stalled
};

using DocumentCommandRecomputeId = std::uint64_t;

enum class DocumentCommandRecomputeState
{
    Running,
    Cancelling,
    Completed,
    PartialFailure,
    Cancelled
};

enum class DocumentCommandRecomputeFeatureState
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

/** One pointer-free feature observation copied from a recompute plan. */
struct AppExport DocumentCommandRecomputeFeatureObservation
{
    std::string featureId;
    DocumentCommandRecomputeFeatureState state {
        DocumentCommandRecomputeFeatureState::Waiting};
    std::string diagnostic;
    /** False when the node settled without running the feature's execute(). */
    bool executed {true};
};

/**
 * Copyable, pointer-free observation of one admitted recompute command.
 *
 * Unlike DocumentRecomputeSnapshot, this record is observation-only and never
 * carries owner-thread execution or polling semantics.
 */
struct AppExport DocumentCommandRecomputeObservation
{
    DocumentCommandRecomputeId id {0};
    DocumentCommandRecomputeState state {DocumentCommandRecomputeState::Running};
    std::size_t completedFeatures {0};
    std::size_t failedFeatures {0};
    std::size_t totalFeatures {0};
    double progress {0.0};
    std::string diagnostic;
    std::vector<DocumentCommandRecomputeFeatureObservation> features;

    [[nodiscard]] bool terminal() const noexcept
    {
        return state == DocumentCommandRecomputeState::Completed
            || state == DocumentCommandRecomputeState::PartialFailure
            || state == DocumentCommandRecomputeState::Cancelled;
    }
};

/**
 * Copyable, pointer-free observation of one admitted document command.
 *
 * Status snapshots are immutable value records. Observation must not advance
 * work, poll coordinators, or acquire a model lock.
 */
struct AppExport DocumentCommandSnapshot
{
    DocumentCommandId id {0};
    DocumentCommandKind kind {DocumentCommandKind::Recompute};
    DocumentCommandState state {DocumentCommandState::Running};
    DocumentRevisionIdentityBinding document;
    double progress {0.0};
    std::string diagnostic;
    std::optional<DocumentCommandRecomputeObservation> recompute;

    [[nodiscard]] bool terminal() const noexcept
    {
        return state == DocumentCommandState::Completed
            || state == DocumentCommandState::Failed
            || state == DocumentCommandState::Cancelled;
    }
};

/**
 * Immutable observation and cooperative cancellation handle for one command.
 *
 * status() is intentionally side-effect free. Unlike the legacy
 * RecomputeHandle compatibility facade, this type never pumps work while
 * callers inspect state.
 */
class AppExport DocumentCommandHandle
{
public:
    DocumentCommandHandle() = default;
    explicit DocumentCommandHandle(DocumentCommandId id,
                                   DocumentRevisionIdentityBinding document);

    DocumentCommandHandle(const DocumentCommandHandle&) = default;
    DocumentCommandHandle& operator=(const DocumentCommandHandle&) = default;
    DocumentCommandHandle(DocumentCommandHandle&&) noexcept = default;
    DocumentCommandHandle& operator=(DocumentCommandHandle&&) noexcept = default;
    ~DocumentCommandHandle() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] DocumentCommandId id() const noexcept;
    [[nodiscard]] DocumentRevisionIdentityBinding document() const noexcept;

    /** Read-only observation that cannot advance execution or acquire locks. */
    [[nodiscard]] DocumentCommandSnapshot status() const;

    /** Cooperative cancellation request; safe even when the owner thread is stalled. */
    [[nodiscard]] bool cancel(std::string reason = "command cancelled by caller");

private:
    DocumentCommandId _id {0};
    DocumentRevisionIdentityBinding _document;
};

/**
 * Recompute-specific command handle compatible with DocumentCommandRecomputeObservation.
 *
 * Integrators may adapt the legacy RecomputeHandle to this type once document
 * execution moves onto DocumentExecutionLane.
 */
class AppExport RecomputeCommandHandle
{
public:
    RecomputeCommandHandle() = default;
    RecomputeCommandHandle(DocumentCommandHandle command,
                           DocumentCommandRecomputeId recomputeId);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const DocumentCommandHandle& command() const noexcept;
    [[nodiscard]] DocumentCommandRecomputeId recomputeId() const noexcept;

    [[nodiscard]] DocumentCommandSnapshot status() const;
    [[nodiscard]] bool cancel(std::string reason = "recompute cancelled by caller");

private:
    DocumentCommandHandle _command;
    DocumentCommandRecomputeId _recomputeId {0};
};

AppExport const char* documentCommandStateName(DocumentCommandState state) noexcept;

static_assert(!std::is_pointer_v<DocumentCommandSnapshot>);
static_assert(std::is_copy_constructible_v<DocumentCommandSnapshot>);
static_assert(std::is_copy_constructible_v<DocumentCommandRecomputeObservation>);
static_assert(!std::is_pointer_v<DocumentCommandRecomputeObservation>);
static_assert(std::is_copy_constructible_v<DocumentCommandHandle>);
static_assert(!std::is_constructible_v<DocumentCommandHandle, Document*>);
static_assert(!std::is_constructible_v<DocumentCommandHandle, DocumentObject*>);

}  // namespace App
