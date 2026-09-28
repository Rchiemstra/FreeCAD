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

DocumentExecutionClosePolicy::UnresponsiveLaneAction
DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(const bool stalled) noexcept
{
    return stalled ? UnresponsiveLaneAction::RequestProcessExit
                   : UnresponsiveLaneAction::KeepWaiting;
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
    requestShutdown("document execution lane destroyed");
    joinThread();
    unregisterLane(_identity.documentInstanceId);
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
    std::lock_guard lock(_mutex);
    return !_active;
}

bool DocumentExecutionLane::permitsApplicationClose() const noexcept
{
    {
        std::lock_guard lock(_mutex);
        if (_active) {
            if (!isOwnerThread()) {
                return false;
            }
            return _active->command.kind == DocumentCommandKind::Close;
        }
    }
    // Application::closeDocument publishes Closing and drains outstanding
    // collaboration admissions; do not reject here while they are still held.
    return true;
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

    if (_active) {
        if (command.kind == DocumentCommandKind::Recompute
            && _active->command.kind == DocumentCommandKind::Recompute
            && recomputeCommandsCoalesce(command, _active->command)) {
            outcome.result = DocumentCommandSubmitResult::Accepted;
            outcome.commandId = _active->id;
            outcome.diagnostic = "joined active identical recompute";
            return outcome;
        }

        if (command.kind == DocumentCommandKind::Recompute
            || commandRequiresBusyWhileActive(command.kind)) {
            recordBusyRejection(command.kind);
            outcome.result = DocumentCommandSubmitResult::Busy;
            outcome.diagnostic = "document execution lane is busy";
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
        if (_active && _active->recomputeId == id && _active->snapshot.recompute) {
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
    static_cast<void>(reason);
    {
        std::lock_guard lock(_mutex);
        if (!_active || _active->id != id) {
            return false;
        }
        _active->cancelRequested.store(true, std::memory_order_release);
        _active->snapshot.state = DocumentCommandState::Cancelling;
        publishActiveSnapshotLocked(_active->snapshot);
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
}

void DocumentExecutionLane::drainDispatchQueue()
{
    std::vector<std::function<void()>> queue;
    {
        std::lock_guard lock(_mutex);
        queue.swap(_dispatchQueue);
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
            DocumentCommandState finalState = DocumentCommandState::Failed;
            std::string finalDiagnostic = "close failed";
            try {
                const bool closed = GetApplication().closeDocument(documentName.c_str());
                finalState = closed ? DocumentCommandState::Completed
                                    : DocumentCommandState::Failed;
                finalDiagnostic = closed ? "close completed" : "close failed";
            }
            catch (const Base::Exception& exception) {
                finalDiagnostic = commandExceptionDiagnostic(exception);
            }
            catch (const std::exception& exception) {
                finalDiagnostic = commandExceptionDiagnostic(exception);
            }
            catch (...) {
                finalDiagnostic = "close failed with an unknown exception";
            }
            // Finish the admitted Close command before the lane thread exits; do
            // not touch _document after closeDocument() returns.
            completeActiveCommand(finalState, finalDiagnostic);
            requestShutdown("document closed");
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

        if (!_active->recomputeId) {
            const auto& coalescingKey = _active->command.recompute
                ? _active->command.recompute->coalescingKey
                : std::string {};
            try {
                if (coalescingKey.starts_with(laneTestBlockingCoalescingPrefix)) {
                    _active->recomputeId = submitLaneTestBlockingRecompute(
                        _document,
                        coalescingKey.substr(laneTestBlockingCoalescingPrefix.size()));
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
                                _active->crossDocumentReservations =
                                    reservation.reservedInOrder;
                            }
                        }
                    }

                    const bool force = _active->command.recompute
                        && _active->command.recompute->coalescingKey.find("force;")
                            != std::string::npos;
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
                    _active->recomputeId = handle->id();
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

        if (!_active || !_active->recomputeId) {
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
            return _document.save();
        case DocumentCommandKind::Close:
            return false;
        case DocumentCommandKind::Edit: {
            if (!command.command.edit) {
                return false;
            }
            for (const auto& propertyValue : command.command.edit->propertyValues) {
                DocumentObject* object = nullptr;
                if (!propertyValue.stableObjectIdentity.empty()) {
                    for (auto* candidate : _document.getObjects()) {
                        try {
                            if (_document.collaborationObjectIdentity(*candidate)
                                == propertyValue.stableObjectIdentity) {
                                object = candidate;
                                break;
                            }
                        }
                        catch (...) {
                        }
                    }
                }
                if (!object) {
                    return false;
                }
                auto* property = object->getPropertyByName(propertyValue.propertyName.c_str());
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
            return true;
        }
        default:
            return false;
    }
}

void DocumentExecutionLane::pumpActiveRecompute(ActiveCommand& command)
{
    if (!command.recomputeId) {
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

    command.snapshot.state = mapRecomputeState(recomputeSnapshot->state);
    command.snapshot.progress = recomputeSnapshot->progress;
    command.snapshot.diagnostic = recomputeSnapshot->diagnostic;
    command.snapshot.recompute = makeRecomputeObservation(*recomputeSnapshot);

    const bool progressChanged = previousProgress != command.snapshot.progress
        || (command.snapshot.recompute
            && previousCompleted != command.snapshot.recompute->completedFeatures)
        || (command.snapshot.recompute
            && previousRecomputeState != command.snapshot.recompute->state);
    if (progressChanged) {
        touchWatchdogProgress(command);
    }
    updateWatchdogState(command);
    publishActiveSnapshot(command.snapshot);

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
    _terminalSnapshots.emplace(_active->id, _active->snapshot);
    _active.reset();
    _workAvailable.notify_all();
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
    command.lastProgressEpochMilliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (_telemetry) {
        _telemetry->touchWatchdogProgress();
    }
}

void DocumentExecutionLane::updateWatchdogState(ActiveCommand& command)
{
    if (!_telemetry) {
        return;
    }
    const auto telemetrySnapshot = _telemetry->snapshot();
    if (!telemetrySnapshot.watchdog.stalled()) {
        return;
    }
    if (command.snapshot.state == DocumentCommandState::Running) {
        command.snapshot.state = DocumentCommandState::Stalled;
        command.snapshot.diagnostic = telemetrySnapshot.watchdog.diagnostic.empty()
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
