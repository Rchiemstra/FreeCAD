// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/DocumentExecutionTelemetry.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <type_traits>
#include <vector>

using namespace App;

class DocumentExecutionTelemetryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Internal::DocumentExecutionTelemetryTestAccess::reset(
            DocumentExecutionTelemetry::instance());
    }
};

namespace
{

constexpr DocumentInstanceId FirstDocumentId = 101;
constexpr DocumentInstanceId SecondDocumentId = 202;
constexpr DocumentLifecycleEpoch TestLifecycleEpoch = 3;

void recordBusyRejections(DocumentExecutionTelemetryCollector& collector)
{
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Edit);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Undo);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Redo);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Save);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Close);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::CommandAdmission);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Other);
}

}  // namespace

static_assert(std::is_copy_constructible_v<DocumentExecutionDocumentTelemetrySnapshot>);
static_assert(std::is_copy_constructible_v<DocumentExecutionProcessTelemetrySnapshot>);
static_assert(!std::is_pointer_v<decltype(
    std::declval<DocumentExecutionDocumentTelemetrySnapshot>().progress.phase)>);

TEST(DocumentExecutionTelemetryContractTest, exposesStableEnumNames)
{
    EXPECT_STREQ(documentExecutionWatchdogStateName(DocumentExecutionWatchdogState::Idle), "Idle");
    EXPECT_STREQ(documentExecutionWatchdogStateName(DocumentExecutionWatchdogState::Active),
                 "Active");
    EXPECT_STREQ(documentExecutionWatchdogStateName(DocumentExecutionWatchdogState::Stalled),
                 "Stalled");

    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Edit),
                 "Edit");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Undo),
                 "Undo");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Redo),
                 "Redo");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Save),
                 "Save");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Close),
                 "Close");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(
                   DocumentExecutionBusyRejectionKind::CommandAdmission),
                 "CommandAdmission");
    EXPECT_STREQ(documentExecutionBusyRejectionKindName(DocumentExecutionBusyRejectionKind::Other),
                 "Other");
}

TEST_F(DocumentExecutionTelemetryTest, recordsProcessAndDocumentLatencySamples)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    auto collector = telemetry.document(FirstDocumentId, TestLifecycleEpoch);

    telemetry.recordGuiEventLoopLatency(12.5);
    telemetry.recordGuiEventLoopLatency(37.5);
    collector->recordApplySliceDuration(3.5);
    collector->recordApplySliceDuration(4.0);
    collector->recordScenePreparationTime(18.0);
    collector->recordPresentationRevisionLag(9.25);

    const auto documentSnapshot = collector->snapshot();
    EXPECT_EQ(documentSnapshot.documentInstanceId, FirstDocumentId);
    EXPECT_EQ(documentSnapshot.lifecycleEpoch, TestLifecycleEpoch);
    EXPECT_EQ(documentSnapshot.applySliceDuration.sampleCount, 2U);
    EXPECT_DOUBLE_EQ(documentSnapshot.applySliceDuration.minimumMilliseconds, 3.5);
    EXPECT_DOUBLE_EQ(documentSnapshot.applySliceDuration.maximumMilliseconds, 4.0);
    EXPECT_DOUBLE_EQ(documentSnapshot.applySliceDuration.meanMilliseconds(), 3.75);
    EXPECT_EQ(documentSnapshot.scenePreparationTime.sampleCount, 1U);
    EXPECT_DOUBLE_EQ(documentSnapshot.presentationRevisionLagMilliseconds, 9.25);

    telemetry.publishProcessSnapshot();
    const auto processSnapshot = telemetry.processSnapshot();
    EXPECT_GE(processSnapshot.publicationSequence, 1U);
    EXPECT_EQ(processSnapshot.guiEventLoopLatency.sampleCount, 2U);
    EXPECT_DOUBLE_EQ(processSnapshot.guiEventLoopLatency.minimumMilliseconds, 12.5);
    EXPECT_DOUBLE_EQ(processSnapshot.guiEventLoopLatency.maximumMilliseconds, 37.5);
    EXPECT_EQ(processSnapshot.documents.size(), 1U);
    EXPECT_EQ(processSnapshot.documents.front().documentInstanceId, FirstDocumentId);
}

