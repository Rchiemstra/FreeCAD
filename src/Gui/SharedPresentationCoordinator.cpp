// SPDX-License-Identifier: LGPL-2.1-or-later

#include "SharedPresentationCoordinator.h"

#include <Base/Exception.h>

#include <exception>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace
{

using PresentationKeySet = std::unordered_set<Gui::SharedPresentationRevisionKey,
                                              Gui::SharedPresentationRevisionKeyHash>;
using AppKeySet =
    std::unordered_set<App::DocumentRevisionKey, App::DocumentRevisionKeyHash>;

struct SharedPresentationCommitProtocolState
{
    std::mutex mutex;
    bool acceptingCallbacks {true};
    std::size_t workInvocations {0};
    std::size_t completionInvocations {0};
    Gui::SharedPresentationRevisionIndex* revisions {nullptr};
    const Gui::SharedPresentationCommitRequest* request {nullptr};
    const Gui::SharedPresentationCommitCallbacks* callbacks {nullptr};
    const Gui::SharedPresentationCoordinator* coordinator {nullptr};
    std::optional<Gui::SharedPresentationCommitResult> preparedResult;
    std::optional<Gui::SharedPresentationPublicationReservation> pendingPublication;
    std::optional<Gui::SharedPresentationCommitResult> authoritativeResult;
};

static_assert(std::is_nothrow_move_constructible_v<Gui::SharedPresentationPublication>);
static_assert(std::is_nothrow_move_constructible_v<Gui::SharedPresentationCommitResult>);

void validateKey(const Gui::SharedPresentationRevisionKey& key)
{
    if (!key.valid()) {
        throw std::invalid_argument(
            "shared-presentation key requires stable object identity and property name");
    }
}

std::vector<Gui::SharedPresentationRevisionKey> distinctKeys(
    const std::vector<Gui::SharedPresentationRevisionKey>& keys,
    bool allowEmpty)
{
    if (!allowEmpty && keys.empty()) {
        throw std::invalid_argument("shared-presentation publication cannot be empty");
    }

    PresentationKeySet seen;
    seen.reserve(keys.size());
    std::vector<Gui::SharedPresentationRevisionKey> result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        validateKey(key);
        if (!seen.insert(key).second) {
            throw std::invalid_argument(
                "shared-presentation keys must not contain duplicates");
        }
        result.push_back(key);
    }
    return result;
}

std::optional<std::string> validateRequest(
    const Gui::SharedPresentationCommitRequest& request)
{
    if (request.document.documentInstanceId == 0 || request.document.lifecycleEpoch == 0) {
        return "shared-presentation request requires a nonzero document instance and epoch";
    }

    AppKeySet appKeys;
    appKeys.reserve(request.expectedAppRevisions.size());
    for (const auto& observation : request.expectedAppRevisions) {
        if (!observation.key.valid()) {
            return "shared-presentation request contains an invalid App revision key";
        }
        if (!appKeys.insert(observation.key).second) {
            return "shared-presentation request contains duplicate App observations";
        }
    }

    PresentationKeySet observedKeys;
    observedKeys.reserve(request.expectedPresentationRevisions.size());
    for (const auto& observation : request.expectedPresentationRevisions) {
        if (!observation.key.valid()) {
            return "shared-presentation request contains an invalid presentation key";
        }
        if (observation.document.documentInstanceId == 0
            || observation.document.lifecycleEpoch == 0) {
            return "shared-presentation observation requires a nonzero document binding";
        }
        if (!observedKeys.insert(observation.key).second) {
            return "shared-presentation request contains duplicate presentation observations";
        }
    }

    if (request.presentationWrites.empty()) {
        return "shared-presentation request must publish at least one property write";
    }
    PresentationKeySet writes;
    writes.reserve(request.presentationWrites.size());
    for (const auto& key : request.presentationWrites) {
        if (!key.valid()) {
            return "shared-presentation request contains an invalid presentation write";
        }
        if (!writes.insert(key).second) {
            return "shared-presentation request contains duplicate presentation writes";
        }
        if (observedKeys.find(key) == observedKeys.end()) {
            return "every shared-presentation write must have a declared observation";
        }
    }
    return std::nullopt;
}

Gui::SharedPresentationCommitResult makeResult(
    Gui::SharedPresentationCommitStatus status,
    std::string diagnostic)
{
    Gui::SharedPresentationCommitResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    return result;
}

std::string stageFailure(const char* stage, const char* detail)
{
    std::string diagnostic(stage);
    diagnostic += ": ";
    diagnostic += detail && *detail ? detail : "unknown failure";
    return diagnostic;
}

struct StepInvocationResult
{
    Gui::SharedPresentationStepResult reported;
    std::exception_ptr exception;
};

