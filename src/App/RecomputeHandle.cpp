// SPDX-License-Identifier: LGPL-2.1-or-later

#include "RecomputeHandle.h"

#include "Document.h"
#include "DocumentExecutionLane.h"
#include "DocumentObserver.h"
#include "DocumentWouldBlock.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <algorithm>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace App
{

const char* documentRecomputeStateName(const DocumentRecomputeState state) noexcept
{
    switch (state) {
        case DocumentRecomputeState::Running:
            return "running";
        case DocumentRecomputeState::Cancelling:
            return "cancelling";
        case DocumentRecomputeState::Completed:
            return "completed";
        case DocumentRecomputeState::PartialFailure:
            return "partial_failure";
        case DocumentRecomputeState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

const char* documentRecomputeFeatureStateName(
    const DocumentRecomputeFeatureState state) noexcept
{
    switch (state) {
        case DocumentRecomputeFeatureState::Waiting:
            return "waiting";
        case DocumentRecomputeFeatureState::Preparing:
            return "preparing";
        case DocumentRecomputeFeatureState::Committing:
            return "committing";
        case DocumentRecomputeFeatureState::Committed:
            return "committed";
        case DocumentRecomputeFeatureState::Stale:
            return "stale";
        case DocumentRecomputeFeatureState::Failed:
            return "failed";
        case DocumentRecomputeFeatureState::Blocked:
            return "blocked";
        case DocumentRecomputeFeatureState::Cancelling:
            return "cancelling";
        case DocumentRecomputeFeatureState::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

RecomputeHandle::RecomputeHandle(Document& document, const DocumentRecomputeId id)
    : _document(std::make_unique<DocumentWeakPtrT>(&document))
    , _id(id)
{}

RecomputeHandle::RecomputeHandle(Document& document,
                                 const DocumentCommandId laneCommandId,
                                 DocumentRevisionIdentityBinding documentIdentity)
    : _document(std::make_unique<DocumentWeakPtrT>(&document))
    , _laneCommandId(laneCommandId)
    , _laneIdentity(documentIdentity)
{}

RecomputeHandle::~RecomputeHandle() = default;

DocumentRecomputeId RecomputeHandle::resolvedRecomputeId() const noexcept
{
    if (_laneCommandId != 0) {
        const DocumentCommandHandle commandHandle(_laneCommandId, _laneIdentity);
        const auto snapshot = commandHandle.status();
        if (snapshot.recompute && snapshot.recompute->id != 0) {
            return static_cast<DocumentRecomputeId>(snapshot.recompute->id);
        }
    }
    return _id;
}

DocumentRecomputeId RecomputeHandle::id() const noexcept
{
    return resolvedRecomputeId();
}

DocumentRecomputeSnapshot RecomputeHandle::snapshotFromLaneCommand() const
{
    DocumentRecomputeSnapshot unavailable;
    unavailable.id = resolvedRecomputeId();
    unavailable.state = DocumentRecomputeState::Cancelled;
    unavailable.diagnostic = "recompute result is unavailable";

    const DocumentCommandHandle commandHandle(_laneCommandId, _laneIdentity);
    const auto commandSnapshot = commandHandle.status();
    if (!commandSnapshot.recompute) {
        if (commandSnapshot.terminal()) {
            unavailable.diagnostic = commandSnapshot.diagnostic.empty()
                ? "document recompute command finished without observation"
                : commandSnapshot.diagnostic;
        }
        return unavailable;
    }

    const auto& observation = *commandSnapshot.recompute;
    DocumentRecomputeSnapshot snapshot;
    snapshot.id = observation.id != 0 ? observation.id : unavailable.id;
    snapshot.state =
        static_cast<DocumentRecomputeState>(static_cast<int>(observation.state));
    snapshot.completedFeatures = observation.completedFeatures;
    snapshot.failedFeatures = observation.failedFeatures;
    snapshot.totalFeatures = observation.totalFeatures;
    snapshot.progress = observation.progress;
    snapshot.diagnostic = observation.diagnostic;
    snapshot.features.reserve(observation.features.size());
    for (const auto& feature : observation.features) {
        DocumentRecomputeFeatureSnapshot copied;
        copied.featureId = feature.featureId;
        copied.state =
            static_cast<DocumentRecomputeFeatureState>(static_cast<int>(feature.state));
        copied.diagnostic = feature.diagnostic;
        copied.executed = feature.executed;
        snapshot.features.push_back(std::move(copied));
    }
    return snapshot;
}

Document* RecomputeHandle::document() const noexcept
{
    return _document ? **_document : nullptr;
}

DocumentRecomputeSnapshot RecomputeHandle::closedDocumentSnapshot() const
{
    DocumentRecomputeSnapshot snapshot;
    snapshot.id = _id;
    snapshot.state = DocumentRecomputeState::Cancelled;
    snapshot.diagnostic = "recompute document is no longer live";
    return snapshot;
}

void RecomputeHandle::finalizeIfTerminal(
    Document& document,
    const DocumentRecomputeSnapshot& snapshot)
{
    if (snapshot.terminal()
        && document.recomputeCoordinator().claimPresentationFinalization(_id)) {
        document.finalizeDetachedRecompute(snapshot);
    }
}

DocumentRecomputeSnapshot RecomputeHandle::status()
{
    auto* owner = document();
    if (!owner) {
        return closedDocumentSnapshot();
    }

    if (_laneCommandId != 0) {
        return snapshotFromLaneCommand();
    }

    if (const auto* lane = owner->executionLane()) {
        if (const auto published = lane->recomputeStatus(_id)) {
            return *published;
        }
    }

    const auto snapshot = owner->recomputeCoordinator().status(_id);
    if (!snapshot) {
        DocumentRecomputeSnapshot unavailable;
        unavailable.id = _id;
        unavailable.state = DocumentRecomputeState::Cancelled;
        unavailable.diagnostic = "recompute result is unavailable";
        return unavailable;
    }
    return *snapshot;
}

bool RecomputeHandle::poll()
{
    auto* owner = document();
    if (!owner) {
        return true;
    }

    if (_laneCommandId != 0) {
        return status().terminal();
    }

    if (owner->executionLane() && !owner->isCollaborationOwnerThread()) {
        // Observation-only: the lane owner thread pumps during executeActiveRecompute().
        return status().terminal();
    }

    static_cast<void>(owner->recomputeCoordinator().poll(_id));
    const auto snapshot = owner->recomputeCoordinator().status(_id);
    if (!snapshot) {
        return true;
    }
    finalizeIfTerminal(*owner, *snapshot);
    return snapshot->terminal();
}

bool RecomputeHandle::cancel(std::string reason)
{
    auto* owner = document();
    if (!owner) {
        return false;
    }

    if (_laneCommandId != 0) {
        DocumentCommandHandle commandHandle(_laneCommandId, _laneIdentity);
        return commandHandle.cancel(std::move(reason));
    }

    const bool accepted = owner->recomputeCoordinator().cancel(_id, std::move(reason));
    if (owner->executionLane() && !owner->isCollaborationOwnerThread()) {
        return accepted;
    }

    static_cast<void>(poll());
    return accepted;
}

DocumentRecomputeSnapshot RecomputeHandle::wait(const std::chrono::milliseconds timeout)
{
    DocumentWouldBlock::throwIfGuiThread("RecomputeHandle::wait()", "RecomputeHandle::poll()");

    const auto boundedTimeout = std::max(timeout, 0ms);
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;

    const auto waitOnOwner = [&]() -> DocumentRecomputeSnapshot {
        while (true) {
            auto snapshot = status();
            if (snapshot.terminal() || std::chrono::steady_clock::now() >= deadline) {
                if (!snapshot.terminal()) {
                    static_cast<void>(poll());
                    snapshot = status();
                }
                return snapshot;
            }
            static_cast<void>(poll());
            if (QCoreApplication::instance()) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
            }
            std::this_thread::sleep_for(2ms);
        }
    };

    auto* owner = document();
    if (!owner) {
        return closedDocumentSnapshot();
    }
    if (owner->executionLane() && !owner->isCollaborationOwnerThread()) {
        return owner->executionLane()->dispatchToOwner(waitOnOwner);
    }
    return waitOnOwner();
}

}  // namespace App