TEST_F(DocumentExecutionTelemetryTest, aggregatesBusyRejectionsAcrossDocuments)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    auto first = telemetry.document(FirstDocumentId, TestLifecycleEpoch);
    auto second = telemetry.document(SecondDocumentId, TestLifecycleEpoch);

    recordBusyRejections(*first);
    recordBusyRejections(*second);

    const auto processSnapshot = telemetry.snapshot();
    EXPECT_EQ(processSnapshot.busyRejections.edit, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.undo, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.redo, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.save, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.close, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.commandAdmission, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.other, 2U);
    EXPECT_EQ(processSnapshot.busyRejections.total(), 14U);
}

TEST(DocumentExecutionTelemetryTest, reportsWatchdogStallAfterFiveSeconds)
{
    DocumentExecutionTelemetryCollector collector(FirstDocumentId, TestLifecycleEpoch, 8);
    collector.beginWatchdog("recompute-plan-7");

    DocumentExecutionProgressSnapshot progress;
    progress.active = true;
    progress.fraction = 0.25;
    progress.phase = "preparing";
    progress.operationId = "recompute-plan-7";
    progress.lastProgressEpochMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count()
        - DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds - 1;
    collector.updateProgress(progress);

    const auto snapshot = collector.snapshot();
    EXPECT_EQ(snapshot.watchdog.state, DocumentExecutionWatchdogState::Stalled);
    EXPECT_TRUE(snapshot.watchdog.stalled());
    EXPECT_GE(snapshot.watchdog.millisecondsSinceLastProgress,
              DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds);
    EXPECT_FALSE(snapshot.watchdog.diagnostic.empty());
    EXPECT_NE(snapshot.watchdog.diagnostic.find("not terminated"), std::string::npos);
}

TEST(DocumentExecutionTelemetryTest, watchdogReturnsIdleWhenInactive)
{
    DocumentExecutionTelemetryCollector collector(FirstDocumentId, TestLifecycleEpoch);
    const auto snapshot = collector.snapshot();
    EXPECT_EQ(snapshot.watchdog.state, DocumentExecutionWatchdogState::Idle);
    EXPECT_FALSE(snapshot.watchdog.stalled());
}

TEST(DocumentExecutionTelemetryTest, recentSampleBuffersAreBounded)
{
    DocumentExecutionTelemetryCollector collector(FirstDocumentId, TestLifecycleEpoch, 4);
    for (int sample = 1; sample <= 6; ++sample) {
        collector.recordApplySliceDuration(static_cast<double>(sample));
    }

    const auto snapshot = collector.snapshot();
    EXPECT_EQ(snapshot.applySliceDuration.sampleCount, 6U);
    EXPECT_EQ(snapshot.applySliceDuration.recentSamplesMilliseconds.size(), 4U);
    EXPECT_DOUBLE_EQ(snapshot.applySliceDuration.recentSamplesMilliseconds.front(), 3.0);
    EXPECT_DOUBLE_EQ(snapshot.applySliceDuration.recentSamplesMilliseconds.back(), 6.0);
}

TEST_F(DocumentExecutionTelemetryTest, publishedSnapshotIsAtomicallyReplaced)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    auto collector = telemetry.document(FirstDocumentId, TestLifecycleEpoch);

    telemetry.recordGuiEventLoopLatency(5.0);
    telemetry.publishProcessSnapshot();
    const auto firstPublication = telemetry.processSnapshot();

    collector->recordApplySliceDuration(2.0);
    telemetry.publishProcessSnapshot();
    const auto secondPublication = telemetry.processSnapshot();

    EXPECT_LT(firstPublication.publicationSequence, secondPublication.publicationSequence);
    EXPECT_EQ(firstPublication.documents.front().applySliceDuration.sampleCount, 0U);
    EXPECT_EQ(secondPublication.documents.front().applySliceDuration.sampleCount, 1U);
}

TEST_F(DocumentExecutionTelemetryTest, documentSnapshotLookupHonorsRemoval)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    telemetry.document(FirstDocumentId, TestLifecycleEpoch)->recordApplySliceDuration(1.0);
    EXPECT_TRUE(telemetry.documentSnapshot(FirstDocumentId).has_value());

    telemetry.removeDocument(FirstDocumentId);
    EXPECT_FALSE(telemetry.documentSnapshot(FirstDocumentId).has_value());
}

