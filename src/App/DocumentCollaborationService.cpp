// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCollaborationService.h"

#include "CollaborativeSetPropertyOperation.h"
#include "Document.h"
#include "MainThreadSignal.h"

#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Uuid.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace
{

template<typename Result, typename Callable>
Result invokeOnDocumentThread(Callable&& callable)
{
    if (!App::MainThreadSignalConfig::hasHooks()
        || App::MainThreadSignalConfig::isMainThread()) {
        return std::forward<Callable>(callable)();
    }

    std::optional<Result> result;
    std::exception_ptr failure;
    {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        App::MainThreadSignalConfig::invoke(
            [&] {
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
        throw std::runtime_error("main-thread collaboration dispatch returned no result");
    }
    return std::move(*result);
}

std::vector<App::DocumentRevisionKey> canonicalKeys(
    std::vector<App::DocumentRevisionKey> keys)
{
    if (std::ranges::any_of(keys, [](const auto& key) { return !key.valid(); })) {
        throw std::invalid_argument("snapshot revision key is invalid");
    }
    std::sort(keys.begin(), keys.end(), [](const auto& left, const auto& right) {
        return left < right;
    });
    if (std::ranges::adjacent_find(keys) != keys.end()) {
        throw std::invalid_argument("snapshot revision keys contain a duplicate");
    }
    return keys;
}

App::DocumentCommitResult rejectedCommit(App::DocumentCommitStatus status,
                                         const App::PreparedEdit& edit,
                                         std::string message)
{
    App::DocumentCommitResult result;
    result.status = status;
    result.operationId = edit.operationId();
    result.message = std::move(message);
    return result;
}

}  // namespace

using namespace App;

DocumentCollaborationService::DocumentCollaborationService(Document& document)
    : _document(document)
    , _coordinator(document)
{
    ensureCollaborativeSetPropertyOperationRegistered();
}

Document& DocumentCollaborationService::document() const noexcept
{
    return _document;
}

EditSession DocumentCollaborationService::beginEditSession(std::string actorId)
{
    if (actorId.empty()) {
        throw std::invalid_argument("edit session actor identity is required");
    }
    const auto identity = _document.collaborationIdentity();
    if (identity.state != DocumentLifecycleState::Live) {
        throw Base::RuntimeError("cannot begin an edit session for a non-live document");
    }

    EditSession session(Base::Uuid::createUuid(), std::move(actorId), identity.instanceId);
    std::lock_guard lock(_sessionMutex);
    const auto [position, inserted] = _sessions.emplace(session.sessionId(), session);
    if (!inserted) {
        throw Base::RuntimeError("generated duplicate edit session identity");
    }
    return position->second;
}

std::optional<EditSession> DocumentCollaborationService::sessionStatus(
    const std::string& sessionId) const
{
    std::lock_guard lock(_sessionMutex);
    const auto found = _sessions.find(sessionId);
    if (found == _sessions.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool DocumentCollaborationService::cancelEdit(const std::string& sessionId, std::string reason)
{
    if (reason.empty()) {
        reason = "cancelled by caller";
    }
    std::lock_guard lock(_sessionMutex);
    const auto found = _sessions.find(sessionId);
    if (found == _sessions.end()) {
        return false;
    }
    found->second._status = EditSessionStatus::Cancelled;
    found->second._cancellationReason = std::move(reason);
    return true;
}

EditSession DocumentCollaborationService::requireActiveSession(
    const std::string& sessionId) const
{
    std::lock_guard lock(_sessionMutex);
    const auto found = _sessions.find(sessionId);
    if (found == _sessions.end()) {
        throw std::invalid_argument("unknown edit session");
    }
    if (found->second.status() != EditSessionStatus::Active) {
        throw Base::RuntimeError("edit session is cancelled");
    }
    return found->second;
}

CollaborationEditSnapshot DocumentCollaborationService::snapshotForEdit(
    const std::string& sessionId,
    std::vector<DocumentRevisionKey> keys) const
{
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "off-owner collaboration snapshots require a document-thread dispatcher");
    }
    return invokeOnDocumentThread<CollaborationEditSnapshot>(
        [this, sessionId, keys = std::move(keys)]() mutable {
            return snapshotForEditOnDocumentThread(sessionId, std::move(keys));
        });
}

CollaborationEditSnapshot DocumentCollaborationService::snapshotForEditOnDocumentThread(
    const std::string& sessionId,
    std::vector<DocumentRevisionKey> keys) const
{
    if (!_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError("collaboration snapshot was not dispatched to the document owner thread");
    }
    std::lock_guard lock(_document.collaborationCommitMutex());
    if (_document.collaborationStableReadBlocked()) {
        throw Base::RuntimeError(
            "collaboration snapshots require a stable document boundary");
    }
    const EditSession session = requireActiveSession(sessionId);
    const auto identity = _document.collaborationIdentity();
    if (identity.state != DocumentLifecycleState::Live
        || identity.instanceId != session.documentInstanceId()) {
        throw Base::RuntimeError("edit session targets a stale document instance");
    }
    keys = canonicalKeys(std::move(keys));
    return {session.sessionId(),
            identity.instanceId,
            identity.lifecycleEpoch,
            _document.collaborationRevisions().capture(keys)};
}

PreparedEdit DocumentCollaborationService::prepareEdit(
    const std::string& sessionId,
    std::string operationId,
    const CollaborativeOperationIntent& intent,
    std::string provenance)
{
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "off-owner collaboration preparation requires a document-thread dispatcher");
    }
    return invokeOnDocumentThread<PreparedEdit>(
        [this,
         sessionId,
         operationId = std::move(operationId),
         &intent,
         provenance = std::move(provenance)]() mutable {
            return prepareEditOnDocumentThread(sessionId,
                                               std::move(operationId),
                                               intent,
                                               std::move(provenance));
        });
}

