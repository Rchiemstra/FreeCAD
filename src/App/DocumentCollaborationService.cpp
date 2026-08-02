// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCollaborationService.h"

#include "Application.h"
#include "CollaborativeSetPropertyOperation.h"
#include "Document.h"
#include "DocumentObject.h"
#include "MainThreadSignal.h"

#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Base/Uuid.h>

#include <boost/scope_exit.hpp>

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

App::DocumentCommitResult rejectedCompatibilityCommit(App::DocumentCommitStatus status,
                                                       std::string operationId,
                                                       std::string message)
{
    App::DocumentCommitResult result;
    result.status = status;
    result.operationId = std::move(operationId);
    result.message = std::move(message);
    return result;
}

class CompatibilityMutationOperation final : public App::CollaborativeOperation
{
public:
    explicit CompatibilityMutationOperation(App::CollaborationCompatibilityCallback callback)
        : _callback(std::move(callback))
    {}

    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return "App.LegacyCompatibilityMutation.v1";
    }

    void apply(App::Document&) const override
    {
        _callback();
    }

    [[nodiscard]] App::CollaborativePostconditionResult
    checkPostcondition(const App::Document&) const override
    {
        return {true, {}};
    }

private:
    App::CollaborationCompatibilityCallback _callback;
};

}  // namespace

using namespace App;

class DocumentCollaborationService::LifecyclePin
{
public:
    explicit LifecyclePin(const DocumentCollaborationService& service)
        : _gate(service.lifetimeGate())
    {
        if (!_gate) {
            return;
        }
        {
            std::lock_guard lock(_gate->mutex);
            if (_gate->sealed) {
                return;
            }
            ++_gate->activeAccesses;
            ++_gate->accessOwners[std::this_thread::get_id()];
        }
        _pinned = true;
        if (const auto hook = _postLifecycleAdmissionTestHook.load(
                std::memory_order_acquire)) {
            hook();
        }
    }

    ~LifecyclePin()
    {
        if (_pinned) {
            std::lock_guard lock(_gate->mutex);
            const auto owner = _gate->accessOwners.find(std::this_thread::get_id());
            if (owner == _gate->accessOwners.end() || _gate->activeAccesses == 0) {
                return;
            }
            if (--owner->second == 0) {
                _gate->accessOwners.erase(owner);
            }
            --_gate->activeAccesses;
            _gate->changed.notify_all();
        }
    }

    LifecyclePin(const LifecyclePin&) = delete;
    LifecyclePin& operator=(const LifecyclePin&) = delete;

    explicit operator bool() const noexcept
    {
        return _pinned;
    }

private:
    std::shared_ptr<Internal::CollaborationServiceLifetimeGate> _gate;
    bool _pinned {false};
};

DocumentCollaborationService::LifecyclePin
DocumentCollaborationService::pinDocumentAccess() const
{
    return LifecyclePin(*this);
}

std::shared_ptr<Internal::CollaborationServiceLifetimeGate>
DocumentCollaborationService::lifetimeGate() const
{
    return GetApplication().collaborationServiceLifetimeGate(*this);
}

DocumentCollaborationService::DocumentCollaborationService(Document& document)
    : _document(document)
    , _coordinator(document)
{
    ensureCollaborativeSetPropertyOperationRegistered();
    GetApplication().registerCollaborationServiceLifetime(*this);
}

DocumentCollaborationService::~DocumentCollaborationService()
{
    std::vector<PreparedEditExecutionId> preparationIds;
    {
        std::lock_guard lock(_preparationMutex);
        preparationIds.reserve(_pendingDetachedPreparations.size());
        for (const auto& [executionId, pending] : _pendingDetachedPreparations) {
            static_cast<void>(pending);
            preparationIds.push_back(executionId);
        }
        _pendingDetachedPreparations.clear();
    }

    auto& executor = GetApplication().preparedEditExecutor();
    for (const auto executionId : preparationIds) {
        static_cast<void>(executor.abandon(executionId));
    }
}

Document& DocumentCollaborationService::document() const noexcept
{
    return _document;
}

