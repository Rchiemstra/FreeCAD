// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionTelemetry.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace App
{

namespace
{

using SteadyClock = std::chrono::steady_clock;

std::uint64_t steadyEpochMilliseconds()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now().time_since_epoch())
            .count());
}

void appendJsonString(std::string& output, std::string_view value)
{
    output.push_back('"');
    for (const char character : value) {
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output.push_back(character);
                break;
        }
    }
    output.push_back('"');
}

void appendLatencyStats(std::string& output,
                        std::string_view fieldName,
                        const DocumentExecutionLatencySampleStats& stats)
{
    output += '"';
    output.append(fieldName);
    output += "\":{\"sample_count\":";
    output += std::to_string(stats.sampleCount);
    output += ",\"minimum_milliseconds\":";
    output += std::to_string(stats.minimumMilliseconds);
    output += ",\"maximum_milliseconds\":";
    output += std::to_string(stats.maximumMilliseconds);
    output += ",\"mean_milliseconds\":";
    output += std::to_string(stats.meanMilliseconds());
    output += ",\"recent_samples_milliseconds\":[";
    for (std::size_t index = 0; index < stats.recentSamplesMilliseconds.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += std::to_string(stats.recentSamplesMilliseconds[index]);
    }
    output += "]}";
}

void appendBusyRejections(std::string& output,
                          std::string_view fieldName,
                          const DocumentExecutionBusyRejectionCounts& counts)
{
    output += '"';
    output.append(fieldName);
    output += "\":{\"edit\":";
    output += std::to_string(counts.edit);
    output += ",\"undo\":";
    output += std::to_string(counts.undo);
    output += ",\"redo\":";
    output += std::to_string(counts.redo);
    output += ",\"save\":";
    output += std::to_string(counts.save);
    output += ",\"close\":";
    output += std::to_string(counts.close);
    output += ",\"command_admission\":";
    output += std::to_string(counts.commandAdmission);
    output += ",\"other\":";
    output += std::to_string(counts.other);
    output += ",\"total\":";
    output += std::to_string(counts.total());
    output.push_back('}');
}

DocumentExecutionLatencySampleStats makeLatencyStats(std::uint64_t sampleCount,
                                                     double minimumMilliseconds,
                                                     double maximumMilliseconds,
                                                     double sumMilliseconds,
                                                     const std::vector<double>& recent)
{
    DocumentExecutionLatencySampleStats stats;
    stats.sampleCount = sampleCount;
    stats.minimumMilliseconds = minimumMilliseconds;
    stats.maximumMilliseconds = maximumMilliseconds;
    stats.sumMilliseconds = sumMilliseconds;
    stats.recentSamplesMilliseconds = recent;
    return stats;
}

void recordLatencySample(std::uint64_t& sampleCount,
                         double& minimumMilliseconds,
                         double& maximumMilliseconds,
                         double& sumMilliseconds,
                         std::vector<double>& recent,
                         std::size_t capacity,
                         double milliseconds)
{
    if (milliseconds < 0.0) {
        return;
    }

    ++sampleCount;
    if (sampleCount == 1) {
        minimumMilliseconds = milliseconds;
        maximumMilliseconds = milliseconds;
    }
    else {
        minimumMilliseconds = std::min(minimumMilliseconds, milliseconds);
        maximumMilliseconds = std::max(maximumMilliseconds, milliseconds);
    }
    sumMilliseconds += milliseconds;

    recent.push_back(milliseconds);
    if (recent.size() > capacity) {
        recent.erase(recent.begin(), recent.begin() + (recent.size() - capacity));
    }
}

