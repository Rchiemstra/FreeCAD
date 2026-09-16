// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MutationClassification.h"

#include "Document.h"
#include "DocumentObject.h"
#include "PropertyContainer.h"

#include <Base/Exception.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace
{

struct AtomicPresentationMutationAdmission
{
    std::mutex mutex;
    const App::Document* target {nullptr};
    const App::DocumentRevisionIndex* targetRevisionIndex {nullptr};
    std::thread::id owner;
    unsigned int legacyDepth {0};
    unsigned int preparedDepth {0};
    unsigned int legacyReadOnlyDepth {0};
    unsigned int preparedReadOnlyDepth {0};
};

thread_local const App::DocumentRevisionIndex* coordinatorRevisionGrantIndex {nullptr};
thread_local unsigned int coordinatorRevisionGrantDepth {0};

AtomicPresentationMutationAdmission& atomicPresentationMutationAdmission()
{
    // A prepared commit may execute arbitrary extension code. Keep its
    // admission state alive through late extension teardown, and make it
    // process-scoped so joined worker threads cannot escape an owner-thread
    // mutation/read-only boundary merely because thread_local state is empty.
    static auto* admission = new AtomicPresentationMutationAdmission;
    return *admission;
}

unsigned int mutationTargetDepth(
    const AtomicPresentationMutationAdmission& admission) noexcept
{
    return admission.legacyDepth + admission.preparedDepth;
}

unsigned int readOnlyTargetDepth(
    const AtomicPresentationMutationAdmission& admission) noexcept
{
    return admission.legacyReadOnlyDepth + admission.preparedReadOnlyDepth;
}

bool revisionMutationAllowedLocked(
    AtomicPresentationMutationAdmission& admission,
    const App::DocumentRevisionIndex& index) noexcept
{
    if (!admission.target) {
        return true;
    }
    if (readOnlyTargetDepth(admission) != 0) {
        const_cast<App::Document*>(admission.target)
            ->noteCollaborationReadOnlyMutationAttempt();
        return false;
    }
    return admission.owner == std::this_thread::get_id()
        && admission.targetRevisionIndex == &index
        && coordinatorRevisionGrantIndex == &index
        && coordinatorRevisionGrantDepth != 0;
}

void beginMutationTarget(App::Document& document, bool prepared, bool readOnly)
{
    auto& admission = atomicPresentationMutationAdmission();
    std::lock_guard lock(admission.mutex);
    const auto caller = std::this_thread::get_id();
    if (admission.target
        && (admission.target != &document || admission.owner != caller)) {
        throw Base::RuntimeError(
            "an atomic presentation mutation target is already active in this process");
    }
    // Public compatibility nesting must never acquire or release a slice of a
    // coordinator-owned boundary. Conversely, a prepared mutation cannot be
    // embedded in a caller-owned legacy scope whose lifetime it cannot prove.
    if ((prepared && admission.legacyDepth != 0)
        || (!prepared && admission.preparedDepth != 0)) {
        throw Base::RuntimeError(
            "atomic presentation mutation target ownership cannot be mixed");
    }
    auto& depth = prepared ? admission.preparedDepth : admission.legacyDepth;
    auto& readOnlyDepth = prepared ? admission.preparedReadOnlyDepth
                                   : admission.legacyReadOnlyDepth;
    if (depth == std::numeric_limits<unsigned int>::max()
        || (readOnly
            && readOnlyDepth == std::numeric_limits<unsigned int>::max())) {
        throw Base::RuntimeError("atomic presentation mutation target depth overflow");
    }
    if (!admission.target) {
        admission.target = &document;
        admission.targetRevisionIndex = &document.collaborationRevisions();
        admission.owner = caller;
    }
    ++depth;
    if (readOnly) {
        ++readOnlyDepth;
    }
}

void endMutationTarget(const App::Document& document,
                       bool prepared,
                       bool readOnly) noexcept
{
    try {
        auto& admission = atomicPresentationMutationAdmission();
        std::lock_guard lock(admission.mutex);
        auto& depth = prepared ? admission.preparedDepth : admission.legacyDepth;
        auto& readOnlyDepth = prepared ? admission.preparedReadOnlyDepth
                                       : admission.legacyReadOnlyDepth;
        if (admission.target != &document
            || admission.owner != std::this_thread::get_id() || depth == 0
            || (readOnly && readOnlyDepth == 0)) {
            return;
        }
        if (readOnly) {
            --readOnlyDepth;
        }
        --depth;
        if (mutationTargetDepth(admission) == 0) {
            admission.target = nullptr;
            admission.targetRevisionIndex = nullptr;
            admission.owner = std::thread::id {};
            admission.legacyReadOnlyDepth = 0;
            admission.preparedReadOnlyDepth = 0;
        }
    }
    catch (...) {
        // Teardown is noexcept. A mutex failure must not replace the original
        // operation result or exception.
    }
}

using App::CollaborationContainerKind;
using App::CollaborationMutationSource;
using App::CollaborationPropertyFamily;
using App::DocumentRevisionKey;
using App::DocumentRevisionPublicationRequest;
using App::MutationClassificationInput;
using App::MutationKind;
using App::MutationRevisionEffects;

MutationRevisionEffects unknownMutation()
{
    return {{DocumentRevisionKey::unknownModelMutation(), std::nullopt}};
}

MutationRevisionEffects canonicalEffects(MutationRevisionEffects effects)
{
    const auto less = [](const DocumentRevisionPublicationRequest& left,
                         const DocumentRevisionPublicationRequest& right) {
        if (left.key != right.key) {
            return left.key < right.key;
        }
        return left.stableObjectIdentity < right.stableObjectIdentity;
    };
    const auto equal = [](const DocumentRevisionPublicationRequest& left,
                          const DocumentRevisionPublicationRequest& right) {
        return left.key == right.key
            && left.stableObjectIdentity == right.stableObjectIdentity;
    };
    std::sort(effects.begin(), effects.end(), less);
    effects.erase(std::unique(effects.begin(), effects.end(), equal), effects.end());
    return effects;
}

bool hasValidObjectIdentity(const MutationClassificationInput& input)
{
    return !input.objectName.empty() && input.stableObjectIdentity
        && !input.stableObjectIdentity->empty();
}

bool isStrictObjectSite(const MutationClassificationInput& input)
{
    return input.containerKind == CollaborationContainerKind::DocumentObject
        && hasValidObjectIdentity(input);
}

bool isStrictDocumentSite(const MutationClassificationInput& input)
{
    return input.containerKind == CollaborationContainerKind::Document && input.objectName.empty()
        && !input.stableObjectIdentity;
}

DocumentRevisionPublicationRequest objectEffect(const DocumentRevisionKey& key,
                                                const MutationClassificationInput& input)
{
    return {key, input.stableObjectIdentity};
}

}  // namespace

