// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"
#include "DocumentCommandHandle.h"
#include "DocumentExecutionTelemetry.h"
#include "DocumentHandle.h"
#include "DocumentRecomputeCoordinator.h"
#include "DocumentRevisionIndex.h"
#include "DocumentWouldBlock.h"

#include <Base/Interpreter.h>

#include <FCGlobal.h>

#include <Python.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace App
{

class Document;
class DocumentExecutionTelemetryCollector;

/**
 * Close policy for document execution lanes.
 *
 * A lane thread is never forcibly terminated inside the process (no
 * QThread::terminate, pthread_kill, or similar). When close admission cannot
 * complete because work is active or the lane is stalled, the application may
 * only keep waiting cooperatively or offer whole-process exit.
 */
namespace DocumentExecutionClosePolicy
{

enum class UnresponsiveLaneAction
{
    KeepWaiting,
    RequestProcessExit
};

[[nodiscard]] constexpr bool laneThreadTerminationIsForbidden() noexcept
{
    return true;
}

[[nodiscard]] constexpr const char* unresponsiveLaneGuidance() noexcept
{
    return "Keep waiting for cooperative lane shutdown or exit the whole process; never "
           "terminate the document-owner thread inside FreeCAD.";
}

[[nodiscard]] UnresponsiveLaneAction recommendedActionWhileLaneBusy() noexcept;

}  // namespace DocumentExecutionClosePolicy

/** Wake lane idle waiters after collaboration admission for close is released. */
AppExport void notifyDocumentExecutionLaneCloseAdmissionReleased(
    const Document& document) noexcept;

/**
 * Serial document-owner thread for one live App::Document.
 *
 * The lane admits at most one model command at a time, executes coordinator
 * work on its owner thread, and publishes pointer-free immutable snapshots for
 * observation. Status checks never advance work or acquire model locks.
 */
class AppExport DocumentExecutionLane
    : public std::enable_shared_from_this<DocumentExecutionLane>
{
public:
    static std::shared_ptr<DocumentExecutionLane> create(
        Document& document,
        DocumentRevisionIdentityBinding identity);

    DocumentExecutionLane(const DocumentExecutionLane&) = delete;
    DocumentExecutionLane& operator=(const DocumentExecutionLane&) = delete;

    ~DocumentExecutionLane();

    [[nodiscard]] Document& document() noexcept;
    [[nodiscard]] const Document& document() const noexcept;
    [[nodiscard]] DocumentRevisionIdentityBinding identity() const noexcept;
    [[nodiscard]] DocumentHandle handle() const noexcept;
    [[nodiscard]] std::thread::id ownerThreadId() const noexcept;
    [[nodiscard]] bool isOwnerThread() const noexcept;
    [[nodiscard]] bool isIdle() const noexcept;
    /** True when idle or the owner thread is executing an admitted Close command. */
    [[nodiscard]] bool permitsApplicationClose() const noexcept;
    [[nodiscard]] bool shutdownRequested() const noexcept;

    [[nodiscard]] DocumentCommandSubmitOutcome trySubmit(DocumentCommand command);

    [[nodiscard]] DocumentCommandSnapshot commandStatus(DocumentCommandId id) const;
    [[nodiscard]] std::optional<DocumentRecomputeSnapshot> recomputeStatus(
        DocumentRecomputeId id) const;
    [[nodiscard]] bool cancelCommand(DocumentCommandId id, std::string reason = {});

    /** Request cooperative cancellation of active work and lane shutdown. */
    void requestShutdown(std::string reason = "document execution lane shutdown");

    /**
     * Run \p fn on the lane owner thread and wait for the result.
     *
     * Must not be called from the GUI thread when \p fn would block on model
     * work. When \p releaseGilWhileWaiting is true, releases the GIL for the
     * wait if this thread holds it (see PyGILState_Check()).
     */
    template<typename Fn>
    auto dispatchToOwner(Fn&& fn, const bool releaseGilWhileWaiting = true)
        -> std::invoke_result_t<Fn>
    {
        using Result = std::invoke_result_t<Fn>;
        if (isOwnerThread()) {
            if constexpr (std::is_void_v<Result>) {
                std::forward<Fn>(fn)();
                return;
            }
            else {
                return std::forward<Fn>(fn)();
            }
        }

        DocumentWouldBlock::throwIfGuiThread(
            "DocumentExecutionLane::dispatchToOwner",
            "Document.*Async() or executionHandle().trySubmit()");

        std::optional<Base::PyGILStateRelease> release;
        if (releaseGilWhileWaiting && Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }

        // std::function requires a copyable target; share the promise and
        // callable so the queued lambda is copy-constructible.
        auto sharedFn = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        if constexpr (std::is_void_v<Result>) {
            auto promise = std::make_shared<std::promise<void>>();
            auto future = promise->get_future();
            {
                std::lock_guard lock(_mutex);
                _dispatchQueue.push_back([promise, sharedFn]() {
                    try {
                        (*sharedFn)();
                        promise->set_value();
                    }
                    catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
                _workAvailable.notify_one();
            }
            future.get();
            return;
        }
        else {
            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            {
                std::lock_guard lock(_mutex);
                _dispatchQueue.push_back([promise, sharedFn]() {
                    try {
                        promise->set_value((*sharedFn)());
                    }
                    catch (...) {
                        promise->set_exception(std::current_exception());
                    }
                });
                _workAvailable.notify_one();
            }
            return future.get();
        }
    }

    void joinThread();
    /** Release the owner thread without waiting; used when the GUI closes a document. */
    void detachThread();

    void notifyCloseAdmissionReleased() const noexcept;

    [[nodiscard]] static std::shared_ptr<DocumentExecutionLane> find(
        DocumentInstanceId instanceId);

private:
    friend struct DocumentHandle::State;

    /** In-flight command; complete before use in std::optional. */
    struct ActiveCommand
    {
        DocumentCommandId id {0};
        DocumentCommand command;
        DocumentCommandSnapshot snapshot;
        std::optional<DocumentRecomputeId> recomputeId;
        std::atomic<bool> cancelRequested {false};
        std::uint64_t lastProgressEpochMilliseconds {0};

        ActiveCommand() = default;
        ActiveCommand(const ActiveCommand&) = delete;
        ActiveCommand& operator=(const ActiveCommand&) = delete;
        ActiveCommand(ActiveCommand&& other) noexcept
            : id(other.id)
            , command(std::move(other.command))
            , snapshot(std::move(other.snapshot))
            , recomputeId(std::move(other.recomputeId))
            , cancelRequested(other.cancelRequested.load(std::memory_order_relaxed))
            , lastProgressEpochMilliseconds(other.lastProgressEpochMilliseconds)
        {}
        ActiveCommand& operator=(ActiveCommand&& other) noexcept
        {
            if (this != &other) {
                id = other.id;
                command = std::move(other.command);
                snapshot = std::move(other.snapshot);
                recomputeId = std::move(other.recomputeId);
                cancelRequested.store(
                    other.cancelRequested.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                lastProgressEpochMilliseconds = other.lastProgressEpochMilliseconds;
            }
            return *this;
        }
    };

    DocumentExecutionLane(Document& document, DocumentRevisionIdentityBinding identity);

    /** Starts the owner thread after \c create establishes \c shared_ptr ownership. */
    void startOwnerThread();

    void threadMain();
    void drainDispatchQueue();
    void executeActiveCommand();
    void executeActiveRecompute();
    [[nodiscard]] bool executeInstantCommand(ActiveCommand& command);
    void pumpActiveRecompute(ActiveCommand& command);
    [[nodiscard]] DocumentRecomputeSnapshot recomputeSnapshotFromObservation(
        const DocumentCommandRecomputeObservation& observation) const;
    void completeActiveCommand(DocumentCommandState state, std::string diagnostic = {});
    void publishActiveSnapshot(const DocumentCommandSnapshot& snapshot);
    void publishActiveSnapshotLocked(const DocumentCommandSnapshot& snapshot);
    void touchWatchdogProgress(ActiveCommand& command);
    void updateWatchdogState(ActiveCommand& command);
    [[nodiscard]] DocumentCommandRecomputeObservation makeRecomputeObservation(
        const DocumentRecomputeSnapshot& recompute) const;
    [[nodiscard]] DocumentExecutionBusyRejectionKind busyKind(
        DocumentCommandKind kind) const noexcept;
    void recordBusyRejection(DocumentCommandKind kind);

    Document& _document;
    const DocumentRevisionIdentityBinding _identity;
    std::shared_ptr<DocumentHandle::State> _handleState;

    std::thread _thread;
    std::thread::id _ownerThreadId {};
    mutable std::mutex _mutex;
    mutable std::condition_variable _workAvailable;
    bool _threadReady {false};
    bool _shutdownRequested {false};
    std::string _shutdownReason;

    std::optional<ActiveCommand> _active;
    std::unordered_map<DocumentCommandId, DocumentCommandSnapshot> _terminalSnapshots;
    DocumentCommandId _nextCommandId {1};

    std::vector<std::function<void()>> _dispatchQueue;

    std::shared_ptr<DocumentExecutionTelemetryCollector> _telemetry;
};

}  // namespace App
