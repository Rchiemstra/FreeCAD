// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionLane.h"

#include "Application.h"
#include "Document.h"

#include "DocumentCrossDocumentSnapshot.h"
#include "DocumentExecutionTelemetry.h"
#include "DocumentObject.h"
#include "DocumentWouldBlock.h"
#include "Property.h"
#include "RecomputeHandle.h"

#include <Base/Exception.h>
#include <Base/Persistence.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <sstream>
#include <utility>

using namespace std::chrono_literals;

namespace App
{

namespace DocumentExecutionLaneDetail
{
thread_local bool g_guiDispatchPumpActive = false;
}  // namespace DocumentExecutionLaneDetail

bool documentExecutionLaneGuiDispatchPumpActive() noexcept
{
    return DocumentExecutionLaneDetail::g_guiDispatchPumpActive;
}

DocumentExecutionLaneGuiDispatchPumpScope::DocumentExecutionLaneGuiDispatchPumpScope()
{
    DocumentExecutionLaneDetail::g_guiDispatchPumpActive = true;
}

DocumentExecutionLaneGuiDispatchPumpScope::~DocumentExecutionLaneGuiDispatchPumpScope()
{
    DocumentExecutionLaneDetail::g_guiDispatchPumpActive = false;
}

// ---------------------------------------------------------------------------
// M4/M9: Process-level terminal-snapshot archive (LRU, ~64 per document instance)
// ---------------------------------------------------------------------------
namespace
{

struct TerminalArchive
{
    static constexpr std::size_t kLruCap = 64;
    static constexpr std::size_t kClosedDocumentCap = 32;

    struct Entry
    {
        DocumentCommandId commandId {0};
        DocumentCommandSnapshot snapshot;
    };

    mutable std::mutex mutex;
    std::unordered_map<DocumentInstanceId, std::deque<Entry>> perInstance;
    std::deque<DocumentInstanceId> closedOrder;

    void add(DocumentInstanceId instanceId,
             DocumentCommandId commandId,
             const DocumentCommandSnapshot& snapshot)
    {
        std::lock_guard lock(mutex);
        auto& bucket = perInstance[instanceId];
        // Evict oldest entries when over the cap.
        while (bucket.size() >= kLruCap) {
            bucket.pop_front();
        }
        bucket.push_back({commandId, snapshot});
    }

    /** Remember a closed document and drop the oldest closed buckets past the cap. */
    void noteDocumentClosed(DocumentInstanceId instanceId)
    {
        std::lock_guard lock(mutex);
        closedOrder.erase(
            std::remove(closedOrder.begin(), closedOrder.end(), instanceId),
            closedOrder.end());
        closedOrder.push_back(instanceId);
        while (closedOrder.size() > kClosedDocumentCap) {
            const auto oldest = closedOrder.front();
            closedOrder.pop_front();
            perInstance.erase(oldest);
        }
    }

