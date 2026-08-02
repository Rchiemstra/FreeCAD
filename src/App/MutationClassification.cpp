// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MutationClassification.h"

#include <algorithm>

namespace
{

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

App::MutationRevisionEffects App::classifyMutation(const MutationClassificationInput& input)
{
    switch (input.source) {
        case CollaborationMutationSource::PropertyValue:
            if (input.mutationKind != MutationKind::PropertyWrite || !isStrictObjectSite(input)) {
                return unknownMutation();
            }
            switch (input.propertyFamily) {
                case CollaborationPropertyFamily::ModelValue:
                    return canonicalEffects(
                        {objectEffect(DocumentRevisionKey::objectModel(input.objectName), input)});
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