App::Document* App::documentFromPropertyContainer(const PropertyContainer* container)
{
    if (!container) {
        return nullptr;
    }
    if (const auto* document = dynamic_cast<const Document*>(container)) {
        return const_cast<Document*>(document);
    }
    if (const auto* object = dynamic_cast<const DocumentObject*>(container)) {
        return object->getDocument();
    }
    return nullptr;
}

void App::beginAtomicPresentationMutationTarget(Document& document)
{
    beginMutationTarget(document, false, false);
}

void App::endAtomicPresentationMutationTarget(const Document& document) noexcept
{
    endMutationTarget(document, false, false);
}

void App::enforceAtomicPresentationMutationTarget(const Document& document)
{
    auto& admission = atomicPresentationMutationAdmission();
    std::lock_guard lock(admission.mutex);
    if (!admission.target) {
        return;
    }
    if (readOnlyTargetDepth(admission) != 0) {
        const_cast<Document*>(admission.target)
            ->noteCollaborationReadOnlyMutationAttempt();
        throw Base::RuntimeError(
            "mutation is unavailable during a collaboration postcondition check");
    }
    if (admission.owner != std::this_thread::get_id()) {
        throw Base::RuntimeError(
            "mutation is unavailable from a non-owner thread during an atomic presentation callback");
    }
    if (admission.target != &document) {
        throw Base::RuntimeError(
            "cross-document mutation of document '" + std::string(document.getName())
            + "' is unavailable while the atomic presentation callback targets document '"
            + std::string(admission.target->getName()) + "'");
    }
}