StepInvocationResult invokeStep(const Gui::SharedPresentationStep& step) noexcept
{
    try {
        return {step(), {}};
    }
    catch (...) {
        return {{false, {}}, std::current_exception()};
    }
}

std::string stepFailureDiagnostic(StepInvocationResult&& invocation, const char* stage)
{
    if (!invocation.exception) {
        return invocation.reported.diagnostic.empty()
            ? stageFailure(stage, "callback reported failure")
            : std::move(invocation.reported.diagnostic);
    }

    try {
        std::rethrow_exception(invocation.exception);
    }
    catch (const Base::Exception& exception) {
        return stageFailure(stage, exception.what());
    }
    catch (const std::exception& exception) {
        return stageFailure(stage, exception.what());
    }
    catch (...) {
        return stageFailure(stage, "unknown exception");
    }
}

void appendRollbackDiagnostic(std::vector<std::string>& diagnostics,
                              const char* domain,
                              std::string_view detail) noexcept
{
    try {
        std::string diagnostic(domain);
        diagnostic += " rollback failed";
        if (!detail.empty()) {
            diagnostic += ": ";
            diagnostic += detail;
        }
        diagnostics.push_back(std::move(diagnostic));
    }
    catch (...) {
        // A diagnostic allocation failure must never replace the primary
        // App/GUI/postcondition failure that caused rollback.
    }
}

bool runRollback(const Gui::SharedPresentationStep& rollback,
                 const char* domain,
                 std::vector<std::string>& diagnostics) noexcept
{
    try {
        const auto result = rollback();
        if (!result.succeeded) {
            appendRollbackDiagnostic(diagnostics, domain, result.diagnostic);
            return false;
        }
        return true;
    }
    catch (const Base::Exception& exception) {
        appendRollbackDiagnostic(diagnostics, domain, exception.what());
        return false;
    }
    catch (const std::exception& exception) {
        appendRollbackDiagnostic(diagnostics, domain, exception.what());
        return false;
    }
    catch (...) {
        appendRollbackDiagnostic(diagnostics, domain, "unknown exception");
        return false;
    }
}

bool runBothRollbacks(const Gui::SharedPresentationCommitCallbacks& callbacks,
                      std::vector<std::string>& diagnostics) noexcept
{
    const bool guiRestored =
        runRollback(callbacks.rollbackGuiMutation, "GUI", diagnostics);
    const bool appRestored =
        runRollback(callbacks.rollbackAppMutation, "App", diagnostics);
    return guiRestored && appRestored;
}

void appendNonfatalDiagnostic(std::string& diagnostic, std::string_view detail) noexcept
{
    if (detail.empty()) {
        return;
    }
    try {
        if (!diagnostic.empty()) {
            diagnostic += "; ";
        }
        diagnostic += detail;
    }
    catch (...) {
        // Once publication succeeds, diagnostic allocation must not turn the
        // authoritative result into an exception or apparent failure.
    }
}

void appendNonfatalFailure(std::string& diagnostic,
                           const char* stage,
                           const char* detail) noexcept
{
    try {
        const auto formatted = stageFailure(stage, detail);
        appendNonfatalDiagnostic(diagnostic, formatted);
    }
    catch (...) {
        // Preserve the authoritative result if formatting itself exhausts
        // memory after successful publication.
    }
}

}  // namespace

using namespace Gui;

bool SharedPresentationRevisionKey::valid() const noexcept
{
    return !stableObjectIdentity.empty() && !propertyName.empty();
}

std::size_t SharedPresentationRevisionKeyHash::operator()(
    const SharedPresentationRevisionKey& key) const noexcept
{
    const auto objectHash = std::hash<std::string> {}(key.stableObjectIdentity);
    const auto propertyHash = std::hash<std::string> {}(key.propertyName);
    return objectHash
        ^ (propertyHash + 0x9e3779b9U + (objectHash << 6U) + (objectHash >> 2U));
}

SharedPresentationRevisionIndex::SharedPresentationRevisionIndex()
    : SharedPresentationRevisionIndex(
          std::numeric_limits<SharedPresentationRevision>::max(),
          std::numeric_limits<SharedPresentationPublicationSequence>::max())
{}

SharedPresentationRevisionIndex::SharedPresentationRevisionIndex(
    SharedPresentationRevision maximumRevision)
    : SharedPresentationRevisionIndex(
          maximumRevision,
          std::numeric_limits<SharedPresentationPublicationSequence>::max())
{}

SharedPresentationRevisionIndex::SharedPresentationRevisionIndex(
    SharedPresentationRevision maximumRevision,
    SharedPresentationPublicationSequence maximumPublicationSequence)
    : _maximumRevision(maximumRevision)
    , _maximumPublicationSequence(maximumPublicationSequence)
{}

