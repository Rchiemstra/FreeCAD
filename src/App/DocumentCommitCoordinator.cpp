// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCommitCoordinator.h"

#include "CollaborativeOperation.h"
#include "Document.h"
#include "MainThreadSignal.h"
#include "PreparedEdit.h"

#include <Base/Exception.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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
        if (!effect.key.valid() || !effectKeys.insert(effect.key).second) {
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

}  // namespace

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

DocumentCommitResult DocumentCommitCoordinator::commit(const PreparedEdit& edit)
{
    return commitWithPreparationPolicy(edit, true);
}

DocumentCommitResult DocumentCommitCoordinator::commitCompatibility(const PreparedEdit& edit)
{
    return commitWithPreparationPolicy(edit, false);
}

DocumentCommitResult DocumentCommitCoordinator::commitWithPreparationPolicy(
    const PreparedEdit& edit,
    const bool requireDetachedPreparationSupport)
{
    if (!MainThreadSignalConfig::hasHooks()) {
        if (!_document.isCollaborationOwnerThread()) {
            return makeResult(DocumentCommitStatus::Unsupported,
                              edit,
                              "off-owner collaboration commit requires a document-thread dispatcher");
        }
        return commitOnDocumentThread(edit, requireDetachedPreparationSupport);
    }
    if (MainThreadSignalConfig::isMainThread()) {
        return commitOnDocumentThread(edit, requireDetachedPreparationSupport);
    }

    std::optional<DocumentCommitResult> result;
    std::exception_ptr failure;
    {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        MainThreadSignalConfig::invoke(
            [this, &edit, &result, &failure, requireDetachedPreparationSupport] {
                try {
                    result.emplace(
                        commitOnDocumentThread(edit, requireDetachedPreparationSupport));
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
    const bool requireDetachedPreparationSupport)
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
    const auto& operation = edit.operation();
    if (operation.typeId().empty() || operation.typeId() != edit.operationType()) {
        return makeResult(DocumentCommitStatus::InvalidPreparedEdit,
                          edit,
                          "prepared operation payload does not match its declared type");
    }

    const auto& effects = edit.publicationEffects();
    if (!effectsExactlyCoverWrites(effects, edit.writeSet())) {
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
    // The mandatory recompute below is document-wide. Start only from a clean
    // boundary so operation.apply() is the sole source of pending recompute
    // work and the adapter's frozen publication closure remains complete.
    // Revision conflicts take precedence over this admission state so a
    // changed dependency still receives the more precise terminal result.
    if (_document.mustExecute()) {
        return makeResult(DocumentCommitStatus::Busy,
                          edit,
                          "document has pending recompute work outside the prepared operation");
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

    bool cleanupRequired = true;
    const auto emergencyCleanup = [this, priorSuppression](bool* required) noexcept {
        if (!*required) {
            return;
        }
        const auto rollback = _document.rollbackCollaborationTransaction();
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
        }
        _document.setCollaborationRevisionPublicationSuppressed(priorSuppression);
        _document.finishCollaborationCommitNotificationBarrier(false);
    };
    std::unique_ptr<bool, decltype(emergencyCleanup)> cleanupGuard(&cleanupRequired,
                                                                  emergencyCleanup);

    const auto restoreSuppression = [&]() noexcept {
        _document.setCollaborationRevisionPublicationSuppressed(priorSuppression);
    };
    const auto discardNotifications = [&]() noexcept {
        _document.finishCollaborationCommitNotificationBarrier(false);
    };
    const auto abortAndRestore = [&](DocumentCommitResult result) {
        const auto rollback = _document.rollbackCollaborationTransaction();
        restoreSuppression();
        discardNotifications();
        cleanupRequired = false;
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
            std::string message = "original ";
            message += documentCommitStatusName(result.status);
            message += ": ";
            message += result.message;
            message += "; rollback failed: ";
            message += rollback.diagnostic.data();
            return makeResult(DocumentCommitStatus::RollbackFailed, edit, std::move(message));
        }
        return result;
    };
    const auto abortRestoreAndRethrow = [&](std::string_view originalStage) {
        const auto rollback = _document.rollbackCollaborationTransaction();
        restoreSuppression();
        discardNotifications();
        cleanupRequired = false;
        if (!rollback.restored) {
            _document.poisonCollaborationCommit(rollback.diagnostic.data());
            std::string message(originalStage);
            message += "; rollback failed: ";
            message += rollback.diagnostic.data();
            std::throw_with_nested(std::runtime_error(std::move(message)));
        }
        throw;
    };

    try {
        const std::string transactionName = "Collaborative operation " + edit.operationId();
        if (_document.openCollaborationCommitTransaction(transactionName) == 0) {
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
        operation.apply(_document);
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
    try {
        static_cast<void>(_document.recompute({}, true, &recomputeHasError));
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
    if (recomputeHasError) {
        return abortAndRestore(makeResult(DocumentCommitStatus::RecomputeFailed,
                                          edit,
                                          "document recompute reported an object error"));
    }

    CollaborativePostconditionResult postcondition;
    try {
        postcondition = operation.checkPostcondition(_document);
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
    if (!postcondition.satisfied) {
        return abortAndRestore(makeResult(DocumentCommitStatus::PostconditionFailed,
                                          edit,
                                          postcondition.message.empty()
                                              ? "operation postcondition was not satisfied"
                                              : std::move(postcondition.message)));
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
    // pre-commit return; commit() is noexcept after the native transaction is
    // durable, so PublicationFailed can never expose an unrevisioned mutation.
    std::optional<DocumentRevisionPublicationReservation> reservation;
    try {
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
            return abortAndRestore(makeConflictResult(
                edit,
                "semantic revisions changed during commit admission",
                std::move(reservationConflicts)));
        }
        return abortAndRestore(makeResult(DocumentCommitStatus::PublicationFailed,
                                          edit,
                                          "revision publication could not be reserved"));
    }

    auto result = makeResult(DocumentCommitStatus::Committed,
                             edit,
                             "prepared edit committed");
    try {
        if (!_document.commitCollaborationCommitTransaction()) {
            return abortAndRestore(makeResult(DocumentCommitStatus::ApplyFailed,
                                              edit,
                                              "native transaction commit was refused"));
        }
    }
    catch (...) {
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
    result.publishedRevisions = reservation->commit();
    restoreSuppression();
    _document.finishCollaborationCommitNotificationBarrier(true);
    cleanupRequired = false;
    return result;
}