    std::optional<DocumentCommandSnapshot> find(DocumentInstanceId instanceId,
                                                 DocumentCommandId commandId) const
    {
        std::lock_guard lock(mutex);
        const auto it = perInstance.find(instanceId);
        if (it == perInstance.end()) {
            return std::nullopt;
        }
        for (const auto& entry : it->second) {
            if (entry.commandId == commandId) {
                return entry.snapshot;
            }
        }
        return std::nullopt;
    }
};

TerminalArchive& getTerminalArchive()
{
    static TerminalArchive instance;
    return instance;
}

}  // namespace

std::optional<DocumentCommandSnapshot>
findTerminalCommandSnapshot(DocumentInstanceId instanceId,
                            DocumentCommandId commandId) noexcept
{
    try {
        return getTerminalArchive().find(instanceId, commandId);
    }
    catch (...) {
        return std::nullopt;
    }
}

DocumentExecutionClosePolicy::UnresponsiveLaneAction
DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(const bool stalled) noexcept
{
    return stalled ? UnresponsiveLaneAction::RequestProcessExit
                   : UnresponsiveLaneAction::KeepWaiting;
}

std::chrono::milliseconds DocumentExecutionLane::guiSyncAdmissionTimeout() const noexcept
{
    constexpr long kDefaultMs = 500;
    try {
        const long ms = static_cast<long>(
            GetApplication()
                .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Document")
                ->GetInt("GuiSyncAdmissionTimeoutMs", kDefaultMs));
        return std::chrono::milliseconds(ms < 0 ? 0 : ms);
    }
    catch (...) {
        return std::chrono::milliseconds(kDefaultMs);
    }
}

namespace
{

std::mutex g_laneRegistryMutex;
std::unordered_map<DocumentInstanceId, std::weak_ptr<DocumentExecutionLane>> g_laneRegistry;

void registerLane(const std::shared_ptr<DocumentExecutionLane>& lane)
{
    std::lock_guard lock(g_laneRegistryMutex);
    g_laneRegistry[lane->identity().documentInstanceId] = lane;
}

void unregisterLane(DocumentInstanceId instanceId)
{
    std::lock_guard lock(g_laneRegistryMutex);
    g_laneRegistry.erase(instanceId);
}

bool recomputeCommandsCoalesce(const DocumentCommand& left, const DocumentCommand& right)
{
    if (!left.recompute || !right.recompute) {
        return false;
    }
    return left.recompute->coalescingKey == right.recompute->coalescingKey
        && left.recompute->options == right.recompute->options;
}

bool commandRequiresBusyWhileActive(DocumentCommandKind kind) noexcept
{
    // Same predicate as owner-dispatch blocking (including active Recompute).
    return documentCommandKindBlocksOwnerDispatch(kind);
}

constexpr std::string_view laneTestBlockingCoalescingPrefix = "lane-stall:";

std::string commandExceptionDiagnostic(const Base::Exception& exception)
{
    return exception.what();
}

std::string commandExceptionDiagnostic(const std::exception& exception)
{
    return exception.what();
}

DocumentRecomputeId submitLaneTestBlockingRecompute(Document& document,
                                                      std::string_view token)
{
    DocumentRecomputeFeatureRequest feature;
    feature.featureId = "lane-stall-probe";
    feature.operationId = "document-execution-lane-stall-test";
    feature.intent.operationType = "FreeCAD.Tests.DocumentExecutionLaneBlockingRecompute";
    feature.intent.arguments = {{"token", std::string(token)}};
    feature.provenance = "DocumentExecutionLane responsiveness test";

    DocumentRecomputeRequest request;
    request.coalescingKey = std::string(laneTestBlockingCoalescingPrefix) + std::string(token);
    request.features.push_back(std::move(feature));
    return document.recomputeCoordinator().submit(std::move(request));
}

DocumentCommandState mapRecomputeState(DocumentRecomputeState state)
{
    switch (state) {
        case DocumentRecomputeState::Running:
            return DocumentCommandState::Running;
        case DocumentRecomputeState::Cancelling:
            return DocumentCommandState::Cancelling;
        case DocumentRecomputeState::Completed:
            return DocumentCommandState::Completed;
        case DocumentRecomputeState::PartialFailure:
            return DocumentCommandState::Failed;
        case DocumentRecomputeState::Cancelled:
            return DocumentCommandState::Cancelled;
    }
    return DocumentCommandState::Failed;
}

}  // namespace

std::shared_ptr<DocumentExecutionLane> DocumentExecutionLane::create(
    Document& document,
    DocumentRevisionIdentityBinding identity)
{
    auto lane = std::shared_ptr<DocumentExecutionLane>(
        new DocumentExecutionLane(document, identity));
    lane->_handleState->lane = lane;
    registerLane(lane);
    lane->startOwnerThread();
    return lane;
}

std::shared_ptr<DocumentExecutionLane> DocumentExecutionLane::find(DocumentInstanceId instanceId)
{
    std::lock_guard lock(g_laneRegistryMutex);
    const auto found = g_laneRegistry.find(instanceId);
    if (found == g_laneRegistry.end()) {
        return nullptr;
    }
    return found->second.lock();
}

DocumentExecutionLane::DocumentExecutionLane(Document& document,
                                             DocumentRevisionIdentityBinding identity)
    : _document(document)
    , _identity(identity)
    , _handleState(std::make_shared<DocumentHandle::State>())
    , _telemetry(DocumentExecutionTelemetry::instance().document(
          identity.documentInstanceId,
          identity.lifecycleEpoch))
{
    _handleState->identity = identity;
}

void DocumentExecutionLane::startOwnerThread()
{
    _thread = std::thread([this] {
        _ownerThreadId = std::this_thread::get_id();
        _document.bindCollaborationOwnerThread(_ownerThreadId);
        {
            std::lock_guard lock(_mutex);
            _threadReady = true;
        }
        _workAvailable.notify_all();
        threadMain();
    });

    std::unique_lock lock(_mutex);
    _workAvailable.wait(lock, [this] { return _threadReady; });
}

DocumentExecutionLane::~DocumentExecutionLane()
{
    {
        std::lock_guard lock(_mutex);
        _ownerDocumentAlive = false;
    }
    requestShutdown("document execution lane destroyed");
    joinThread();
    unregisterLane(_identity.documentInstanceId);
    getTerminalArchive().noteDocumentClosed(_identity.documentInstanceId);
    DocumentExecutionTelemetry::instance().removeDocument(_identity.documentInstanceId);
}

Document& DocumentExecutionLane::document() noexcept
{
    return _document;
}

const Document& DocumentExecutionLane::document() const noexcept
{
    return _document;
}

DocumentRevisionIdentityBinding DocumentExecutionLane::identity() const noexcept
{
    return _identity;
}

DocumentHandle DocumentExecutionLane::handle() const noexcept
{
    return DocumentHandle(_handleState);
}

std::thread::id DocumentExecutionLane::ownerThreadId() const noexcept
{
    return _ownerThreadId;
}

bool DocumentExecutionLane::isOwnerThread() const noexcept
{
    return std::this_thread::get_id() == _ownerThreadId;
}

void notifyDocumentExecutionLaneCloseAdmissionReleased(const Document& document) noexcept
{
    if (const auto* lane = document.executionLane()) {
        lane->notifyCloseAdmissionReleased();
    }
}

void DocumentExecutionLane::notifyCloseAdmissionReleased() const noexcept
{
    std::lock_guard lock(_mutex);
    _workAvailable.notify_all();
}

bool DocumentExecutionLane::isIdle() const noexcept
{
    if (_recoverySnapshotOwnerWorkInFlight.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard lock(_mutex);
    return !_active;
}

void DocumentExecutionLane::beginRecoverySnapshotOwnerWork() noexcept
{
    _recoverySnapshotOwnerWorkInFlight.store(true, std::memory_order_release);
}

void DocumentExecutionLane::endRecoverySnapshotOwnerWork() noexcept
{
    _recoverySnapshotOwnerWorkInFlight.store(false, std::memory_order_release);
    _workAvailable.notify_all();
}

bool DocumentExecutionLane::permitsApplicationClose() const noexcept
{
    std::lock_guard lock(_mutex);
    if (_active) {
        if (_active->command.kind == DocumentCommandKind::Close
            && (isOwnerThread() || std::this_thread::get_id() == _marshalledCloseThread)) {
            // Headless close runs inline on the owner thread. GUI close is
            // marshalled by the active Close command; admit only that caller.
            return true;
        }
        return false;
    }
    // Application::closeDocument publishes Closing and drains outstanding
    // collaboration admissions; do not reject here while they are still held.
    return true;
}

void DocumentExecutionLane::beginMarshalledCloseAdmission() noexcept
{
    std::lock_guard lock(_mutex);
    _marshalledCloseThread = std::this_thread::get_id();
}

void DocumentExecutionLane::endMarshalledCloseAdmission() noexcept
{
    std::lock_guard lock(_mutex);
    if (_marshalledCloseThread == std::this_thread::get_id()) {
        _marshalledCloseThread = {};
    }
}

bool DocumentExecutionLane::shutdownRequested() const noexcept
{
    std::lock_guard lock(_mutex);
    return _shutdownRequested;
}

bool DocumentExecutionLane::isWatchdogStalled() const noexcept
{
    if (!_telemetry) {
        return false;
    }
    return _telemetry->snapshot().watchdog.stalled();
}

DocumentCommandSubmitOutcome DocumentExecutionLane::trySubmit(DocumentCommand command)
{
    DocumentCommandSubmitOutcome outcome;
    outcome.result = DocumentCommandSubmitResult::Closed;
    outcome.diagnostic = "document execution lane is closed";

    if (command.document != _identity) {
        outcome.result = DocumentCommandSubmitResult::Conflict;
        outcome.diagnostic = "command document identity does not match lane";
        return outcome;
    }

    std::lock_guard lock(_mutex);
    if (_shutdownRequested) {
        outcome.result = DocumentCommandSubmitResult::Closed;
        outcome.diagnostic = _shutdownReason.empty() ? "document execution lane is closed"
                                                     : _shutdownReason;
        return outcome;
    }

    const auto rejectBusyCommandKinds = [&](DocumentCommandKind kind) -> bool {
        if (kind == DocumentCommandKind::Recompute || commandRequiresBusyWhileActive(kind)) {
            recordBusyRejection(kind);
            outcome.result = DocumentCommandSubmitResult::Busy;
            outcome.diagnostic = "document execution lane is busy";
            return true;
        }
        return false;
    };

    if (_recoverySnapshotOwnerWorkInFlight.load(std::memory_order_acquire)) {
        if (rejectBusyCommandKinds(command.kind)) {
            return outcome;
        }
    }

    if (_active) {
        if (command.kind == DocumentCommandKind::Recompute
            && _active->command.kind == DocumentCommandKind::Recompute
            && recomputeCommandsCoalesce(command, _active->command)) {
            outcome.result = DocumentCommandSubmitResult::Accepted;
            outcome.commandId = _active->id;
            outcome.diagnostic = "joined active identical recompute";
            return outcome;
        }

        if (rejectBusyCommandKinds(command.kind)) {
            return outcome;
        }
    }

    switch (command.kind) {
        case DocumentCommandKind::Recompute:
            if (!command.recompute) {
                outcome.result = DocumentCommandSubmitResult::Unsupported;
                outcome.diagnostic = "recompute command is missing payload";
                return outcome;
            }
            break;
        case DocumentCommandKind::Edit:
            if (!command.edit) {
                outcome.result = DocumentCommandSubmitResult::Unsupported;
                outcome.diagnostic = "edit command is missing payload";
                return outcome;
            }
            break;
        case DocumentCommandKind::Undo:
        case DocumentCommandKind::Redo:
        case DocumentCommandKind::Save:
        case DocumentCommandKind::Close:
            break;
        default:
            outcome.result = DocumentCommandSubmitResult::Unsupported;
            outcome.diagnostic = "unsupported document command kind";
            return outcome;
    }

    ActiveCommand active;
    active.id = _nextCommandId++;
    active.command = std::move(command);
    active.snapshot.id = active.id;
    active.snapshot.kind = active.command.kind;
    active.snapshot.document = _identity;
    active.snapshot.state = DocumentCommandState::Running;
    active.snapshot.progress = 0.0;
    active.snapshot.diagnostic = "accepted";
    active.lastProgressEpochMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    _active = std::move(active);
    if (_active->command.kind == DocumentCommandKind::Recompute) {
        DocumentRecomputeId admissionId {0};
        try {
            admissionId = _document.recomputeCoordinator().reserveAdmissionId();
        }
        catch (const std::exception& exception) {
            _active.reset();
            outcome.result = DocumentCommandSubmitResult::Unsupported;
            outcome.diagnostic = exception.what();
            return outcome;
        }
        _active->snapshot.recompute.emplace();
        _active->snapshot.recompute->id =
            static_cast<DocumentCommandRecomputeId>(admissionId);
        _active->snapshot.recompute->state = DocumentCommandRecomputeState::Running;
        _active->snapshot.recompute->diagnostic =
            "admitted; awaiting owner-thread coordinator submit";
        // M3: set recomputeId under the mutex so recomputeStatus can find it immediately.
        _active->recomputeId = static_cast<DocumentRecomputeId>(admissionId);
    }
    publishActiveSnapshotLocked(_active->snapshot);
    if (_telemetry) {
        _telemetry->beginWatchdog(documentCommandKindName(_active->command.kind));
    }

    outcome.result = DocumentCommandSubmitResult::Accepted;
    outcome.commandId = _active->id;
    _workAvailable.notify_one();
    return outcome;
}

DocumentCommandSnapshot DocumentExecutionLane::commandStatus(DocumentCommandId id) const
{
    std::lock_guard lock(_mutex);
    if (_active && _active->id == id) {
        auto snapshot = _active->snapshot;
        // Reflect watchdog stall on observation even when the owner is blocked
        // inside feature execute and has not yet pumped updateWatchdogState.
        if (snapshot.state == DocumentCommandState::Running && _telemetry
            && _telemetry->snapshot().watchdog.stalled()) {
            snapshot.state = DocumentCommandState::Stalled;
            const auto diagnostic = _telemetry->snapshot().watchdog.diagnostic;
            snapshot.diagnostic = diagnostic.empty()
                ? "document execution stalled without progress"
                : diagnostic;
        }
        return snapshot;
    }
    const auto found = _terminalSnapshots.find(id);
    if (found != _terminalSnapshots.end()) {
        return found->second;
    }

    DocumentCommandSnapshot unavailable;
    unavailable.id = id;
    unavailable.document = _identity;
    unavailable.state = DocumentCommandState::Failed;
    unavailable.diagnostic = "command is not known to the execution lane";
    return unavailable;
}

std::optional<DocumentRecomputeSnapshot> DocumentExecutionLane::recomputeStatus(
    DocumentRecomputeId id) const
{
    {
        std::lock_guard lock(_mutex);
        // Check by recomputeId (set at trySubmit under mutex, updated after submission).
        if (_active && _active->recomputeId == id && _active->snapshot.recompute) {
            return recomputeSnapshotFromObservation(*_active->snapshot.recompute);
        }
        // H5: also match by the admission id stored in the snapshot (covers the window
        // between trySubmit and coordinator submission; with M3 they are always equal).
        if (_active && _active->snapshot.recompute
            && static_cast<DocumentRecomputeId>(_active->snapshot.recompute->id) == id) {
            return recomputeSnapshotFromObservation(*_active->snapshot.recompute);
        }
        for (const auto& [commandId, snapshot] : _terminalSnapshots) {
            static_cast<void>(commandId);
            if (snapshot.recompute && snapshot.recompute->id == id) {
                return recomputeSnapshotFromObservation(*snapshot.recompute);
            }
        }
    }
    return _document.recomputeCoordinator().status(id);
}

bool DocumentExecutionLane::cancelCommand(DocumentCommandId id, std::string reason)
{
    std::optional<DocumentRecomputeId> recomputeId;
    {
        std::lock_guard lock(_mutex);
        if (!_active || _active->id != id) {
            return false;
        }
        _active->cancelRequested.store(true, std::memory_order_release);
        _active->snapshot.state = DocumentCommandState::Cancelling;
        if (!reason.empty()) {
            _active->snapshot.diagnostic = reason;
        }
        recomputeId = _active->recomputeId;
        publishActiveSnapshotLocked(_active->snapshot);
    }
    // L6: forward the reason. cancel() must not take the coordinator operation
    // lock off the owner thread (N7); the coordinator records the flag under
    // its state lock and the owner applies prepared-edit cancellation.
    if (recomputeId) {
        static_cast<void>(_document.recomputeCoordinator().cancel(
            *recomputeId, reason.empty() ? "command cancelled" : reason));
    }
    _workAvailable.notify_one();
    return true;
}

void DocumentExecutionLane::requestShutdown(std::string reason)
{
    {
        std::lock_guard lock(_mutex);
        if (_shutdownRequested) {
            return;
        }
        _shutdownRequested = true;
        _shutdownReason = std::move(reason);
        if (_active) {
            _active->cancelRequested.store(true, std::memory_order_release);
            _active->snapshot.state = DocumentCommandState::Cancelling;
        }
    }
    _workAvailable.notify_all();
}

void DocumentExecutionLane::joinThread()
{
    if (_thread.joinable()) {
        if (_thread.get_id() != std::this_thread::get_id()) {
            _thread.join();
        }
        else {
            _thread.detach();
        }
    }
}

void DocumentExecutionLane::detachThread()
{
    if (_thread.joinable() && _thread.get_id() != std::this_thread::get_id()) {
        _thread.detach();
    }
}

void DocumentExecutionLane::threadMain()
{
    const auto selfPin = shared_from_this();
    static_cast<void>(selfPin);
    while (true) {
        {
            std::unique_lock lock(_mutex);
            _workAvailable.wait(lock, [this] {
                return _shutdownRequested || _active.has_value() || !_dispatchQueue.empty();
            });
        }

        drainDispatchQueue();

        std::unique_lock lock(_mutex);
        if (_active) {
            lock.unlock();
            try {
                executeActiveCommand();
            }
            catch (const Base::Exception& exception) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      commandExceptionDiagnostic(exception));
            }
            catch (const std::exception& exception) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      commandExceptionDiagnostic(exception));
            }
            catch (...) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      "document command failed with an unknown exception");
            }
            continue;
        }

        if (_shutdownRequested) {
            break;
        }
    }
    // M5: drain any remaining queued dispatches so waiting threads are not
    // stranded with broken_promise or forever-blocked future::get().
    drainDispatchQueue();
}

