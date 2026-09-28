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
    auto* owner = document();
    if (!owner) {
        return closedDocumentSnapshot();
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
    return status().terminal();
}

bool RecomputeHandle::cancel(std::string reason)
{
    auto* owner = document();
    if (!owner) {
        return false;
    }

    return owner->recomputeCoordinator().cancel(_id, std::move(reason));
}

DocumentRecomputeSnapshot RecomputeHandle::wait(const std::chrono::milliseconds timeout)
{
    DocumentWouldBlock::throwIfGuiThread("RecomputeHandle::wait()", "RecomputeHandle::poll()");

    const auto boundedTimeout = std::max(timeout, 0ms);
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;

    auto* owner = document();
    if (!owner) {
        return closedDocumentSnapshot();
    }

    const auto waitOnOwner = [&]() -> DocumentRecomputeSnapshot {
        while (true) {
            // Pump coordinator work on the owner thread only inside wait().
            static_cast<void>(owner->recomputeCoordinator().poll(_id));
            auto snapshot = owner->recomputeCoordinator().status(_id);
            if (!snapshot) {
                return closedDocumentSnapshot();
            }
            finalizeIfTerminal(*owner, *snapshot);
            snapshot = status();
            if (snapshot.terminal() || std::chrono::steady_clock::now() >= deadline) {
                return snapshot;
            }
            if (QCoreApplication::instance()) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
            }
            std::this_thread::sleep_for(2ms);
        }
    };
    if (owner->executionLane() && !owner->isCollaborationOwnerThread()) {
        return owner->executionLane()->dispatchToOwner(waitOnOwner, true, true);
    }
    return waitOnOwner();
}

}  // namespace App
