// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/DocumentRevisionIndex.h"
#include <3rdParty/json/single_include/nlohmann/json.hpp>

#include <atomic>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace App;

namespace
{

constexpr DocumentInstanceId TestDocumentInstanceId = 41;
constexpr DocumentLifecycleEpoch TestLifecycleEpoch = 7;

void bindTestIdentity(DocumentRevisionIndex& index)
{
    index.bindDocumentIdentity(TestDocumentInstanceId, TestLifecycleEpoch);
}

DocumentRevisionPublicationRequest objectChange(const DocumentRevisionKey& key,
                                                std::string stableObjectIdentity = "100")
{
    return {key, std::move(stableObjectIdentity)};
}

DocumentRevisionCursor testCursor(
    DocumentPublicationSequence afterSequence,
    DocumentInstanceId documentInstanceId = TestDocumentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch = TestLifecycleEpoch)
{
    return {documentInstanceId, lifecycleEpoch, afterSequence};
}

}  // namespace

TEST(DocumentRevisionKeyTest, factoriesEnforceObjectAndDocumentScopes)
{
    EXPECT_TRUE(DocumentRevisionKey::objectExistence("Cube").valid());
    EXPECT_TRUE(DocumentRevisionKey::objectModel("Cube").valid());
    EXPECT_TRUE(DocumentRevisionKey::objectStructure("Cube").valid());
    EXPECT_TRUE(DocumentRevisionKey::documentStructure().valid());
    EXPECT_TRUE(DocumentRevisionKey::unknownModelMutation().valid());

    EXPECT_FALSE(DocumentRevisionKey::objectModel({}).valid());
    EXPECT_FALSE(
        (DocumentRevisionKey {DocumentRevisionKind::DocumentStructure, "Cube"}).valid());
    EXPECT_FALSE(
        (DocumentRevisionKey {DocumentRevisionKind::UnknownModelMutation, "Cube"}).valid());
}

TEST(DocumentRevisionKeyTest, supportsValueEqualityOrderingAndHashing)
{
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto sameModel = DocumentRevisionKey::objectModel("Cube");
    const auto otherModel = DocumentRevisionKey::objectModel("Sphere");
    const auto documentStructure = DocumentRevisionKey::documentStructure();
    const auto unknownMutation = DocumentRevisionKey::unknownModelMutation();

    EXPECT_EQ(model, sameModel);
    EXPECT_NE(model, otherModel);
    EXPECT_TRUE(model < otherModel || otherModel < model);
    EXPECT_NE(documentStructure, unknownMutation);

    std::unordered_set<DocumentRevisionKey, DocumentRevisionKeyHash> keys;
    keys.insert(model);
    keys.insert(sameModel);
    keys.insert(otherModel);
    keys.insert(documentStructure);
    keys.insert(unknownMutation);
    EXPECT_EQ(keys.size(), 4U);
    EXPECT_EQ(keys.count(documentStructure), 1U);
    EXPECT_EQ(keys.count(unknownMutation), 1U);
}

TEST(DocumentRevisionIndexTest, startsEveryScopeAtZeroAndAdvancesIndependently)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto existence = DocumentRevisionKey::objectExistence("Cube");
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");
    const auto document = DocumentRevisionKey::documentStructure();
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();

    for (const auto& key : {existence, model, structure, document, wildcard}) {
        EXPECT_EQ(index.current(key), 0U);
    }

    const auto published = index.publish({objectChange(model)});
    ASSERT_EQ(published.size(), 1U);
    EXPECT_EQ(published.front(), DocumentRevisionObservation(model, 1));
    EXPECT_EQ(index.current(model), 1U);
    EXPECT_EQ(index.current(existence), 0U);
    EXPECT_EQ(index.current(structure), 0U);
    EXPECT_EQ(index.current(document), 0U);
    EXPECT_EQ(index.current(wildcard), 0U);
}

TEST(DocumentRevisionIndexTest, captureDeduplicatesInFirstSeenOrder)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto first = DocumentRevisionKey::objectModel("Cube");
    const auto second = DocumentRevisionKey::documentStructure();
    static_cast<void>(index.publish({second}));

    const auto captured = index.capture({first, second, first});
    ASSERT_EQ(captured.size(), 2U);
    EXPECT_EQ(captured[0], DocumentRevisionObservation(first, 0));
    EXPECT_EQ(captured[1], DocumentRevisionObservation(second, 1));
}

TEST(DocumentRevisionIndexTest, validateReturnsOnlyExactStaleObservations)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto changed = DocumentRevisionKey::objectModel("Cube");
    const auto unchanged = DocumentRevisionKey::objectModel("Sphere");
    const auto observations = index.capture({changed, unchanged});

    static_cast<void>(index.publish({objectChange(changed)}));
    const auto conflicts = index.validate(observations);

    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts.front(), DocumentRevisionConflict(changed, 0, 1));
}