void DocumentExecutionLane::drainDispatchQueue()
{
    std::vector<std::function<void()>> queue;
    bool runTasks = false;
    {
        std::lock_guard lock(_mutex);
        queue.swap(_dispatchQueue);
        runTasks = _ownerDocumentAlive;
    }
    if (!runTasks) {
        // The document is already gone. Drop the tasks so their promises break
        // instead of running against a deleted Document (M5 / N3).
        return;
    }
    for (auto& task : queue) {
        task();
    }
}

void DocumentExecutionLane::executeActiveCommand()
{
    const auto selfPin = shared_from_this();
    static_cast<void>(selfPin);
    if (!_active) {
        return;
    }

    if (_active->command.kind != DocumentCommandKind::Recompute) {
        if (_active->cancelRequested.load(std::memory_order_acquire)) {
            completeActiveCommand(DocumentCommandState::Cancelled, "command cancelled");
            return;
        }

        if (_active->command.kind == DocumentCommandKind::Close) {
            const std::string documentName = _document.getName();
            // Run anything already queued while the document is still alive.
            // postToOwner rejects new work for the rest of this Close.
            drainDispatchQueue();
            DocumentCommandState finalState = DocumentCommandState::Failed;
            std::string finalDiagnostic = "close failed";
            // H2: Marshal Application::closeDocument to the GUI thread via
            // MainThreadSignalConfig::invoke so DocMap/_pActiveDoc stay on the GUI thread.
            // N1: admit that marshalled call, and shut the lane down only if it succeeds.
            bool closedResult = false;
            bool closeCallDone = false;
            std::string closeException;
            MainThreadSignalConfig::invoke(
                [this, &documentName, &closedResult, &closeCallDone, &closeException]() {
                    struct Admission
                    {
                        DocumentExecutionLane& lane;
                        explicit Admission(DocumentExecutionLane& lane)
                            : lane(lane)
                        {
                            lane.beginMarshalledCloseAdmission();
                        }
                        ~Admission()
                        {
                            lane.endMarshalledCloseAdmission();
                        }
                    } admission {*this};
                    try {
                        closedResult = GetApplication().closeDocument(documentName.c_str());
                    }
                    catch (const Base::Exception& ex) {
                        closeException = ex.what();
                    }
                    catch (const std::exception& ex) {
                        closeException = ex.what();
                    }
                    catch (...) {
                        closeException = "close failed with an unknown exception";
                    }
                    closeCallDone = true;
                },
                /*blocking=*/true);
            if (!closeException.empty()) {
                finalDiagnostic = closeException;
            }
            else {
                finalState = closedResult ? DocumentCommandState::Completed
                                          : DocumentCommandState::Failed;
                finalDiagnostic = closedResult ? "close completed" : "close failed";
            }
            const bool closedOk = closeException.empty() && closedResult;
            if (closedOk) {
                std::lock_guard lock(_mutex);
                _ownerDocumentAlive = false;
            }
            // Finish the admitted Close command before the lane thread exits; do
            // not touch _document after closeDocument() returns.
            completeActiveCommand(finalState, finalDiagnostic);
            if (closedOk) {
                requestShutdown("document closed");
            }
            return;
        }

        try {
            const bool succeeded = executeInstantCommand(*_active);
            completeActiveCommand(succeeded ? DocumentCommandState::Completed
                                            : DocumentCommandState::Failed,
                                  succeeded ? "completed" : "command failed");
        }
        catch (const Base::Exception& exception) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  commandExceptionDiagnostic(exception));
        }
        catch (const std::exception& exception) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  commandExceptionDiagnostic(exception));
        }
        catch (...) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  "command failed with an unknown exception");
        }
        return;
    }

    executeActiveRecompute();
}