void incrementBusyRejection(DocumentExecutionBusyRejectionCounts& counts,
                            DocumentExecutionBusyRejectionKind kind)
{
    switch (kind) {
        case DocumentExecutionBusyRejectionKind::Edit:
            ++counts.edit;
            break;
        case DocumentExecutionBusyRejectionKind::Undo:
            ++counts.undo;
            break;
        case DocumentExecutionBusyRejectionKind::Redo:
            ++counts.redo;
            break;
        case DocumentExecutionBusyRejectionKind::Save:
            ++counts.save;
            break;
        case DocumentExecutionBusyRejectionKind::Close:
            ++counts.close;
            break;
        case DocumentExecutionBusyRejectionKind::CommandAdmission:
            ++counts.commandAdmission;
            break;
        case DocumentExecutionBusyRejectionKind::Other:
            ++counts.other;
            break;
    }
}

DocumentExecutionWatchdogSnapshot buildWatchdogSnapshot(bool watchdogActive,
                                                        const std::string& operationId,
                                                        std::uint64_t lastProgressEpochMilliseconds,
                                                        std::uint64_t nowEpochMilliseconds)
{
    DocumentExecutionWatchdogSnapshot watchdog;
    watchdog.activeOperationId = operationId;

    if (!watchdogActive) {
        watchdog.state = DocumentExecutionWatchdogState::Idle;
        return watchdog;
    }

    watchdog.state = DocumentExecutionWatchdogState::Active;
    if (lastProgressEpochMilliseconds <= nowEpochMilliseconds) {
        watchdog.millisecondsSinceLastProgress =
            nowEpochMilliseconds - lastProgressEpochMilliseconds;
    }

    if (watchdog.millisecondsSinceLastProgress
        >= DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds) {
        watchdog.state = DocumentExecutionWatchdogState::Stalled;
        watchdog.diagnostic =
            "no progress for "
            + std::to_string(watchdog.millisecondsSinceLastProgress)
            + " ms (diagnostic only; document thread not terminated)";
    }

    return watchdog;
}

}  // namespace

const char* documentExecutionWatchdogStateName(DocumentExecutionWatchdogState state) noexcept
{
    switch (state) {
        case DocumentExecutionWatchdogState::Idle:
            return "Idle";
        case DocumentExecutionWatchdogState::Active:
            return "Active";
        case DocumentExecutionWatchdogState::Stalled:
            return "Stalled";
    }
    return "Unknown";
}

const char* documentExecutionBusyRejectionKindName(
    DocumentExecutionBusyRejectionKind kind) noexcept
{
    switch (kind) {
        case DocumentExecutionBusyRejectionKind::Edit:
            return "Edit";
        case DocumentExecutionBusyRejectionKind::Undo:
            return "Undo";
        case DocumentExecutionBusyRejectionKind::Redo:
            return "Redo";
        case DocumentExecutionBusyRejectionKind::Save:
            return "Save";
        case DocumentExecutionBusyRejectionKind::Close:
            return "Close";
        case DocumentExecutionBusyRejectionKind::CommandAdmission:
            return "CommandAdmission";
        case DocumentExecutionBusyRejectionKind::Other:
            return "Other";
    }
    return "Unknown";
}

double DocumentExecutionLatencySampleStats::meanMilliseconds() const noexcept
{
    if (sampleCount == 0) {
        return 0.0;
    }
    return sumMilliseconds / static_cast<double>(sampleCount);
}

std::uint64_t DocumentExecutionBusyRejectionCounts::total() const noexcept
{
    return edit + undo + redo + save + close + commandAdmission + other;
}