TEST(DocumentRevisionIndexTest, wildcardPublicationIsDocumentWideAndMonotonic)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto beforeMutation = index.capture({wildcard});

    EXPECT_EQ(index.publishUnknownModelMutation(), DocumentRevisionObservation(wildcard, 1));
    EXPECT_EQ(index.publishUnknownModelMutation(), DocumentRevisionObservation(wildcard, 2));
    EXPECT_EQ(index.current(model), 0U);

    const auto conflicts = index.validate(beforeMutation);
    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts.front(), DocumentRevisionConflict(wildcard, 0, 2));
}

TEST(DocumentRevisionIndexTest, documentStructureAndWildcardPublishIndependently)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto documentStructure = DocumentRevisionKey::documentStructure();
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();

    const auto structurePublication = index.publish({documentStructure});
    ASSERT_EQ(structurePublication.size(), 1U);
    EXPECT_EQ(structurePublication.front(), DocumentRevisionObservation(documentStructure, 1));
    EXPECT_EQ(index.current(documentStructure), 1U);
    EXPECT_EQ(index.current(wildcard), 0U);

    EXPECT_EQ(index.publishUnknownModelMutation(), DocumentRevisionObservation(wildcard, 1));
    EXPECT_EQ(index.current(documentStructure), 1U);
    EXPECT_EQ(index.current(wildcard), 1U);
}

TEST(DocumentRevisionIndexTest, publicationIncrementsEachDistinctKeyOnce)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");

    const auto published = index.publish(
        {objectChange(model),
         objectChange(structure),
         objectChange(model),
         objectChange(structure)});
    ASSERT_EQ(published.size(), 2U);
    EXPECT_EQ(published[0], DocumentRevisionObservation(model, 1));
    EXPECT_EQ(published[1], DocumentRevisionObservation(structure, 1));
    EXPECT_EQ(index.current(model), 1U);
    EXPECT_EQ(index.current(structure), 1U);
}

TEST(DocumentRevisionIndexTest, declaredPositiveDeltasAdvanceMultipleKeysAtomically)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");
    const auto documentStructure = DocumentRevisionKey::documentStructure();

    const auto published = index.publish(
        std::vector<DocumentRevisionPublicationRequest> {
            {model, std::string {"17"}, 2},
            {structure, std::string {"17"}, 4},
            {documentStructure, std::nullopt, 3},
        });

    ASSERT_EQ(published.size(), 3U);
    EXPECT_EQ(published[0], DocumentRevisionObservation(model, 2));
    EXPECT_EQ(published[1], DocumentRevisionObservation(structure, 4));
    EXPECT_EQ(published[2], DocumentRevisionObservation(documentStructure, 3));
    EXPECT_EQ(index.current(model), 2U);
    EXPECT_EQ(index.current(structure), 4U);
    EXPECT_EQ(index.current(documentStructure), 3U);

    const auto journal = index.pollPublications(testCursor(0));
    EXPECT_EQ(journal.latestSequence, 1U);
    ASSERT_EQ(journal.events.size(), 1U);
    ASSERT_EQ(journal.events.front().changes.size(), 3U);
    EXPECT_EQ(journal.events.front().changes[0].revision, 2U);
    EXPECT_EQ(journal.events.front().changes[1].revision, 4U);
    EXPECT_EQ(journal.events.front().changes[2].revision, 3U);
}

TEST(DocumentRevisionIndexTest, duplicateAndExpandedObjectModelDeltasMergeByMaximum)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto angle = DocumentRevisionKey::objectProperty("Cube", "Angle");
    const auto label = DocumentRevisionKey::objectProperty("Cube", "Label");
    const auto model = DocumentRevisionKey::objectModel("Cube");

    const auto published = index.publish(
        std::vector<DocumentRevisionPublicationRequest> {
            {angle, std::string {"17"}, 2},
            {model, std::string {"17"}, 5},
            {label, std::string {"17"}, 3},
            {model, std::string {"17"}, 4},
        });

    ASSERT_EQ(published.size(), 3U);
    EXPECT_EQ(published[0], DocumentRevisionObservation(angle, 2));
    EXPECT_EQ(published[1], DocumentRevisionObservation(model, 5));
    EXPECT_EQ(published[2], DocumentRevisionObservation(label, 3));
    EXPECT_EQ(index.current(angle), 2U);
    EXPECT_EQ(index.current(label), 3U);
    EXPECT_EQ(index.current(model), 5U);

    const auto journal = index.pollPublications(testCursor(0));
    EXPECT_EQ(journal.latestSequence, 1U);
    ASSERT_EQ(journal.events.size(), 1U);
    ASSERT_EQ(journal.events.front().changes.size(), 3U);
}

TEST(DocumentRevisionIndexTest, zeroDeltaIsRejectedWithoutPublishingAnyState)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");
    const auto before = index.pollPublications(testCursor(0)).toJson();

    EXPECT_THROW(
        static_cast<void>(index.publish(
            std::vector<DocumentRevisionPublicationRequest> {
                {model, std::string {"17"}, 2},
                {structure, std::string {"17"}, 0},
            })),
        std::invalid_argument);
    EXPECT_EQ(index.current(model), 0U);
    EXPECT_EQ(index.current(structure), 0U);
    EXPECT_EQ(index.pollPublications(testCursor(0)).toJson(), before);
}

