// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"
#include "DocumentCommandHandle.h"
#include "DocumentExecutionTelemetry.h"
#include "DocumentHandle.h"
#include "DocumentRecomputeCoordinator.h"
#include "DocumentRevisionIndex.h"
#include "DocumentWouldBlock.h"
#include "MainThreadSignal.h"

#include <Base/Interpreter.h>

#include <FCGlobal.h>

#include <Python.h>

#include <QCoreApplication>
#include <QEventLoop>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

namespace Internal
{
struct CollaborationServiceLifetimeGate;
}  // namespace Internal

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

/**
 * When @p stalled is false, keep waiting cooperatively. When the diagnostic
 * watchdog has marked the lane Stalled, offer whole-process exit instead of
 * in-process thread termination.
 */
[[nodiscard]] AppExport UnresponsiveLaneAction recommendedActionWhileLaneBusy(
    bool stalled = false) noexcept;

}  // namespace DocumentExecutionClosePolicy

/** Wake lane idle waiters after collaboration admission for close is released. */
AppExport void notifyDocumentExecutionLaneCloseAdmissionReleased(
    const Document& document) noexcept;

/** True while the GUI thread pumps Qt inside dispatchToOwner's wait loop. */
[[nodiscard]] AppExport bool documentExecutionLaneGuiDispatchPumpActive() noexcept;

/**
 * Look up a terminal snapshot in the process-level archive.
 *
 * Returns the snapshot if (instanceId, commandId) was recorded after the lane
 * completed (M4/M9: closed-document handle lookups stay Completed/Close instead
 * of synthesising Failed/"lane not active").
 */
[[nodiscard]] AppExport std::optional<DocumentCommandSnapshot>
findTerminalCommandSnapshot(DocumentInstanceId instanceId,
                            DocumentCommandId commandId) noexcept;

/** RAII marker for GUI dispatch wait-loop pumping (GilTryLock deadlock avoidance). */
class AppExport DocumentExecutionLaneGuiDispatchPumpScope final
{
public:
    DocumentExecutionLaneGuiDispatchPumpScope();
    ~DocumentExecutionLaneGuiDispatchPumpScope();