std::string DocumentExecutionDocumentTelemetrySnapshot::toJson() const
{
    std::string output = "{\"document_instance_id\":";
    output += std::to_string(documentInstanceId);
    output += ",\"lifecycle_epoch\":";
    output += std::to_string(lifecycleEpoch);
    output.push_back(',');
    appendLatencyStats(output, "apply_slice_duration", applySliceDuration);
    output.push_back(',');
    appendLatencyStats(output, "scene_preparation_time", scenePreparationTime);
    output += ",\"presentation_revision_lag_milliseconds\":";
    output += std::to_string(presentationRevisionLagMilliseconds);
    output.push_back(',');
    appendBusyRejections(output, "busy_rejections", busyRejections);
    output += ",\"progress\":{\"active\":";
    output += progress.active ? "true" : "false";
    output += ",\"fraction\":";
    output += std::to_string(progress.fraction);
    output += ",\"phase\":";
    appendJsonString(output, progress.phase);
    output += ",\"operation_id\":";
    appendJsonString(output, progress.operationId);
    output += ",\"last_progress_epoch_milliseconds\":";
    output += std::to_string(progress.lastProgressEpochMilliseconds);
    output += "},\"watchdog\":{\"state\":";
    appendJsonString(output, documentExecutionWatchdogStateName(watchdog.state));
    output += ",\"milliseconds_since_last_progress\":";
    output += std::to_string(watchdog.millisecondsSinceLastProgress);
    output += ",\"stall_threshold_milliseconds\":";
    output += std::to_string(DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds);
    output += ",\"active_operation_id\":";
    appendJsonString(output, watchdog.activeOperationId);
    output += ",\"diagnostic\":";
    appendJsonString(output, watchdog.diagnostic);
    output += ",\"stalled\":";
    output += watchdog.stalled() ? "true" : "false";
    output += "}}";
    return output;
}

std::string DocumentExecutionProcessTelemetrySnapshot::toJson() const
{
    std::string output = "{\"publication_sequence\":";
    output += std::to_string(publicationSequence);
    output.push_back(',');
    appendLatencyStats(output, "gui_event_loop_latency", guiEventLoopLatency);
    output.push_back(',');
    appendBusyRejections(output, "busy_rejections", busyRejections);
    output += ",\"documents\":[";
    for (std::size_t index = 0; index < documents.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += documents[index].toJson();
    }
    output += "]}";
    return output;
}

DocumentExecutionTelemetryCollector::DocumentExecutionTelemetryCollector(
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::size_t sampleCapacity)
    : _documentInstanceId(documentInstanceId)
    , _lifecycleEpoch(lifecycleEpoch)
    , _sampleCapacity(sampleCapacity == 0 ? DefaultSampleCapacity : sampleCapacity)
{}

DocumentInstanceId DocumentExecutionTelemetryCollector::documentInstanceId() const noexcept
{
    return _documentInstanceId;
}

DocumentLifecycleEpoch DocumentExecutionTelemetryCollector::lifecycleEpoch() const noexcept
{
    return _lifecycleEpoch;
}

void DocumentExecutionTelemetryCollector::recordApplySliceDuration(double milliseconds)
{
    std::lock_guard lock(_mutex);
    recordLatencySample(_applySliceSampleCount,
                        _applySliceMinimumMilliseconds,
                        _applySliceMaximumMilliseconds,
                        _applySliceSumMilliseconds,
                        _applySliceRecent,
                        _sampleCapacity,
                        milliseconds);
}

void DocumentExecutionTelemetryCollector::recordScenePreparationTime(double milliseconds)
{
    std::lock_guard lock(_mutex);
    recordLatencySample(_scenePreparationSampleCount,
                        _scenePreparationMinimumMilliseconds,
                        _scenePreparationMaximumMilliseconds,
                        _scenePreparationSumMilliseconds,
                        _scenePreparationRecent,
                        _sampleCapacity,
                        milliseconds);
}

void DocumentExecutionTelemetryCollector::recordPresentationRevisionLag(double milliseconds)
{
    if (milliseconds < 0.0) {
        return;
    }

    std::lock_guard lock(_mutex);
    _presentationRevisionLagMilliseconds = milliseconds;
}

void DocumentExecutionTelemetryCollector::recordBusyRejection(
    DocumentExecutionBusyRejectionKind kind)
{
    std::lock_guard lock(_mutex);
    incrementBusyRejection(_busyRejections, kind);
}

void DocumentExecutionTelemetryCollector::updateProgress(
    const DocumentExecutionProgressSnapshot& progress)
{
    std::lock_guard lock(_mutex);
    _progress = progress;
    if (_progress.active && _progress.lastProgressEpochMilliseconds == 0) {
        _progress.lastProgressEpochMilliseconds = steadyEpochMilliseconds();
    }
    if (_watchdogActive) {
        _watchdogLastProgressEpochMilliseconds = _progress.lastProgressEpochMilliseconds;
    }
}