void SharedPresentationRevisionIndex::bindDocumentIdentity(
    App::DocumentInstanceId documentInstanceId,
    App::DocumentLifecycleEpoch lifecycleEpoch)
{
    if (documentInstanceId == 0 || lifecycleEpoch == 0) {
        throw std::invalid_argument(
            "shared-presentation identity values must be nonzero");
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        _documentIdentity =
            App::DocumentRevisionIdentityBinding {documentInstanceId, lifecycleEpoch};
        return;
    }
    if (_documentIdentity->documentInstanceId != documentInstanceId) {
        throw std::logic_error(
            "shared-presentation index cannot be rebound to another document instance");
    }
    if (lifecycleEpoch < _documentIdentity->lifecycleEpoch) {
        throw std::invalid_argument(
            "shared-presentation document lifecycle epoch cannot rewind");
    }
    _documentIdentity->lifecycleEpoch = lifecycleEpoch;
}

std::optional<App::DocumentRevisionIdentityBinding>
SharedPresentationRevisionIndex::documentIdentity() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _documentIdentity;
}

SharedPresentationRevision SharedPresentationRevisionIndex::currentLocked(
    const SharedPresentationRevisionKey& key) const noexcept
{
    const auto found = _revisions.find(key);
    return found == _revisions.end() ? 0 : found->second;
}

SharedPresentationRevision SharedPresentationRevisionIndex::current(
    const SharedPresentationRevisionKey& key) const
{
    validateKey(key);
    std::lock_guard<std::mutex> lock(_mutex);
    return currentLocked(key);
}

std::vector<SharedPresentationRevisionObservation>
SharedPresentationRevisionIndex::capture(
    const std::vector<SharedPresentationRevisionKey>& keys) const
{
    const auto validatedKeys = distinctKeys(keys, true);
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        throw std::logic_error(
            "shared-presentation identity must be bound before capture");
    }

    std::vector<SharedPresentationRevisionObservation> observations;
    observations.reserve(validatedKeys.size());
    for (const auto& key : validatedKeys) {
        observations.push_back({*_documentIdentity, key, currentLocked(key)});
    }
    return observations;
}

SharedPresentationValidationResult SharedPresentationRevisionIndex::validate(
    const std::vector<SharedPresentationRevisionObservation>& observations) const
{
    PresentationKeySet seen;
    seen.reserve(observations.size());
    for (const auto& observation : observations) {
        validateKey(observation.key);
        if (!seen.insert(observation.key).second) {
            throw std::invalid_argument(
                "shared-presentation observations must not contain duplicates");
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    SharedPresentationValidationResult result;
    result.currentDocument = _documentIdentity;
    if (!_documentIdentity) {
        result.status = SharedPresentationValidationStatus::Unbound;
        return result;
    }
    if (_poisoned.load(std::memory_order_acquire)) {
        result.status = SharedPresentationValidationStatus::Poisoned;
        return result;
    }
    for (const auto& observation : observations) {
        if (observation.document.documentInstanceId
            != _documentIdentity->documentInstanceId) {
            result.status = SharedPresentationValidationStatus::ForeignDocument;
            return result;
        }
        if (observation.document.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
            result.status = SharedPresentationValidationStatus::EpochMismatch;
            return result;
        }
    }

    result.conflicts.reserve(observations.size());
    for (const auto& observation : observations) {
        const auto revision = currentLocked(observation.key);
        if (revision != observation.revision) {
            result.conflicts.push_back({observation.key, observation.revision, revision});
        }
    }
    result.status = result.conflicts.empty() ? SharedPresentationValidationStatus::Valid
                                             : SharedPresentationValidationStatus::Conflict;
    return result;
}

SharedPresentationPublication SharedPresentationRevisionIndex::publish(
    const std::vector<SharedPresentationRevisionKey>& keys)
{
    auto reservation = reservePublication({}, keys);
    if (!reservation.ready()) {
        if (reservation.status() == SharedPresentationValidationStatus::Poisoned) {
            throw std::logic_error(
                "shared-presentation revision stream is poisoned");
        }
        throw std::logic_error("shared-presentation publication was not ready");
    }
    return reservation.commit();
}

SharedPresentationPublicationSequence
SharedPresentationRevisionIndex::latestPublicationSequence() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _publicationSequence;
}

bool SharedPresentationRevisionIndex::poisoned() const
{
    return _poisoned.load(std::memory_order_acquire);
}

void SharedPresentationRevisionIndex::markInconsistentAfterUnpublishedMutation() noexcept
{
    _poisoned.store(true, std::memory_order_release);
}

SharedPresentationPersistenceCapture
SharedPresentationRevisionIndex::capturePersistence() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        throw std::logic_error(
            "shared-presentation identity must be bound before persistence capture");
    }
    if (_poisoned.load(std::memory_order_acquire)) {
        throw std::logic_error(
            "poisoned shared-presentation state cannot be captured for persistence");
    }
    return {*_documentIdentity, _publicationSequence};
}