TEST(DocumentRevisionIndexTest, declaredDeltaOverflowPreservesEveryPublicationState)
{
    DocumentRevisionIndex index(5);
    bindTestIdentity(index);
    const auto exhausted = DocumentRevisionKey::objectModel("Cube");
    const auto other = DocumentRevisionKey::objectModel("Sphere");
    static_cast<void>(index.publish(
        std::vector<DocumentRevisionPublicationRequest> {
            {exhausted, std::string {"17"}, 4},
        }));
    const auto beforeOverflow = index.pollPublications(testCursor(0)).toJson();

    EXPECT_THROW(
        static_cast<void>(index.publish(
            std::vector<DocumentRevisionPublicationRequest> {
                {other, std::string {"29"}, 3},
                {exhausted, std::string {"17"}, 2},
            })),
        std::overflow_error);
    EXPECT_EQ(index.current(exhausted), 4U);
    EXPECT_EQ(index.current(other), 0U);
    EXPECT_EQ(index.pollPublications(testCursor(0)).toJson(), beforeOverflow);
}

TEST(DocumentRevisionIndexTest, publicationRequestScopesAndDuplicateIdentitiesAreAtomic)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto documentStructure = DocumentRevisionKey::documentStructure();

    EXPECT_THROW(static_cast<void>(index.publish({model})), std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(index.publish(
            std::vector<DocumentRevisionPublicationRequest> {
                {model, std::string {}}})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(index.publish(
            std::vector<DocumentRevisionPublicationRequest> {
                {documentStructure, std::string {"17"}}})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(index.publish(
            {objectChange(model, "17"), objectChange(model, "29")})),
        std::invalid_argument);
    EXPECT_EQ(index.current(model), 0U);
    EXPECT_EQ(index.current(documentStructure), 0U);
    EXPECT_EQ(index.pollPublications(testCursor(0)).latestSequence, 0U);

    const auto published = index.publish(
        {objectChange(model, "17"),
         objectChange(model, "17"),
         {documentStructure, std::nullopt}});
    ASSERT_EQ(published.size(), 2U);
    const auto poll = index.pollPublications(testCursor(0));
    ASSERT_EQ(poll.events.size(), 1U);
    ASSERT_EQ(poll.events.front().changes.size(), 2U);
    EXPECT_EQ(poll.events.front().changes[0].stableObjectIdentity,
              std::optional<std::string> {"17"});
    EXPECT_EQ(poll.events.front().changes[1].stableObjectIdentity, std::nullopt);
}

TEST(DocumentRevisionIndexTest, overflowLeavesEveryPublishedKeyUnchanged)
{
    DocumentRevisionIndex index(1);
    bindTestIdentity(index);
    const auto exhausted = DocumentRevisionKey::objectModel("Cube");
    const auto other = DocumentRevisionKey::objectModel("Sphere");
    static_cast<void>(index.publish({objectChange(exhausted)}));
    const auto journalBeforeOverflow = index.pollPublications(testCursor(0)).toJson();

    EXPECT_THROW(
        static_cast<void>(index.publish({objectChange(other), objectChange(exhausted)})),
        std::overflow_error);
    EXPECT_EQ(index.current(exhausted), 1U);
    EXPECT_EQ(index.current(other), 0U);
    const auto journalAfterOverflow = index.pollPublications(testCursor(0));
    EXPECT_EQ(journalAfterOverflow.toJson(), journalBeforeOverflow);
    EXPECT_EQ(journalAfterOverflow.latestSequence, 1U);
}

TEST(DocumentRevisionIndexTest, publicationSequenceOverflowLeavesJournalAndRevisionsUnchanged)
{
    DocumentRevisionIndex index(
        std::numeric_limits<DocumentRevision>::max(),
        1,
        4);
    bindTestIdentity(index);
    const auto first = DocumentRevisionKey::objectModel("Cube");
    const auto second = DocumentRevisionKey::objectModel("Sphere");
    static_cast<void>(index.publish({objectChange(first, "17")}));
    const auto beforeOverflow = index.pollPublications(testCursor(0)).toJson();

    EXPECT_THROW(
        static_cast<void>(index.publish({objectChange(second, "29")})),
        std::overflow_error);
    EXPECT_EQ(index.current(first), 1U);
    EXPECT_EQ(index.current(second), 0U);

    const auto afterOverflow = index.pollPublications(testCursor(0));
    EXPECT_EQ(afterOverflow.toJson(), beforeOverflow);
    EXPECT_EQ(afterOverflow.latestSequence, 1U);
    ASSERT_EQ(afterOverflow.events.size(), 1U);
    EXPECT_EQ(afterOverflow.events.front().changes.front().key, first);
}

TEST(DocumentRevisionIndexTest, invalidKeyLeavesPublicationUnchanged)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto valid = DocumentRevisionKey::objectModel("Cube");
    const DocumentRevisionKey invalid {DocumentRevisionKind::DocumentStructure, "Cube"};

    EXPECT_THROW(
        static_cast<void>(index.publish(
            {objectChange(valid), {invalid, std::nullopt}})),
        std::invalid_argument);
    EXPECT_EQ(index.current(valid), 0U);
    EXPECT_THROW(index.current(invalid), std::invalid_argument);
}