void DocumentExecutionTelemetryCollector::clearProgress()
{
    std::lock_guard lock(_mutex);
    _progress = {};
}

void DocumentExecutionTelemetryCollector::beginWatchdog(std::string operationId)
{
    const auto now = steadyEpochMilliseconds();
    std::lock_guard lock(_mutex);
    _watchdogActive = true;
    _watchdogOperationId = std::move(operationId);
    _watchdogLastProgressEpochMilliseconds = now;
    if (_progress.active && _progress.lastProgressEpochMilliseconds == 0) {
        _progress.lastProgressEpochMilliseconds = now;
    }
}

void DocumentExecutionTelemetryCollector::touchWatchdogProgress()
{
    const auto now = steadyEpochMilliseconds();
    std::lock_guard lock(_mutex);
    if (_watchdogActive) {
        _watchdogLastProgressEpochMilliseconds = now;
    }
    if (_progress.active) {
        _progress.lastProgressEpochMilliseconds = now;
    }
}

void DocumentExecutionTelemetryCollector::endWatchdog()
{
    std::lock_guard lock(_mutex);
    _watchdogActive = false;
    _watchdogOperationId.clear();
    _watchdogLastProgressEpochMilliseconds = 0;
}

DocumentExecutionDocumentTelemetrySnapshot
DocumentExecutionTelemetryCollector::snapshot() const
{
    const auto nowEpochMilliseconds = steadyEpochMilliseconds();

    std::lock_guard lock(_mutex);
    DocumentExecutionDocumentTelemetrySnapshot snapshot;
    snapshot.documentInstanceId = _documentInstanceId;
    snapshot.lifecycleEpoch = _lifecycleEpoch;
    snapshot.applySliceDuration =
        makeLatencyStats(_applySliceSampleCount,
                         _applySliceMinimumMilliseconds,
                         _applySliceMaximumMilliseconds,
                         _applySliceSumMilliseconds,
                         _applySliceRecent);
    snapshot.scenePreparationTime =
        makeLatencyStats(_scenePreparationSampleCount,
                         _scenePreparationMinimumMilliseconds,
                         _scenePreparationMaximumMilliseconds,
                         _scenePreparationSumMilliseconds,
                         _scenePreparationRecent);
    snapshot.presentationRevisionLagMilliseconds = _presentationRevisionLagMilliseconds;
    snapshot.busyRejections = _busyRejections;
    snapshot.progress = _progress;
    snapshot.watchdog = buildWatchdogSnapshot(_watchdogActive,
                                            _watchdogOperationId,
                                            _watchdogLastProgressEpochMilliseconds,
                                            nowEpochMilliseconds);
    return snapshot;
}

void Internal::DocumentExecutionTelemetryTestAccess::reset(
    DocumentExecutionTelemetry& telemetry)
{
    std::lock_guard lock(telemetry._mutex);
    telemetry._publicationSequence = 0;
    telemetry._guiEventLoopSampleCount = 0;
    telemetry._guiEventLoopMinimumMilliseconds = 0.0;
    telemetry._guiEventLoopMaximumMilliseconds = 0.0;
    telemetry._guiEventLoopSumMilliseconds = 0.0;
    telemetry._guiEventLoopRecent.clear();
    telemetry._processBusyRejections = {};
    telemetry._documents.clear();
    telemetry._publishedSnapshot.reset();
}

DocumentExecutionTelemetry& DocumentExecutionTelemetry::instance()
{
    static DocumentExecutionTelemetry telemetry;
    return telemetry;
}

DocumentExecutionTelemetry::DocumentExecutionTelemetry(std::size_t sampleCapacity)
    : _sampleCapacity(sampleCapacity == 0 ? DefaultSampleCapacity : sampleCapacity)
{}