void DocumentExecutionLane::executeActiveRecompute()
{
    while (true) {
        drainDispatchQueue();

        {
            std::lock_guard lock(_mutex);
            if (!_active) {
                return;
            }
        }

        if (!_active->recomputeSubmitted) {
            const auto& coalescingKey = _active->command.recompute
                ? _active->command.recompute->coalescingKey
                : std::string {};
            try {
                if (coalescingKey.starts_with(laneTestBlockingCoalescingPrefix)) {
                    const auto submittedId = submitLaneTestBlockingRecompute(
                        _document,
                        coalescingKey.substr(laneTestBlockingCoalescingPrefix.size()));
                    std::lock_guard lock(_mutex);
                    if (_active) {
                        _active->recomputeId = submittedId;
                        _active->recomputeSubmitted = true;
                    }
                }
                else {
                    std::vector<DocumentObject*> objects;
                    if (_active->command.recompute) {
                        objects.reserve(_active->command.recompute->featureIds.size());
                        for (const auto& featureId : _active->command.recompute->featureIds) {
                            if (auto* object = _document.getObject(featureId.c_str())) {
                                objects.push_back(object);
                            }
                        }
                    }

                    // Cross-document XLink inputs: reserve in stable ID order and
                    // capture revision-bound snapshots. Undeclared live references
                    // are rejected as Unsupported before recompute starts.
                    {
                        std::vector<DocumentRevisionIdentityBinding> foreign;
                        std::vector<DocumentRevisionKey> structureKeys {
                            DocumentRevisionKey::documentStructure()};
                        for (auto* object : objects) {
                            if (!object) {
                                continue;
                            }
                            for (auto* linked :
                                 object->getOutList(DocumentObject::OutListNoHidden)) {
                                if (!linked || !linked->getDocument()
                                    || linked->getDocument() == &_document) {
                                    continue;
                                }
                                if (auto identity =
                                        linked->getDocument()
                                            ->collaborationRevisions()
                                            .documentIdentity()) {
                                    foreign.push_back(*identity);
                                    static_cast<void>(captureCrossDocumentSnapshot(
                                        *linked->getDocument(), structureKeys));
                                }
                            }
                        }
                        if (!foreign.empty()) {
                            const bool declaresSnapshots =
                                _active->command.recompute
                                && _active->command.recompute->declaresCrossDocumentSnapshots;
                            const auto dependencyKind = declaresSnapshots
                                ? DocumentCrossDocumentDependencyKind::DeclaredSnapshot
                                : DocumentCrossDocumentDependencyKind::UndeclaredLiveReference;
                            const auto reservation =
                                tryReserveDocumentsForCrossDocumentCommand(
                                    foreign,
                                    dependencyKind);
                            if (reservation.result
                                == DocumentCrossDocumentReservationResult::Unsupported) {
                                completeActiveCommand(
                                    DocumentCommandState::Failed,
                                    reservation.diagnostic.empty()
                                        ? "cross-document live references are Unsupported"
                                        : reservation.diagnostic);
                                return;
                            }
                            if (reservation.result
                                == DocumentCrossDocumentReservationResult::Busy
                                || reservation.result
                                    == DocumentCrossDocumentReservationResult::Conflict) {
                                completeActiveCommand(
                                    DocumentCommandState::Failed,
                                    reservation.diagnostic.empty()
                                        ? "cross-document reservation failed"
                                        : reservation.diagnostic);
                                return;
                            }
                            if (reservation.result
                                == DocumentCrossDocumentReservationResult::Reserved) {
                                std::lock_guard lock(_mutex);
                                if (_active) {
                                    _active->crossDocumentReservations =
                                        reservation.reservedInOrder;
                                }
                            }
                        }
                    }

                    const bool force = _active->command.recompute
                        && _active->command.recompute->force;
                    const int options = _active->command.recompute
                        ? _active->command.recompute->options
                        : 0;
                    DocumentRecomputeId admissionId {0};
                    if (_active->snapshot.recompute && _active->snapshot.recompute->id != 0) {
                        admissionId = static_cast<DocumentRecomputeId>(
                            _active->snapshot.recompute->id);
                    }
                    auto handle = _document.recomputeAsync(
                        objects, force, options, RecomputeVenue::OwnerThread, admissionId);
                    const auto submittedId = handle->id();
                    std::lock_guard lock(_mutex);
                    if (_active) {
                        _active->recomputeId = submittedId;
                        _active->recomputeSubmitted = true;
                    }
                }
                touchWatchdogProgress(*_active);
            }
            catch (const Base::Exception& exception) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      commandExceptionDiagnostic(exception));
                return;
            }
            catch (const std::exception& exception) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      commandExceptionDiagnostic(exception));
                return;
            }
            catch (...) {
                completeActiveCommand(DocumentCommandState::Failed,
                                      "recompute submission failed with an unknown exception");
                return;
            }
        }

        try {
            pumpActiveRecompute(*_active);
        }
        catch (const Base::Exception& exception) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  commandExceptionDiagnostic(exception));
            return;
        }
        catch (const std::exception& exception) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  commandExceptionDiagnostic(exception));
            return;
        }
        catch (...) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  "recompute polling failed with an unknown exception");
            return;
        }

        if (!_active || !_active->recomputeId || !_active->recomputeSubmitted) {
            return;
        }

        const auto recomputeSnapshot =
            _document.recomputeCoordinator().status(_active->recomputeId.value());
        if (!recomputeSnapshot) {
            completeActiveCommand(DocumentCommandState::Failed,
                                  "recompute result is unavailable");
            return;
        }

        if (recomputeSnapshot->terminal()) {
            if (recomputeSnapshot->state == DocumentRecomputeState::Completed
                && _document.recomputeCoordinator().claimPresentationFinalization(
                    _active->recomputeId.value())) {
                try {
                    _document.finalizeDetachedRecompute(*recomputeSnapshot);
                }
                catch (const Base::Exception& exception) {
                    completeActiveCommand(DocumentCommandState::Failed,
                                          commandExceptionDiagnostic(exception));
                    return;
                }
                catch (const std::exception& exception) {
                    completeActiveCommand(DocumentCommandState::Failed,
                                          commandExceptionDiagnostic(exception));
                    return;
                }
                catch (...) {
                    completeActiveCommand(DocumentCommandState::Failed,
                                          "presentation finalization failed with an unknown exception");
                    return;
                }
            }
            const auto commandState = mapRecomputeState(recomputeSnapshot->state);
            completeActiveCommand(commandState, recomputeSnapshot->diagnostic);
            return;
        }

        if (_shutdownRequested) {
            return;
        }

        drainDispatchQueue();
        std::this_thread::sleep_for(1ms);
    }
}