EditSession DocumentCollaborationService::beginEditSession(std::string actorId)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        throw Base::RuntimeError("cannot begin an edit session while document is closing");
    }
    if (actorId.empty()) {
        throw std::invalid_argument("edit session actor identity is required");
    }
    DocumentIdentity identity;
    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        identity = _document.collaborationIdentity();
        if (identity.state != DocumentLifecycleState::Live) {
            throw Base::RuntimeError(
                "cannot begin an edit session for a non-live document");
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };

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
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return std::nullopt;
    }
    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        if (_document.collaborationIdentity().state != DocumentLifecycleState::Live
            || _document.collaborationNotificationsReplaying()) {
            return std::nullopt;
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };
    std::lock_guard lock(_sessionMutex);
    const auto found = _sessions.find(sessionId);
    if (found == _sessions.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool DocumentCollaborationService::cancelEdit(const std::string& sessionId, std::string reason)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return false;
    }
    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        if (_document.collaborationIdentity().state != DocumentLifecycleState::Live
            || _document.collaborationNotificationsReplaying()) {
            return false;
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };
    if (reason.empty()) {
        reason = "cancelled by caller";
    }
    {
        std::lock_guard lock(_sessionMutex);
        const auto found = _sessions.find(sessionId);
        if (found == _sessions.end()) {
            return false;
        }
        found->second._status = EditSessionStatus::Cancelled;
        found->second._cancellationReason = std::move(reason);
    }
    if (const auto hook = _postCancelSessionTestHook.load(std::memory_order_acquire)) {
        hook();
    }

    std::vector<PreparedEditExecutionId> preparationIds;
    {
        std::lock_guard lock(_preparationMutex);
        for (const auto& [executionId, pending] : _pendingDetachedPreparations) {
            if (pending.sessionId == sessionId) {
                preparationIds.push_back(executionId);
            }
        }
    }
    for (const auto executionId : preparationIds) {
        static_cast<void>(GetApplication().preparedEditExecutor().cancel(executionId));
    }
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
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        throw Base::RuntimeError("cannot capture a collaboration snapshot while document is closing");
    }
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
    _document.beginCollaborationStableReadCapture();
    BOOST_SCOPE_EXIT_ALL(&) {
        _document.finishCollaborationStableReadCapture();
    };
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
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        throw Base::RuntimeError("cannot prepare collaboration work while document is closing");
    }
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
    _document.beginCollaborationStableReadCapture();
    BOOST_SCOPE_EXIT_ALL(&) {
        _document.finishCollaborationStableReadCapture();
    };
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
    if (preparation.isDetached()) {
        throw Base::RuntimeError(
            "operation requires detached preparation through the asynchronous executor");
    }
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

PreparedEditExecutionId DocumentCollaborationService::prepareEditAsync(
    const std::string& sessionId,
    std::string operationId,
    const CollaborativeOperationIntent& intent,
    std::string provenance)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        throw Base::RuntimeError(
            "cannot prepare detached collaboration work while document is closing");
    }
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "off-owner detached preparation requires a document-thread dispatcher");
    }
    return invokeOnDocumentThread<PreparedEditExecutionId>(
        [this,
         sessionId,
         operationId = std::move(operationId),
         &intent,
         provenance = std::move(provenance)]() mutable {
            return prepareEditAsyncOnDocumentThread(sessionId,
                                                    std::move(operationId),
                                                    intent,
                                                    std::move(provenance));
        });
}