SharedPresentationPersistStatus SharedPresentationRevisionIndex::markPersisted(
    const SharedPresentationPersistenceCapture& capture,
    SharedPresentationSaveDisposition disposition)
{
    if (disposition != SharedPresentationSaveDisposition::Succeeded) {
        return SharedPresentationPersistStatus::NotMarked;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        return SharedPresentationPersistStatus::Unbound;
    }
    if (_poisoned.load(std::memory_order_acquire)) {
        return SharedPresentationPersistStatus::Poisoned;
    }
    if (capture.document.documentInstanceId != _documentIdentity->documentInstanceId) {
        return SharedPresentationPersistStatus::ForeignDocument;
    }
    if (capture.document.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
        return SharedPresentationPersistStatus::EpochMismatch;
    }
    if (capture.publicationSequence > _publicationSequence) {
        return SharedPresentationPersistStatus::FutureSequence;
    }
    if (capture.publicationSequence < _persistedPublicationSequence) {
        return SharedPresentationPersistStatus::Rewind;
    }
    _persistedPublicationSequence = capture.publicationSequence;
    return SharedPresentationPersistStatus::Marked;
}

SharedPresentationPersistenceState
SharedPresentationRevisionIndex::persistenceState() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        throw std::logic_error(
            "shared-presentation identity must be bound before persistence state query");
    }
    const bool poisonedState = _poisoned.load(std::memory_order_acquire);
    return {*_documentIdentity,
            _publicationSequence,
            _persistedPublicationSequence,
            poisonedState || _publicationSequence != _persistedPublicationSequence,
            poisonedState};
}

SharedPresentationPublicationReservation
SharedPresentationRevisionIndex::reservePublication(
    const std::vector<SharedPresentationRevisionObservation>& expected,
    const std::vector<SharedPresentationRevisionKey>& changes)
{
    PresentationKeySet expectedKeys;
    expectedKeys.reserve(expected.size());
    for (const auto& observation : expected) {
        validateKey(observation.key);
        if (!expectedKeys.insert(observation.key).second) {
            throw std::invalid_argument(
                "shared-presentation observations must not contain duplicates");
        }
    }
    const auto validatedChanges = distinctKeys(changes, false);

    std::unique_lock<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        lock.unlock();
        return SharedPresentationPublicationReservation(
            nullptr,
            std::move(lock),
            SharedPresentationValidationStatus::Unbound,
            {},
            {},
            {});
    }
    if (_poisoned.load(std::memory_order_acquire)) {
        lock.unlock();
        return SharedPresentationPublicationReservation(
            nullptr,
            std::move(lock),
            SharedPresentationValidationStatus::Poisoned,
            {},
            {},
            {});
    }
    for (const auto& observation : expected) {
        if (observation.document.documentInstanceId
            != _documentIdentity->documentInstanceId) {
            lock.unlock();
            return SharedPresentationPublicationReservation(
                nullptr,
                std::move(lock),
                SharedPresentationValidationStatus::ForeignDocument,
                {},
                {},
                {});
        }
        if (observation.document.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
            lock.unlock();
            return SharedPresentationPublicationReservation(
                nullptr,
                std::move(lock),
                SharedPresentationValidationStatus::EpochMismatch,
                {},
                {},
                {});
        }
    }

    std::vector<SharedPresentationRevisionConflict> conflicts;
    conflicts.reserve(expected.size());
    for (const auto& observation : expected) {
        const auto revision = currentLocked(observation.key);
        if (revision != observation.revision) {
            conflicts.push_back({observation.key, observation.revision, revision});
        }
    }
    if (!conflicts.empty()) {
        lock.unlock();
        return SharedPresentationPublicationReservation(
            nullptr,
            std::move(lock),
            SharedPresentationValidationStatus::Conflict,
            std::move(conflicts),
            {},
            {});
    }

    if (_publicationSequence == _maximumPublicationSequence) {
        throw std::overflow_error("shared-presentation publication sequence overflow");
    }
    for (const auto& key : validatedChanges) {
        if (currentLocked(key) == _maximumRevision) {
            throw std::overflow_error("shared-presentation revision counter overflow");
        }
    }

    SharedPresentationPublication publication;
    publication.document = *_documentIdentity;
    publication.publicationSequence = _publicationSequence + 1;
    publication.changes.reserve(validatedChanges.size());
    for (const auto& key : validatedChanges) {
        publication.changes.push_back(
            {*_documentIdentity, key, currentLocked(key) + 1});
    }

    std::vector<SharedPresentationRevision*> revisionSlots;
    revisionSlots.reserve(validatedChanges.size());
    _revisions.reserve(_revisions.size() + validatedChanges.size());
    for (const auto& key : validatedChanges) {
        auto [revision, inserted] = _revisions.try_emplace(key, 0);
        static_cast<void>(inserted);
        revisionSlots.push_back(&revision->second);
    }

    return SharedPresentationPublicationReservation(
        this,
        std::move(lock),
        SharedPresentationValidationStatus::Valid,
        {},
        std::move(revisionSlots),
        std::move(publication));
}