    DocumentExecutionLaneGuiDispatchPumpScope(
        const DocumentExecutionLaneGuiDispatchPumpScope&) = delete;
    DocumentExecutionLaneGuiDispatchPumpScope& operator=(
        const DocumentExecutionLaneGuiDispatchPumpScope&) = delete;
};

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
    /**
     * Marks non-command owner-thread work (recovery snapshot IO) that must
     * reject conflicting trySubmit admissions while in flight.
     */
    void beginRecoverySnapshotOwnerWork() noexcept;
    void endRecoverySnapshotOwnerWork() noexcept;
    /** True when idle, or the owner thread is executing Close, or this thread
     *  is the one the active Close command marshalled Application::closeDocument onto. */
    [[nodiscard]] bool permitsApplicationClose() const noexcept;
    /** Atomically verify close permission and reject further trySubmit/dispatch admissions. */
    [[nodiscard]] bool tryBeginApplicationClose() noexcept;
    /** Roll back a non-irrevocable tryBeginApplicationClose() claim. */
    void cancelApplicationClose() noexcept;
    /** Drop queued owner-dispatch tasks once Application::closeDocument() is irrevocable. */
    void commitApplicationClose() noexcept;
    /** Mark this thread as the marshalled Application::closeDocument caller. */
    void beginMarshalledCloseAdmission() noexcept;
    void endMarshalledCloseAdmission() noexcept;
    [[nodiscard]] bool shutdownRequested() const noexcept;
    /** True when the lane is closing, shutting down, or its owner document is gone. */
    [[nodiscard]] bool isClosingOrShuttingDown() const noexcept;
    /** Diagnostic-only: true when the watchdog has marked active work Stalled. */
    [[nodiscard]] bool isWatchdogStalled() const noexcept;

    [[nodiscard]] DocumentCommandSubmitOutcome trySubmit(DocumentCommand command);

    /**
     * Post a callable to the owner thread without blocking the caller.
     *
     * Returns a future that carries the result (or captured exception). The
     * owner thread runs the callable as soon as it is free. Throws
     * DocumentWouldBlock if the lane is shutting down, or if an active model
     * command prevents dispatch and allowWhileCommandActive is false.
     */
    template<typename Fn>
    [[nodiscard]] auto postToOwner(Fn&& fn, const bool allowWhileCommandActive = true)
        -> std::future<std::invoke_result_t<Fn>>
    {
        auto [future, task] = makeDispatchTask(std::forward<Fn>(fn));
        {
            std::lock_guard lock(_mutex);
            if (ownerDispatchRejectedLocked()) {
                throw DocumentWouldBlock(
                    "document execution lane is closing or shutting down");
            }
            if (commandBlocksDispatchLocked(allowWhileCommandActive)) {
                throw DocumentWouldBlock("document execution lane is busy executing model work");
            }
            _dispatchQueue.push_back(std::move(task));
            _workAvailable.notify_one();
        }
        return future;
    }

    [[nodiscard]] DocumentCommandSnapshot commandStatus(DocumentCommandId id) const;
    [[nodiscard]] std::optional<DocumentRecomputeSnapshot> recomputeStatus(
        DocumentRecomputeId id) const;
    [[nodiscard]] bool cancelCommand(DocumentCommandId id, std::string reason = {});

    /** Mark the owning Document as closing so detached drain skips queued callbacks. */
    void markOwnerDocumentDestroyed() noexcept;
    /** Request cooperative cancellation of active work and lane shutdown. */
    void requestShutdown(std::string reason = "document execution lane shutdown");

    /**
     * Run \p fn on the lane owner thread and wait for the result.
     *
     * Off the GUI thread this refuses while model work runs unless
     * \p allowWhileCommandActive. On the GUI thread it is the synchronous
     * compatibility path: it waits at most \c GuiSyncAdmissionTimeoutMs (default
     * 500 ms, read from User parameter:BaseApp/Preferences/Document) for the lane
     * to become free; if the lane is stalled or the timeout elapses it raises
     * DocumentWouldBlock naming the active command kind. Once admitted, waits
     * until completion by servicing only MainThreadSignalConfig::serviceMarshalledTasks()
     * (no Qt event loop, no additional timeout).
     * When \p releaseGilWhileWaiting is true, releases the GIL
     * for the wait if this thread holds it (see PyGILState_Check()); the GUI
     * thread always releases it so owner-thread Python can run.
     */
    template<typename Fn>
    auto dispatchToOwner(Fn&& fn,
                         const bool releaseGilWhileWaiting = true,
                         const bool allowWhileCommandActive = false)
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

        const bool onGui = DocumentWouldBlock::isGuiThread();
        if (onGui && MainThreadSignalConfig::insideBlockingInvoke()) {
            // The owner may be the sender that is blocked on this very functor.
            throw DocumentWouldBlock(
                "a synchronous document call cannot wait for the document owner from "
                "inside a notification the owner is blocked on; use the asynchronous API");
        }
        if (!onGui) {
            std::lock_guard lock(_mutex);
            if (ownerDispatchRejectedLocked()) {
                throw DocumentWouldBlock(
                    "document execution lane is closing or shutting down");
            }
            if (commandBlocksDispatchLocked(allowWhileCommandActive)) {
                throw DocumentWouldBlock(
                    "document execution lane is busy executing model work; use "
                    "DocumentHandle::trySubmit() instead");
            }
        }

        std::optional<Base::PyGILStateRelease> release;
        if ((releaseGilWhileWaiting || onGui) && Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }

        // std::function requires a copyable target; share the promise and
        // callable so the queued lambda is copy-constructible.
        auto [future, task] = makeDispatchTask(std::forward<Fn>(fn));

        if (!onGui) {
            {
                std::lock_guard lock(_mutex);
                if (ownerDispatchRejectedLocked()) {
                    throw DocumentWouldBlock("document execution lane is closing or shutting down");
                }
                if (commandBlocksDispatchLocked(allowWhileCommandActive)) {
                    throw DocumentWouldBlock(
                        "document execution lane is busy executing model work; use "
                        "DocumentHandle::trySubmit() instead"
                    );
                }
                _dispatchQueue.push_back(std::move(task));
                _workAvailable.notify_one();
            }
            return future.get();
        }

        // --- GUI-thread bounded admission ---
        // Check for stall immediately (no wait).
        if (isWatchdogStalled()) {
            std::string msg = "document execution lane is stalled";
            {
                std::lock_guard lock(_mutex);
                if (_active) {
                    msg += ": active command is ";
                    msg += documentCommandKindName(_active->command.kind);
                    if (!_active->snapshot.diagnostic.empty()) {
                        msg += " (";
                        msg += _active->snapshot.diagnostic;
                        msg += ")";
                    }
                }
            }
            throw DocumentWouldBlock(msg.c_str());
        }

        // Read admission timeout from preferences (default 500 ms).
        const auto admissionTimeout = guiSyncAdmissionTimeout();
        const auto admissionDeadline = std::chrono::steady_clock::now() + admissionTimeout;

        DocumentExecutionLaneGuiDispatchPumpScope pumping;
        constexpr auto serviceSlice = std::chrono::milliseconds(2);
        while (true) {
            {
                std::lock_guard lock(_mutex);
                if (ownerDispatchRejectedLocked()) {
                    throw DocumentWouldBlock(
                        "document execution lane is closing or shutting down");
                }
                if (!commandBlocksDispatchLocked(allowWhileCommandActive)) {
                    _dispatchQueue.push_back(std::move(task));
                    _workAvailable.notify_one();
                    break;
                }
                // Check stall or timeout under the lock so the diagnostic is fresh.
                const bool stalledNow = isWatchdogStalled();
                const bool timedOut = std::chrono::steady_clock::now() >= admissionDeadline;
                if (stalledNow || timedOut) {
                    std::string msg = stalledNow
                        ? "document execution lane is stalled"
                        : "GUI admission timeout: document execution lane is busy";
                    if (_active) {
                        msg += "; active command: ";
                        msg += documentCommandKindName(_active->command.kind);
                        if (!_active->snapshot.diagnostic.empty()) {
                            msg += " (";
                            msg += _active->snapshot.diagnostic;
                            msg += ")";
                        }
                    }
                    throw DocumentWouldBlock(msg.c_str());
                }
            }
            MainThreadSignalConfig::serviceMarshalledTasks(serviceSlice);
        }
        while (future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) {
            MainThreadSignalConfig::serviceMarshalledTasks(serviceSlice);
        }
        return future.get();
    }

    void joinThread();
    /** Release the owner thread without waiting; used when the GUI closes a document. */
    void detachThread();

    void notifyCloseAdmissionReleased() const noexcept;

    [[nodiscard]] static std::shared_ptr<DocumentExecutionLane> find(
        DocumentInstanceId instanceId);