TEST(DocumentRevisionIndexTest, forwardOnlyPublicationsPreventAbaValidation)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto beforeChanges = index.capture({model});

    static_cast<void>(index.publish({objectChange(model)}));
    static_cast<void>(index.publish({objectChange(model)}));

    EXPECT_EQ(index.current(model), 2U);
    const auto conflicts = index.validate(beforeChanges);
    ASSERT_EQ(conflicts.size(), 1U);
    EXPECT_EQ(conflicts.front(), DocumentRevisionConflict(model, 0, 2));
}

TEST(DocumentRevisionIndexTest, concurrentPublicationsDoNotLoseIncrements)
{
    constexpr int threadCount = 8;
    constexpr int publicationsPerThread = 500;
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    std::atomic<bool> failed {false};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([&]() {
            try {
                for (int publication = 0; publication < publicationsPerThread; ++publication) {
                    static_cast<void>(
                        index.publish({objectChange(model), objectChange(model)}));
                }
            }
            catch (...) {
                failed = true;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(failed.load());
    EXPECT_EQ(index.current(model),
              static_cast<DocumentRevision>(threadCount * publicationsPerThread));
}

TEST(DocumentRevisionIndexTest, concurrentCaptureNeverSeesPartialMultiKeyPublication)
{
    constexpr int publicationCount = 1000;
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");
    std::atomic<bool> readerReady {false};
    std::atomic<bool> firstPublicationDone {false};
    std::atomic<bool> inProgressCaptureDone {false};
    std::atomic<bool> writerDone {false};
    std::atomic<bool> sawPartialPublication {false};
    std::atomic<std::size_t> observationCount {0};
    std::atomic<std::size_t> inProgressObservationCount {0};

    std::thread writer([&]() {
        while (!readerReady.load()) {
            std::this_thread::yield();
        }
        static_cast<void>(index.publish({objectChange(model), objectChange(structure)}));
        firstPublicationDone = true;
        while (!inProgressCaptureDone.load()) {
            std::this_thread::yield();
        }
        for (int publication = 1; publication < publicationCount; ++publication) {
            static_cast<void>(index.publish({objectChange(model), objectChange(structure)}));
        }
        writerDone = true;
    });

    readerReady = true;
    while (!firstPublicationDone.load()) {
        std::this_thread::yield();
    }
    do {
        const auto captured = index.capture({model, structure});
        ++observationCount;
        if (captured.size() != 2 || captured[0].revision != captured[1].revision) {
            sawPartialPublication = true;
        }
        if (!inProgressCaptureDone.load()) {
            ++inProgressObservationCount;
            inProgressCaptureDone = true;
        }
        std::this_thread::yield();
    } while (!writerDone.load());
    writer.join();

    EXPECT_FALSE(sawPartialPublication.load());
    EXPECT_GT(observationCount.load(), 0U);
    EXPECT_GT(inProgressObservationCount.load(), 0U);
    EXPECT_EQ(index.current(model), publicationCount);
    EXPECT_EQ(index.current(structure), publicationCount);
}

TEST(DocumentRevisionIndexTest, concurrentValidateNeverReportsPartialMultiKeyConflict)
{
    constexpr int publicationCount = 500;
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const auto structure = DocumentRevisionKey::objectStructure("Cube");
    const auto expected = index.capture({model, structure});
    ASSERT_TRUE(index.validate(expected).empty());

    std::atomic<bool> start {false};
    std::atomic<bool> firstPublicationDone {false};
    std::atomic<bool> firstValidationDone {false};
    std::atomic<bool> midpointPublicationDone {false};
    std::atomic<bool> midpointValidationDone {false};
    std::atomic<bool> writerDone {false};
    std::thread writer([&]() {
        while (!start.load()) {
            std::this_thread::yield();
        }
        static_cast<void>(index.publish({objectChange(model), objectChange(structure)}));
        firstPublicationDone = true;
        while (!firstValidationDone.load()) {
            std::this_thread::yield();
        }
        for (int publication = 1; publication < publicationCount; ++publication) {
            static_cast<void>(index.publish({objectChange(model), objectChange(structure)}));
            if (publication == publicationCount / 2) {
                midpointPublicationDone = true;
                while (!midpointValidationDone.load()) {
                    std::this_thread::yield();
                }
            }
        }
        writerDone = true;
    });

    bool sawIncoherentResult = false;
    std::size_t validationCount = 0;
    const auto isCompleteConflictSet = [&](const std::vector<DocumentRevisionConflict>& conflicts) {
        return conflicts.size() == 2 && conflicts[0].key == model
            && conflicts[1].key == structure && conflicts[0].expected == 0
            && conflicts[1].expected == 0 && conflicts[0].current == conflicts[1].current
            && conflicts[0].current > 0;
    };

    start = true;
    while (!firstPublicationDone.load()) {
        std::this_thread::yield();
    }
    do {
        const auto conflicts = index.validate(expected);
        ++validationCount;
        if (!conflicts.empty() && !isCompleteConflictSet(conflicts)) {
            sawIncoherentResult = true;
        }
        if (validationCount == 1) {
            if (!isCompleteConflictSet(conflicts)) {
                sawIncoherentResult = true;
            }
            firstValidationDone = true;
        }
        else if (midpointPublicationDone.load() && !midpointValidationDone.load()) {
            midpointValidationDone = true;
        }
        std::this_thread::yield();
    } while (!writerDone.load());
    writer.join();

    const auto finalConflicts = index.validate(expected);
    EXPECT_FALSE(sawIncoherentResult);
    EXPECT_GT(validationCount, 1U);
    EXPECT_TRUE(midpointValidationDone.load());
    ASSERT_TRUE(isCompleteConflictSet(finalConflicts));
    EXPECT_EQ(finalConflicts[0].current, publicationCount);
    EXPECT_EQ(finalConflicts[1].current, publicationCount);
}

TEST(DocumentRevisionPublicationEventTest, payloadTypesContainValuesInsteadOfPointers)
{
    static_assert(
        !std::is_pointer_v<decltype(DocumentRevisionIdentityBinding::documentInstanceId)>);
    static_assert(
        !std::is_pointer_v<decltype(DocumentRevisionIdentityBinding::lifecycleEpoch)>);
    static_assert(
        !std::is_pointer_v<decltype(DocumentRevisionPublicationEvent::documentInstanceId)>);
    static_assert(
        !std::is_pointer_v<decltype(DocumentRevisionPublicationEvent::lifecycleEpoch)>);
    static_assert(
        !std::is_pointer_v<decltype(DocumentRevisionPublicationEvent::publicationSequence)>);
    static_assert(std::is_same_v<decltype(DocumentRevisionChange::stableObjectIdentity),
                                 std::optional<std::string>>);
    static_assert(
        std::is_same_v<decltype(DocumentRevisionPublicationRequest::stableObjectIdentity),
                       std::optional<std::string>>);
    static_assert(!std::is_pointer_v<decltype(DocumentRevisionCursor::documentInstanceId)>);
    static_assert(!std::is_pointer_v<decltype(DocumentRevisionCursor::lifecycleEpoch)>);
    static_assert(!std::is_pointer_v<decltype(DocumentRevisionCursor::afterSequence)>);
    static_assert(
        std::is_same_v<typename decltype(DocumentRevisionPublicationEvent::changes)::value_type,
                       DocumentRevisionChange>);
    SUCCEED();
}

TEST(DocumentRevisionPublicationEventTest, publicationRequiresBoundIdentityWithoutChangingState)
{
    DocumentRevisionIndex index;
    const auto model = DocumentRevisionKey::objectModel("Cube");

    EXPECT_FALSE(index.documentIdentity().has_value());
    EXPECT_THROW(
        static_cast<void>(index.publish({objectChange(model)})),
        std::logic_error);
    EXPECT_EQ(index.current(model), 0U);

    EXPECT_THROW(
        static_cast<void>(index.pollPublications(testCursor(0))),
        std::logic_error);
}

TEST(DocumentRevisionPublicationEventTest, identityBindingIsIdempotentAndEpochMovesOnlyForward)
{
    DocumentRevisionIndex index;
    const auto model = DocumentRevisionKey::objectModel("Cube");

    EXPECT_THROW(index.bindDocumentIdentity(0, 3), std::invalid_argument);
    EXPECT_THROW(index.bindDocumentIdentity(91, 0), std::invalid_argument);
    EXPECT_NO_THROW(index.bindDocumentIdentity(91, 3));
    EXPECT_NO_THROW(index.bindDocumentIdentity(91, 3));
    static_cast<void>(index.publish({objectChange(model)}));

    EXPECT_NO_THROW(index.bindDocumentIdentity(91, 4));
    EXPECT_NO_THROW(index.bindDocumentIdentity(91, 4));
    static_cast<void>(index.publish({objectChange(model)}));
    EXPECT_THROW(index.bindDocumentIdentity(91, 3), std::invalid_argument);
    EXPECT_THROW(index.bindDocumentIdentity(92, 5), std::logic_error);

    const auto identity = index.documentIdentity();
    ASSERT_TRUE(identity.has_value());
    EXPECT_EQ(*identity, (DocumentRevisionIdentityBinding {91, 4}));

    const auto poll = index.pollPublications({91, 4, 0});
    ASSERT_EQ(poll.events.size(), 2U);
    EXPECT_EQ(poll.events[0].documentInstanceId, 91U);
    EXPECT_EQ(poll.events[0].lifecycleEpoch, 3U);
    EXPECT_EQ(poll.events[1].documentInstanceId, 91U);
    EXPECT_EQ(poll.events[1].lifecycleEpoch, 4U);
}

TEST(DocumentRevisionPublicationEventTest, atomicBatchCarriesStableIdentityAndDeterministicJson)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube\"\n");
    const auto structure = DocumentRevisionKey::objectStructure("Sphere");
    const auto documentStructure = DocumentRevisionKey::documentStructure();

    const auto observations = index.publish(
        {objectChange(model, "17\\A"),
         objectChange(structure, "29"),
         {documentStructure, std::nullopt}});
    ASSERT_EQ(observations.size(), 3U);

    const auto poll = index.pollPublications(testCursor(0));
    ASSERT_EQ(poll.events.size(), 1U);
    const auto& event = poll.events.front();
    EXPECT_EQ(event.documentInstanceId, TestDocumentInstanceId);
    EXPECT_EQ(event.lifecycleEpoch, TestLifecycleEpoch);
    EXPECT_EQ(event.publicationSequence, 1U);
    ASSERT_EQ(event.changes.size(), 3U);
    EXPECT_EQ(event.changes[0].key, model);
    EXPECT_EQ(event.changes[0].revision, observations[0].revision);
    EXPECT_EQ(event.changes[0].stableObjectIdentity, std::optional<std::string> {"17\\A"});
    EXPECT_EQ(event.changes[1].key, structure);
    EXPECT_EQ(event.changes[1].revision, observations[1].revision);
    EXPECT_EQ(event.changes[1].stableObjectIdentity, std::optional<std::string> {"29"});
    EXPECT_EQ(event.changes[2].key, documentStructure);
    EXPECT_EQ(event.changes[2].revision, observations[2].revision);
    EXPECT_EQ(event.changes[2].stableObjectIdentity, std::nullopt);

    const std::string expectedJson =
        R"json({"document_instance_id":41,"lifecycle_epoch":7,)json"
        R"json("publication_sequence":1,"changes":[)json"
        R"json({"kind":"ObjectModel","subject":"Cube\"\n","revision":1,)json"
        R"json("stable_object_identity":"17\\A"},)json"
        R"json({"kind":"ObjectStructure","subject":"Sphere","revision":1,)json"
        R"json("stable_object_identity":"29"},)json"
        R"json({"kind":"DocumentStructure","subject":"","revision":1,)json"
        R"json("stable_object_identity":null}]})json";
    EXPECT_EQ(event.toJson(), expectedJson);
    const auto parsed = nlohmann::json::parse(event.toJson());
    EXPECT_EQ(parsed.at("document_instance_id").get<DocumentInstanceId>(),
              TestDocumentInstanceId);
    EXPECT_EQ(parsed.at("changes").size(), 3U);
    EXPECT_TRUE(parsed.at("changes").at(2).at("stable_object_identity").is_null());
}

