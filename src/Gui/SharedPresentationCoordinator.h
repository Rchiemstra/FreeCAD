// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/CollaborationRegistry.h>
#include <App/DocumentRevisionIndex.h>

#include <FCGlobal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gui
{

using SharedPresentationRevision = std::uint64_t;
using SharedPresentationPublicationSequence = std::uint64_t;

/**
 * One deliberately shared ViewProvider property.
 *
 * Both fields are copied values.  In particular, stableObjectIdentity is not
 * a document object name or an encoded pointer, and propertyName remains a
 * separate scalar instead of being concatenated into the object identity.
 */
struct GuiExport SharedPresentationRevisionKey
{
    std::string stableObjectIdentity;
    std::string propertyName;

    [[nodiscard]] bool valid() const noexcept;
};

inline bool operator==(const SharedPresentationRevisionKey& left,
                       const SharedPresentationRevisionKey& right) noexcept
{
    return left.stableObjectIdentity == right.stableObjectIdentity
        && left.propertyName == right.propertyName;
}

inline bool operator!=(const SharedPresentationRevisionKey& left,
                       const SharedPresentationRevisionKey& right) noexcept
{
    return !(left == right);
}

inline bool operator<(const SharedPresentationRevisionKey& left,
                      const SharedPresentationRevisionKey& right) noexcept
{
    if (left.stableObjectIdentity != right.stableObjectIdentity) {
        return left.stableObjectIdentity < right.stableObjectIdentity;
    }
    return left.propertyName < right.propertyName;
}

struct GuiExport SharedPresentationRevisionKeyHash
{
    std::size_t operator()(const SharedPresentationRevisionKey& key) const noexcept;
};

/** An exact document-instance/epoch-bound presentation observation. */
struct GuiExport SharedPresentationRevisionObservation
{
    App::DocumentRevisionIdentityBinding document;
    SharedPresentationRevisionKey key;
    SharedPresentationRevision revision {0};
};

inline bool operator==(const SharedPresentationRevisionObservation& left,
                       const SharedPresentationRevisionObservation& right) noexcept
{
    return left.document == right.document && left.key == right.key
        && left.revision == right.revision;
}

inline bool operator!=(const SharedPresentationRevisionObservation& left,
                       const SharedPresentationRevisionObservation& right) noexcept
{
    return !(left == right);
}

struct GuiExport SharedPresentationRevisionConflict
{
    SharedPresentationRevisionKey key;
    SharedPresentationRevision expected {0};
    SharedPresentationRevision current {0};
};

inline bool operator==(const SharedPresentationRevisionConflict& left,
                       const SharedPresentationRevisionConflict& right) noexcept
{
    return left.key == right.key && left.expected == right.expected
        && left.current == right.current;
}

/** Pointer-free record produced by exactly one successful publication. */
struct GuiExport SharedPresentationPublication
{
    App::DocumentRevisionIdentityBinding document;
    SharedPresentationPublicationSequence publicationSequence {0};
    std::vector<SharedPresentationRevisionObservation> changes;
};

enum class SharedPresentationValidationStatus
{
    Valid,
    Unbound,
    Poisoned,
    ForeignDocument,
    EpochMismatch,
    Conflict
};

struct GuiExport SharedPresentationValidationResult
{
    SharedPresentationValidationStatus status {SharedPresentationValidationStatus::Valid};
    std::optional<App::DocumentRevisionIdentityBinding> currentDocument;
    std::vector<SharedPresentationRevisionConflict> conflicts;

    [[nodiscard]] bool valid() const noexcept
    {
        return status == SharedPresentationValidationStatus::Valid;
    }
};

/** Stable publication point captured for one presentation persistence write. */
struct GuiExport SharedPresentationPersistenceCapture
{
    App::DocumentRevisionIdentityBinding document;
    SharedPresentationPublicationSequence publicationSequence {0};
};

enum class SharedPresentationSaveDisposition
{
    Succeeded,
    Failed,
    Abandoned
};

enum class SharedPresentationPersistStatus
{
    Marked,
    NotMarked,
    Unbound,
    Poisoned,
    ForeignDocument,
    EpochMismatch,
    FutureSequence,
    Rewind
};

/** Exact current/persisted state for the index's current document epoch. */
struct GuiExport SharedPresentationPersistenceState
{
    App::DocumentRevisionIdentityBinding document;
    SharedPresentationPublicationSequence currentPublicationSequence {0};
    SharedPresentationPublicationSequence persistedPublicationSequence {0};
    bool hasUnpersistedChanges {false};
    bool poisoned {false};
};

class SharedPresentationPublicationReservation;
class SharedPresentationCoordinator;

/**
 * Thread-safe revision storage for the shared-presentation domain of one live
 * document instance.  Revisions and publication sequence only advance; an
 * epoch advance rebinds future observations without rewinding either stream.
 * An incomplete live rollback permanently poisons the instance: validation,
 * publication, coordinated admission, and persistence then fail closed.
 */
class GuiExport SharedPresentationRevisionIndex
{
public:
    SharedPresentationRevisionIndex();
    explicit SharedPresentationRevisionIndex(SharedPresentationRevision maximumRevision);
    SharedPresentationRevisionIndex(
        SharedPresentationRevision maximumRevision,
        SharedPresentationPublicationSequence maximumPublicationSequence);

    SharedPresentationRevisionIndex(const SharedPresentationRevisionIndex&) = delete;
    SharedPresentationRevisionIndex& operator=(const SharedPresentationRevisionIndex&) = delete;

    void bindDocumentIdentity(App::DocumentInstanceId documentInstanceId,
                              App::DocumentLifecycleEpoch lifecycleEpoch);
    [[nodiscard]] std::optional<App::DocumentRevisionIdentityBinding> documentIdentity() const;

    [[nodiscard]] SharedPresentationRevision
    current(const SharedPresentationRevisionKey& key) const;
    [[nodiscard]] std::vector<SharedPresentationRevisionObservation>
    capture(const std::vector<SharedPresentationRevisionKey>& keys) const;
    [[nodiscard]] SharedPresentationValidationResult
    validate(const std::vector<SharedPresentationRevisionObservation>& observations) const;

    /** Atomically publish one nonempty, duplicate-free set of property keys. */
    [[nodiscard]] SharedPresentationPublication
    publish(const std::vector<SharedPresentationRevisionKey>& keys);

    [[nodiscard]] SharedPresentationPublicationSequence latestPublicationSequence() const;
    [[nodiscard]] bool poisoned() const;
    /**
     * Permanently fail-close after a live presentation mutation could not be
     * revision-published. Safe for exception recovery: no lock or allocation.
     */
    void markInconsistentAfterUnpublishedMutation() noexcept;

    /** Capture the exact current publication point before starting a save. */
    [[nodiscard]] SharedPresentationPersistenceCapture capturePersistence() const;
    /**
     * Advance the persisted marker only for a successful, current-epoch save.
     * Failed and abandoned dispositions are explicit no-ops.
     */
    [[nodiscard]] SharedPresentationPersistStatus markPersisted(
        const SharedPresentationPersistenceCapture& capture,
        SharedPresentationSaveDisposition disposition);
    [[nodiscard]] SharedPresentationPersistenceState persistenceState() const;

private:
    friend class SharedPresentationPublicationReservation;
    friend class SharedPresentationCoordinator;

    using RevisionMap = std::unordered_map<SharedPresentationRevisionKey,
                                           SharedPresentationRevision,
                                           SharedPresentationRevisionKeyHash>;

    [[nodiscard]] SharedPresentationRevision
    currentLocked(const SharedPresentationRevisionKey& key) const noexcept;
    [[nodiscard]] SharedPresentationPublicationReservation reservePublication(
        const std::vector<SharedPresentationRevisionObservation>& expected,
        const std::vector<SharedPresentationRevisionKey>& changes);

    const SharedPresentationRevision _maximumRevision;
    const SharedPresentationPublicationSequence _maximumPublicationSequence;
    mutable std::mutex _mutex;
    RevisionMap _revisions;
    std::optional<App::DocumentRevisionIdentityBinding> _documentIdentity;
    SharedPresentationPublicationSequence _publicationSequence {0};
    SharedPresentationPublicationSequence _persistedPublicationSequence {0};
    std::atomic<bool> _poisoned {false};
};

/** Move-only hidden publication; commit is allocation-free and noexcept. */
class GuiExport SharedPresentationPublicationReservation
{
public:
    SharedPresentationPublicationReservation() = delete;
    SharedPresentationPublicationReservation(
        const SharedPresentationPublicationReservation&) = delete;
    SharedPresentationPublicationReservation& operator=(
        const SharedPresentationPublicationReservation&) = delete;

    SharedPresentationPublicationReservation(
        SharedPresentationPublicationReservation&& other) noexcept;
    SharedPresentationPublicationReservation& operator=(
        SharedPresentationPublicationReservation&& other) noexcept;
    ~SharedPresentationPublicationReservation() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] SharedPresentationValidationStatus status() const noexcept;
    [[nodiscard]] const std::vector<SharedPresentationRevisionConflict>&
    conflicts() const noexcept;
    [[nodiscard]] SharedPresentationPublication commit() noexcept;
    /** Permanently fail-close the owning index and cancel this publication. */
    void poison() noexcept;
    void cancel() noexcept;

private:
    friend class SharedPresentationRevisionIndex;

    SharedPresentationPublicationReservation(
        SharedPresentationRevisionIndex* owner,
        std::unique_lock<std::mutex>&& lock,
        SharedPresentationValidationStatus status,
        std::vector<SharedPresentationRevisionConflict> conflicts,
        std::vector<SharedPresentationRevision*> revisionSlots,
        SharedPresentationPublication publication) noexcept;

    SharedPresentationRevisionIndex* _owner {nullptr};
    std::unique_lock<std::mutex> _lock;
    SharedPresentationValidationStatus _status {SharedPresentationValidationStatus::Unbound};
    std::vector<SharedPresentationRevisionConflict> _conflicts;
    std::vector<SharedPresentationRevision*> _revisionSlots;
    SharedPresentationPublication _publication;
};