private:
    friend struct DocumentHandle::State;

    /** True when the active command must finish before a dispatch may run. Holds _mutex. */
    [[nodiscard]] bool commandBlocksDispatchLocked(const bool allowWhileCommandActive) const
    {
        return _active && !allowWhileCommandActive
            && documentCommandKindBlocksOwnerDispatch(_active->command.kind);
    }

    /** Holds _mutex. Reject posts during Close, after the document is gone, and after shutdown. */
    [[nodiscard]] bool ownerDispatchRejectedLocked() const
    {
        return _shutdownRequested || !_ownerDocumentAlive || _applicationClosing
            || (_active && _active->command.kind == DocumentCommandKind::Close);
    }

    template<typename Fn>
    static auto makeDispatchTask(
        Fn&& fn
    ) -> std::pair<std::future<std::invoke_result_t<Fn>>, std::function<void(const std::function<void()>&)>>
    {
        using Result = std::invoke_result_t<Fn>;
        auto sharedFn = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::function<void(const std::function<void()>&)> task =
            [promise, sharedFn](const std::function<void()>& finishDispatch) mutable {
                std::exception_ptr failure;
                if constexpr (std::is_void_v<Result>) {
                    try {
                        (*sharedFn)();
                    }
                    catch (...) {
                        failure = std::current_exception();
                    }
                    sharedFn.reset();
                    finishDispatch();
                    if (failure) {
                        promise->set_exception(failure);
                    }
                    else {
                        promise->set_value();
                    }
                }
                else if constexpr (std::is_reference_v<Result>) {
                    std::remove_reference_t<Result>* resultPtr = nullptr;
                    try {
                        resultPtr = std::addressof((*sharedFn)());
                    }
                    catch (...) {
                        failure = std::current_exception();
                    }
                    sharedFn.reset();
                    finishDispatch();
                    if (failure) {
                        promise->set_exception(failure);
                    }
                    else {
                        promise->set_value(*resultPtr);
                    }
                }
                else {
                    std::optional<Result> result;
                    try {
                        result.emplace((*sharedFn)());
                    }
                    catch (...) {
                        failure = std::current_exception();
                    }
                    sharedFn.reset();
                    finishDispatch();
                    if (failure) {
                        promise->set_exception(failure);
                    }
                    else {
                        promise->set_value(std::move(*result));
                    }
                }
            };
        return {std::move(future), std::move(task)};
    }

    /** In-flight command; complete before use in std::optional. */
    struct ActiveCommand
    {
        DocumentCommandId id {0};
        DocumentCommand command;
        DocumentCommandSnapshot snapshot;
        std::optional<DocumentRecomputeId> recomputeId;
        /** True once recomputeAsync/submitLaneTestBlockingRecompute has been called. */
        bool recomputeSubmitted {false};
        std::vector<DocumentRevisionIdentityBinding> crossDocumentReservations;
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
            , recomputeSubmitted(other.recomputeSubmitted)
            , crossDocumentReservations(std::move(other.crossDocumentReservations))
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
                recomputeSubmitted = other.recomputeSubmitted;
                crossDocumentReservations = std::move(other.crossDocumentReservations);
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
    /** Holds _mutex. Caps per-lane terminal snapshots (M4). */
    void rememberTerminalSnapshotLocked(DocumentCommandId id,
                                        const DocumentCommandSnapshot& snapshot);
    void publishActiveSnapshot(const DocumentCommandSnapshot& snapshot);
    void publishActiveSnapshotLocked(const DocumentCommandSnapshot& snapshot);
    void touchWatchdogProgress(ActiveCommand& command);
    void updateWatchdogState(DocumentCommandSnapshot& snapshot);
    [[nodiscard]] DocumentCommandRecomputeObservation makeRecomputeObservation(
        const DocumentRecomputeSnapshot& recompute) const;
    [[nodiscard]] DocumentExecutionBusyRejectionKind busyKind(
        DocumentCommandKind kind) const noexcept;
    void recordBusyRejection(DocumentCommandKind kind);

    /** Read GuiSyncAdmissionTimeoutMs from preferences; default 500 ms. */
    [[nodiscard]] std::chrono::milliseconds guiSyncAdmissionTimeout() const noexcept;

    Document& _document;
    std::shared_ptr<Internal::CollaborationServiceLifetimeGate> _lifetimeGate;
    const DocumentRevisionIdentityBinding _identity;
    std::shared_ptr<DocumentHandle::State> _handleState;

    std::thread _thread;
    std::thread::id _ownerThreadId {};
    mutable std::mutex _mutex;
    mutable std::condition_variable _workAvailable;
    bool _threadReady {false};
    bool _shutdownRequested {false};
    std::string _shutdownReason;
    /** False once Application::closeDocument has destroyed the document. */
    bool _ownerDocumentAlive {true};
    /** True while Application::closeDocument has claimed close admission. */
    bool _applicationClosing {false};
    /** Thread currently inside the Close command's marshalled closeDocument(). */
    std::thread::id _marshalledCloseThread {};

    std::optional<ActiveCommand> _active;
    std::unordered_map<DocumentCommandId, DocumentCommandSnapshot> _terminalSnapshots;
    /** Insertion order for _terminalSnapshots so the map can be capped. */
    std::deque<DocumentCommandId> _terminalOrder;
    DocumentCommandId _nextCommandId {1};

    std::deque<std::function<void(const std::function<void()>&)>> _dispatchQueue;
    std::size_t _activeDispatchCount {0};

    std::atomic<bool> _recoverySnapshotOwnerWorkInFlight {false};

    std::shared_ptr<DocumentExecutionTelemetryCollector> _telemetry;
};

}  // namespace App