PreparedEditExecutionId DocumentCollaborationService::prepareEditAsyncOnDocumentThread(
    const std::string& sessionId,
    std::string operationId,
    const CollaborativeOperationIntent& intent,
    std::string provenance)
{
    if (!_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "detached preparation was not dispatched to the document owner thread");
    }

    PendingDetachedPreparation pending;
    CollaborativeOperationPreparation::DetachedTask detachedTask;
    bool lifecyclePinned = false;
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        if (_document.collaborationStableReadBlocked()) {
            throw Base::RuntimeError(
                "detached preparation requires a stable document boundary");
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
        const EditSession session = requireActiveSession(sessionId);
        const auto identity = _document.collaborationIdentity();
        if (identity.state != DocumentLifecycleState::Live
            || identity.instanceId != session.documentInstanceId()) {
            throw Base::RuntimeError("edit session targets a stale document instance");
        }
        if (!_document.collaborationPreparationSupported()) {
            throw Base::RuntimeError(
                "document contains a mutable Python payload that cannot be prepared off-thread");
        }

        auto preparation =
            CollaborativeOperationRegistry::instance().prepare(_document, intent);
        if (!preparation.isDetached()) {
            throw Base::RuntimeError(
                "operation does not provide detached preparation");
        }

        std::vector<DocumentRevisionKey> dependencyUnion = preparation.readSet;
        dependencyUnion.insert(dependencyUnion.end(),
                               preparation.writeSet.begin(),
                               preparation.writeSet.end());
        std::sort(dependencyUnion.begin(),
                  dependencyUnion.end(),
                  [](const auto& left, const auto& right) { return left < right; });
        dependencyUnion.erase(
            std::unique(dependencyUnion.begin(), dependencyUnion.end()),
            dependencyUnion.end());

        pending.sessionId = sessionId;
        pending.adapterRegistrationId = preparation.registrationId;
        pending.operationId = std::move(operationId);
        pending.documentInstanceId = identity.instanceId;
        pending.lifecycleEpoch = identity.lifecycleEpoch;
        pending.operationType = intent.operationType;
        pending.expectedRevisions =
            _document.collaborationRevisions().capture(dependencyUnion);
        pending.readSet = std::move(preparation.readSet);
        pending.writeSet = std::move(preparation.writeSet);
        pending.publicationEffects = std::move(preparation.publicationEffects);
        pending.provenance = std::move(provenance);
        if (pending.adapterRegistrationId == 0) {
            throw std::invalid_argument(
                "detached preparation adapter registration must be nonzero");
        }
        auto canonical = validatePreparedEditMetadata(
            pending.operationId,
            pending.documentInstanceId,
            pending.lifecycleEpoch,
            pending.operationType,
            std::move(pending.expectedRevisions),
            std::move(pending.readSet),
            std::move(pending.writeSet),
            std::move(pending.publicationEffects),
            pending.provenance);
        pending.expectedRevisions = std::move(canonical.expectedRevisions);
        pending.readSet = std::move(canonical.readSet);
        pending.writeSet = std::move(canonical.writeSet);
        pending.publicationEffects = std::move(canonical.publicationEffects);
        detachedTask = std::move(preparation.detachedTask);
    }

    // Never acquire the executor queue while the document commit mutex is held.
    auto& executor = GetApplication().preparedEditExecutor();
    const auto executionId = executor.submit(std::move(detachedTask));
    if (const auto hook = _postSubmitTestHook.load(std::memory_order_acquire)) {
        hook();
    }
    try {
        std::lock_guard lock(_preparationMutex);
        const auto [position, inserted] =
            _pendingDetachedPreparations.emplace(executionId, std::move(pending));
        if (!inserted) {
            throw Base::RuntimeError("duplicate detached preparation identity");
        }
    }
    catch (...) {
        static_cast<void>(executor.abandon(executionId));
        throw;
    }

    const auto currentSession = sessionStatus(sessionId);
    if (!currentSession || currentSession->status() != EditSessionStatus::Active) {
        static_cast<void>(executor.cancel(executionId));
    }
    return executionId;
}

std::optional<PreparedEditExecutionSnapshot>
DocumentCollaborationService::preparedEditStatus(
    const PreparedEditExecutionId executionId) const
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return std::nullopt;
    }
    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        if (_document.collaborationIdentity().state != DocumentLifecycleState::Live
            || _document.collaborationNotificationsReplaying()) {
            return std::nullopt;
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };
    {
        std::lock_guard lock(_preparationMutex);
        if (!_pendingDetachedPreparations.contains(executionId)) {
            return std::nullopt;
        }
    }
    return GetApplication().preparedEditExecutor().status(executionId);
}

bool DocumentCollaborationService::cancelPreparedEdit(
    const PreparedEditExecutionId executionId)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return false;
    }
    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        if (_document.collaborationIdentity().state != DocumentLifecycleState::Live
            || _document.collaborationNotificationsReplaying()) {
            return false;
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };
    {
        std::lock_guard lock(_preparationMutex);
        if (!_pendingDetachedPreparations.contains(executionId)) {
            return false;
        }
    }
    return GetApplication().preparedEditExecutor().cancel(executionId);
}