bool DocumentExecutionLane::executeInstantCommand(ActiveCommand& command)
{
    switch (command.command.kind) {
        case DocumentCommandKind::Undo: {
            const int steps = command.command.transaction ? command.command.transaction->steps : 1;
            if (steps <= 0) {
                return false;
            }
            for (int step = 0; step < steps; ++step) {
                if (!_document.undo()) {
                    return false;
                }
            }
            return true;
        }
        case DocumentCommandKind::Redo: {
            const int steps = command.command.transaction ? command.command.transaction->steps : 1;
            if (steps <= 0) {
                return false;
            }
            for (int step = 0; step < steps; ++step) {
                if (!_document.redo()) {
                    return false;
                }
            }
            return true;
        }
        case DocumentCommandKind::Save:
            if (command.command.save && command.command.save->saveAs
                && !command.command.save->targetPath.empty()) {
                return _document
                    .saveAsWithOutcome(command.command.save->targetPath.c_str(),
                                       command.command.save->overwrite,
                                       command.command.save->expectedDestinationSha256)
                    .succeeded();
            }
            // M10: Canonical save with no file name fails with a clear diagnostic.
            if (!command.command.save || !command.command.save->saveAs) {
                const char* fn = _document.FileName.getValue();
                if (!fn || *fn == '\0') {
                    throw Base::ValueError(
                        "document has no file name; use Save As");
                }
            }
            return _document.save();
        case DocumentCommandKind::Close:
            return false;
        case DocumentCommandKind::Edit: {
            if (!command.command.edit) {
                return false;
            }

            // M2: Resolve stable object identities once before opening a transaction.
            std::unordered_map<std::string, DocumentObject*> resolvedObjects;
            for (const auto& propertyValue : command.command.edit->propertyValues) {
                if (!propertyValue.stableObjectIdentity.empty()
                    && resolvedObjects.count(propertyValue.stableObjectIdentity) == 0) {
                    DocumentObject* object = nullptr;
                    for (auto* candidate : _document.getObjects()) {
                        // Do not swallow lookup exceptions (M2).
                        if (_document.collaborationObjectIdentity(*candidate)
                            == propertyValue.stableObjectIdentity) {
                            object = candidate;
                            break;
                        }
                    }
                    resolvedObjects[propertyValue.stableObjectIdentity] = object;
                }
            }

            // Open a real undo transaction (not only a booking) so property-editor
            // edits show up in UndoNames. Name it "Edit"; operationId is an internal
            // token and must not be the Undo menu label (M2).
            const int transactionId = _document._openTransaction("Edit");
            if (transactionId == 0) {
                return false;
            }
            bool transactionCommitted = false;
            struct TxGuard
            {
                Document& doc;
                bool& committed;
                ~TxGuard()
                {
                    if (!committed) {
                        try {
                            doc._abortTransaction();
                        }
                        catch (...) {
                        }
                    }
                }
            } txGuard {_document, transactionCommitted};

            for (const auto& propertyValue : command.command.edit->propertyValues) {
                App::Property* property = nullptr;
                if (propertyValue.stableObjectIdentity.empty()) {
                    property = _document.getPropertyByName(propertyValue.propertyName.c_str());
                }
                else {
                    auto* object = resolvedObjects[propertyValue.stableObjectIdentity];
                    if (!object) {
                        return false;
                    }
                    property = object->getPropertyByName(propertyValue.propertyName.c_str());
                }
                if (!property) {
                    return false;
                }
                std::unique_ptr<Property> copied(
                    static_cast<Property*>(property->getTypeId().createInstance()));
                if (!copied) {
                    return false;
                }
                std::istringstream stream(propertyValue.copiedValue);
                copied->restoreFromStream(stream);
                property->Paste(*copied);
            }
            if (!_document._commitTransaction(false, true)) {
                return false;
            }
            transactionCommitted = true;
            return true;
        }
        default:
            return false;
    }
}

