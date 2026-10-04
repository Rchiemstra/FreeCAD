// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborationRegistry.h"

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace App
{

class DocumentExecutionTelemetry;

namespace Internal
{
/** Test-only access for resetting the process-wide telemetry singleton. */
class AppExport DocumentExecutionTelemetryTestAccess
{
public:
    static void reset(DocumentExecutionTelemetry& telemetry);
};
}

enum class DocumentExecutionWatchdogState
{
    Idle,
    Active,
    Stalled
};

enum class DocumentExecutionBusyRejectionKind
{
    Edit,
    Undo,
    Redo,
    Save,
    Close,
    CommandAdmission,
    Other
};

[[nodiscard]] AppExport const char*
documentExecutionWatchdogStateName(DocumentExecutionWatchdogState state) noexcept;

[[nodiscard]] AppExport const char*
documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind kind) noexcept;

/** Rolling latency observation copied into immutable telemetry snapshots. */
struct AppExport DocumentExecutionLatencySampleStats
{
    std::uint64_t sampleCount {0};
    double minimumMilliseconds {0.0};
    double maximumMilliseconds {0.0};
    double sumMilliseconds {0.0};
    std::vector<double> recentSamplesMilliseconds;

    [[nodiscard]] double meanMilliseconds() const noexcept;
};

/** Pointer-free Busy rejection counters for one document or the process. */
struct AppExport DocumentExecutionBusyRejectionCounts
{
    std::uint64_t edit {0};
    std::uint64_t undo {0};
    std::uint64_t redo {0};
    std::uint64_t save {0};
    std::uint64_t close {0};
    std::uint64_t commandAdmission {0};
    std::uint64_t other {0};

    [[nodiscard]] std::uint64_t total() const noexcept;
};

/** Pointer-free document progress copied into telemetry snapshots. */
struct AppExport DocumentExecutionProgressSnapshot
{
    bool active {false};
    double fraction {0.0};
    std::string phase;
    std::string operationId;
    std::uint64_t lastProgressEpochMilliseconds {0};
};

/**
 * Diagnostic-only watchdog observation.
 *
 * The watchdog never terminates document threads. Stalled is reported after
 * five seconds without progress while an operation remains active.
 */
struct AppExport DocumentExecutionWatchdogSnapshot
{
    static constexpr std::uint64_t StallThresholdMilliseconds = 5000;

    DocumentExecutionWatchdogState state {DocumentExecutionWatchdogState::Idle};
    std::uint64_t millisecondsSinceLastProgress {0};
    std::string activeOperationId;
    std::string diagnostic;

    [[nodiscard]] bool stalled() const noexcept
    {
        return state == DocumentExecutionWatchdogState::Stalled;
    }
};

/** Copyable, pointer-free per-document telemetry snapshot. */
struct AppExport DocumentExecutionDocumentTelemetrySnapshot
{
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    DocumentExecutionLatencySampleStats applySliceDuration;
    DocumentExecutionLatencySampleStats scenePreparationTime;
    double presentationRevisionLagMilliseconds {0.0};
    DocumentExecutionBusyRejectionCounts busyRejections;
    DocumentExecutionProgressSnapshot progress;
    DocumentExecutionWatchdogSnapshot watchdog;

    [[nodiscard]] std::string toJson() const;
};

/** Copyable, pointer-free process-wide telemetry snapshot. */
struct AppExport DocumentExecutionProcessTelemetrySnapshot
{
    std::uint64_t publicationSequence {0};
    DocumentExecutionLatencySampleStats guiEventLoopLatency;
    DocumentExecutionBusyRejectionCounts busyRejections;
    std::vector<DocumentExecutionDocumentTelemetrySnapshot> documents;

    [[nodiscard]] std::string toJson() const;
};

/**
 * Passive per-document telemetry collector.
 *
 * Recording methods only update counters and samples. They never execute or
 * schedule document work.
 */
class AppExport DocumentExecutionTelemetryCollector
{
public:
    static constexpr std::size_t DefaultSampleCapacity = 256;

    DocumentExecutionTelemetryCollector(DocumentInstanceId documentInstanceId,
                                        DocumentLifecycleEpoch lifecycleEpoch,
                                        std::size_t sampleCapacity = DefaultSampleCapacity);

    DocumentExecutionTelemetryCollector(const DocumentExecutionTelemetryCollector&) = delete;
    DocumentExecutionTelemetryCollector& operator=(const DocumentExecutionTelemetryCollector&) =
        delete;