std::optional<CollaborationPreparedEditResult>
DocumentCollaborationService::takePreparedEdit(
    const std::string& sessionId,
    const PreparedEditExecutionId executionId)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return std::nullopt;
    }
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "off-owner detached result collection requires a document-thread dispatcher");
    }
    return invokeOnDocumentThread<std::optional<CollaborationPreparedEditResult>>(
        [this, sessionId, executionId] {
            return takePreparedEditOnDocumentThread(sessionId, executionId);
        });
}

std::optional<CollaborationPreparedEditResult>
DocumentCollaborationService::takePreparedEditOnDocumentThread(
    const std::string& sessionId,
    const PreparedEditExecutionId executionId)
{
    if (!_document.isCollaborationOwnerThread()) {
        throw Base::RuntimeError(
            "detached result collection was not dispatched to the document owner thread");
    }

    bool lifecyclePinned = false;
    {
        std::lock_guard lock(_document.collaborationCommitMutex());
        const auto identity = _document.collaborationIdentity();
        if (identity.state != DocumentLifecycleState::Live
            || _document.collaborationStableReadBlocked()) {
            return std::nullopt;
        }
        _document.beginCollaborationStableReadCapture();
        lifecyclePinned = true;
    }
    BOOST_SCOPE_EXIT_ALL(&) {
        if (lifecyclePinned) {
            _document.finishCollaborationStableReadCapture();
        }
    };

    PendingDetachedPreparation pending;
    std::optional<PreparedEditExecutionResult> terminal;
    {
        std::lock_guard lock(_preparationMutex);
        const auto found = _pendingDetachedPreparations.find(executionId);
        if (found == _pendingDetachedPreparations.end()) {
            return std::nullopt;
        }
        if (found->second.sessionId != sessionId) {
            throw std::invalid_argument(
                "detached preparation belongs to a different edit session");
        }
        if (found->second.collecting) {
            return std::nullopt;
        }
        found->second.collecting = true;
    }

    // Executor locks and the document-service mutex must never be nested.
    terminal = GetApplication().preparedEditExecutor().takeResult(executionId);
    if (const auto hook = _postTakeResultTestHook.load(std::memory_order_acquire)) {
        hook();
    }
    {
        std::lock_guard lock(_preparationMutex);
        const auto found = _pendingDetachedPreparations.find(executionId);
        if (found == _pendingDetachedPreparations.end()) {
            return std::nullopt;
        }
        if (!terminal) {
            found->second.collecting = false;
            return std::nullopt;
        }
        pending = std::move(found->second);
        _pendingDetachedPreparations.erase(found);
    }

    CollaborationPreparedEditResult result;
    result.executionId = executionId;
    result.status = terminal->status;
    result.diagnostic = std::move(terminal->diagnostic);
    if (terminal->status != PreparedEditExecutionStatus::Completed) {
        return result;
    }

    std::lock_guard lock(_document.collaborationCommitMutex());
    const auto session = sessionStatus(sessionId);
    if (!session || session->status() != EditSessionStatus::Active) {
        result.status = PreparedEditExecutionStatus::Cancelled;
        result.diagnostic = session
            ? session->cancellationReason().value_or("edit session is cancelled")
            : "edit session no longer exists";
        return result;
    }
    const auto identity = _document.collaborationIdentity();
    if (identity.state != DocumentLifecycleState::Live
        || identity.instanceId != pending.documentInstanceId
        || identity.lifecycleEpoch != pending.lifecycleEpoch
        || session->documentInstanceId() != pending.documentInstanceId) {
        result.status = PreparedEditExecutionStatus::Failed;
        result.diagnostic = "detached preparation targets a stale document instance";
        return result;
    }
    if (!CollaborativeOperationRegistry::instance().matches(
            pending.adapterRegistrationId, pending.operationType)) {
        result.status = PreparedEditExecutionStatus::Failed;
        result.diagnostic = "detached adapter registration is no longer trusted";
        return result;
    }
    if (!terminal->operation
        || terminal->operation->typeId() != pending.operationType) {
        result.status = PreparedEditExecutionStatus::Failed;
        result.diagnostic = "detached task returned a mismatched operation type";
        return result;
    }

    try {
        result.preparedEdit.reset(new PreparedEdit(
            PreparedEdit::ConstructionKey {},
            pending.adapterRegistrationId,
            std::move(pending.operationId),
            pending.documentInstanceId,
            pending.lifecycleEpoch,
            std::move(pending.operationType),
            std::move(pending.expectedRevisions),
            std::move(pending.readSet),
            std::move(pending.writeSet),
            std::move(pending.publicationEffects),
            std::move(pending.provenance),
            std::move(terminal->operation)));
    }
    catch (const std::exception& error) {
        result.status = PreparedEditExecutionStatus::Failed;
        result.diagnostic =
            std::string("detached prepared edit validation failed: ") + error.what();
    }
    return result;
}