void DocumentExecutionLane::pumpActiveRecompute(ActiveCommand& command)
{
    if (!command.recomputeSubmitted || !command.recomputeId) {
        return;
    }

    const auto previousProgress = command.snapshot.progress;
    const auto previousCompleted = command.snapshot.recompute
        ? command.snapshot.recompute->completedFeatures
        : std::size_t {0};
    const auto previousRecomputeState = command.snapshot.recompute
        ? command.snapshot.recompute->state
        : DocumentCommandRecomputeState::Running;

    if (command.cancelRequested.load(std::memory_order_acquire)) {
        static_cast<void>(_document.recomputeCoordinator().cancel(
            *command.recomputeId,
            "document command cancelled"));
    }

    static_cast<void>(_document.recomputeCoordinator().poll(*command.recomputeId));
    const auto recomputeSnapshot = _document.recomputeCoordinator().status(*command.recomputeId);
    if (!recomputeSnapshot) {
        return;
    }

    // commandStatus() copies command.snapshot from other threads under _mutex,
    // so update a local copy and publish it under the lock.
    auto snapshot = command.snapshot;
    snapshot.state = mapRecomputeState(recomputeSnapshot->state);
    snapshot.progress = recomputeSnapshot->progress;
    snapshot.diagnostic = recomputeSnapshot->diagnostic;
    snapshot.recompute = makeRecomputeObservation(*recomputeSnapshot);

    const bool progressChanged = previousProgress != snapshot.progress
        || (snapshot.recompute && previousCompleted != snapshot.recompute->completedFeatures)
        || (snapshot.recompute && previousRecomputeState != snapshot.recompute->state);
    if (progressChanged) {
        touchWatchdogProgress(command);
    }
    updateWatchdogState(snapshot);
    publishActiveSnapshot(snapshot);

    if (_telemetry) {
        DocumentExecutionProgressSnapshot progress;
        progress.active = !recomputeSnapshot->terminal();
        progress.fraction = recomputeSnapshot->progress;
        progress.phase = documentRecomputeStateName(recomputeSnapshot->state);
        progress.operationId = documentCommandKindName(command.command.kind);
        progress.lastProgressEpochMilliseconds = command.lastProgressEpochMilliseconds;
        _telemetry->updateProgress(progress);
    }
}