SharedPresentationPublicationReservation::SharedPresentationPublicationReservation(
    SharedPresentationRevisionIndex* owner,
    std::unique_lock<std::mutex>&& lock,
    SharedPresentationValidationStatus status,
    std::vector<SharedPresentationRevisionConflict> conflicts,
    std::vector<SharedPresentationRevision*> revisionSlots,
    SharedPresentationPublication publication) noexcept
    : _owner(owner)
    , _lock(std::move(lock))
    , _status(status)
    , _conflicts(std::move(conflicts))
    , _revisionSlots(std::move(revisionSlots))
    , _publication(std::move(publication))
{}

SharedPresentationPublicationReservation::SharedPresentationPublicationReservation(
    SharedPresentationPublicationReservation&& other) noexcept
    : _owner(std::exchange(other._owner, nullptr))
    , _lock(std::move(other._lock))
    , _status(other._status)
    , _conflicts(std::move(other._conflicts))
    , _revisionSlots(std::move(other._revisionSlots))
    , _publication(std::move(other._publication))
{}

SharedPresentationPublicationReservation&
SharedPresentationPublicationReservation::operator=(
    SharedPresentationPublicationReservation&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    cancel();
    _owner = std::exchange(other._owner, nullptr);
    _lock = std::move(other._lock);
    _status = other._status;
    _conflicts = std::move(other._conflicts);
    _revisionSlots = std::move(other._revisionSlots);
    _publication = std::move(other._publication);
    return *this;
}

SharedPresentationPublicationReservation::~SharedPresentationPublicationReservation() noexcept
{
    cancel();
}

bool SharedPresentationPublicationReservation::ready() const noexcept
{
    return _owner && _status == SharedPresentationValidationStatus::Valid
        && _lock.owns_lock();
}

SharedPresentationValidationStatus
SharedPresentationPublicationReservation::status() const noexcept
{
    return _status;
}

const std::vector<SharedPresentationRevisionConflict>&
SharedPresentationPublicationReservation::conflicts() const noexcept
{
    return _conflicts;
}

SharedPresentationPublication SharedPresentationPublicationReservation::commit() noexcept
{
    if (!ready()) {
        return {};
    }
    for (auto* revision : _revisionSlots) {
        ++(*revision);
    }
    ++_owner->_publicationSequence;
    auto publication = std::move(_publication);
    _owner = nullptr;
    _lock.unlock();
    return publication;
}

void SharedPresentationPublicationReservation::poison() noexcept
{
    if (_owner && _lock.owns_lock()) {
        // The reservation already owns the index mutex.  Setting one scalar
        // cannot allocate or throw, and no unrevisioned partial state can be
        // followed by another validation/publication admission.
        _owner->_poisoned.store(true, std::memory_order_release);
    }
    cancel();
}

void SharedPresentationPublicationReservation::cancel() noexcept
{
    _owner = nullptr;
    if (_lock.owns_lock()) {
        _lock.unlock();
    }
}

const char* Gui::sharedPresentationCommitStatusName(
    SharedPresentationCommitStatus status) noexcept
{
    switch (status) {
        case SharedPresentationCommitStatus::Committed:
            return "Committed";
        case SharedPresentationCommitStatus::StaleDocument:
            return "StaleDocument";
        case SharedPresentationCommitStatus::InvalidRequest:
            return "InvalidRequest";
        case SharedPresentationCommitStatus::MissingCallback:
            return "MissingCallback";
        case SharedPresentationCommitStatus::SerializationFailed:
            return "SerializationFailed";
        case SharedPresentationCommitStatus::AppValidationFailed:
            return "AppValidationFailed";
        case SharedPresentationCommitStatus::AppConflict:
            return "AppConflict";
        case SharedPresentationCommitStatus::PresentationConflict:
            return "PresentationConflict";
        case SharedPresentationCommitStatus::PresentationPublicationFailed:
            return "PresentationPublicationFailed";
        case SharedPresentationCommitStatus::AppApplyFailed:
            return "AppApplyFailed";
        case SharedPresentationCommitStatus::GuiApplyFailed:
            return "GuiApplyFailed";
        case SharedPresentationCommitStatus::PostconditionFailed:
            return "PostconditionFailed";
        case SharedPresentationCommitStatus::AppCommitFailed:
            return "AppCommitFailed";
        case SharedPresentationCommitStatus::RollbackFailed:
            return "RollbackFailed";
    }
    return "Unknown";
}

