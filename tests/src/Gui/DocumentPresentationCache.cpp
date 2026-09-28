// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>

#include <Gui/DocumentPresentationCache.h>
#include <Gui/PresentationApplyScheduler.h>

#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr App::DocumentInstanceId TestDocumentInstance = 91;
constexpr App::DocumentLifecycleEpoch TestDocumentEpoch = 3;

void ensureCoinInitialized()
{
    // Idempotent; SoSeparator construction requires the Coin type database.
    SoDB::init();
}

Gui::PresentationDelta makeDelta(Gui::PresentationSequence sequence,
                                 App::DocumentRevision sourceRevision,
                                 std::string marker)
{
    Gui::PresentationDelta delta;
    delta.revision.documentInstanceId = TestDocumentInstance;
    delta.revision.lifecycleEpoch = TestDocumentEpoch;
    delta.revision.sequence = sequence;
    delta.revision.sourceModelRevision = sourceRevision;
    delta.status.state = Gui::DocumentPresentationState::Committed;
    delta.status.statusMessage = marker;

    Gui::PresentationTreeNode node;
    node.stableObjectIdentity = marker;
    node.label = marker;
    delta.tree.push_back(std::move(node));
    return delta;
}

Gui::PresentationDelta makeDeltaWithRenderBuffers(Gui::PresentationSequence sequence,
                                                  App::DocumentRevision sourceRevision,
                                                  std::size_t bufferCount)
{
    auto delta = makeDelta(sequence, sourceRevision, "render-heavy");
    delta.renderBuffers.resize(bufferCount);
    for (std::size_t index = 0; index < bufferCount; ++index) {
        auto& buffer = delta.renderBuffers[index];
        buffer.stableObjectIdentity = "buffer-" + std::to_string(index);
        buffer.vertices.assign(4096, static_cast<float>(index) + 0.25F);
        buffer.normals.assign(4096, 1.0F);
        buffer.indices.assign(2048, static_cast<std::uint32_t>(index));
    }
    return delta;
}

}  // namespace

TEST(DocumentPresentationCacheTest, RejectsStalePackets)
{
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);

    auto first = makeDelta(1, 10, "first");
    EXPECT_EQ(cache.tryCommit(std::move(first)), Gui::PresentationCommitResult::Accepted);

    auto duplicateSequence = makeDelta(1, 11, "duplicate-sequence");
    EXPECT_EQ(cache.tryCommit(std::move(duplicateSequence)),
              Gui::PresentationCommitResult::StaleSequence);

    auto regressedSource = makeDelta(2, 9, "regressed-source");
    EXPECT_EQ(cache.tryCommit(std::move(regressedSource)),
              Gui::PresentationCommitResult::StaleSourceRevision);

    auto wrongEpoch = makeDelta(3, 10, "wrong-epoch");
    wrongEpoch.revision.lifecycleEpoch = TestDocumentEpoch + 1;
    EXPECT_EQ(cache.tryCommit(std::move(wrongEpoch)),
              Gui::PresentationCommitResult::StaleDocumentIdentity);

    auto wrongInstance = makeDelta(3, 10, "wrong-instance");
    wrongInstance.revision.documentInstanceId = TestDocumentInstance + 1;
    EXPECT_EQ(cache.tryCommit(std::move(wrongInstance)),
              Gui::PresentationCommitResult::StaleDocumentIdentity);
}

TEST(DocumentPresentationCacheTest, AtomicRevisionSwap)
{
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);

    auto first = makeDelta(1, 1, "revision-a");
    EXPECT_EQ(cache.tryCommit(std::move(first)), Gui::PresentationCommitResult::Accepted);

    const auto committed = cache.current();
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->revision.sequence, 1U);
    EXPECT_EQ(committed->tree.front().label, "revision-a");

    auto second = makeDelta(2, 2, "revision-b");
    EXPECT_EQ(cache.tryCommit(std::move(second)), Gui::PresentationCommitResult::Accepted);

    const auto swapped = cache.current();
    ASSERT_TRUE(swapped.has_value());
    EXPECT_EQ(swapped->revision.sequence, 2U);
    EXPECT_EQ(swapped->tree.size(), 1U);
    EXPECT_EQ(swapped->tree.front().label, "revision-b");
    EXPECT_NE(swapped->tree.front().label, "revision-a");
    EXPECT_EQ(cache.committedSequence(), 2U);
}

TEST(PresentationApplySchedulerTest, CoalescesToNewestStagingRevision)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);

    scheduler.enqueue(makeDelta(1, 1, "seq-1"));
    scheduler.enqueue(makeDelta(2, 2, "seq-2"));
    scheduler.enqueue(makeDelta(3, 3, "seq-3"));

    const auto staging = scheduler.stagingRevision();
    ASSERT_TRUE(staging.has_value());
    EXPECT_EQ(staging->sequence, 3U);

    const auto pumpResult = scheduler.pump(0);
    EXPECT_TRUE(pumpResult.stagingComplete);
    EXPECT_TRUE(pumpResult.committedRevision);
    EXPECT_EQ(pumpResult.slicesApplied, 4U);

    const auto committed = cache.current();
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->revision.sequence, 3U);
    EXPECT_EQ(committed->tree.front().label, "seq-3");
    EXPECT_EQ(cache.status().state, Gui::DocumentPresentationState::Committed);
}

TEST(PresentationApplySchedulerTest, RecordsSliceBudgetAcrossPumpTurns)
{
    ensureCoinInitialized();
    Gui::DocumentPresentationCache cache;
    cache.bindDocumentIdentity(TestDocumentInstance, TestDocumentEpoch);
    Gui::PresentationApplyScheduler scheduler(cache);

    scheduler.enqueue(makeDeltaWithRenderBuffers(1, 1, 48));

    const std::size_t expectedSlices = 3 + 48;
    std::size_t totalSlices = 0;
    std::size_t pumpTurns = 0;

    while (scheduler.hasStagingWork()) {
        const auto result = scheduler.pump(1);
        ++pumpTurns;
        EXPECT_EQ(result.sliceDurationsMicros.size(), result.slicesApplied);
        totalSlices += result.slicesApplied;
        if (result.stagingComplete) {
            EXPECT_TRUE(result.committedRevision);
            break;
        }
    }

    EXPECT_EQ(totalSlices, expectedSlices);
    EXPECT_GT(pumpTurns, 1U);
    EXPECT_EQ(cache.committedSequence(), 1U);
}
