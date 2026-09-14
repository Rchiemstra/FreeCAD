// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentRevisionIndex.h"
#include "MutationKind.h"

#include <FCGlobal.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace App
{

class Document;
class DocumentCommitCoordinator;
class PropertyContainer;

/**
 * Index-lock-ordered admission lease for direct revision mutations.
 *
 * Only DocumentRevisionIndex can construct it. Holding the process admission
 * mutex from the final authorization check through the indexed state change
 * closes the check/lock race with prepared-target activation.
 */
class AppExport CollaborationRevisionMutationAdmissionLease final
{
public:
    CollaborationRevisionMutationAdmissionLease(
        const CollaborationRevisionMutationAdmissionLease&) = delete;
    CollaborationRevisionMutationAdmissionLease& operator=(
        const CollaborationRevisionMutationAdmissionLease&) = delete;
    ~CollaborationRevisionMutationAdmissionLease() noexcept;

private:
    friend class DocumentRevisionIndex;

    explicit CollaborationRevisionMutationAdmissionLease(
        const DocumentRevisionIndex& index);

    std::unique_lock<std::mutex> _lock;
};

/**
 * Unforgeable process-target ownership used by the native commit coordinator.
 *
 * The legacy begin/end functions below remain ABI compatible, but their
 * nesting is accounted independently and therefore cannot release this scope.
 */
class AppExport CollaborationPreparedMutationTargetScope final
{
public:
    CollaborationPreparedMutationTargetScope(
        const CollaborationPreparedMutationTargetScope&) = delete;
    CollaborationPreparedMutationTargetScope& operator=(
        const CollaborationPreparedMutationTargetScope&) = delete;
    ~CollaborationPreparedMutationTargetScope() noexcept;

private:
    friend class DocumentCommitCoordinator;

    explicit CollaborationPreparedMutationTargetScope(Document& document);

    Document* _document {nullptr};
};

/** Private bridge used by Document's token-owned compatibility audits. */
class AppExport CollaborationPreparedMutationTargetAccess final
{
private:
    friend class Document;

    static void begin(Document& document, bool readOnly);
    static void end(const Document& document, bool readOnly) noexcept;
};

/** Resolve the document that owns a mutation funnel's property container. */
[[nodiscard]] AppExport Document*
documentFromPropertyContainer(const PropertyContainer* container);

/**
 * Bind and enforce the document and owner thread targeted by one atomic
 * presentation callback. Admission is process-scoped, so a joined worker
 * cannot escape the boundary through otherwise-empty thread-local state.
 *
 * This is a collaboration-integrity guard, not document mutation authority.
 * It survives retirement of the legacy MCP owner/capability gate so a callback
 * still cannot write through a second document.
 */
AppExport void beginAtomicPresentationMutationTarget(Document& document);
AppExport void endAtomicPresentationMutationTarget(const Document& document) noexcept;
AppExport void enforceAtomicPresentationMutationTarget(const Document& document);
AppExport void enforceAtomicPresentationMutationTarget(const Document* document);

/** Nest a read-only validation callback on the current mutation target. */
AppExport void beginCollaborationReadOnlyMutationTarget(Document& document);
AppExport void endCollaborationReadOnlyMutationTarget(const Document& document) noexcept;

/** Reject application-wide lifecycle state changes while a commit target is bound. */
AppExport void enforceCollaborationLifecycleMutationAllowed();

/**
 * Reject direct semantic-revision publication while a prepared mutation owns
 * the process.  The legacy no-index entry points deliberately fail closed
 * during that boundary; new callers must identify the index they intend to
 * mutate.
 */
AppExport void enforceCollaborationRevisionMutationAllowed();
[[nodiscard]] AppExport bool collaborationRevisionMutationAllowed() noexcept;
AppExport void enforceCollaborationRevisionMutationAllowed(
    const DocumentRevisionIndex& index);
[[nodiscard]] AppExport bool collaborationRevisionMutationAllowed(
    const DocumentRevisionIndex& index) noexcept;

/**
 * Coordinator-only, effect-scoped admission for the target revision index.
 *
 * Extension callbacks cannot construct this scope.  The coordinator opens it
 * only while reserving or publishing its already-validated target revision
 * effects; it is never live during apply, recompute, or postcondition code.
 */
class AppExport CollaborationRevisionMutationGrant final
{
public:
    CollaborationRevisionMutationGrant(const CollaborationRevisionMutationGrant&) = delete;
    CollaborationRevisionMutationGrant& operator=(
        const CollaborationRevisionMutationGrant&) = delete;
    ~CollaborationRevisionMutationGrant() noexcept;

private:
    friend class DocumentCommitCoordinator;

    explicit CollaborationRevisionMutationGrant(
        const DocumentRevisionIndex& index) noexcept;

    const DocumentRevisionIndex* _index {nullptr};
};

/** The post-change funnel that observed a compatibility mutation. */
enum class CollaborationMutationSource
{
    PropertyValue,
    PropertyStatus,
    DynamicPropertySchema,
    ObjectAddition,
    ObjectRemoval,
    Unknown
};

/** A deliberately coarse property family used to reject unsupported value semantics. */
enum class CollaborationPropertyFamily
{
    NotApplicable,
    ModelValue,
    Link,
    Unknown
};

/** The scope of the PropertyContainer at the mutation funnel. */
enum class CollaborationContainerKind
{
    DocumentObject,
    Document,
    Unknown
};

/**
 * Pointer-free description of one completed local mutation.
 *
 * Callers derive these values while they have access to the live mutation site. The classifier
 * neither retains nor resolves live objects. Object-scoped sites require both objectName and a
 * non-empty stableObjectIdentity; document-scoped sites require neither.
 */
struct AppExport MutationClassificationInput
{
    CollaborationMutationSource source {CollaborationMutationSource::Unknown};
    MutationKind mutationKind {MutationKind::PropertyWrite};
    CollaborationPropertyFamily propertyFamily {CollaborationPropertyFamily::Unknown};
    CollaborationContainerKind containerKind {CollaborationContainerKind::Unknown};
    std::string objectName;
    std::optional<std::string> stableObjectIdentity;
    /** Separate scalar; never concatenate it with objectName. */
    std::string propertyName;
};

using MutationRevisionEffects = std::vector<DocumentRevisionPublicationRequest>;

/**
 * Return the smallest revision-effect set proven by the supplied mutation site.
 *
 * An unrecognized or internally inconsistent description always returns the document wildcard.
 * This function is deterministic and has no document mutation, locking, or publication effects.
 */
[[nodiscard]] AppExport MutationRevisionEffects
classifyMutation(const MutationClassificationInput& input);

}  // namespace App