TEST(DocumentRevisionPublicationEventTest, pollingUsesLastSeenCursorAndZeroLimitIsMetadataOnly)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    static_cast<void>(index.publish({objectChange(model)}));
    static_cast<void>(index.publish({objectChange(model)}));
    static_cast<void>(index.publish({objectChange(model)}));

    const auto firstPage = index.pollPublications(testCursor(0), 2);
    ASSERT_EQ(firstPage.events.size(), 2U);
    EXPECT_EQ(firstPage.status, DocumentRevisionCursorStatus::Valid);
    EXPECT_EQ(firstPage.currentIdentity,
              (DocumentRevisionIdentityBinding {TestDocumentInstanceId, TestLifecycleEpoch}));
    EXPECT_FALSE(firstPage.gap);
    EXPECT_EQ(firstPage.oldestAvailableSequence, 1U);
    EXPECT_EQ(firstPage.latestSequence, 3U);
    EXPECT_EQ(firstPage.events[0].publicationSequence, 1U);
    EXPECT_EQ(firstPage.events[1].publicationSequence, 2U);
    EXPECT_EQ(firstPage.nextCursor, testCursor(2));

    const auto secondPage = index.pollPublications(firstPage.nextCursor, 2);
    ASSERT_EQ(secondPage.events.size(), 1U);
    EXPECT_EQ(secondPage.events.front().publicationSequence, 3U);
    EXPECT_EQ(secondPage.nextCursor, testCursor(3));
    EXPECT_TRUE(index.pollPublications(secondPage.nextCursor).events.empty());

    const auto metadataOnly = index.pollPublications(testCursor(1), 0);
    EXPECT_TRUE(metadataOnly.events.empty());
    EXPECT_EQ(metadataOnly.nextCursor, testCursor(1));
    EXPECT_EQ(
        metadataOnly.toJson(),
        R"json({"status":"Valid","requested_cursor":{)json"
        R"json("document_instance_id":41,"lifecycle_epoch":7,"after_sequence":1},)json"
        R"json("current_identity":{"document_instance_id":41,"lifecycle_epoch":7},)json"
        R"json("next_cursor":{"document_instance_id":41,"lifecycle_epoch":7,)json"
        R"json("after_sequence":1},"oldest_available_sequence":1,"latest_sequence":3,)json"
        R"json("gap":false,"events":[]})json");
    const auto parsedPoll = nlohmann::json::parse(metadataOnly.toJson());
    EXPECT_EQ(parsedPoll.at("status").get<std::string>(), "Valid");
    EXPECT_EQ(parsedPoll.at("current_identity")
                  .at("document_instance_id")
                  .get<DocumentInstanceId>(),
              TestDocumentInstanceId);
}

