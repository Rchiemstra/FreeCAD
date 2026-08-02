// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/MutationClassification.h"

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace App;

namespace
{

MutationClassificationInput objectSite(CollaborationMutationSource source,
                                       MutationKind kind,
                                       CollaborationPropertyFamily propertyFamily)
{
    return {
        source,
        kind,
        propertyFamily,
        CollaborationContainerKind::DocumentObject,
        "Cube",
        "object-41",
    };
}

void expectEffect(const DocumentRevisionPublicationRequest& actual,
                  const DocumentRevisionKey& expectedKey,
                  const std::optional<std::string>& expectedIdentity)
{
    EXPECT_EQ(actual.key, expectedKey);
    EXPECT_EQ(actual.stableObjectIdentity, expectedIdentity);
}

void expectOnlyWildcard(const MutationClassificationInput& input)
{
    const auto effects = classifyMutation(input);
    ASSERT_EQ(effects.size(), 1U);
    expectEffect(effects.front(), DocumentRevisionKey::unknownModelMutation(), std::nullopt);
}

}  // namespace

TEST(MutationClassificationTest, classifiesObjectModelValueWithoutWildcard)
{
    const auto effects = classifyMutation(objectSite(CollaborationMutationSource::PropertyValue,
                                                     MutationKind::PropertyWrite,
                                                     CollaborationPropertyFamily::ModelValue));

    ASSERT_EQ(effects.size(), 1U);
    expectEffect(effects.front(), DocumentRevisionKey::objectModel("Cube"), "object-41");
}

TEST(MutationClassificationTest, classifiesLinkValueAsObjectAndDocumentStructure)
{
    const auto effects = classifyMutation(objectSite(CollaborationMutationSource::PropertyValue,
                                                     MutationKind::PropertyWrite,
                                                     CollaborationPropertyFamily::Link));

    ASSERT_EQ(effects.size(), 2U);
    expectEffect(effects[0], DocumentRevisionKey::objectStructure("Cube"), "object-41");
    expectEffect(effects[1], DocumentRevisionKey::documentStructure(), std::nullopt);
}

TEST(MutationClassificationTest, classifiesPropertyStatusAtItsContainerScope)
{
    const auto objectEffects = classifyMutation(
        objectSite(CollaborationMutationSource::PropertyStatus,
                   MutationKind::StructuralProperty,
                   CollaborationPropertyFamily::NotApplicable));

    ASSERT_EQ(objectEffects.size(), 1U);
    expectEffect(objectEffects[0], DocumentRevisionKey::objectStructure("Cube"), "object-41");

    const MutationClassificationInput documentSite {
        CollaborationMutationSource::PropertyStatus,
        MutationKind::StructuralProperty,
        CollaborationPropertyFamily::NotApplicable,
        CollaborationContainerKind::Document,
        {},
        std::nullopt,
    };
    const auto documentEffects = classifyMutation(documentSite);
    ASSERT_EQ(documentEffects.size(), 1U);
    expectEffect(documentEffects[0], DocumentRevisionKey::documentStructure(), std::nullopt);
}

TEST(MutationClassificationTest, classifiesDynamicPropertySchemaAtItsContainerScope)
{
    const auto objectEffects = classifyMutation(
        objectSite(CollaborationMutationSource::DynamicPropertySchema,
                   MutationKind::StructuralProperty,
                   CollaborationPropertyFamily::NotApplicable));
    ASSERT_EQ(objectEffects.size(), 1U);
    expectEffect(objectEffects[0], DocumentRevisionKey::objectStructure("Cube"), "object-41");

    const MutationClassificationInput documentSite {
        CollaborationMutationSource::DynamicPropertySchema,
        MutationKind::StructuralProperty,
        CollaborationPropertyFamily::NotApplicable,
        CollaborationContainerKind::Document,
        {},
        std::nullopt,
    };
    const auto documentEffects = classifyMutation(documentSite);
    ASSERT_EQ(documentEffects.size(), 1U);
    expectEffect(documentEffects[0], DocumentRevisionKey::documentStructure(), std::nullopt);
}

TEST(MutationClassificationTest, classifiesObjectLifecycleWithSmallestPhaseOneEffects)
{
    const auto addition = classifyMutation(objectSite(CollaborationMutationSource::ObjectAddition,
                                                      MutationKind::AddObject,
                                                      CollaborationPropertyFamily::NotApplicable));
    const auto removal = classifyMutation(objectSite(CollaborationMutationSource::ObjectRemoval,
                                                     MutationKind::RemoveObject,
                                                     CollaborationPropertyFamily::NotApplicable));

    for (const auto* effects : {&addition, &removal}) {
        ASSERT_EQ(effects->size(), 2U);
        expectEffect((*effects)[0], DocumentRevisionKey::objectExistence("Cube"), "object-41");
        expectEffect((*effects)[1], DocumentRevisionKey::documentStructure(), std::nullopt);
    }
}