SharedPresentationCommitResult SharedPresentationCoordinator::commit(
    SharedPresentationRevisionIndex& revisions,
    const SharedPresentationCommitRequest& request,
    const SharedPresentationCommitCallbacks& callbacks) const
{
    if (!callbacks.serialize) {
        return makeResult(SharedPresentationCommitStatus::MissingCallback,
                          "shared-presentation commit requires an App serialization boundary");
    }

    auto protocol = std::make_shared<SharedPresentationCommitProtocolState>();
    protocol->revisions = &revisions;
    protocol->request = &request;
    protocol->callbacks = &callbacks;
    protocol->coordinator = this;
    const std::weak_ptr<SharedPresentationCommitProtocolState> weakProtocol = protocol;
    std::exception_ptr serializationException;
    try {
        callbacks.serialize(
            [weakProtocol]() {
                const auto state = weakProtocol.lock();
                if (!state) {
                    return;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->acceptingCallbacks) {
                    return;
                }
                ++state->workInvocations;
                if (state->workInvocations != 1 || state->completionInvocations != 0) {
                    return;
                }
                state->preparedResult.emplace(state->coordinator->prepareSerialized(
                    *state->revisions,
                    *state->request,
                    *state->callbacks,
                    state->pendingPublication));
            },
            [weakProtocol](SharedPresentationSerializationResult boundary) {
                const auto state = weakProtocol.lock();
                if (!state) {
                    return;
                }
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!state->acceptingCallbacks) {
                    return;
                }
                ++state->completionInvocations;
                if (state->completionInvocations != 1) {
                    return;
                }

                if (state->workInvocations != 1 || !state->preparedResult) {
                    Gui::SharedPresentationCommitResult result;
                    result.status = SharedPresentationCommitStatus::SerializationFailed;
                    if (state->pendingPublication) {
                        state->pendingPublication->cancel();
                    }
                    result.diagnostic = state->workInvocations == 0
                        ? "App serialization boundary completed without invoking work"
                        : "App serialization boundary invoked work more than once";
                    state->authoritativeResult.emplace(std::move(result));
                    return;
                }

                if (!boundary.admitted) {
                    Gui::SharedPresentationCommitResult result;
                    result.status = SharedPresentationCommitStatus::SerializationFailed;
                    if (state->pendingPublication) {
                        state->pendingPublication->cancel();
                    }
                    result.diagnostic = boundary.diagnostic.empty()
                        ? "App serialization boundary rejected shared-presentation work"
                        : std::move(boundary.diagnostic);
                    state->authoritativeResult.emplace(std::move(result));
                    return;
                }

                if (!state->preparedResult->committed()) {
                    state->authoritativeResult.emplace(
                        std::move(*state->preparedResult));
                    return;
                }

                state->authoritativeResult.emplace(
                    state->coordinator->completeSerialized(
                        *state->callbacks,
                        *state->pendingPublication,
                        std::move(*state->preparedResult)));
            });
    }
    catch (...) {
        serializationException = std::current_exception();
    }

    std::optional<SharedPresentationCommitResult> authoritativeResult;
    std::size_t workInvocations = 0;
    std::size_t completionInvocations = 0;
    {
        std::lock_guard<std::mutex> lock(protocol->mutex);
        protocol->acceptingCallbacks = false;
        workInvocations = protocol->workInvocations;
        completionInvocations = protocol->completionInvocations;
        if (protocol->authoritativeResult) {
            authoritativeResult.emplace(std::move(*protocol->authoritativeResult));
        }
        else if (protocol->pendingPublication) {
            // work() only validates and reserves.  Missing completion or a
            // boundary exception therefore discards hidden value state here;
            // no App/GUI rollback exists or occurs outside serialization.
            protocol->pendingPublication->cancel();
        }
        protocol->revisions = nullptr;
        protocol->request = nullptr;
        protocol->callbacks = nullptr;
        protocol->coordinator = nullptr;
    }

    if (authoritativeResult) {
        // A boundary exception/protocol error after successful completion
        // cannot retroactively convert an already-published commit to failure.
        if (serializationException) {
            try {
                std::rethrow_exception(serializationException);
            }
            catch (const Base::Exception& exception) {
                appendNonfatalFailure(
                    authoritativeResult->diagnostic,
                    "App serialization boundary failed",
                    exception.what());
            }
            catch (const std::exception& exception) {
                appendNonfatalFailure(
                    authoritativeResult->diagnostic,
                    "App serialization boundary failed",
                    exception.what());
            }
            catch (...) {
                appendNonfatalDiagnostic(
                    authoritativeResult->diagnostic,
                    "App serialization boundary failed: unknown exception");
            }
        }
        if (workInvocations != 1 || completionInvocations != 1) {
            appendNonfatalDiagnostic(
                authoritativeResult->diagnostic,
                "App serialization boundary violated the exact-once protocol after completion");
        }
        return std::move(*authoritativeResult);
    }

    SharedPresentationCommitResult result;
    result.status = SharedPresentationCommitStatus::SerializationFailed;
    if (serializationException) {
        try {
            std::rethrow_exception(serializationException);
        }
        catch (const Base::Exception& exception) {
            result.diagnostic =
                stageFailure("App serialization boundary failed", exception.what());
        }
        catch (const std::exception& exception) {
            result.diagnostic =
                stageFailure("App serialization boundary failed", exception.what());
        }
        catch (...) {
            result.diagnostic = "App serialization boundary failed: unknown exception";
        }
    }
    else if (workInvocations == 0) {
        result.diagnostic =
            "App serialization boundary returned without invoking work or completion";
    }
    else {
        result.diagnostic =
            "App serialization boundary returned without authoritative completion";
    }
    return result;
}