TEST(DocumentRevisionPublicationEventTest, pollingRejectsForeignStaleAndFutureCursors)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    static_cast<void>(index.publish({objectChange(model)}));

    const auto foreign = index.pollPublications(testCursor(0, 99, TestLifecycleEpoch));
    EXPECT_EQ(foreign.status, DocumentRevisionCursorStatus::ForeignDocument);
    EXPECT_EQ(foreign.currentIdentity,
              (DocumentRevisionIdentityBinding {TestDocumentInstanceId, TestLifecycleEpoch}));
    EXPECT_TRUE(foreign.events.empty());
    EXPECT_EQ(foreign.nextCursor, testCursor(0, 99, TestLifecycleEpoch));

    const auto stale = index.pollPublications(testCursor(0, TestDocumentInstanceId, 6));
    EXPECT_EQ(stale.status, DocumentRevisionCursorStatus::StaleEpoch);
    EXPECT_TRUE(stale.events.empty());

    const auto future = index.pollPublications(testCursor(2));
    EXPECT_EQ(future.status, DocumentRevisionCursorStatus::FutureSequence);
    EXPECT_EQ(future.latestSequence, 1U);
    EXPECT_TRUE(future.events.empty());

    index.bindDocumentIdentity(TestDocumentInstanceId, TestLifecycleEpoch + 1);
    const auto previousEpoch = index.pollPublications(testCursor(1));
    EXPECT_EQ(previousEpoch.status, DocumentRevisionCursorStatus::StaleEpoch);
    EXPECT_EQ(previousEpoch.currentIdentity,
              (DocumentRevisionIdentityBinding {TestDocumentInstanceId,
                                                TestLifecycleEpoch + 1}));

    DocumentRevisionIndex replacement;
    replacement.bindDocumentIdentity(100, 1);
    const auto replacementPoll = replacement.pollPublications(testCursor(1));
    EXPECT_EQ(replacementPoll.status, DocumentRevisionCursorStatus::ForeignDocument);
    EXPECT_EQ(replacementPoll.currentIdentity,
              (DocumentRevisionIdentityBinding {100, 1}));
    EXPECT_TRUE(replacementPoll.events.empty());
}