TEST(MutationClassificationTest, everyUnsupportedMutationKindFallsBackToWildcard)
{
    constexpr std::array unsupportedKinds {
        MutationKind::AddObject,
        MutationKind::RemoveObject,
        MutationKind::Recompute,
        MutationKind::Undo,
        MutationKind::Redo,
        MutationKind::Save,
        MutationKind::SaveAs,
        MutationKind::Close,
        MutationKind::TransactionOpen,
        MutationKind::TransactionCommit,
        MutationKind::TransactionAbort,
        MutationKind::ImportExport,
        MutationKind::BulkCopy,
        MutationKind::StructuralProperty,
    };
    for (const auto kind : unsupportedKinds) {
        SCOPED_TRACE(mutationKindName(kind));
        expectOnlyWildcard(objectSite(CollaborationMutationSource::PropertyValue,
                                      kind,
                                      CollaborationPropertyFamily::ModelValue));
    }

    auto invalidKind = objectSite(CollaborationMutationSource::PropertyValue,
                                  MutationKind::PropertyWrite,
                                  CollaborationPropertyFamily::ModelValue);
    invalidKind.mutationKind = static_cast<MutationKind>(0);
    expectOnlyWildcard(invalidKind);
}

TEST(MutationClassificationTest, unknownSourcePropertyFamilyAndContainerFallBackToWildcard)
{
    auto site = objectSite(CollaborationMutationSource::PropertyValue,
                           MutationKind::PropertyWrite,
                           CollaborationPropertyFamily::ModelValue);

    site.source = CollaborationMutationSource::Unknown;
    expectOnlyWildcard(site);
    site.source = static_cast<CollaborationMutationSource>(999);
    expectOnlyWildcard(site);

    site.source = CollaborationMutationSource::PropertyValue;
    site.propertyFamily = CollaborationPropertyFamily::Unknown;
    expectOnlyWildcard(site);
    site.propertyFamily = CollaborationPropertyFamily::NotApplicable;
    expectOnlyWildcard(site);
    site.propertyFamily = static_cast<CollaborationPropertyFamily>(999);
    expectOnlyWildcard(site);

    site.propertyFamily = CollaborationPropertyFamily::ModelValue;
    site.containerKind = CollaborationContainerKind::Unknown;
    expectOnlyWildcard(site);
    site.containerKind = CollaborationContainerKind::Document;
    expectOnlyWildcard(site);
    site.containerKind = static_cast<CollaborationContainerKind>(999);
    expectOnlyWildcard(site);
}

TEST(MutationClassificationTest, incompleteOrContradictoryObjectIdentityFallsBackToWildcard)
{
    auto site = objectSite(CollaborationMutationSource::PropertyValue,
                           MutationKind::PropertyWrite,
                           CollaborationPropertyFamily::ModelValue);
    site.objectName.clear();
    expectOnlyWildcard(site);

    site.objectName = "Cube";
    site.stableObjectIdentity.reset();
    expectOnlyWildcard(site);

    site.stableObjectIdentity = "";
    expectOnlyWildcard(site);

    MutationClassificationInput documentSite {
        CollaborationMutationSource::DynamicPropertySchema,
        MutationKind::StructuralProperty,
        CollaborationPropertyFamily::NotApplicable,
        CollaborationContainerKind::Document,
        "Cube",
        std::nullopt,
    };
    expectOnlyWildcard(documentSite);
    documentSite.objectName.clear();
    documentSite.stableObjectIdentity = "object-41";
    expectOnlyWildcard(documentSite);
}

TEST(MutationClassificationTest, mismatchedKnownSitesFallBackToWildcard)
{
    expectOnlyWildcard(objectSite(CollaborationMutationSource::PropertyStatus,
                                  MutationKind::PropertyWrite,
                                  CollaborationPropertyFamily::NotApplicable));
    expectOnlyWildcard(objectSite(CollaborationMutationSource::PropertyStatus,
                                  MutationKind::StructuralProperty,
                                  CollaborationPropertyFamily::ModelValue));
    expectOnlyWildcard(objectSite(CollaborationMutationSource::DynamicPropertySchema,
                                  MutationKind::StructuralProperty,
                                  CollaborationPropertyFamily::Link));
    expectOnlyWildcard(objectSite(CollaborationMutationSource::ObjectAddition,
                                  MutationKind::RemoveObject,
                                  CollaborationPropertyFamily::NotApplicable));
    expectOnlyWildcard(objectSite(CollaborationMutationSource::ObjectRemoval,
                                  MutationKind::RemoveObject,
                                  CollaborationPropertyFamily::ModelValue));
}