void DocumentExecutionLane::completeActiveCommand(DocumentCommandState state,
                                                    std::string diagnostic)
{
    DocumentCommandSnapshot completedSnapshot;
    DocumentCommandId completedCommandId = 0;
    DocumentInstanceId instanceId = _identity.documentInstanceId;
    {
        std::lock_guard lock(_mutex);
        if (!_active) {
            return;
        }

        if (!_active->crossDocumentReservations.empty()) {
            releaseCrossDocumentReservations(_active->crossDocumentReservations);
            _active->crossDocumentReservations.clear();
        }

        _active->snapshot.state = state;
        if (!diagnostic.empty()) {
            _active->snapshot.diagnostic = std::move(diagnostic);
        }
        if (_telemetry) {
            _telemetry->endWatchdog();
            _telemetry->clearProgress();
        }
        completedSnapshot = _active->snapshot;
        completedCommandId = _active->id;
        rememberTerminalSnapshotLocked(_active->id, _active->snapshot);
        _active.reset();
        _workAvailable.notify_all();
    }
    // M4/M9: publish to the process-level terminal archive so
    // DocumentCommandHandle::status() can report the real outcome even after
    // the lane is torn down (e.g. successful Close stays Completed/Close/"close completed").
    getTerminalArchive().add(instanceId, completedCommandId, completedSnapshot);
}