TEST(DocumentRevisionPublicationEventTest, jsonEscapesControlsPreservesUnicodeAndRejectsInvalidUtf8)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    std::string subject = "Cube\"\\\b\f\n\r\t";
    subject.push_back('\x01');
    subject += "\xe2\x82\xac";
    const std::string stableIdentity = "17-\xf0\x9f\x94\xa9";
    const auto model = DocumentRevisionKey::objectModel(subject);
    static_cast<void>(index.publish({objectChange(model, stableIdentity)}));

    const auto poll = index.pollPublications(testCursor(0));
    ASSERT_EQ(poll.events.size(), 1U);
    const auto eventJson = poll.events.front().toJson();
    EXPECT_NE(eventJson.find("\\\""), std::string::npos);
    EXPECT_NE(eventJson.find("\\\\"), std::string::npos);
    EXPECT_NE(eventJson.find("\\b"), std::string::npos);
    EXPECT_NE(eventJson.find("\\f"), std::string::npos);
    EXPECT_NE(eventJson.find("\\n"), std::string::npos);
    EXPECT_NE(eventJson.find("\\r"), std::string::npos);
    EXPECT_NE(eventJson.find("\\t"), std::string::npos);
    EXPECT_NE(eventJson.find("\\u0001"), std::string::npos);
    const auto parsed = nlohmann::json::parse(eventJson);
    EXPECT_EQ(parsed.at("changes").at(0).at("subject").get<std::string>(), subject);
    EXPECT_EQ(parsed.at("changes").at(0).at("stable_object_identity").get<std::string>(),
              stableIdentity);

    const std::string invalidUtf8 {"\xc3\x28", 2};
    const auto journalBeforeInvalid = index.pollPublications(testCursor(0)).toJson();
    EXPECT_THROW(
        static_cast<void>(index.publish(
            {objectChange(DocumentRevisionKey::objectModel(invalidUtf8), "29")})),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(index.publish({objectChange(
            DocumentRevisionKey::objectModel("Sphere"),
            invalidUtf8)})),
        std::invalid_argument);
    EXPECT_EQ(index.pollPublications(testCursor(0)).toJson(), journalBeforeInvalid);

    DocumentRevisionPublicationEvent invalidValue;
    invalidValue.documentInstanceId = TestDocumentInstanceId;
    invalidValue.lifecycleEpoch = TestLifecycleEpoch;
    invalidValue.publicationSequence = 2;
    invalidValue.changes.push_back(
        {DocumentRevisionKey::objectModel("Cube"),
         2,
         std::string {"\xff", 1}});
    EXPECT_THROW(invalidValue.toJson(), std::invalid_argument);
}

