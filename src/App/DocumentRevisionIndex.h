// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborationRegistry.h"

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace App
{

using DocumentRevision = std::uint64_t;
using DocumentPublicationSequence = std::uint64_t;

class DocumentRevisionPublicationReservation;

enum class DocumentRevisionKind
{
    ObjectExistence,
    ObjectModel,
    ObjectStructure,
    DocumentStructure,
    UnknownModelMutation,
    ObjectProperty
};

struct AppExport DocumentRevisionKey
{
    DocumentRevisionKind kind;
    std::string subject;
    // UTF-8 property name for ObjectProperty only. Kept as a trailing,
    // defaulted scalar so existing {kind, subject} aggregate initializers
    // remain source-compatible.
    std::string propertyName {};

    static DocumentRevisionKey objectExistence(std::string subject);
    static DocumentRevisionKey objectModel(std::string subject);
    static DocumentRevisionKey objectStructure(std::string subject);
    static DocumentRevisionKey objectProperty(std::string subject, std::string propertyName);
    static DocumentRevisionKey documentStructure();
    static DocumentRevisionKey unknownModelMutation();

    [[nodiscard]] bool valid() const noexcept;
};

inline bool operator==(const DocumentRevisionKey& left, const DocumentRevisionKey& right) noexcept
{
    return left.kind == right.kind && left.subject == right.subject
        && left.propertyName == right.propertyName;
}

inline bool operator!=(const DocumentRevisionKey& left, const DocumentRevisionKey& right) noexcept
{
    return !(left == right);
}

inline bool operator<(const DocumentRevisionKey& left, const DocumentRevisionKey& right) noexcept
{
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    if (left.subject != right.subject) {
        return left.subject < right.subject;
    }
    return left.propertyName < right.propertyName;
}

struct AppExport DocumentRevisionKeyHash
{
    std::size_t operator()(const DocumentRevisionKey& key) const noexcept;
};

struct AppExport DocumentRevisionObservation
{
    DocumentRevisionObservation(DocumentRevisionKey key, DocumentRevision revision);

    // Non-const so these remain Assignable for std::vector / DocumentCommitResult.
    DocumentRevisionKey key;
    DocumentRevision revision;
};

inline bool operator==(const DocumentRevisionObservation& left,
                       const DocumentRevisionObservation& right) noexcept
{
    return left.key == right.key && left.revision == right.revision;
}

inline bool operator!=(const DocumentRevisionObservation& left,
                       const DocumentRevisionObservation& right) noexcept
{
    return !(left == right);
}

struct AppExport DocumentRevisionConflict
{
    DocumentRevisionConflict(DocumentRevisionKey key,
                             DocumentRevision expected,
                             DocumentRevision current);

    // Non-const so these remain Assignable for std::vector / DocumentCommitResult.
    DocumentRevisionKey key;
    DocumentRevision expected;
    DocumentRevision current;
};

inline bool operator==(const DocumentRevisionConflict& left,
                       const DocumentRevisionConflict& right) noexcept
{
    return left.key == right.key && left.expected == right.expected
        && left.current == right.current;
}

inline bool operator!=(const DocumentRevisionConflict& left,
                       const DocumentRevisionConflict& right) noexcept
{
    return !(left == right);
}

/** Pointer-free identity copied into each publication event. */
struct AppExport DocumentRevisionIdentityBinding
{
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
};

inline bool operator==(const DocumentRevisionIdentityBinding& left,
                       const DocumentRevisionIdentityBinding& right) noexcept
{
    return left.documentInstanceId == right.documentInstanceId
        && left.lifecycleEpoch == right.lifecycleEpoch;
}

inline bool operator!=(const DocumentRevisionIdentityBinding& left,
                       const DocumentRevisionIdentityBinding& right) noexcept
{
    return !(left == right);
}

/** One semantic key changed by an atomic publication. */
struct AppExport DocumentRevisionChange
{
    DocumentRevisionKey key;
    DocumentRevision revision {0};
    std::optional<std::string> stableObjectIdentity;
};

/** One requested semantic change and its stable, pointer-free object identity. */
struct AppExport DocumentRevisionPublicationRequest
{
    DocumentRevisionKey key;
    std::optional<std::string> stableObjectIdentity;
};

/** One pointer-free App property explicitly admitted by an atomic presentation request. */
struct AppExport CollaborationAtomicPresentationWrite
{
    std::string stableObjectIdentity;
    std::string propertyName;
};

inline bool operator==(const CollaborationAtomicPresentationWrite& left,
                       const CollaborationAtomicPresentationWrite& right) noexcept
{
    return left.stableObjectIdentity == right.stableObjectIdentity
        && left.propertyName == right.propertyName;
}

inline bool operator<(const CollaborationAtomicPresentationWrite& left,
                      const CollaborationAtomicPresentationWrite& right) noexcept
{
    if (left.stableObjectIdentity != right.stableObjectIdentity) {
        return left.stableObjectIdentity < right.stableObjectIdentity;
    }
    return left.propertyName < right.propertyName;
}

/** Serializable, pointer-free record of one atomic revision publication. */
struct AppExport DocumentRevisionPublicationEvent
{
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    DocumentPublicationSequence publicationSequence {0};
    std::vector<DocumentRevisionChange> changes;

    [[nodiscard]] std::string toJson() const;
};

/** Identity-bound last-seen publication cursor. */
struct AppExport DocumentRevisionCursor
{
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    DocumentPublicationSequence afterSequence {0};
};

inline bool operator==(const DocumentRevisionCursor& left,
                       const DocumentRevisionCursor& right) noexcept
{
    return left.documentInstanceId == right.documentInstanceId
        && left.lifecycleEpoch == right.lifecycleEpoch
        && left.afterSequence == right.afterSequence;
}

inline bool operator!=(const DocumentRevisionCursor& left,
                       const DocumentRevisionCursor& right) noexcept
{
    return !(left == right);
}

enum class DocumentRevisionCursorStatus
{
    Valid,
    ForeignDocument,
    StaleEpoch,
    FutureSequence
};

/** Result of polling publications after an identity-bound cursor. */
struct AppExport DocumentRevisionPollResult
{
    DocumentRevisionCursorStatus status {DocumentRevisionCursorStatus::Valid};
    DocumentRevisionCursor requestedCursor;
    DocumentRevisionIdentityBinding currentIdentity;
    DocumentRevisionCursor nextCursor;
    DocumentPublicationSequence oldestAvailableSequence {0};
    DocumentPublicationSequence latestSequence {0};
    bool gap {false};
    std::vector<DocumentRevisionPublicationEvent> events;

    [[nodiscard]] std::string toJson() const;
};

/**
 * Thread-safe semantic revision storage for one live document instance.
 *
 * Every key starts at zero. Publications advance counters only and never
 * restore an earlier value. A publication containing more than one key is
 * all-or-nothing, including when any counter has reached its configured
 * ceiling.
 */
class AppExport DocumentRevisionIndex
{
public:
    static constexpr std::size_t DefaultJournalCapacity = 1024;
    static constexpr std::size_t UnlimitedPollEvents =
        std::numeric_limits<std::size_t>::max();

    DocumentRevisionIndex();

    /** A bounded ceiling is injectable so overflow behavior can be tested. */
    explicit DocumentRevisionIndex(DocumentRevision maximumRevision);

    /** A zero journal capacity is invalid. */
    DocumentRevisionIndex(DocumentRevision maximumRevision, std::size_t journalCapacity);

    /** All ceilings are injectable for atomic overflow tests. */
    DocumentRevisionIndex(DocumentRevision maximumRevision,
                          DocumentPublicationSequence maximumPublicationSequence,
                          std::size_t journalCapacity);

    /**
     * Bind this index to one document instance and its current lifecycle epoch.
     * Repeating the same binding is idempotent. The epoch may advance for the
     * bound instance, but neither an epoch rewind nor an instance replacement
     * is accepted.
     */
    void bindDocumentIdentity(DocumentInstanceId documentInstanceId,
                              DocumentLifecycleEpoch lifecycleEpoch);
    [[nodiscard]] std::optional<DocumentRevisionIdentityBinding> documentIdentity() const;

    [[nodiscard]] DocumentRevision current(const DocumentRevisionKey& key) const;
    [[nodiscard]] std::vector<DocumentRevisionObservation>
    capture(const std::vector<DocumentRevisionKey>& keys) const;
    [[nodiscard]] std::vector<DocumentRevisionConflict>
    validate(const std::vector<DocumentRevisionObservation>& observations) const;

    /** Convenience publication for document-scoped keys, which carry no object identity. */
    [[nodiscard]] std::vector<DocumentRevisionObservation>
    publish(const std::vector<DocumentRevisionKey>& documentScopedKeys);
    /**
     * Canonical mixed-scope publication with one identity decision per semantic key.
     * Each ObjectProperty request also publishes the matching ObjectModel key with
     * the same identity. Expanded duplicates advance only once per publication.
     */
    [[nodiscard]] std::vector<DocumentRevisionObservation>
    publish(const std::vector<DocumentRevisionPublicationRequest>& changes);
    /** Explicit empty publication path, avoiding ambiguous publish({}) calls. */
    [[nodiscard]] std::vector<DocumentRevisionObservation> publishEmpty();
    [[nodiscard]] DocumentRevisionObservation publishUnknownModelMutation();

    /**
     * Atomically revalidate expected revisions and reserve a publication.
     *
     * A ready reservation owns the index lock and has already performed every
     * potentially-throwing allocation needed by publication. Until it is
     * committed or cancelled, readers and other publishers wait and the
     * reserved event is not observable. This lets a document transaction
     * finish before the revision boundary becomes visible without leaving an
     * allocation-failure window after the live commit.
     */
    [[nodiscard]] DocumentRevisionPublicationReservation reservePublication(
        const std::vector<DocumentRevisionObservation>& expectedRevisions,
        const std::vector<DocumentRevisionPublicationRequest>& changes);

    /**
     * Return journal events newer than cursor.afterSequence, in publication
     * order. A zero maxEvents returns metadata only and leaves nextCursor
     * unchanged. gap is true when retained history no longer reaches the
     * requested cursor. Foreign-document, stale-epoch, and future-sequence
     * cursors return an explicit non-Valid status and no events.
     */
    [[nodiscard]] DocumentRevisionPollResult pollPublications(
        const DocumentRevisionCursor& cursor,
        std::size_t maxEvents = UnlimitedPollEvents) const;

private:
    friend class DocumentRevisionPublicationReservation;

    using RevisionMap =
        std::unordered_map<DocumentRevisionKey, DocumentRevision, DocumentRevisionKeyHash>;

    [[nodiscard]] DocumentRevision currentLocked(const DocumentRevisionKey& key) const noexcept;

    const DocumentRevision _maximumRevision;
    const DocumentPublicationSequence _maximumPublicationSequence;
    const std::size_t _journalCapacity;
    mutable std::mutex _mutex;
    RevisionMap _revisions;
    std::optional<DocumentRevisionIdentityBinding> _documentIdentity;
    DocumentPublicationSequence _publicationSequence {0};
    std::deque<DocumentRevisionPublicationEvent> _journal;
};

/** Move-only, exception-safe reservation for one atomic revision publication. */
class AppExport DocumentRevisionPublicationReservation
{
public:
    DocumentRevisionPublicationReservation() = delete;
    DocumentRevisionPublicationReservation(const DocumentRevisionPublicationReservation&) = delete;
    DocumentRevisionPublicationReservation& operator=(
        const DocumentRevisionPublicationReservation&) = delete;

    DocumentRevisionPublicationReservation(DocumentRevisionPublicationReservation&& other) noexcept;
    DocumentRevisionPublicationReservation& operator=(
        DocumentRevisionPublicationReservation&& other) noexcept;
    ~DocumentRevisionPublicationReservation() noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const std::vector<DocumentRevisionConflict>& conflicts() const noexcept;

    /** Publish the already-reserved event and release the index lock. */
    [[nodiscard]] std::vector<DocumentRevisionObservation> commit() noexcept;

    /** Discard the hidden event and release the index lock. Idempotent. */
    void cancel() noexcept;

private:
    friend class DocumentRevisionIndex;

    DocumentRevisionPublicationReservation(
        DocumentRevisionIndex* owner,
        std::unique_lock<std::mutex>&& lock,
        std::vector<DocumentRevisionConflict> conflicts,
        std::vector<DocumentRevisionObservation> observations,
        std::vector<DocumentRevision*> revisionSlots,
        bool journalPrepared) noexcept;

    DocumentRevisionIndex* _owner {nullptr};
    std::unique_lock<std::mutex> _lock;
    std::vector<DocumentRevisionConflict> _conflicts;
    std::vector<DocumentRevisionObservation> _observations;
    std::vector<DocumentRevision*> _revisionSlots;
    bool _journalPrepared {false};
};

}  // namespace App
