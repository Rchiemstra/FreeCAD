// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCommitCoordinator.h"

#include "CollaborativeOperation.h"
#include "Document.h"
#include "DocumentCollaborationService.h"
#include "DocumentObject.h"
#include "MainThreadSignal.h"
#include "MutationClassification.h"
#include "PreparedEdit.h"

#include <Base/Exception.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace
{

using KeySet =
    std::unordered_set<App::DocumentRevisionKey, App::DocumentRevisionKeyHash>;

static_assert(noexcept(std::declval<std::vector<App::DocumentRevisionObservation>&>() =
                       std::declval<std::vector<App::DocumentRevisionObservation>&&>()));
static_assert(std::is_nothrow_move_constructible_v<App::DocumentCommitResult>);

KeySet asKeySet(const std::vector<App::DocumentRevisionKey>& keys)
{
    KeySet result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        if (!key.valid() || !result.insert(key).second) {
            return {};
        }
    }
    return result;
}

bool effectsExactlyCoverWrites(
    const std::vector<App::DocumentRevisionPublicationRequest>& effects,
    const std::vector<App::DocumentRevisionKey>& writeSet)
{
    if (effects.size() != writeSet.size()) {
        return false;
    }

    KeySet effectKeys;
    effectKeys.reserve(effects.size());
    for (const auto& effect : effects) {
        if (!effect.key.valid() || effect.revisionDelta == 0
            || !effectKeys.insert(effect.key).second) {
            return false;
        }
    }
    const auto writes = asKeySet(writeSet);
    return writes.size() == writeSet.size() && effectKeys == writes;
}

App::DocumentCommitResult makeResult(App::DocumentCommitStatus status,
                                     const App::PreparedEdit& edit,
                                     std::string message)
{
    App::DocumentCommitResult result;
    result.status = status;
    result.operationId = edit.operationId();
    result.message = std::move(message);
    return result;
}

App::DocumentCommitResult makeConflictResult(
    const App::PreparedEdit& edit,
    std::string message,
    std::vector<App::DocumentRevisionConflict> conflicts)
{
    return {App::DocumentCommitStatus::Conflict,
            edit.operationId(),
            std::move(message),
            std::move(conflicts),
            {}};
}

std::string stageFailure(std::string_view stage, const char* detail)
{
    std::string message(stage);
    message += ": ";
    message += detail ? detail : "unknown failure";
    return message;
}

std::string pendingRecomputeDetail(App::Document& document, std::string message)
{
    bool first = true;
    for (auto* object : document.getObjects()) {
        if (!object || (!object->isTouched() && object->mustExecute() == 0)) {
            continue;
        }
        message += first ? ": " : ", ";
        first = false;
        const char* name = object->getNameInDocument();
        message += name && *name ? name : "<unnamed>";
        message += " [";
        message += object->getStatusString();
        message += "]";
    }
    return message;
}

struct PostconditionObjectState
{
    App::DocumentObject* object {nullptr};
    unsigned long status {0};
    bool touched {false};
};

struct PostconditionDocumentState
{
    std::vector<PostconditionObjectState> objects;
    App::DocumentObject* activeObject {nullptr};
    App::DocumentFileState fileState {App::DocumentFileState::NotSaved};
    App::DocumentFileChanges fileChanges;
    bool pendingRecompute {false};
};

PostconditionDocumentState capturePostconditionState(App::Document& document)
{
    PostconditionDocumentState state;
    const auto objects = document.getObjects();
    state.objects.reserve(objects.size());
    for (auto* object : objects) {
        state.objects.push_back({object, object->getStatus(), object->isTouched()});
    }
    state.activeObject = document.getActiveObject();
    state.fileState = document.getFileChangeState();
    state.fileChanges = document.getPendingFileChanges();
    state.pendingRecompute = document.mustExecute();
    return state;
}