TEST(MutationClassificationTest, everyUnrecognizedValidTupleFallsBackToWildcard)
{
    constexpr std::array sources {
        CollaborationMutationSource::PropertyValue,
        CollaborationMutationSource::PropertyStatus,
        CollaborationMutationSource::DynamicPropertySchema,
        CollaborationMutationSource::ObjectAddition,
        CollaborationMutationSource::ObjectRemoval,
        CollaborationMutationSource::Unknown,
    };
    constexpr std::array kinds {
        MutationKind::PropertyWrite,
        MutationKind::AddObject,
        MutationKind::RemoveObject,
        MutationKind::Recompute,
        MutationKind::Undo,
        MutationKind::Redo,
        MutationKind::Save,
        MutationKind::SaveAs,
        MutationKind::Close,
        MutationKind::TransactionOpen,
        MutationKind::TransactionCommit,
        MutationKind::TransactionAbort,
        MutationKind::ImportExport,
        MutationKind::BulkCopy,
        MutationKind::StructuralProperty,
    };
    constexpr std::array families {
        CollaborationPropertyFamily::NotApplicable,
        CollaborationPropertyFamily::ModelValue,
        CollaborationPropertyFamily::Link,
        CollaborationPropertyFamily::Unknown,
    };
    constexpr std::array containers {
        CollaborationContainerKind::DocumentObject,
        CollaborationContainerKind::Document,
        CollaborationContainerKind::Unknown,
    };

    for (const auto source : sources) {
        for (const auto kind : kinds) {
            for (const auto family : families) {
                for (const auto container : containers) {
                    const bool objectContainer =
                        container == CollaborationContainerKind::DocumentObject;
                    const MutationClassificationInput input {
                        source,
                        kind,
                        family,
                        container,
                        objectContainer ? "Cube" : "",
                        objectContainer ? std::optional<std::string> {"object-41"} : std::nullopt,
                    };
                    const bool supportedPropertyValue =
                        source == CollaborationMutationSource::PropertyValue
                        && kind == MutationKind::PropertyWrite && objectContainer
                        && (family == CollaborationPropertyFamily::ModelValue
                            || family == CollaborationPropertyFamily::Link);
                    const bool supportedPropertyStatus =
                        source == CollaborationMutationSource::PropertyStatus
                        && kind == MutationKind::StructuralProperty
                        && family == CollaborationPropertyFamily::NotApplicable
                        && container != CollaborationContainerKind::Unknown;
                    const bool supportedDynamicSchema =
                        source == CollaborationMutationSource::DynamicPropertySchema
                        && kind == MutationKind::StructuralProperty
                        && family == CollaborationPropertyFamily::NotApplicable
                        && container != CollaborationContainerKind::Unknown;
                    const bool supportedAddition =
                        source == CollaborationMutationSource::ObjectAddition
                        && kind == MutationKind::AddObject
                        && family == CollaborationPropertyFamily::NotApplicable && objectContainer;
                    const bool supportedRemoval =
                        source == CollaborationMutationSource::ObjectRemoval
                        && kind == MutationKind::RemoveObject
                        && family == CollaborationPropertyFamily::NotApplicable && objectContainer;

                    if (!(supportedPropertyValue || supportedPropertyStatus
                          || supportedDynamicSchema || supportedAddition || supportedRemoval)) {
                        expectOnlyWildcard(input);
                    }
                }
            }
        }
    }
}

TEST(MutationClassificationTest, classificationIsDeterministicAndReturnsIndependentValues)
{
    const auto input = objectSite(CollaborationMutationSource::PropertyValue,
                                  MutationKind::PropertyWrite,
                                  CollaborationPropertyFamily::Link);
    auto first = classifyMutation(input);
    const auto second = classifyMutation(input);

    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].key, second[index].key);
        EXPECT_EQ(first[index].stableObjectIdentity, second[index].stableObjectIdentity);
    }

    first[0].key.subject = "mutated-copy";
    EXPECT_EQ(second[0].key, DocumentRevisionKey::objectStructure("Cube"));
}

TEST(MutationClassificationTest, everyTypedResultIsCanonicalAndDeduplicated)
{
    const std::array inputs {
        objectSite(CollaborationMutationSource::PropertyValue,
                   MutationKind::PropertyWrite,
                   CollaborationPropertyFamily::ModelValue),
        objectSite(CollaborationMutationSource::PropertyValue,
                   MutationKind::PropertyWrite,
                   CollaborationPropertyFamily::Link),
        objectSite(CollaborationMutationSource::PropertyStatus,
                   MutationKind::StructuralProperty,
                   CollaborationPropertyFamily::NotApplicable),
        objectSite(CollaborationMutationSource::DynamicPropertySchema,
                   MutationKind::StructuralProperty,
                   CollaborationPropertyFamily::NotApplicable),
        objectSite(CollaborationMutationSource::ObjectAddition,
                   MutationKind::AddObject,
                   CollaborationPropertyFamily::NotApplicable),
        objectSite(CollaborationMutationSource::ObjectRemoval,
                   MutationKind::RemoveObject,
                   CollaborationPropertyFamily::NotApplicable),
    };

    for (const auto& input : inputs) {
        const auto effects = classifyMutation(input);
        for (std::size_t index = 1; index < effects.size(); ++index) {
            EXPECT_TRUE(effects[index - 1].key < effects[index].key)
                << "duplicate or non-canonical keys at index " << index;
        }
    }
}