/** A callback stage's complete value result. */
struct GuiExport SharedPresentationStepResult
{
    bool succeeded {false};
    std::string diagnostic;
};

enum class SharedPresentationCommitStatus
{
    Committed,
    StaleDocument,
    InvalidRequest,
    MissingCallback,
    SerializationFailed,
    AppValidationFailed,
    AppConflict,
    PresentationConflict,
    PresentationPublicationFailed,
    AppApplyFailed,
    GuiApplyFailed,
    PostconditionFailed,
    AppCommitFailed,
    RollbackFailed
};

[[nodiscard]] GuiExport const char*
sharedPresentationCommitStatusName(SharedPresentationCommitStatus status) noexcept;

/** Complete pointer-free result of one synchronous coordinated commit. */
struct GuiExport SharedPresentationCommitResult
{
    SharedPresentationCommitStatus status {SharedPresentationCommitStatus::InvalidRequest};
    std::string diagnostic;
    std::vector<App::DocumentRevisionConflict> appConflicts;
    std::vector<SharedPresentationRevisionConflict> presentationConflicts;
    std::vector<std::string> rollbackDiagnostics;
    std::optional<SharedPresentationPublication> publishedPresentation;

    [[nodiscard]] bool committed() const noexcept
    {
        return status == SharedPresentationCommitStatus::Committed;
    }
};