void DocumentExecutionTelemetry::recordGuiEventLoopLatency(double milliseconds)
{
    std::lock_guard lock(_mutex);
    recordLatencySample(_guiEventLoopSampleCount,
                        _guiEventLoopMinimumMilliseconds,
                        _guiEventLoopMaximumMilliseconds,
                        _guiEventLoopSumMilliseconds,
                        _guiEventLoopRecent,
                        _sampleCapacity,
                        milliseconds);
}

DocumentExecutionTelemetryCollector& DocumentExecutionTelemetry::document(
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch)
{
    std::lock_guard lock(_mutex);
    const auto found = _documents.find(documentInstanceId);
    if (found != _documents.end()) {
        return *found->second;
    }

    auto collector = std::make_shared<DocumentExecutionTelemetryCollector>(
        documentInstanceId,
        lifecycleEpoch,
        _sampleCapacity);
    _documents.emplace(documentInstanceId, collector);
    return *collector;
}

void DocumentExecutionTelemetry::removeDocument(DocumentInstanceId documentInstanceId)
{
    std::lock_guard lock(_mutex);
    _documents.erase(documentInstanceId);
}

std::optional<DocumentExecutionDocumentTelemetrySnapshot>
DocumentExecutionTelemetry::documentSnapshot(DocumentInstanceId documentInstanceId) const
{
    std::lock_guard lock(_mutex);
    const auto found = _documents.find(documentInstanceId);
    if (found == _documents.end()) {
        return std::nullopt;
    }
    return found->second->snapshot();
}

void DocumentExecutionTelemetry::publishProcessSnapshot()
{
    std::lock_guard lock(_mutex);
    ++_publicationSequence;
    _publishedSnapshot =
        std::make_shared<const DocumentExecutionProcessTelemetrySnapshot>(
            assembleProcessSnapshot());
}

DocumentExecutionProcessTelemetrySnapshot DocumentExecutionTelemetry::processSnapshot() const
{
    std::lock_guard lock(_mutex);
    if (_publishedSnapshot) {
        return *_publishedSnapshot;
    }
    return assembleProcessSnapshot();
}

DocumentExecutionProcessTelemetrySnapshot DocumentExecutionTelemetry::snapshot() const
{
    return processSnapshot();
}

DocumentExecutionProcessTelemetrySnapshot
DocumentExecutionTelemetry::assembleProcessSnapshot() const
{
    DocumentExecutionProcessTelemetrySnapshot snapshot;
    snapshot.publicationSequence = _publicationSequence;
    snapshot.guiEventLoopLatency =
        makeLatencyStats(_guiEventLoopSampleCount,
                         _guiEventLoopMinimumMilliseconds,
                         _guiEventLoopMaximumMilliseconds,
                         _guiEventLoopSumMilliseconds,
                         _guiEventLoopRecent);
    snapshot.busyRejections = _processBusyRejections;

    std::vector<DocumentInstanceId> documentIds;
    documentIds.reserve(_documents.size());
    for (const auto& entry : _documents) {
        documentIds.push_back(entry.first);
    }
    std::sort(documentIds.begin(), documentIds.end());

    snapshot.documents.reserve(documentIds.size());
    for (const auto documentInstanceId : documentIds) {
        const auto found = _documents.find(documentInstanceId);
        if (found != _documents.end()) {
            snapshot.documents.push_back(found->second->snapshot());
            const auto& documentBusy = snapshot.documents.back().busyRejections;
            snapshot.busyRejections.edit += documentBusy.edit;
            snapshot.busyRejections.undo += documentBusy.undo;
            snapshot.busyRejections.redo += documentBusy.redo;
            snapshot.busyRejections.save += documentBusy.save;
            snapshot.busyRejections.close += documentBusy.close;
            snapshot.busyRejections.commandAdmission += documentBusy.commandAdmission;
            snapshot.busyRejections.other += documentBusy.other;
        }
    }

    return snapshot;
}

}  // namespace App
