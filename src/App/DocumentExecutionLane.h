// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"
#include "DocumentCommandHandle.h"
#include "DocumentHandle.h"
#include "DocumentRecomputeCoordinator.h"
#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

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
#include <unordered_map>
#include <vector>

namespace App
{

class Document;
class DocumentExecutionTelemetryCollector;

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
     * work. The Python GIL must be released by callers that hold it.
     */
    template<typename Fn>
    auto dispatchToOwner(Fn&& fn) -> std::invoke_result_t<Fn>
    {
        using Result = std::invoke_result_t<Fn>;
        if (isOwnerThread()) {
            return std::forward<Fn>(fn)();
        }

        std::promise<Result> promise;
        auto future = promise.get_future();
        {
            std::lock_guard lock(_mutex);
            _dispatchQueue.push_back([promise = std::move(promise), fn = std::forward<Fn>(fn)]() mutable {
                try {
                    promise.set_value(fn());
                }
                catch (...) {
                    promise.set_exception(std::current_exception());
                }
            });
            _workAvailable.notify_one();
        }
        return future.get();
    }

    void joinThread();

    [[nodiscard]] static std::shared_ptr<DocumentExecutionLane> find(
        DocumentInstanceId instanceId);

private:
    friend struct DocumentHandle::State;

    struct ActiveCommand;

    DocumentExecutionLane(Document& document, DocumentRevisionIdentityBinding identity);

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
    std::condition_variable _workAvailable;
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