DocumentCommitResult DocumentCollaborationService::commitEdit(const std::string& sessionId,
                                                               const PreparedEdit& edit)
{
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return rejectedCommit(DocumentCommitStatus::StaleDocument,
                              edit,
                              "document close has sealed collaboration access");
    }
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
    if (_document.collaborationNotificationsReplaying()) {
        return rejectedCommit(DocumentCommitStatus::Busy,
                              edit,
                              "a committed collaboration boundary is notifying observers");
    }
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

DocumentCommitResult DocumentCollaborationService::commitCompatibilityMutation(
    CollaborationCompatibilityMutation mutation,
    CollaborationCompatibilityCallback callback)
{
    const std::string rejectedOperationId = "legacy-compatibility";
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::StaleDocument,
            rejectedOperationId,
            "document close has sealed compatibility mutation access");
    }
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::Unsupported,
            rejectedOperationId,
            "off-owner compatibility mutation requires a document-thread dispatcher");
    }
    return invokeOnDocumentThread<DocumentCommitResult>(
        [this, mutation = std::move(mutation), callback = std::move(callback)]() mutable {
            return commitCompatibilityMutationOnDocumentThread(std::move(mutation),
                                                               std::move(callback));
        });
}

DocumentCommitResult
DocumentCollaborationService::commitCompatibilityMutationOnDocumentThread(
    CollaborationCompatibilityMutation mutation,
    CollaborationCompatibilityCallback callback)
{
    const std::string rejectedOperationId = "legacy-compatibility";
    if (!_document.isCollaborationOwnerThread()) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::Unsupported,
            rejectedOperationId,
            "compatibility mutation was not dispatched to the document owner thread");
    }
    if (!callback) {
        return rejectedCompatibilityCommit(DocumentCommitStatus::InvalidPreparedEdit,
                                           rejectedOperationId,
                                           "compatibility mutation callback is required");
    }

    std::lock_guard lock(_document.collaborationCommitMutex());
    const auto identity = _document.collaborationIdentity();
    if (identity.state != DocumentLifecycleState::Live) {
        return rejectedCompatibilityCommit(DocumentCommitStatus::StaleDocument,
                                           rejectedOperationId,
                                           "compatibility mutation targets a non-live document");
    }

    std::vector<DocumentRevisionPublicationRequest> effects {
        {DocumentRevisionKey::unknownModelMutation(), std::nullopt},
    };
    switch (mutation.scope) {
        case CollaborationCompatibilityScope::ObjectModel: {
            if (mutation.objectName.empty() || mutation.stableObjectIdentity.empty()) {
                return rejectedCompatibilityCommit(
                    DocumentCommitStatus::InvalidPreparedEdit,
                    rejectedOperationId,
                    "object compatibility mutation requires name and stable identity");
            }
            const auto* object = _document.getObject(mutation.objectName.c_str());
            if (!object
                || _document.collaborationObjectIdentity(*object)
                    != mutation.stableObjectIdentity) {
                return rejectedCompatibilityCommit(
                    DocumentCommitStatus::StaleDocument,
                    rejectedOperationId,
                    "object compatibility mutation targets a stale object identity");
            }
            effects.push_back({DocumentRevisionKey::objectModel(mutation.objectName),
                               mutation.stableObjectIdentity});
            break;
        }
        case CollaborationCompatibilityScope::UnknownModel:
            if (!mutation.objectName.empty() || !mutation.stableObjectIdentity.empty()) {
                return rejectedCompatibilityCommit(
                    DocumentCommitStatus::InvalidPreparedEdit,
                    rejectedOperationId,
                    "unknown compatibility mutation cannot declare object scope");
            }
            break;
        default:
            return rejectedCompatibilityCommit(DocumentCommitStatus::InvalidPreparedEdit,
                                               rejectedOperationId,
                                               "unknown compatibility mutation scope");
    }

    std::sort(effects.begin(), effects.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });
    std::vector<DocumentRevisionKey> writeSet;
    writeSet.reserve(effects.size());
    for (const auto& effect : effects) {
        writeSet.push_back(effect.key);
    }
    const auto expected = _document.collaborationRevisions().capture(writeSet);
    const std::string operationId = Base::Uuid::createUuid();
    auto operation = std::make_unique<CompatibilityMutationOperation>(std::move(callback));
    const std::string operationType(operation->typeId());
    PreparedEdit edit(PreparedEdit::ConstructionKey {},
                      1,
                      operationId,
                      identity.instanceId,
                      identity.lifecycleEpoch,
                      operationType,
                      expected,
                      {},
                      writeSet,
                      std::move(effects),
                      "legacy-gui-compatibility",
                      std::move(operation));
    return _coordinator.commitCompatibility(edit);
}