SharedPresentationCommitResult SharedPresentationCoordinator::prepareSerialized(
    SharedPresentationRevisionIndex& revisions,
    const SharedPresentationCommitRequest& request,
    const SharedPresentationCommitCallbacks& callbacks,
    std::optional<SharedPresentationPublicationReservation>& pendingPublication) const
{
    // Lifecycle deliberately precedes declaration, dependency, and callback
    // validation, so stale work cannot probe a replacement/non-live document.
    if (!callbacks.currentIdentity) {
        return makeResult(SharedPresentationCommitStatus::MissingCallback,
                          "shared-presentation commit requires a lifecycle query");
    }

    std::optional<App::DocumentIdentity> identity;
    try {
        identity = callbacks.currentIdentity();
    }
    catch (const Base::Exception& exception) {
        return makeResult(SharedPresentationCommitStatus::StaleDocument,
                          stageFailure("document identity unavailable", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(SharedPresentationCommitStatus::StaleDocument,
                          stageFailure("document identity unavailable", exception.what()));
    }
    catch (...) {
        return makeResult(SharedPresentationCommitStatus::StaleDocument,
                          stageFailure("document identity unavailable", "unknown exception"));
    }
    if (!identity || identity->instanceId != request.document.documentInstanceId
        || identity->lifecycleEpoch != request.document.lifecycleEpoch
        || identity->state != App::DocumentLifecycleState::Live) {
        return makeResult(SharedPresentationCommitStatus::StaleDocument,
                          "shared-presentation request targets a stale or non-live document");
    }

    const auto presentationIdentity = revisions.documentIdentity();
    if (!presentationIdentity || *presentationIdentity != request.document) {
        return makeResult(
            SharedPresentationCommitStatus::StaleDocument,
            "shared-presentation revision stream is not bound to the live document epoch");
    }

    // Observation lifecycle binding is part of stale validation, not request
    // declaration validation.  It therefore precedes malformed keys, missing
    // callbacks, and App conflicts just like the live request identity above.
    for (const auto& observation : request.expectedPresentationRevisions) {
        if (observation.document != request.document) {
            return makeResult(
                SharedPresentationCommitStatus::StaleDocument,
                "presentation observation targets a stale or foreign document epoch");
        }
    }

    if (revisions.poisoned()) {
        return makeResult(
            SharedPresentationCommitStatus::RollbackFailed,
            "shared-presentation revision stream is poisoned after an incomplete rollback");
    }

    if (const auto invalid = validateRequest(request)) {
        return makeResult(SharedPresentationCommitStatus::InvalidRequest, *invalid);
    }
    if (!callbacks.validateAppRevisions || !callbacks.applyAppMutation
        || !callbacks.applyGuiMutation || !callbacks.checkPostcondition
        || !callbacks.makeAppDurable
        || !callbacks.rollbackGuiMutation || !callbacks.rollbackAppMutation) {
        return makeResult(
            SharedPresentationCommitStatus::MissingCallback,
            "shared-presentation commit requires validation, apply, postcondition, App durability, and rollback callbacks");
    }

    std::vector<App::DocumentRevisionConflict> appConflicts;
    try {
        appConflicts = callbacks.validateAppRevisions(request.expectedAppRevisions);
    }
    catch (const Base::Exception& exception) {
        return makeResult(SharedPresentationCommitStatus::AppValidationFailed,
                          stageFailure("App revision validation failed", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(SharedPresentationCommitStatus::AppValidationFailed,
                          stageFailure("App revision validation failed", exception.what()));
    }
    catch (...) {
        return makeResult(SharedPresentationCommitStatus::AppValidationFailed,
                          stageFailure("App revision validation failed", "unknown exception"));
    }
    if (!appConflicts.empty()) {
        auto result = makeResult(SharedPresentationCommitStatus::AppConflict,
                                 "one or more declared App revisions changed before commit");
        result.appConflicts = std::move(appConflicts);
        return result;
    }

    std::optional<SharedPresentationPublicationReservation> reservation;
    try {
        reservation.emplace(revisions.reservePublication(
            request.expectedPresentationRevisions, request.presentationWrites));
    }
    catch (const std::invalid_argument& exception) {
        return makeResult(SharedPresentationCommitStatus::InvalidRequest, exception.what());
    }
    catch (const Base::Exception& exception) {
        return makeResult(
            SharedPresentationCommitStatus::PresentationPublicationFailed,
            stageFailure("presentation publication reservation failed", exception.what()));
    }
    catch (const std::exception& exception) {
        return makeResult(
            SharedPresentationCommitStatus::PresentationPublicationFailed,
            stageFailure("presentation publication reservation failed", exception.what()));
    }
    catch (...) {
        return makeResult(
            SharedPresentationCommitStatus::PresentationPublicationFailed,
            stageFailure("presentation publication reservation failed", "unknown exception"));
    }
    if (!reservation->ready()) {
        if (reservation->status() == SharedPresentationValidationStatus::Poisoned) {
            return makeResult(
                SharedPresentationCommitStatus::RollbackFailed,
                "shared-presentation revision stream became poisoned during admission");
        }
        if (reservation->status() == SharedPresentationValidationStatus::ForeignDocument
            || reservation->status() == SharedPresentationValidationStatus::EpochMismatch
            || reservation->status() == SharedPresentationValidationStatus::Unbound) {
            return makeResult(
                SharedPresentationCommitStatus::StaleDocument,
                "presentation observations no longer target the live document epoch");
        }
        auto result = makeResult(
            SharedPresentationCommitStatus::PresentationConflict,
            "one or more declared presentation revisions changed before commit");
        result.presentationConflicts.assign(reservation->conflicts().begin(),
                                            reservation->conflicts().end());
        return result;
    }

    // work() stops at a hidden, allocation-complete reservation.  No live
    // App/GUI callback runs until the boundary authoritatively admits its
    // completion callback.
    auto successResult = makeResult(SharedPresentationCommitStatus::Committed,
                                    "shared-presentation commit completed");
    pendingPublication.emplace(std::move(*reservation));
    return successResult;
}

SharedPresentationCommitResult SharedPresentationCoordinator::completeSerialized(
    const SharedPresentationCommitCallbacks& callbacks,
    SharedPresentationPublicationReservation& pendingPublication,
    SharedPresentationCommitResult preparedSuccess) const
{
    // preparedSuccess and the hidden publication were fully allocated by
    // work().  After live mutation begins, only rollback-safe diagnostic work
    // or noexcept publication/moves remain.

    const auto failAndRollback = [&](SharedPresentationCommitStatus status,
                                     StepInvocationResult&& failure,
                                     const char* stage) {
        SharedPresentationCommitResult result;
        result.status = status;
        // Always restore both domains in reverse apply order.  These callbacks
        // are required to tolerate a partially applied or not-yet-applied step.
        const bool restored =
            runBothRollbacks(callbacks, result.rollbackDiagnostics);
        if (restored) {
            pendingPublication.cancel();
        }
        else {
            pendingPublication.poison();
            result.status = SharedPresentationCommitStatus::RollbackFailed;
        }
        // Construct/format diagnostics only after both rollback attempts.  In
        // particular, a bad_alloc here cannot bypass restoration.
        auto primaryDiagnostic =
            stepFailureDiagnostic(std::move(failure), stage);
        if (restored) {
            result.diagnostic = std::move(primaryDiagnostic);
        }
        else {
            result.diagnostic = "original ";
            result.diagnostic += sharedPresentationCommitStatusName(status);
            result.diagnostic += ": ";
            result.diagnostic += primaryDiagnostic;
            result.diagnostic += "; shared-presentation stream poisoned after rollback failure";
        }
        return result;
    };

    auto appApply = invokeStep(callbacks.applyAppMutation);
    if (!appApply.reported.succeeded) {
        return failAndRollback(SharedPresentationCommitStatus::AppApplyFailed,
                               std::move(appApply),
                               "App mutation");
    }

    auto guiApply = invokeStep(callbacks.applyGuiMutation);
    if (!guiApply.reported.succeeded) {
        return failAndRollback(SharedPresentationCommitStatus::GuiApplyFailed,
                               std::move(guiApply),
                               "GUI mutation");
    }

    auto postcondition = invokeStep(callbacks.checkPostcondition);
    if (!postcondition.reported.succeeded) {
        return failAndRollback(SharedPresentationCommitStatus::PostconditionFailed,
                               std::move(postcondition),
                               "shared-presentation postcondition");
    }

    auto appDurability = invokeStep(callbacks.makeAppDurable);
    if (!appDurability.reported.succeeded) {
        return failAndRollback(SharedPresentationCommitStatus::AppCommitFailed,
                               std::move(appDurability),
                               "App native transaction commit");
    }

    preparedSuccess.publishedPresentation.emplace(pendingPublication.commit());
    return preparedSuccess;
}