    [[nodiscard]] DocumentInstanceId documentInstanceId() const noexcept;
    [[nodiscard]] DocumentLifecycleEpoch lifecycleEpoch() const noexcept;

    void recordApplySliceDuration(double milliseconds);
    void recordScenePreparationTime(double milliseconds);
    void recordPresentationRevisionLag(double milliseconds);
    void recordBusyRejection(DocumentExecutionBusyRejectionKind kind);

    void updateProgress(const DocumentExecutionProgressSnapshot& progress);
    void clearProgress();

    void beginWatchdog(std::string operationId);
    void touchWatchdogProgress();
    void endWatchdog();

    /**
     * Return a complete immutable snapshot. Readers never observe partially
     * updated fields because publication copies all counters under one lock.
     */
    [[nodiscard]] DocumentExecutionDocumentTelemetrySnapshot snapshot() const;

private:
    friend class Internal::DocumentExecutionTelemetryTestAccess;

    const DocumentInstanceId _documentInstanceId;
    const DocumentLifecycleEpoch _lifecycleEpoch;
    const std::size_t _sampleCapacity;
    mutable std::mutex _mutex;
    std::uint64_t _applySliceSampleCount {0};
    double _applySliceMinimumMilliseconds {0.0};
    double _applySliceMaximumMilliseconds {0.0};
    double _applySliceSumMilliseconds {0.0};
    std::vector<double> _applySliceRecent;
    std::uint64_t _scenePreparationSampleCount {0};
    double _scenePreparationMinimumMilliseconds {0.0};
    double _scenePreparationMaximumMilliseconds {0.0};
    double _scenePreparationSumMilliseconds {0.0};
    std::vector<double> _scenePreparationRecent;
    double _presentationRevisionLagMilliseconds {0.0};
    DocumentExecutionBusyRejectionCounts _busyRejections;
    DocumentExecutionProgressSnapshot _progress;
    bool _watchdogActive {false};
    std::string _watchdogOperationId;
    std::uint64_t _watchdogLastProgressEpochMilliseconds {0};
};

/**
 * Process-wide passive telemetry registry.
 *
 * GUI and lane code record samples here without changing execution behavior.
 * Published snapshots are immutable value copies assembled under one lock.
 */
class AppExport DocumentExecutionTelemetry
{
public:
    static DocumentExecutionTelemetry& instance();

    DocumentExecutionTelemetry(const DocumentExecutionTelemetry&) = delete;
    DocumentExecutionTelemetry& operator=(const DocumentExecutionTelemetry&) = delete;

    void recordGuiEventLoopLatency(double milliseconds);

    [[nodiscard]] std::shared_ptr<DocumentExecutionTelemetryCollector>
    document(DocumentInstanceId documentInstanceId, DocumentLifecycleEpoch lifecycleEpoch);

    void removeDocument(DocumentInstanceId documentInstanceId);

    [[nodiscard]] std::optional<DocumentExecutionDocumentTelemetrySnapshot>
    documentSnapshot(DocumentInstanceId documentInstanceId) const;

    /** Assemble and atomically replace the published process-wide snapshot. */
    void publishProcessSnapshot();

    [[nodiscard]] DocumentExecutionProcessTelemetrySnapshot processSnapshot() const;

    /**
     * Return the latest published process snapshot when available, otherwise
     * assemble one on demand.
     */
    [[nodiscard]] DocumentExecutionProcessTelemetrySnapshot snapshot() const;

private:
    friend class Internal::DocumentExecutionTelemetryTestAccess;

    explicit DocumentExecutionTelemetry(std::size_t sampleCapacity = DefaultSampleCapacity);

    static constexpr std::size_t DefaultSampleCapacity = 256;

    [[nodiscard]] DocumentExecutionProcessTelemetrySnapshot assembleProcessSnapshot() const;

    const std::size_t _sampleCapacity;
    mutable std::mutex _mutex;
    std::uint64_t _publicationSequence {0};
    std::uint64_t _guiEventLoopSampleCount {0};
    double _guiEventLoopMinimumMilliseconds {0.0};
    double _guiEventLoopMaximumMilliseconds {0.0};
    double _guiEventLoopSumMilliseconds {0.0};
    std::vector<double> _guiEventLoopRecent;
    DocumentExecutionBusyRejectionCounts _processBusyRejections;
    std::unordered_map<DocumentInstanceId, std::shared_ptr<DocumentExecutionTelemetryCollector>>
        _documents;
    std::shared_ptr<const DocumentExecutionProcessTelemetrySnapshot> _publishedSnapshot;
};

}  // namespace App