DocumentCommitResult DocumentCollaborationService::serializeCompatibilityCallback(
    CollaborationCompatibilityCallback callback)
{
    const std::string operationId = "serialized-gui-compatibility";
    auto lifecyclePin = pinDocumentAccess();
    if (!lifecyclePin) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::StaleDocument,
            operationId,
            "document close has sealed serialized compatibility access");
    }
    if (!MainThreadSignalConfig::hasHooks() && !_document.isCollaborationOwnerThread()) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::Unsupported,
            operationId,
            "off-owner serialized compatibility requires a document-thread dispatcher");
    }
    return invokeOnDocumentThread<DocumentCommitResult>(
        [this, callback = std::move(callback)]() mutable {
            return serializeCompatibilityCallbackOnDocumentThread(std::move(callback));
        });
}

DocumentCommitResult
DocumentCollaborationService::serializeCompatibilityCallbackOnDocumentThread(
    CollaborationCompatibilityCallback callback)
{
    const std::string operationId = "serialized-gui-compatibility";
    if (!_document.isCollaborationOwnerThread()) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::Unsupported,
            operationId,
            "serialized compatibility was not dispatched to the document owner thread");
    }
    if (!callback) {
        return rejectedCompatibilityCommit(DocumentCommitStatus::InvalidPreparedEdit,
                                           operationId,
                                           "serialized compatibility callback is required");
    }

    std::lock_guard lock(_document.collaborationCommitMutex());
    if (_document.collaborationIdentity().state != DocumentLifecycleState::Live) {
        return rejectedCompatibilityCommit(DocumentCommitStatus::StaleDocument,
                                           operationId,
                                           "serialized compatibility targets a non-live document");
    }
    if (_document.collaborationNotificationsReplaying()) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::Busy,
            operationId,
            "a committed collaboration boundary is notifying observers");
    }
    try {
        callback();
    }
    catch (const Base::Exception& exception) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::ApplyFailed,
            operationId,
            std::string("serialized compatibility callback failed: ") + exception.what());
    }
    catch (const std::exception& exception) {
        return rejectedCompatibilityCommit(
            DocumentCommitStatus::ApplyFailed,
            operationId,
            std::string("serialized compatibility callback failed: ") + exception.what());
    }
    catch (...) {
        return rejectedCompatibilityCommit(DocumentCommitStatus::ApplyFailed,
                                           operationId,
                                           "unknown serialized compatibility callback failure");
    }
    return rejectedCompatibilityCommit(DocumentCommitStatus::Committed,
                                       operationId,
                                       "compatibility callback serialized without model revision");
}