TEST(DocumentRevisionPublicationEventTest, boundedEvictionReportsGapWithoutBlockingRevisionAdvance)
{
    EXPECT_THROW(
        static_cast<void>(DocumentRevisionIndex(
            std::numeric_limits<DocumentRevision>::max(),
            0)),
        std::invalid_argument);

    DocumentRevisionIndex index(std::numeric_limits<DocumentRevision>::max(), 2);
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    static_cast<void>(index.publish({objectChange(model)}));
    static_cast<void>(index.publish({objectChange(model)}));
    const auto latest = index.publish({objectChange(model)});

    ASSERT_EQ(latest.size(), 1U);
    EXPECT_EQ(latest.front().revision, 3U);
    EXPECT_EQ(index.current(model), 3U);

    const auto gap = index.pollPublications(testCursor(0));
    EXPECT_TRUE(gap.gap);
    EXPECT_EQ(gap.oldestAvailableSequence, 2U);
    EXPECT_EQ(gap.latestSequence, 3U);
    EXPECT_EQ(gap.nextCursor, testCursor(3));
    ASSERT_EQ(gap.events.size(), 2U);
    EXPECT_EQ(gap.events[0].publicationSequence, 2U);
    EXPECT_EQ(gap.events[1].publicationSequence, 3U);

    const auto atRetentionBoundary = index.pollPublications(testCursor(1));
    EXPECT_FALSE(atRetentionBoundary.gap);
    EXPECT_EQ(atRetentionBoundary.events.size(), 2U);
}

TEST(DocumentRevisionPublicationReservationTest, cancellationLeavesNoRevisionOrEvent)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    const std::vector expected {DocumentRevisionObservation(model, 0)};

    {
        auto reservation = index.reservePublication(expected, {objectChange(model)});
        ASSERT_TRUE(reservation.ready());
        EXPECT_TRUE(reservation.conflicts().empty());
        reservation.cancel();
    }

    EXPECT_EQ(index.current(model), 0U);
    const auto poll = index.pollPublications(testCursor(0));
    EXPECT_EQ(poll.latestSequence, 0U);
    EXPECT_TRUE(poll.events.empty());
}

TEST(DocumentRevisionPublicationReservationTest, commitPublishesPreallocatedBoundary)
{
    static_assert(noexcept(
        std::declval<DocumentRevisionPublicationReservation&>().commit()));

    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    auto reservation = index.reservePublication(
        {DocumentRevisionObservation(model, 0)},
        {objectChange(model, "incarnation-7")});
    ASSERT_TRUE(reservation.ready());

    const auto observations = reservation.commit();
    ASSERT_EQ(observations.size(), 1U);
    EXPECT_EQ(observations.front(), DocumentRevisionObservation(model, 1));
    EXPECT_EQ(index.current(model), 1U);
    const auto poll = index.pollPublications(testCursor(0));
    ASSERT_EQ(poll.events.size(), 1U);
    EXPECT_EQ(poll.events.front().publicationSequence, 1U);
    ASSERT_EQ(poll.events.front().changes.size(), 1U);
    EXPECT_EQ(poll.events.front().changes.front().stableObjectIdentity,
              std::optional<std::string>("incarnation-7"));
}

TEST(DocumentRevisionPublicationReservationTest, staleExpectationRejectsWithoutReservation)
{
    DocumentRevisionIndex index;
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    static_cast<void>(index.publish({objectChange(model)}));

    auto reservation = index.reservePublication(
        {DocumentRevisionObservation(model, 0)},
        {objectChange(model)});
    EXPECT_FALSE(reservation.ready());
    ASSERT_EQ(reservation.conflicts().size(), 1U);
    EXPECT_EQ(reservation.conflicts().front(), DocumentRevisionConflict(model, 0, 1));
    EXPECT_TRUE(reservation.commit().empty());
    EXPECT_EQ(index.current(model), 1U);
    EXPECT_EQ(index.pollPublications(testCursor(0)).events.size(), 1U);
}

TEST(DocumentRevisionPublicationEventTest, concurrentEventsAreJournaledInSequenceOrder)
{
    constexpr int threadCount = 4;
    constexpr int publicationsPerThread = 50;
    constexpr int publicationCount = threadCount * publicationsPerThread;
    DocumentRevisionIndex index(std::numeric_limits<DocumentRevision>::max(), publicationCount);
    bindTestIdentity(index);
    const auto model = DocumentRevisionKey::objectModel("Cube");
    std::atomic<int> readyCount {0};
    std::atomic<bool> start {false};
    std::atomic<bool> failed {false};
    std::vector<std::thread> threads;

    for (int thread = 0; thread < threadCount; ++thread) {
        threads.emplace_back([&]() {
            ++readyCount;
            while (!start.load()) {
                std::this_thread::yield();
            }
            try {
                for (int publication = 0; publication < publicationsPerThread; ++publication) {
                    static_cast<void>(index.publish({objectChange(model)}));
                }
            }
            catch (...) {
                failed = true;
            }
        });
    }
    while (readyCount.load() != threadCount) {
        std::this_thread::yield();
    }
    start = true;
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_FALSE(failed.load());
    const auto poll = index.pollPublications(testCursor(0));
    EXPECT_FALSE(poll.gap);
    EXPECT_EQ(poll.oldestAvailableSequence, 1U);
    EXPECT_EQ(poll.latestSequence, publicationCount);
    ASSERT_EQ(poll.events.size(), publicationCount);
    for (int eventIndex = 0; eventIndex < publicationCount; ++eventIndex) {
        const auto expectedSequence =
            static_cast<DocumentPublicationSequence>(eventIndex + 1);
        const auto& event = poll.events[eventIndex];
        EXPECT_EQ(event.publicationSequence, expectedSequence);
        ASSERT_EQ(event.changes.size(), 1U);
        EXPECT_EQ(event.changes.front().revision, expectedSequence);
    }
}
