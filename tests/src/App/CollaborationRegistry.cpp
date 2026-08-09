// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborationRegistry.h"
#include "App/Document.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <array>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace App;

class CollaborationRegistryTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        addDocument("collaborationRegistry");
    }

    void TearDown() override
    {
        for (auto name = _documentNames.rbegin(); name != _documentNames.rend(); ++name) {
            App::GetApplication().closeDocument(name->c_str());
        }
    }

    Document& document(std::size_t index = 0)
    {
        return *_documents.at(index);
    }

    Document& addDocument(const char* stem)
    {
        auto name = App::GetApplication().getUniqueDocumentName(stem);
        auto* document = App::GetApplication().newDocument(name.c_str(), stem);
        _documentNames.push_back(std::move(name));
        _documents.push_back(document);
        return *document;
    }

private:
    std::vector<std::string> _documentNames;
    std::vector<Document*> _documents;
};

TEST_F(CollaborationRegistryTest, registrationIsIdempotent)
{
    CollaborationRegistry registry;

    const auto first = registry.registerDocument(document());
    const auto second = registry.registerDocument(document());

    EXPECT_EQ(first, second);
    EXPECT_NE(first.instanceId, 0U);
    EXPECT_NE(first.lifecycleEpoch, 0U);
    EXPECT_EQ(first.state, DocumentLifecycleState::Live);
    EXPECT_EQ(registry.identity(document()), first);
    EXPECT_EQ(registry.identity(first.instanceId), first);
}

TEST_F(CollaborationRegistryTest, instanceIdsAndEpochsIncreaseMonotonically)
{
    CollaborationRegistry registry;
    auto& secondDocument = addDocument("collaborationRegistrySecond");

    const auto first = registry.registerDocument(document());
    const auto second = registry.registerDocument(secondDocument);
    const auto advanced = registry.advanceEpoch(document());
    const auto closing = registry.markClosing(secondDocument);
    const auto closed = registry.closeDocument(secondDocument);

    ASSERT_TRUE(advanced.has_value());
    ASSERT_TRUE(closing.has_value());
    ASSERT_TRUE(closed.has_value());
    EXPECT_GT(second.instanceId, first.instanceId);
    EXPECT_GT(second.lifecycleEpoch, first.lifecycleEpoch);
    EXPECT_GT(advanced->lifecycleEpoch, second.lifecycleEpoch);
    EXPECT_GT(closing->lifecycleEpoch, advanced->lifecycleEpoch);
    EXPECT_GT(closed->lifecycleEpoch, closing->lifecycleEpoch);
}

TEST_F(CollaborationRegistryTest, validationRejectsStaleEpochsAndNonLiveDocuments)
{
    CollaborationRegistry registry;
    const auto original = registry.registerDocument(document());

    EXPECT_EQ(registry.validate(original.instanceId, original.lifecycleEpoch),
              DocumentIdentityValidation::Valid);
    EXPECT_EQ(registry.validate(0, original.lifecycleEpoch),
              DocumentIdentityValidation::UnknownInstance);

    const auto advanced = registry.advanceEpoch(document());
    ASSERT_TRUE(advanced.has_value());
    EXPECT_EQ(registry.validate(original.instanceId, original.lifecycleEpoch),
              DocumentIdentityValidation::EpochMismatch);
    EXPECT_EQ(registry.validate(advanced->instanceId, advanced->lifecycleEpoch),
              DocumentIdentityValidation::Valid);

    const auto closing = registry.markClosing(document());
    ASSERT_TRUE(closing.has_value());
    EXPECT_EQ(registry.validate(closing->instanceId, closing->lifecycleEpoch),
              DocumentIdentityValidation::NotLive);
    EXPECT_EQ(registry.registerDocument(document()), *closing);
    EXPECT_FALSE(registry.advanceEpoch(document()).has_value());
    EXPECT_FALSE(registry.markClosing(document()).has_value());
}