void DocumentExecutionLane::publishActiveSnapshotLocked(
    const DocumentCommandSnapshot& snapshot)
{
    if (!_active || _active->id != snapshot.id) {
        return;
    }
    _active->snapshot = snapshot;
}

void DocumentExecutionLane::publishActiveSnapshot(const DocumentCommandSnapshot& snapshot)
{
    std::lock_guard lock(_mutex);
    publishActiveSnapshotLocked(snapshot);
}

void DocumentExecutionLane::touchWatchdogProgress(ActiveCommand& command)
{
    const auto epoch = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    {
        std::lock_guard lock(_mutex);
        command.lastProgressEpochMilliseconds = epoch;
    }
    if (_telemetry) {
        _telemetry->touchWatchdogProgress();
    }
}

void DocumentExecutionLane::rememberTerminalSnapshotLocked(
    DocumentCommandId id,
    const DocumentCommandSnapshot& snapshot)
{
    constexpr std::size_t kCap = 64;
    const auto existing = _terminalSnapshots.find(id);
    if (existing != _terminalSnapshots.end()) {
        existing->second = snapshot;
        return;
    }
    _terminalOrder.push_back(id);
    _terminalSnapshots.emplace(id, snapshot);
    while (_terminalOrder.size() > kCap) {
        const auto oldest = _terminalOrder.front();
        _terminalOrder.pop_front();
        _terminalSnapshots.erase(oldest);
    }
}

void DocumentExecutionLane::updateWatchdogState(DocumentCommandSnapshot& snapshot)
{
    if (!_telemetry) {
        return;
    }
    const auto telemetrySnapshot = _telemetry->snapshot();
    if (!telemetrySnapshot.watchdog.stalled()) {
        return;
    }
    if (snapshot.state == DocumentCommandState::Running) {
        snapshot.state = DocumentCommandState::Stalled;
        snapshot.diagnostic = telemetrySnapshot.watchdog.diagnostic.empty()
            ? "document execution stalled without progress"
            : telemetrySnapshot.watchdog.diagnostic;
    }
}

DocumentRecomputeSnapshot DocumentExecutionLane::recomputeSnapshotFromObservation(
    const DocumentCommandRecomputeObservation& observation) const
{
    DocumentRecomputeSnapshot snapshot;
    snapshot.id = observation.id;
    snapshot.state = static_cast<DocumentRecomputeState>(static_cast<int>(observation.state));
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

DocumentCommandRecomputeObservation DocumentExecutionLane::makeRecomputeObservation(
    const DocumentRecomputeSnapshot& recompute) const
{
    DocumentCommandRecomputeObservation observation;
    observation.id = recompute.id;
    observation.state = static_cast<DocumentCommandRecomputeState>(
        static_cast<int>(recompute.state));
    observation.completedFeatures = recompute.completedFeatures;
    observation.failedFeatures = recompute.failedFeatures;
    observation.totalFeatures = recompute.totalFeatures;
    observation.progress = recompute.progress;
    observation.diagnostic = recompute.diagnostic;
    observation.features.reserve(recompute.features.size());
    for (const auto& feature : recompute.features) {
        DocumentCommandRecomputeFeatureObservation copied;
        copied.featureId = feature.featureId;
        copied.state = static_cast<DocumentCommandRecomputeFeatureState>(
            static_cast<int>(feature.state));
        copied.diagnostic = feature.diagnostic;
        copied.executed = feature.executed;
        observation.features.push_back(std::move(copied));
    }
    return observation;
}

DocumentExecutionBusyRejectionKind DocumentExecutionLane::busyKind(
    DocumentCommandKind kind) const noexcept
{
    switch (kind) {
        case DocumentCommandKind::Edit:
            return DocumentExecutionBusyRejectionKind::Edit;
        case DocumentCommandKind::Undo:
            return DocumentExecutionBusyRejectionKind::Undo;
        case DocumentCommandKind::Redo:
            return DocumentExecutionBusyRejectionKind::Redo;
        case DocumentCommandKind::Save:
            return DocumentExecutionBusyRejectionKind::Save;
        case DocumentCommandKind::Close:
            return DocumentExecutionBusyRejectionKind::Close;
        case DocumentCommandKind::Recompute:
            return DocumentExecutionBusyRejectionKind::CommandAdmission;
        default:
            return DocumentExecutionBusyRejectionKind::Other;
    }
}

void DocumentExecutionLane::recordBusyRejection(DocumentCommandKind kind)
{
    if (_telemetry) {
        _telemetry->recordBusyRejection(busyKind(kind));
    }
    DocumentExecutionTelemetry::instance().publishProcessSnapshot();
}

}  // namespace App