PreparedEdit DocumentCollaborationService::prepareEditOnDocumentThread(
    const std::string& sessionId,
    std::string operationId,
    const CollaborativeOperationIntent& intent,
    std::string provenance)
{
    if (!_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "collaboration preparation was not dispatched to the document owner thread");
    }
    std::lock_guard lock(_document.collaborationCommitMutex());
    if (_document.collaborationStableReadBlocked()) {
        throw Base::RuntimeError(
            "collaboration preparation requires a stable document boundary");
    }
    const EditSession session = requireActiveSession(sessionId);
    const auto identity = _document.collaborationIdentity();
    if (identity.state != DocumentLifecycleState::Live
        || identity.instanceId != session.documentInstanceId()) {
        throw Base::RuntimeError("edit session targets a stale document instance");
    }
    if (!_document.collaborationPreparationSupported()) {
        throw Base::RuntimeError(
            "document contains a mutable Python payload that cannot be prepared");
    }

    auto preparation =
        CollaborativeOperationRegistry::instance().prepare(_document, intent);
    std::vector<DocumentRevisionKey> dependencyUnion = preparation.readSet;
    dependencyUnion.insert(dependencyUnion.end(),
                           preparation.writeSet.begin(),
                           preparation.writeSet.end());
    std::sort(dependencyUnion.begin(),
              dependencyUnion.end(),
              [](const auto& left, const auto& right) { return left < right; });
    dependencyUnion.erase(std::unique(dependencyUnion.begin(), dependencyUnion.end()),
                          dependencyUnion.end());
    auto expected = _document.collaborationRevisions().capture(dependencyUnion);

    return PreparedEdit(PreparedEdit::ConstructionKey {},
                        preparation.registrationId,
                        std::move(operationId),
                        identity.instanceId,
                        identity.lifecycleEpoch,
                        intent.operationType,
                        std::move(expected),
                        std::move(preparation.readSet),
                        std::move(preparation.writeSet),
                        std::move(preparation.publicationEffects),
                        std::move(provenance),
                        std::move(preparation.operation));
}

DocumentCommitResult DocumentCollaborationService::commitEdit(const std::string& sessionId,
                                                               const PreparedEdit& edit)
{
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        return _coordinator.commit(edit);
    }
    return invokeOnDocumentThread<DocumentCommitResult>(
        [this, sessionId, &edit] { return commitEditOnDocumentThread(sessionId, edit); });
}

DocumentCommitResult DocumentCollaborationService::commitEditOnDocumentThread(
    const std::string& sessionId,
    const PreparedEdit& edit)
{
    if (!_document.isCollaborationOwnerThread()) {
        return rejectedCommit(DocumentCommitStatus::Unsupported,
                              edit,
                              "collaboration commit was not dispatched to the document owner thread");
    }
    std::lock_guard lock(_document.collaborationCommitMutex());
    const auto session = sessionStatus(sessionId);
    if (!session) {
        return rejectedCommit(DocumentCommitStatus::InvalidPreparedEdit,
                              edit,
                              "unknown edit session");
    }
    if (session->documentInstanceId() != edit.documentInstanceId()) {
        return rejectedCommit(DocumentCommitStatus::StaleDocument,
                              edit,
                              "edit session and prepared edit target different documents");
    }
    if (session->status() != EditSessionStatus::Active) {
        return rejectedCommit(DocumentCommitStatus::Cancelled,
                              edit,
                              session->cancellationReason().value_or("edit session is cancelled"));
    }
    if (!CollaborativeOperationRegistry::instance().matches(
            edit.adapterRegistrationId(), edit.operationType())) {
        return rejectedCommit(DocumentCommitStatus::InvalidPreparedEdit,
                              edit,
                              "prepared edit adapter registration is no longer trusted");
    }
    return _coordinator.commit(edit);
}