void App::enforceAtomicPresentationMutationTarget(const Document* document)
{
    if (document) {
        enforceAtomicPresentationMutationTarget(*document);
    }
}

void App::beginCollaborationReadOnlyMutationTarget(Document& document)
{
    beginMutationTarget(document, false, true);
}

void App::endCollaborationReadOnlyMutationTarget(const Document& document) noexcept
{
    endMutationTarget(document, false, true);
}

App::CollaborationPreparedMutationTargetScope::CollaborationPreparedMutationTargetScope(
    Document& document)
    : _document(&document)
{
    beginMutationTarget(document, true, false);
}

App::CollaborationPreparedMutationTargetScope::~CollaborationPreparedMutationTargetScope() noexcept
{
    if (_document) {
        endMutationTarget(*_document, true, false);
    }
}

void App::CollaborationPreparedMutationTargetAccess::begin(Document& document,
                                                           bool readOnly)
{
    beginMutationTarget(document, true, readOnly);
}

void App::CollaborationPreparedMutationTargetAccess::end(
    const Document& document,
    bool readOnly) noexcept
{
    endMutationTarget(document, true, readOnly);
}

void App::enforceCollaborationLifecycleMutationAllowed()
{
    auto& admission = atomicPresentationMutationAdmission();
    std::lock_guard lock(admission.mutex);
    if (!admission.target) {
        return;
    }
    if (readOnlyTargetDepth(admission) != 0) {
        const_cast<Document*>(admission.target)
            ->noteCollaborationReadOnlyMutationAttempt();
    }
    throw Base::RuntimeError(
        "document lifecycle changes are unavailable during the prepared commit for document '"
        + std::string(admission.target->getName())
        + "'; create or close documents before starting the commit (MCP callers should use "
          "create_document)");
}

void App::enforceCollaborationRevisionMutationAllowed()
{
    if (collaborationRevisionMutationAllowed()) {
        return;
    }
    throw Base::RuntimeError(
        "semantic revision mutation requires indexed coordinator admission during a prepared commit");
}

bool App::collaborationRevisionMutationAllowed() noexcept
{
    try {
        auto& admission = atomicPresentationMutationAdmission();
        std::lock_guard lock(admission.mutex);
        if (!admission.target) {
            return true;
        }
        if (readOnlyTargetDepth(admission) != 0) {
            const_cast<Document*>(admission.target)
                ->noteCollaborationReadOnlyMutationAttempt();
            return false;
        }
        // This ABI-preserved entry point cannot prove which document index is
        // being changed.  Fail closed throughout a prepared mutation, even
        // while the coordinator has its private indexed grant.
        return false;
    }
    catch (...) {
        // Revision publication is never required for cleanup. If process-wide
        // admission cannot be inspected, fail closed without terminating a
        // noexcept reservation/destructor path.
        return false;
    }
}

void App::enforceCollaborationRevisionMutationAllowed(
    const DocumentRevisionIndex& index)
{
    if (collaborationRevisionMutationAllowed(index)) {
        return;
    }
    throw Base::RuntimeError(
        "semantic revision mutation is unavailable during a prepared commit");
}

bool App::collaborationRevisionMutationAllowed(
    const DocumentRevisionIndex& index) noexcept
{
    try {
        auto& admission = atomicPresentationMutationAdmission();
        std::lock_guard lock(admission.mutex);
        return revisionMutationAllowedLocked(admission, index);
    }
    catch (...) {
        return false;
    }
}

App::CollaborationRevisionMutationAdmissionLease::
    CollaborationRevisionMutationAdmissionLease(
        const DocumentRevisionIndex& index)
    : _lock(atomicPresentationMutationAdmission().mutex)
{
    auto& admission = atomicPresentationMutationAdmission();
    if (!revisionMutationAllowedLocked(admission, index)) {
        throw Base::RuntimeError(
            "semantic revision mutation is unavailable during a prepared commit");
    }
}

App::CollaborationRevisionMutationAdmissionLease::~CollaborationRevisionMutationAdmissionLease()
    noexcept = default;

