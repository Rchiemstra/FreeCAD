// SPDX-License-Identifier: LGPL-2.1-or-later

#include "RecomputeHandle.h"

#include "Document.h"
#include "DocumentCollaborationService.h"
#include "MainThreadSignal.h"

#include <Base/Exception.h>
#include <Base/Interpreter.h>

#include <QCoreApplication>
#include <QEventLoop>

#include <algorithm>
#include <exception>
#include <optional>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace App
{

namespace
{

template<typename Result, typename OwnerPredicate, typename Callable>
Result invokeForDocumentOwner(OwnerPredicate&& isDocumentOwnerThread,
                              Callable&& callable)
{
    if (isDocumentOwnerThread()) {
        return std::forward<Callable>(callable)();
    }
    if (!MainThreadSignalConfig::hasHooks()
        || MainThreadSignalConfig::isMainThread()) {
        throw Base::RuntimeError(
            "recompute handle access requires its document owner-thread dispatcher");
    }

    std::optional<Result> result;
    std::exception_ptr failure;
    {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        MainThreadSignalConfig::invoke(
            [&] {
                if (!isDocumentOwnerThread()) {
                    failure = std::make_exception_ptr(Base::RuntimeError(
                        "recompute handle dispatcher does not own the document"));
                    return;
                }
                try {
                    result.emplace(std::forward<Callable>(callable)());
                }
                catch (...) {
                    failure = std::current_exception();
                }
            },
            true);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (!result) {
        throw Base::RuntimeError(
            "recompute handle dispatcher did not execute the request");
    }
    return std::move(*result);
}

}  // namespace

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
    : _document(&document)
    , _lifetimeGate(document.collaborationService().lifetimeGate())
    , _id(id)
{}

RecomputeHandle::~RecomputeHandle() = default;

DocumentRecomputeId RecomputeHandle::id() const noexcept
{
    return _id;
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
    if (snapshot.terminal()) {
        document.finalizeDetachedRecompute(snapshot);
    }
}

DocumentRecomputeSnapshot RecomputeHandle::status()
{
    DocumentCollaborationService::LifecyclePin lifecyclePin(_lifetimeGate);
    if (!lifecyclePin) {
        return closedDocumentSnapshot();
    }
    return invokeForDocumentOwner<DocumentRecomputeSnapshot>(
        [this] { return _document->isCollaborationOwnerThread(); },
        [this] { return statusWithPinnedDocument(*_document); });
}

DocumentRecomputeSnapshot RecomputeHandle::statusWithPinnedDocument(Document& document)
{
    static_cast<void>(document.recomputeCoordinator().poll(_id));
    auto snapshot = document.recomputeCoordinator().status(_id);
    if (!snapshot) {
        DocumentRecomputeSnapshot unavailable;
        unavailable.id = _id;
        unavailable.state = DocumentRecomputeState::Cancelled;
        unavailable.diagnostic = "recompute result is unavailable";
        return unavailable;
    }
    finalizeIfTerminal(document, *snapshot);
    return *snapshot;
}

bool RecomputeHandle::poll()
{
    return status().terminal();
}

bool RecomputeHandle::cancel(std::string reason)
{
    DocumentCollaborationService::LifecyclePin lifecyclePin(_lifetimeGate);
    if (!lifecyclePin) {
        return false;
    }
    return invokeForDocumentOwner<bool>(
        [this] { return _document->isCollaborationOwnerThread(); },
        [this, reason = std::move(reason)]() mutable {
            const bool accepted =
                _document->recomputeCoordinator().cancel(_id, std::move(reason));
            static_cast<void>(statusWithPinnedDocument(*_document));
            return accepted;
        });
}

DocumentRecomputeSnapshot RecomputeHandle::wait(const std::chrono::milliseconds timeout)
{
    const auto boundedTimeout = std::max(timeout, 0ms);
    const auto deadline = std::chrono::steady_clock::now() + boundedTimeout;
    while (true) {
        auto snapshot = status();
        if (snapshot.terminal() || std::chrono::steady_clock::now() >= deadline) {
            return snapshot;
        }
        if (QCoreApplication::instance()) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
        }
        std::this_thread::sleep_for(2ms);
    }
}

}  // namespace App