/** All dependencies and effects are copied values; write targets must be observed. */
struct GuiExport SharedPresentationCommitRequest
{
    App::DocumentRevisionIdentityBinding document;
    std::vector<App::DocumentRevisionObservation> expectedAppRevisions;
    std::vector<SharedPresentationRevisionObservation> expectedPresentationRevisions;
    std::vector<SharedPresentationRevisionKey> presentationWrites;
};

/** The App boundary's authoritative decision for one admitted work callback. */
struct GuiExport SharedPresentationSerializationResult
{
    bool admitted {false};
    std::string diagnostic;
};

using SharedPresentationCommitWork = std::function<void()>;
using SharedPresentationCommitCompletion =
    std::function<void(SharedPresentationSerializationResult)>;
using SharedPresentationSerializationBoundary = std::function<void(
    SharedPresentationCommitWork&&,
    SharedPresentationCommitCompletion&&)>;
using SharedPresentationLifecycleQuery =
    std::function<std::optional<App::DocumentIdentity>()>;
using SharedPresentationAppRevisionValidator = std::function<
    std::vector<App::DocumentRevisionConflict>(
        const std::vector<App::DocumentRevisionObservation>&)>;
using SharedPresentationStep = std::function<SharedPresentationStepResult()>;

/**
 * Synchronous value/callback seams supplied by the App/Gui integration.
 *
 * serialize must invoke work exactly once and then completion exactly once,
 * both synchronously while holding the document's App commit/recompute
 * serialization boundary.  work only validates lifecycle/dependencies and
 * reserves a hidden presentation publication; it never mutates App or GUI.
 * An admitted completion performs App/GUI/postcondition callbacks and then
 * publishes.  Rejection merely cancels the hidden reservation.  This two-stage
 * protocol means abandonment before completion has no live state to restore.
 *
 * Mutation steps remain reversible through completion and must not
 * independently publish an App revision.  Rollback steps restore captured
 * GUI and App values and are safe even when the corresponding apply step
 * failed part-way through.
 */
struct GuiExport SharedPresentationCommitCallbacks
{
    SharedPresentationSerializationBoundary serialize;
    SharedPresentationLifecycleQuery currentIdentity;
    SharedPresentationAppRevisionValidator validateAppRevisions;
    SharedPresentationStep applyAppMutation;
    SharedPresentationStep applyGuiMutation;
    SharedPresentationStep checkPostcondition;
    /** Private integration step which makes the already-validated App mutation durable. */
    SharedPresentationStep makeAppDurable;
    SharedPresentationStep rollbackGuiMutation;
    SharedPresentationStep rollbackAppMutation;
};

/**
 * Short App/Gui commit boundary.  It stores no callback and no App/Gui pointer;
 * every supplied callable is consumed synchronously by commit().
 */
class GuiExport SharedPresentationCoordinator
{
public:
    [[nodiscard]] SharedPresentationCommitResult commit(
        SharedPresentationRevisionIndex& revisions,
        const SharedPresentationCommitRequest& request,
        const SharedPresentationCommitCallbacks& callbacks) const;

private:
    [[nodiscard]] SharedPresentationCommitResult prepareSerialized(
        SharedPresentationRevisionIndex& revisions,
        const SharedPresentationCommitRequest& request,
        const SharedPresentationCommitCallbacks& callbacks,
        std::optional<SharedPresentationPublicationReservation>& pendingPublication) const;
    [[nodiscard]] SharedPresentationCommitResult completeSerialized(
        const SharedPresentationCommitCallbacks& callbacks,
        SharedPresentationPublicationReservation& pendingPublication,
        SharedPresentationCommitResult preparedSuccess) const;
};

}  // namespace Gui