bool postconditionStateUnchanged(App::Document& document,
                                 const PostconditionDocumentState& before)
{
    const auto objects = document.getObjects();
    if (objects.size() != before.objects.size()
        || document.getActiveObject() != before.activeObject
        || document.getFileChangeState() != before.fileState
        || document.getPendingFileChanges().toUnderlyingType()
            != before.fileChanges.toUnderlyingType()
        || document.mustExecute() != before.pendingRecompute) {
        return false;
    }
    for (std::size_t index = 0; index < objects.size(); ++index) {
        const auto& expected = before.objects[index];
        if (objects[index] != expected.object
            || objects[index]->getStatus() != expected.status
            || objects[index]->isTouched() != expected.touched) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::atomic<App::DocumentCommitCoordinator::PostReservationTestHook>
    App::DocumentCommitCoordinator::_postReservationTestHook {nullptr};

using namespace App;

const char* App::documentCommitStatusName(DocumentCommitStatus status) noexcept
{
    switch (status) {
        case DocumentCommitStatus::Committed:
            return "Committed";
        case DocumentCommitStatus::StaleDocument:
            return "StaleDocument";
        case DocumentCommitStatus::InvalidPreparedEdit:
            return "InvalidPreparedEdit";
        case DocumentCommitStatus::Conflict:
            return "Conflict";
        case DocumentCommitStatus::Cancelled:
            return "Cancelled";
        case DocumentCommitStatus::Unsupported:
            return "Unsupported";
        case DocumentCommitStatus::Busy:
            return "Busy";
        case DocumentCommitStatus::ApplyFailed:
            return "ApplyFailed";
        case DocumentCommitStatus::RecomputeFailed:
            return "RecomputeFailed";
        case DocumentCommitStatus::PostconditionFailed:
            return "PostconditionFailed";
        case DocumentCommitStatus::PublicationFailed:
            return "PublicationFailed";
        case DocumentCommitStatus::RollbackFailed:
            return "RollbackFailed";
    }
    return "Unknown";
}

DocumentCommitCoordinator::DocumentCommitCoordinator(Document& document) noexcept
    : _document(document)
{}

Document& DocumentCommitCoordinator::document() const noexcept
{
    return _document;
}

int DocumentCommitCoordinator::openCompatibilityTransaction(
    TransactionName name,
    const int transactionId)
{
    return _document.openCompatibilityTransactionImpl(std::move(name), transactionId);
}

int DocumentCommitCoordinator::openMutationTransaction(
    std::string name,
    const int transactionId)
{
    return _document._openTransaction(std::move(name), transactionId);
}

int DocumentCommitCoordinator::setActiveCompatibilityTransaction(
    TransactionName name,
    const int transactionId)
{
    return _document.setActiveCompatibilityTransactionImpl(std::move(name), transactionId);
}

void DocumentCommitCoordinator::commitCompatibilityTransaction()
{
    _document.commitCompatibilityTransactionImpl();
}

void DocumentCommitCoordinator::abortCompatibilityTransaction()
{
    _document.abortCompatibilityTransactionImpl();
}

bool DocumentCommitCoordinator::undoCompatibilityTransaction(const int transactionId)
{
    return _document.undoCompatibilityTransactionImpl(transactionId);
}

bool DocumentCommitCoordinator::redoCompatibilityTransaction(const int transactionId)
{
    return _document.redoCompatibilityTransactionImpl(transactionId);
}

void DocumentCommitCoordinator::clearCompatibilityTransactionHistory()
{
    _document.clearCompatibilityTransactionHistoryImpl();
}

bool DocumentCommitCoordinator::commitApplicationTransaction()
{
    return _document._commitTransaction();
}

void DocumentCommitCoordinator::abortApplicationTransaction()
{
    _document._abortTransaction();
}

int DocumentCommitCoordinator::openNativeCommitTransaction(
    std::string name,
    const bool retainUndoHistory)
{
    return _document.openCollaborationCommitTransaction(
        std::move(name), retainUndoHistory);
}

bool DocumentCommitCoordinator::commitNativeCommitTransaction(
    const bool retainUndoHistory)
{
    return _document.commitCollaborationCommitTransaction(retainUndoHistory);
}

CollaborationRollbackResult DocumentCommitCoordinator::rollbackNativeCommitTransaction(
    const bool preservePendingRecompute) noexcept
{
    return preservePendingRecompute
        ? _document.rollbackCollaborationTransactionPreservingPendingRecompute()
        : _document.rollbackCollaborationTransaction();
}

DocumentCommitResult DocumentCommitCoordinator::commit(const PreparedEdit& edit)
{
    return commitWithPreparationPolicy(edit, true, false);
}

DocumentCommitResult DocumentCommitCoordinator::commitRecompute(const PreparedEdit& edit)
{
    if (_document.collaborationDerivedRecomputeGranted()) {
        return commitDerivedRecomputeInActiveTransaction(edit);
    }
    // A full recompute owns the single externally visible stable boundary.
    // Per-feature coordinator commits remain atomic, but their intermediate
    // stable signals are deferred until RecomputeHandle finalization.
    auto stableNotificationDeferral =
        _document.openCollaborationRecomputeStableNotificationDeferral();
    return commitWithPreparationPolicyAndOptions(
        edit,
        false,
        false,
        CollaborationCompatibilityRecomputePolicy::Deferred,
        false,
        false);
}

DocumentCommitResult DocumentCommitCoordinator::commitCompatibility(
    const PreparedEdit& edit,
    const bool structural)
{
    return commitCompatibilityWithPolicy(
        edit, structural, CollaborationCompatibilityRecomputePolicy::Eager);
}

DocumentCommitResult DocumentCommitCoordinator::commitCompatibilityWithPolicy(
    const PreparedEdit& edit,
    const bool structural,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy)
{
    return commitCompatibilityWithOptions(edit, structural, recomputePolicy, false);
}

DocumentCommitResult DocumentCommitCoordinator::commitCompatibilityWithOptions(
    const PreparedEdit& edit,
    const bool structural,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy,
    const bool trustedStructural)
{
    return commitWithPreparationPolicyAndOptions(
        edit, false, structural, recomputePolicy, trustedStructural);
}

DocumentCommitResult DocumentCommitCoordinator::commitWithPreparationPolicy(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility)
{
    return commitWithPreparationPolicyAndRecompute(
        edit,
        requireDetachedPreparationSupport,
        structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy::Eager);
}

DocumentCommitResult DocumentCommitCoordinator::commitWithPreparationPolicyAndRecompute(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy)
{
    return commitWithPreparationPolicyAndOptions(
        edit,
        requireDetachedPreparationSupport,
        structuralCompatibility,
        recomputePolicy,
        false);
}

DocumentCommitResult DocumentCommitCoordinator::commitWithPreparationPolicyAndOptions(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy,
    const bool trustedStructural,
    const bool retainUndoHistory)
{
    if (!MainThreadSignalConfig::hasHooks()) {
        if (!_document.isCollaborationOwnerThread()) {
            return makeResult(DocumentCommitStatus::Unsupported,
                              edit,
                              "off-owner collaboration commit requires a document-thread dispatcher");
        }
        return commitOnDocumentThreadWithOptions(edit,
                                                 requireDetachedPreparationSupport,
                                                 structuralCompatibility,
                                                 recomputePolicy,
                                                 trustedStructural,
                                                 retainUndoHistory);
    }
    if (MainThreadSignalConfig::isMainThread()) {
        return commitOnDocumentThreadWithOptions(edit,
                                                 requireDetachedPreparationSupport,
                                                 structuralCompatibility,
                                                 recomputePolicy,
                                                 trustedStructural,
                                                 retainUndoHistory);
    }

    std::optional<DocumentCommitResult> result;
    std::exception_ptr failure;
    {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        MainThreadSignalConfig::invoke(
            [this,
             &edit,
             &result,
             &failure,
              requireDetachedPreparationSupport,
              structuralCompatibility,
              recomputePolicy,
              trustedStructural,
              retainUndoHistory] {
                try {
                    result.emplace(commitOnDocumentThreadWithOptions(
                        edit,
                        requireDetachedPreparationSupport,
                        structuralCompatibility,
                        recomputePolicy,
                        trustedStructural,
                        retainUndoHistory));
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
        throw std::runtime_error("main-thread collaboration dispatch returned no result");
    }
    return std::move(*result);
}

DocumentCommitResult DocumentCommitCoordinator::commitOnDocumentThread(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility)
{
    return commitOnDocumentThreadWithRecompute(
        edit,
        requireDetachedPreparationSupport,
        structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy::Eager);
}

DocumentCommitResult DocumentCommitCoordinator::commitOnDocumentThreadWithRecompute(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy)
{
    return commitOnDocumentThreadWithOptions(edit,
                                             requireDetachedPreparationSupport,
                                             structuralCompatibility,
                                             recomputePolicy,
                                             false,
                                             true);
}

DocumentCommitResult
DocumentCommitCoordinator::commitDerivedRecomputeInActiveTransaction(
    const PreparedEdit& edit)
{
    if (!_document.isCollaborationOwnerThread()) {
        return makeResult(DocumentCommitStatus::Unsupported,
                          edit,
                          "derived recompute was not dispatched to the document owner thread");
    }
    if (!_document.collaborationDerivedRecomputeGranted()
        || !_document.hasPendingTransaction()
        || !_document.collaborationRevisionPublicationSuppressed()) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "derived recompute requires the active coordinator transaction");
    }
    if (_document.collaborationNotificationsReplaying()) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "a committed collaboration boundary is notifying observers");
    }
    if (_document.collaborationCommitPoisoned()) {
        return makeResult(DocumentCommitStatus::RollbackFailed,
                          edit,
                          _document.collaborationCommitPoisonDiagnostic());
    }

    std::optional<DocumentIdentity> identity;
    try {
        identity = _document.collaborationIdentity();
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          stageFailure("document identity unavailable", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          stageFailure("document identity unavailable", exception.what()));
    }
    if (!identity || identity->instanceId != edit.documentInstanceId()
        || identity->lifecycleEpoch != edit.lifecycleEpoch()
        || identity->state != DocumentLifecycleState::Live) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          "prepared recompute targets a stale or non-live document instance");
    }

    const auto& operation = edit.operation();
    if (operation.typeId().empty() || operation.typeId() != edit.operationType()) {
        return makeResult(DocumentCommitStatus::InvalidPreparedEdit,
                          edit,
                          "prepared recompute payload does not match its declared type");
    }
    const auto& declaredEffects = edit.publicationEffects();
    if (!effectsExactlyCoverWrites(declaredEffects, edit.writeSet())) {
        return makeResult(
            DocumentCommitStatus::InvalidPreparedEdit,
            edit,
            "recompute revision effects must exactly match the declared write set");
    }
    auto conflicts = _document.collaborationRevisions().validate(edit.expectedRevisions());
    if (!conflicts.empty()) {
        return makeConflictResult(edit,
                                  "recompute revisions changed before derived commit",
                                  std::move(conflicts));
    }

    try {
        operation.apply(_document);
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::ApplyFailed,
                          edit,
                          stageFailure("derived recompute apply failed", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::ApplyFailed,
                          edit,
                          stageFailure("derived recompute apply failed", exception.what()));
    }
    catch (...) {
        return makeResult(DocumentCommitStatus::ApplyFailed,
                          edit,
                          "derived recompute apply failed with an unknown exception");
    }
    if (!_document.hasPendingTransaction()) {
        _document.poisonCollaborationCommit(
            "derived recompute escaped its coordinator transaction");
        return makeResult(DocumentCommitStatus::RollbackFailed,
                          edit,
                          "derived recompute escaped its coordinator transaction");
    }

    CollaborativePostconditionResult postcondition;
    bool postconditionMutationAttempted = false;
    try {
        const auto postconditionState = capturePostconditionState(_document);
        _document.beginCollaborationPreparedReadOnlyPostconditionAudit();
        try {
            postcondition = operation.checkPostcondition(_document);
            postconditionMutationAttempted =
                _document.collaborationAtomicPresentationAuditViolated()
                || !postconditionStateUnchanged(_document, postconditionState);
        }
        catch (...) {
            _document.endCollaborationPreparedAtomicPresentationAudit();
            throw;
        }
        _document.endCollaborationPreparedAtomicPresentationAudit();
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          stageFailure("derived recompute postcondition failed",
                                       exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          stageFailure("derived recompute postcondition failed",
                                       exception.what()));
    }
    catch (...) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          "derived recompute postcondition failed with an unknown exception");
    }
    if (postconditionMutationAttempted) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          "derived recompute postcondition attempted to mutate document state");
    }
    if (!postcondition.satisfied) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          postcondition.message.empty()
                              ? "derived recompute postcondition was not satisfied"
                              : std::move(postcondition.message));
    }
    if (_document.getMutationReadiness().pendingRemoval) {
        return makeResult(DocumentCommitStatus::PostconditionFailed,
                          edit,
                          "derived recompute left pending object removal work");
    }

    // The outer PreparedEdit already owns the authoritative, frozen semantic
    // closure for this mutation and its downstream recompute.  A worker result
    // may apply values inside that transaction, but it must not widen the
    // outer publication with result-local property details.
    return makeResult(DocumentCommitStatus::Committed,
                      edit,
                      "derived recompute applied in the active coordinator transaction");
}

