// SPDX-License-Identifier: LGPL-2.1-or-later

#include "RecomputeHandle.h"

#include "Document.h"
#include "DocumentCollaborationService.h"
#include "DocumentExecutionLane.h"
#include "DocumentObserver.h"
#include "DocumentWouldBlock.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <algorithm>
#include <future>
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
    , _lifetimeGate(document.collaborationService().lifetimeGate())
    , _id(id)
{}

RecomputeHandle::~RecomputeHandle() = default;

DocumentRecomputeId RecomputeHandle::id() const noexcept
{
    return _id;
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
    const DocumentCollaborationService::LifecyclePin pin(_lifetimeGate);
    auto* owner = pin.document();
    if (!owner) {
        return closedDocumentSnapshot();
    }

    std::optional<DocumentRecomputeSnapshot> snapshot;
    if (const auto* lane = owner->executionLane()) {
        snapshot = lane->recomputeStatus(_id);
    }
    else {
        snapshot = owner->recomputeCoordinator().status(_id);
    }
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
    return status().terminal();
}

bool RecomputeHandle::cancel(std::string reason)
{
    const DocumentCollaborationService::LifecyclePin pin(_lifetimeGate);
    auto* owner = pin.document();
    if (!owner) {
        return false;
    }

    return owner->recomputeCoordinator().cancel(_id, std::move(reason));
}

DocumentRecomputeSnapshot RecomputeHandle::wait(const std::chrono::milliseconds timeout)
{
    const auto boundedTimeout = std::max(timeout, 0ms);
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;

    const DocumentCollaborationService::LifecyclePin pin(_lifetimeGate);
    auto* owner = pin.document();
    if (!owner) {
        return closedDocumentSnapshot();
    }
    // With an execution lane a GUI-thread wait hops to the owner and waits
    // without running the Qt event loop (DocumentExecutionLane::dispatchToOwner());
    // without one, waitOnOwner() would pump Qt on the GUI thread.
    auto* lane = owner->executionLane();
    if (!lane) {
        DocumentWouldBlock::throwIfGuiThread("RecomputeHandle::wait()", "RecomputeHandle::poll()");
    }

    const auto waitOnOwner = [&]() -> DocumentRecomputeSnapshot {
        // Register executing-thread admission on the lane owner thread so
        // synchronous callbacks during poll/finalization (e.g. headless
        // signalRecomputed -> closeDocument) are refused instead of deadlocking
        // on the caller's outer LifecyclePin.
        const DocumentCollaborationService::LifecyclePin ownerPin(_lifetimeGate);
        auto* pinnedOwner = ownerPin.document();
        if (!pinnedOwner) {
            return closedDocumentSnapshot();
        }
        if (lane) {
            if (auto snapshot = lane->waitForRecompute(_id, deadline)) {
                return *snapshot;
            }
        }
        while (true) {
            // Pump coordinator work on the owner thread only inside wait().
            static_cast<void>(pinnedOwner->recomputeCoordinator().poll(_id));
            auto coordinatorSnapshot = pinnedOwner->recomputeCoordinator().status(_id);
            if (!coordinatorSnapshot) {
                return closedDocumentSnapshot();
            }
            finalizeIfTerminal(*pinnedOwner, *coordinatorSnapshot);
            const auto snapshot = status();
            if (snapshot.terminal() || std::chrono::steady_clock::now() >= deadline) {
                return snapshot;
            }
            if (QCoreApplication::instance()) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
            }
            std::this_thread::sleep_for(2ms);
        }
    };
    if (lane && !owner->isCollaborationOwnerThread()) {
        try {
            return lane->dispatchToOwner(waitOnOwner, true, true);
        }
        catch (const std::future_error&) {
            return closedDocumentSnapshot();
        }
        catch (const DocumentWouldBlock&) {
            if (lane->isClosingOrShuttingDown()
                || !DocumentCollaborationService::LifecyclePin(_lifetimeGate)) {
                return closedDocumentSnapshot();
            }
            throw;
        }
    }
    return waitOnOwner();
}

}  // namespace App