App::CollaborationRevisionMutationGrant::CollaborationRevisionMutationGrant(
    const DocumentRevisionIndex& index) noexcept
    : _index(&index)
{
    // Construction is coordinator-private and performs no locking or
    // allocation, which keeps the post-native-commit publication path
    // noexcept.  Process target/index/owner admission is still verified by
    // collaborationRevisionMutationAllowed() at the actual index operation.
    if ((coordinatorRevisionGrantIndex && coordinatorRevisionGrantIndex != &index)
        || coordinatorRevisionGrantDepth == std::numeric_limits<unsigned int>::max()) {
        std::terminate();
    }
    coordinatorRevisionGrantIndex = &index;
    ++coordinatorRevisionGrantDepth;
}

App::CollaborationRevisionMutationGrant::~CollaborationRevisionMutationGrant() noexcept
{
    if (!_index) {
        return;
    }
    if (coordinatorRevisionGrantIndex != _index
        || coordinatorRevisionGrantDepth == 0) {
        std::terminate();
    }
    --coordinatorRevisionGrantDepth;
    if (coordinatorRevisionGrantDepth == 0) {
        coordinatorRevisionGrantIndex = nullptr;
    }
}

App::MutationRevisionEffects App::classifyMutation(const MutationClassificationInput& input)
{
    switch (input.source) {
        case CollaborationMutationSource::PropertyValue:
            if (input.mutationKind != MutationKind::PropertyWrite || !isStrictObjectSite(input)) {
                return unknownMutation();
            }
            switch (input.propertyFamily) {
                case CollaborationPropertyFamily::ModelValue:
                    if (input.propertyName.empty()) {
                        return unknownMutation();
                    }
                    // App::DocumentObject::Visibility is deliberately shared
                    // presentation state. Gui publishes its provider revision;
                    // it must not enter the App model revision stream.
                    if (input.propertyName == "Visibility") {
                        return {};
                    }
                    return canonicalEffects(
                        {objectEffect(DocumentRevisionKey::objectProperty(
                                          input.objectName, input.propertyName),
                                      input)});
                case CollaborationPropertyFamily::Link:
                    return canonicalEffects({
                        objectEffect(DocumentRevisionKey::objectStructure(input.objectName), input),
                        {DocumentRevisionKey::documentStructure(), std::nullopt},
                    });
                case CollaborationPropertyFamily::NotApplicable:
                case CollaborationPropertyFamily::Unknown:
                    return unknownMutation();
            }
            return unknownMutation();

        case CollaborationMutationSource::PropertyStatus:
            if (input.mutationKind == MutationKind::StructuralProperty
                && input.propertyFamily == CollaborationPropertyFamily::NotApplicable) {
                if (isStrictObjectSite(input)) {
                    return canonicalEffects({
                        objectEffect(DocumentRevisionKey::objectStructure(input.objectName), input),
                    });
                }
                if (isStrictDocumentSite(input)) {
                    return canonicalEffects(
                        {{DocumentRevisionKey::documentStructure(), std::nullopt}});
                }
            }
            return unknownMutation();

        case CollaborationMutationSource::DynamicPropertySchema:
            if (input.mutationKind != MutationKind::StructuralProperty
                || input.propertyFamily != CollaborationPropertyFamily::NotApplicable) {
                return unknownMutation();
            }
            if (isStrictObjectSite(input)) {
                return canonicalEffects({
                    objectEffect(DocumentRevisionKey::objectStructure(input.objectName), input),
                });
            }
            if (isStrictDocumentSite(input)) {
                return canonicalEffects(
                    {{DocumentRevisionKey::documentStructure(), std::nullopt}});
            }
            return unknownMutation();

        case CollaborationMutationSource::ObjectAddition:
        case CollaborationMutationSource::ObjectRemoval: {
            const MutationKind expectedKind =
                input.source == CollaborationMutationSource::ObjectAddition
                ? MutationKind::AddObject
                : MutationKind::RemoveObject;
            if (input.mutationKind != expectedKind
                || input.propertyFamily != CollaborationPropertyFamily::NotApplicable
                || !isStrictObjectSite(input)) {
                return unknownMutation();
            }
            return canonicalEffects({
                objectEffect(DocumentRevisionKey::objectExistence(input.objectName), input),
                {DocumentRevisionKey::documentStructure(), std::nullopt},
            });
        }

        case CollaborationMutationSource::Unknown:
            return unknownMutation();
    }
    return unknownMutation();
}