DocumentCommitResult DocumentCommitCoordinator::commitOnDocumentThreadWithOptions(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport,
    const bool structuralCompatibility,
    const CollaborationCompatibilityRecomputePolicy recomputePolicy,
    const bool trustedStructural,
    const bool retainUndoHistory)
{
    if (!_document.isCollaborationOwnerThread()) {
        return makeResult(DocumentCommitStatus::Unsupported,
                          edit,
                          "collaboration commit was not dispatched to the document owner thread");
    }
    std::lock_guard<std::recursive_mutex> serialized(_document.collaborationCommitMutex());

    // Lifecycle validation deliberately precedes every call into the typed
    // payload. A stale payload cannot inspect or mutate a replacement or
    // closing document.
    std::optional<DocumentIdentity> identity;
    try {
        identity = _document.collaborationIdentity();
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          stageFailure("document identity unavailable", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          stageFailure("document identity unavailable", exception.what()));
    }
    if (!identity || identity->instanceId != edit.documentInstanceId()
        || identity->lifecycleEpoch != edit.lifecycleEpoch()
        || identity->state != DocumentLifecycleState::Live) {
        return makeResult(DocumentCommitStatus::StaleDocument,
                          edit,
                          "prepared edit targets a stale or non-live document instance");
    }

    if (_document.collaborationNotificationsReplaying()) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "a committed collaboration boundary is notifying observers");
    }
    if (_document.collaborationCommitPoisoned()) {
        return makeResult(DocumentCommitStatus::RollbackFailed,
                          edit,
                          _document.collaborationCommitPoisonDiagnostic());
    }

    if (requireDetachedPreparationSupport && !_document.collaborationPreparationSupported()) {
        return makeResult(DocumentCommitStatus::Unsupported,
                          edit,
                          "document contains a mutable Python payload that cannot be prepared");
    }
    if (_document.hasPendingTransaction() || _document.transacting()
        || _document.getBookedTransactionID() != 0 || _document.isTransactionLocked()) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "document already has a native transaction in progress");
    }
    if (recomputePolicy != CollaborationCompatibilityRecomputePolicy::Eager
        && recomputePolicy != CollaborationCompatibilityRecomputePolicy::Deferred) {
        return makeResult(DocumentCommitStatus::InvalidPreparedEdit,
                          edit,
                          "compatibility mutation has an unknown recompute policy");
    }
    const auto& operation = edit.operation();
    if (operation.typeId().empty() || operation.typeId() != edit.operationType()) {
        return makeResult(DocumentCommitStatus::InvalidPreparedEdit,
                          edit,
                          "prepared operation payload does not match its declared type");
    }

    const auto& declaredEffects = edit.publicationEffects();
    if (!effectsExactlyCoverWrites(declaredEffects, edit.writeSet())) {
        return makeResult(
            DocumentCommitStatus::InvalidPreparedEdit,
            edit,
            "revision effects must be duplicate-free and exactly match the declared write set");
    }

    auto conflicts = _document.collaborationRevisions().validate(edit.expectedRevisions());
    if (!conflicts.empty()) {
        return makeConflictResult(edit,
                                  "one or more semantic revisions changed before commit",
                                  std::move(conflicts));
    }
    // An eager recompute below is document-wide. Start only from a clean
    // boundary so operation.apply() is the sole source of pending recompute
    // work and the adapter's frozen publication closure remains complete.
    // Revision conflicts take precedence over this admission state so a
    // changed dependency still receives the more precise terminal result.
    const bool preexistingPendingRecompute = _document.mustExecute();
    const auto boundaryReadiness = _document.getMutationReadiness();
    if (boundaryReadiness.recomputing) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "document recompute teardown is still active");
    }
    if (boundaryReadiness.pendingRemoval) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "document has pending object removal work");
    }
    if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Eager
        && preexistingPendingRecompute) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "document has pending recompute work outside the prepared operation");
    }

    std::unique_ptr<CollaborationPreparedMutationTargetScope> mutationTarget;
    try {
        mutationTarget.reset(new CollaborationPreparedMutationTargetScope(_document));
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          stageFailure("binding commit mutation target failed",
                                       exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          stageFailure("binding commit mutation target failed",
                                       exception.what()));
    }

    try {
        _document.beginCollaborationCommitNotificationBarrier();
    }
    catch (const Base::Exception& exception) {
        return makeResult(DocumentCommitStatus::ApplyFailed,
                          edit,
                          stageFailure("starting commit notification barrier failed",
                                       exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(DocumentCommitStatus::ApplyFailed,
                          edit,
                          stageFailure("starting commit notification barrier failed",
                                       exception.what()));
    }

    const bool priorSuppression = _document.collaborationRevisionPublicationSuppressed();
    _document.setCollaborationRevisionPublicationSuppressed(true);
    const bool preservePendingRecomputeOnRollback =
        recomputePolicy == CollaborationCompatibilityRecomputePolicy::Deferred
        && preexistingPendingRecompute;
    const auto rollbackTransaction = [this, preservePendingRecomputeOnRollback]() noexcept {
        return rollbackNativeCommitTransaction(preservePendingRecomputeOnRollback);
    };

    bool cleanupRequired = true;
    const auto emergencyCleanup =
        [this,
         priorSuppression,
         &rollbackTransaction,
         &mutationTarget](bool* required) noexcept {
        if (!*required) {
            return;
        }
        const auto rollback = rollbackTransaction();
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
        }
        _document.setCollaborationRevisionPublicationSuppressed(priorSuppression);
        mutationTarget.reset();
        _document.finishCollaborationCommitNotificationBarrier(false);
    };
    std::unique_ptr<bool, decltype(emergencyCleanup)> cleanupGuard(&cleanupRequired,
                                                                  emergencyCleanup);

    const auto restoreSuppression = [&]() noexcept {
        _document.setCollaborationRevisionPublicationSuppressed(priorSuppression);
    };
    const auto discardNotifications = [&]() noexcept {
        // A stable observer can synchronously resume unrelated document work.
        // Release the thread-local target before finish emits that event.
        mutationTarget.reset();
        _document.finishCollaborationCommitNotificationBarrier(false);
    };
    const auto abortAndRestore = [&](DocumentCommitResult result) {
        const auto rollback = rollbackTransaction();
        restoreSuppression();
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
            std::string message = "original ";
            message += documentCommitStatusName(result.status);
            message += ": ";
            message += result.message;
            message += "; rollback failed: ";
            message += rollback.diagnostic.data();
            result = makeResult(DocumentCommitStatus::RollbackFailed,
                                edit,
                                std::move(message));
        }
        discardNotifications();
        cleanupRequired = false;
        return result;
    };
    const auto abortRestoreAndRethrow = [&](std::string_view originalStage) {
        const auto rollback = rollbackTransaction();
        restoreSuppression();
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
            std::string message(originalStage);
            message += "; rollback failed: ";
            message += rollback.diagnostic.data();
            discardNotifications();
            cleanupRequired = false;
            std::throw_with_nested(std::runtime_error(std::move(message)));
        }
        discardNotifications();
        cleanupRequired = false;
        throw;
    };

    try {
        const std::string transactionName = "Collaborative operation " + edit.operationId();
        if (openNativeCommitTransaction(transactionName, retainUndoHistory) == 0) {
            restoreSuppression();
            discardNotifications();
            cleanupRequired = false;
            return makeResult(DocumentCommitStatus::Busy,
                              edit,
                              "document refused to open the native transaction");
        }
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                          edit,
                                          stageFailure("opening transaction failed",
                                                       exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                          edit,
                                          stageFailure("opening transaction failed",
                                                       exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown failure while opening native transaction");
    }

    try {
        if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Deferred) {
            auto recomputeFence = _document.openCollaborationDeferredRecomputeFence();
            if (structuralCompatibility) {
                auto grant = _document.openCollaborationStructuralMutationGrant();
                operation.apply(_document);
            }
            else {
                operation.apply(_document);
            }
        }
        else if (structuralCompatibility) {
            auto grant = _document.openCollaborationStructuralMutationGrant();
            operation.apply(_document);
        }
        else {
            operation.apply(_document);
        }
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                          edit,
                                          stageFailure("operation apply failed", exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                          edit,
                                          stageFailure("operation apply failed", exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown operation apply failure");
    }
    if (!_document.hasPendingTransaction()) {
        _document.poisonCollaborationCommit(
            "prepared operation escaped its native transaction; rollback cannot be proven");
        restoreSuppression();
        discardNotifications();
        cleanupRequired = false;
        return makeResult(DocumentCommitStatus::RollbackFailed,
                          edit,
                          "operation escaped its native transaction; rollback cannot be proven");
    }

    bool recomputeHasError = false;
    if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Eager) {
        try {
            auto derivedRecompute = _document.openCollaborationDerivedRecomputeGrant();
            if (structuralCompatibility) {
                auto grant = _document.openCollaborationStructuralRecomputeGrant(
                    trustedStructural);
                // Temporary coordinator-owned compatibility kill switch:
                // transaction-local structural callbacks can contain native
                // objects whose exact dynamic type is intentionally not
                // serializable. The grant still distinguishes trusted schema
                // mutation from untrusted execute-time attempts. CC-WP13
                // removes this live compatibility path after qualification.
                static_cast<void>(
                    _document.recomputeLegacy({}, true, &recomputeHasError, 0));
            }
            else {
                static_cast<void>(_document.recompute({}, true, &recomputeHasError));
            }
        }
        catch (const Base::Exception& exception) {
            return abortAndRestore(makeResult(DocumentCommitStatus::RecomputeFailed,
                                              edit,
                                              stageFailure("document recompute failed",
                                                           exception.what())));
        }
        catch (const std::exception& exception) {
            return abortAndRestore(makeResult(DocumentCommitStatus::RecomputeFailed,
                                              edit,
                                              stageFailure("document recompute failed",
                                                           exception.what())));
        }
        catch (...) {
            abortRestoreAndRethrow("unknown document recompute failure");
        }
    }
    if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Eager
        && recomputeHasError) {
        std::ostringstream detail;
        detail << "document recompute reported an object error";
        bool first = true;
        for (App::DocumentObject* object : _document.getObjects()) {
            if (!object || object->isValid()) {
                continue;
            }
            detail << (first ? ": " : ", ");
            first = false;
            const char* name = object->getNameInDocument();
            detail << (name && *name ? name : "<unnamed>");
            if (const char* why = _document.getErrorDescription(object); why && *why) {
                detail << " (" << why << ")";
            }
        }
        return abortAndRestore(makeResult(DocumentCommitStatus::RecomputeFailed,
                                          edit,
                                          detail.str()));
    }
    if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Eager
        && _document.mustExecute()) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::RecomputeFailed,
            edit,
            pendingRecomputeDetail(
                _document,
                "document still has pending recompute work after the authoritative recompute")));
    }

    CollaborativePostconditionResult postcondition;
    bool postconditionMutationAttempted = false;
    try {
        const auto postconditionState = capturePostconditionState(_document);
        _document.beginCollaborationPreparedReadOnlyPostconditionAudit();
        try {
            postcondition = operation.checkPostcondition(_document);
            postconditionMutationAttempted =
                _document.collaborationAtomicPresentationAuditViolated()
                || !postconditionStateUnchanged(_document, postconditionState);
        }
        catch (...) {
            _document.endCollaborationPreparedAtomicPresentationAudit();
            throw;
        }
        _document.endCollaborationPreparedAtomicPresentationAudit();
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PostconditionFailed,
                                          edit,
                                          stageFailure("postcondition check failed",
                                                       exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PostconditionFailed,
                                          edit,
                                          stageFailure("postcondition check failed",
                                                       exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown postcondition failure");
    }
    if (postconditionMutationAttempted) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PostconditionFailed,
            edit,
            "postcondition attempted to mutate document state"));
    }
    if (recomputePolicy == CollaborationCompatibilityRecomputePolicy::Eager
        && _document.mustExecute()) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PostconditionFailed,
            edit,
            pendingRecomputeDetail(
                _document,
                "postcondition left pending recompute work after validation")));
    }
    if (!postcondition.satisfied) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PostconditionFailed,
                                          edit,
                                          postcondition.message.empty()
                                              ? "operation postcondition was not satisfied"
                                              : std::move(postcondition.message)));
    }
    if (_document.getMutationReadiness().pendingRemoval) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PostconditionFailed,
            edit,
            "operation left pending object removal work after validation"));
    }

    std::vector<DocumentRevisionPublicationRequest> effects;
    try {
        // Recompute can perform narrowly admitted, model-specific transient
        // schema work (Spreadsheet cell properties). Read the native ledger
        // only after the authoritative recompute and read-only validation.
        auto observedStructuralEffects =
            _document.takeCollaborationObservedStructuralEffects();
        effects = declaredEffects;
        effects.insert(effects.end(),
                       observedStructuralEffects.begin(),
                       observedStructuralEffects.end());
        const auto effectLess = [](const DocumentRevisionPublicationRequest& left,
                                   const DocumentRevisionPublicationRequest& right) {
            if (left.key != right.key) {
                return left.key < right.key;
            }
            return left.stableObjectIdentity < right.stableObjectIdentity;
        };
        std::sort(effects.begin(), effects.end(), effectLess);
        auto output = effects.begin();
        for (auto current = effects.begin(); current != effects.end(); ++current) {
            if (output != effects.begin()
                && (output - 1)->key == current->key
                && (output - 1)->stableObjectIdentity
                    == current->stableObjectIdentity) {
                (output - 1)->revisionDelta = std::max(
                    (output - 1)->revisionDelta, current->revisionDelta);
                continue;
            }
            if (output != current) {
                *output = std::move(*current);
            }
            ++output;
        }
        effects.erase(output, effects.end());
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            stageFailure("structural effect ledger collection failed", exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            stageFailure("structural effect ledger collection failed", exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown structural effect ledger collection failure");
    }

    try {
        _document.prepareCollaborationCommitFinalization();
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          stageFailure("commit finalization preparation failed",
                                                       exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          stageFailure("commit finalization preparation failed",
                                                       exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown commit finalization preparation failure");
    }

    // The reservation revalidates the complete expected set and preallocates
    // a hidden publication. Its destructor cancels that publication on every
    // pre-commit return. The coordinator-private checked commit preserves an
    // explicit success bit even for a valid empty publication.
    std::optional<DocumentRevisionPublicationReservation> reservation;
    try {
        CollaborationRevisionMutationGrant revisionGrant(
            _document.collaborationRevisions());
        reservation.emplace(_document.collaborationRevisions().reservePublication(
            edit.expectedRevisions(), effects));
    }
    catch (const Base::Exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          stageFailure("revision publication reservation failed",
                                                       exception.what())));
    }
    catch (const std::exception& exception) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          stageFailure("revision publication reservation failed",
                                                       exception.what())));
    }
    catch (...) {
        abortRestoreAndRethrow("unknown revision publication reservation failure");
    }
    if (!reservation->ready()) {
        if (!reservation->conflicts().empty()) {
            std::vector<DocumentRevisionConflict> reservationConflicts(
                reservation->conflicts().begin(), reservation->conflicts().end());
            reservation->cancel();
            reservation.reset();
            return abortAndRestore(makeConflictResult(
                edit,
                "semantic revisions changed during commit admission",
                std::move(reservationConflicts)));
        }
        reservation->cancel();
        reservation.reset();
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          "revision publication could not be reserved"));
    }

    try {
        if (const auto hook = _postReservationTestHook.load(
                std::memory_order_acquire)) {
            hook();
        }
    }
    catch (const Base::Exception& exception) {
        reservation->cancel();
        reservation.reset();
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            stageFailure("post-reservation finalization failed", exception.what())));
    }
    catch (const std::exception& exception) {
        reservation->cancel();
        reservation.reset();
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            stageFailure("post-reservation finalization failed", exception.what())));
    }
    catch (...) {
        reservation->cancel();
        reservation.reset();
        return abortAndRestore(makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            "post-reservation finalization failed with an unknown exception"));
    }

    auto result = makeResult(DocumentCommitStatus::Committed,
                             edit,
                             "prepared edit committed");
    try {
        if (!commitNativeCommitTransaction(retainUndoHistory)) {
            reservation->cancel();
            reservation.reset();
            return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                              edit,
                                              "native transaction commit was refused"));
        }
    }
    catch (...) {
        reservation->cancel();
        reservation.reset();
        // If the transaction remains active it is still safe to abort. If a
        // native commit observer threw after consuming it, the integrator's
        // notification barrier owns final recovery and propagates the error.
        if (!_document.hasPendingTransaction()) {
            _document.poisonCollaborationCommit(
                "native transaction commit failed after consuming its transaction");
        }
        abortRestoreAndRethrow("unknown native transaction commit failure");
    }

    // Everything after the native commit is preallocated or noexcept. Publish
    // revisions before releasing deferred observers, so no observer can see a
    // committed model paired with the preceding revision state.
    bool publicationCommitted = false;
    {
        CollaborationRevisionMutationGrant revisionGrant(
            _document.collaborationRevisions());
        publicationCommitted = reservation->commitPrepared(
            result.publishedRevisions);
    }
    reservation.reset();
    if (!publicationCommitted) {
        // Native content is already durable. This path is intentionally
        // impossible under the still-bound target and private grant; if an
        // invariant is violated, quarantine the document rather than report
        // an ambiguous successful commit with missing semantic revisions.
        _document.poisonCollaborationCommit(
            "native transaction committed but semantic revision publication was denied");
        mutationTarget.reset();
        restoreSuppression();
        _document.finishCollaborationCommitNotificationBarrier(true);
        cleanupRequired = false;
        return makeResult(
            DocumentCommitStatus::PublicationFailed,
            edit,
            "native transaction committed but semantic revision publication failed; document quarantined");
    }

    // Deferred observer replay is outside the apply/recompute/postcondition
    // mutation boundary and may legitimately update another document.  Keep
    // the process target bound through reservation commit so no foreign direct
    // revision admission can enter the native-commit/publication interval.
    mutationTarget.reset();
    restoreSuppression();
    _document.finishCollaborationCommitNotificationBarrier(true);
    cleanupRequired = false;
    return result;
}