TEST_F(CollaborationRegistryTest, closeRemovesPointerAndRetainsClosedTombstone)
{
    CollaborationRegistry registry;
    (void)registry.registerDocument(document());

    const auto closed = registry.closeDocument(document());

    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->state, DocumentLifecycleState::Closed);
    EXPECT_FALSE(registry.identity(document()).has_value());
    EXPECT_EQ(registry.identity(closed->instanceId), closed);
    EXPECT_EQ(registry.validate(closed->instanceId, closed->lifecycleEpoch),
              DocumentIdentityValidation::NotLive);
    EXPECT_FALSE(registry.closeDocument(document()).has_value());
}

TEST_F(CollaborationRegistryTest, tombstoneRetentionIsBoundedAndOldestFirst)
{
    CollaborationRegistry registry(1);
    auto& secondDocument = addDocument("collaborationRegistrySecond");
    const auto first = registry.registerDocument(document());
    const auto second = registry.registerDocument(secondDocument);

    const auto firstClosed = registry.closeDocument(document());
    const auto secondClosed = registry.closeDocument(secondDocument);

    ASSERT_TRUE(firstClosed.has_value());
    ASSERT_TRUE(secondClosed.has_value());
    EXPECT_FALSE(registry.identity(first.instanceId).has_value());
    EXPECT_EQ(registry.validate(first.instanceId, firstClosed->lifecycleEpoch),
              DocumentIdentityValidation::UnknownInstance);
    EXPECT_EQ(registry.identity(second.instanceId), secondClosed);
}

TEST_F(CollaborationRegistryTest, zeroCapacityImmediatelyEvictsClosedIdentity)
{
    CollaborationRegistry registry(0);
    const auto identity = registry.registerDocument(document());

    ASSERT_TRUE(registry.closeDocument(document()).has_value());
    EXPECT_FALSE(registry.identity(identity.instanceId).has_value());
}

TEST_F(CollaborationRegistryTest, registeringReusedPointerCannotReviveOldIdentity)
{
    CollaborationRegistry registry;
    const auto original = registry.registerDocument(document());
    const auto closed = registry.closeDocument(document());
    const auto replacement = registry.registerDocument(document());

    ASSERT_TRUE(closed.has_value());
    EXPECT_GT(replacement.instanceId, original.instanceId);
    EXPECT_GT(replacement.lifecycleEpoch, closed->lifecycleEpoch);
    EXPECT_EQ(replacement.state, DocumentLifecycleState::Live);
    EXPECT_EQ(registry.identity(original.instanceId), closed);
    EXPECT_EQ(registry.validate(original.instanceId, original.lifecycleEpoch),
              DocumentIdentityValidation::EpochMismatch);
    EXPECT_EQ(registry.validate(closed->instanceId, closed->lifecycleEpoch),
              DocumentIdentityValidation::NotLive);
    EXPECT_EQ(registry.validate(replacement.instanceId, replacement.lifecycleEpoch),
              DocumentIdentityValidation::Valid);
}

TEST_F(CollaborationRegistryTest, concurrentCallsPreserveOneIdentityAndUniqueEpochs)
{
    CollaborationRegistry registry;
    constexpr std::size_t threadCount = 12;
    std::array<DocumentIdentity, threadCount> registrations;
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (std::size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i] {
            registrations[i] = registry.registerDocument(document());
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_TRUE(std::all_of(registrations.begin(), registrations.end(), [&](const auto& identity) {
        return identity == registrations.front();
    }));

    std::array<std::optional<DocumentIdentity>, threadCount> advances;
    threads.clear();
    for (std::size_t i = 0; i < threadCount; ++i) {
        threads.emplace_back([&, i] {
            advances[i] = registry.advanceEpoch(document());
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::vector<DocumentLifecycleEpoch> epochs;
    epochs.reserve(threadCount);
    for (const auto& advanced : advances) {
        ASSERT_TRUE(advanced.has_value());
        epochs.push_back(advanced->lifecycleEpoch);
    }
    std::sort(epochs.begin(), epochs.end());
    EXPECT_EQ(std::adjacent_find(epochs.begin(), epochs.end()), epochs.end());
    EXPECT_GT(epochs.front(), registrations.front().lifecycleEpoch);
    EXPECT_EQ(registry.identity(document())->lifecycleEpoch, epochs.back());
}
