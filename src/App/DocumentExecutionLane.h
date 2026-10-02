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

/**
 * When @p stalled is false, keep waiting cooperatively. When the diagnostic
 * watchdog has marked the lane Stalled, offer whole-process exit instead of
 * in-process thread termination.
 */
[[nodiscard]] UnresponsiveLaneAction recommendedActionWhileLaneBusy(
    bool stalled = false) noexcept;

}  // namespace DocumentExecutionClosePolicy

/** Wake lane idle waiters after collaboration admission for close is released. */
AppExport void notifyDocumentExecutionLaneCloseAdmissionReleased(
    const Document& document) noexcept;

/** True while the GUI thread pumps Qt inside dispatchToOwner's wait loop. */
[[nodiscard]] AppExport bool documentExecutionLaneGuiDispatchPumpActive() noexcept;

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
    /** True when idle or the owner thread is executing an admitted Close command. */
    [[nodiscard]] bool permitsApplicationClose() const noexcept;
    [[nodiscard]] bool shutdownRequested() const noexcept;
    /** Diagnostic-only: true when the watchdog has marked active work Stalled. */
    [[nodiscard]] bool isWatchdogStalled() const noexcept;

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
     * Off the GUI thread this refuses while model work runs unless
     * \p allowWhileCommandActive. On the GUI thread it is the synchronous
     * compatibility path: it waits for running model work to finish and then
     * for \p fn, executing meanwhile only the functors the owner marshals to the
     * GUI thread (MainThreadSignalConfig::serviceMarshalledTasks()), never the
     * Qt event loop. When \p releaseGilWhileWaiting is true, releases the GIL
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
        auto sharedFn = std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        std::function<void()> task = [promise, sharedFn]() {
            try {
                if constexpr (std::is_void_v<Result>) {
                    (*sharedFn)();
                    promise->set_value();
                }
                else {
                    promise->set_value((*sharedFn)());
                }
            }
            catch (...) {
                promise->set_exception(std::current_exception());
            }
        };

        if (!onGui) {
            {
                std::lock_guard lock(_mutex);
                _dispatchQueue.push_back(std::move(task));
                _workAvailable.notify_one();
            }
            return future.get();
        }

        DocumentExecutionLaneGuiDispatchPumpScope pumping;
        constexpr auto serviceSlice = std::chrono::milliseconds(2);
        while (true) {
            {
                std::lock_guard lock(_mutex);
                if (!commandBlocksDispatchLocked(allowWhileCommandActive)) {
                    _dispatchQueue.push_back(std::move(task));
                    _workAvailable.notify_one();
                    break;
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

    /** In-flight command; complete before use in std::optional. */
    struct ActiveCommand
    {
        DocumentCommandId id {0};
        DocumentCommand command;
        DocumentCommandSnapshot snapshot;
        std::optional<DocumentRecomputeId> recomputeId;
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
    void publishActiveSnapshot(const DocumentCommandSnapshot& snapshot);
    void publishActiveSnapshotLocked(const DocumentCommandSnapshot& snapshot);
    void touchWatchdogProgress(ActiveCommand& command);
    void updateWatchdogState(DocumentCommandSnapshot& snapshot);
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

    std::atomic<bool> _recoverySnapshotOwnerWorkInFlight {false};

    std::shared_ptr<DocumentExecutionTelemetryCollector> _telemetry;
};

}  // namespace App