TEST_F(DocumentExecutionTelemetryTest, documentCollectorSurvivesRemovalWhilePinned)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    auto collector = telemetry.document(FirstDocumentId, TestLifecycleEpoch);
    collector->recordApplySliceDuration(2.5);

    telemetry.removeDocument(FirstDocumentId);
    EXPECT_FALSE(telemetry.documentSnapshot(FirstDocumentId).has_value());

    const auto snapshot = collector->snapshot();
    EXPECT_EQ(snapshot.documentInstanceId, FirstDocumentId);
    EXPECT_EQ(snapshot.applySliceDuration.sampleCount, 1U);
    EXPECT_DOUBLE_EQ(snapshot.applySliceDuration.minimumMilliseconds, 2.5);
}

TEST_F(DocumentExecutionTelemetryTest, documentLookupReplacesCollectorOnLifecycleEpochChange)
{
    auto& telemetry = DocumentExecutionTelemetry::instance();
    constexpr DocumentLifecycleEpoch reopenedLifecycleEpoch = TestLifecycleEpoch + 1;

    auto firstCollector = telemetry.document(FirstDocumentId, TestLifecycleEpoch);
    firstCollector->recordApplySliceDuration(1.0);

    auto secondCollector = telemetry.document(FirstDocumentId, reopenedLifecycleEpoch);
    EXPECT_NE(firstCollector.get(), secondCollector.get());
    EXPECT_EQ(secondCollector->lifecycleEpoch(), reopenedLifecycleEpoch);
    EXPECT_EQ(secondCollector->snapshot().applySliceDuration.sampleCount, 0U);

    secondCollector->recordApplySliceDuration(3.0);
    const auto registrySnapshot = telemetry.documentSnapshot(FirstDocumentId);
    ASSERT_TRUE(registrySnapshot.has_value());
    EXPECT_EQ(registrySnapshot->lifecycleEpoch, reopenedLifecycleEpoch);
    EXPECT_EQ(registrySnapshot->applySliceDuration.sampleCount, 1U);
    EXPECT_DOUBLE_EQ(registrySnapshot->applySliceDuration.minimumMilliseconds, 3.0);
}

TEST_F(DocumentExecutionTelemetryTest, snapshotsSerializeToJson)
{
    DocumentExecutionTelemetryCollector collector(FirstDocumentId, TestLifecycleEpoch);
    collector.recordApplySliceDuration(4.0);
    collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Edit);

    const auto documentJson = collector.snapshot().toJson();
    EXPECT_NE(documentJson.find("\"document_instance_id\":101"), std::string::npos);
    EXPECT_NE(documentJson.find("\"apply_slice_duration\""), std::string::npos);
    EXPECT_NE(documentJson.find("\"busy_rejections\""), std::string::npos);
    EXPECT_NE(documentJson.find("\"watchdog\""), std::string::npos);

    auto& telemetry = DocumentExecutionTelemetry::instance();
    telemetry.recordGuiEventLoopLatency(8.0);
    telemetry.publishProcessSnapshot();
    const auto processJson = telemetry.processSnapshot().toJson();
    EXPECT_NE(processJson.find("\"gui_event_loop_latency\""), std::string::npos);
    EXPECT_NE(processJson.find("\"publication_sequence\""), std::string::npos);
}

TEST(DocumentExecutionTelemetryTest, concurrentRecordingProducesConsistentSnapshots)
{
    DocumentExecutionTelemetryCollector collector(FirstDocumentId, TestLifecycleEpoch, 128);
    std::atomic<bool> start {false};
    std::atomic<int> readyWorkers {0};

    auto worker = [&]() {
        readyWorkers.fetch_add(1, std::memory_order_relaxed);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int sample = 0; sample < 50; ++sample) {
            collector.recordApplySliceDuration(1.0);
            collector.recordScenePreparationTime(2.0);
            collector.recordBusyRejection(DocumentExecutionBusyRejectionKind::Edit);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int index = 0; index < 4; ++index) {
        threads.emplace_back(worker);
    }

    while (readyWorkers.load(std::memory_order_relaxed) < 4) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    const auto snapshot = collector.snapshot();
    EXPECT_EQ(snapshot.applySliceDuration.sampleCount, 200U);
    EXPECT_EQ(snapshot.scenePreparationTime.sampleCount, 200U);
    EXPECT_EQ(snapshot.busyRejections.edit, 200U);
    EXPECT_LE(snapshot.applySliceDuration.recentSamplesMilliseconds.size(), 128U);
}
