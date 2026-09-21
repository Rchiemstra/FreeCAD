// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Gui/EventLoopLatencyStats.h>

#include <cstdint>

using Duration = Gui::EventLoopLatencyStats::Duration;

TEST(EventLoopLatencyStats, emptySnapshotHasZeroValues)
{
    const Gui::EventLoopLatencyStats stats(4);
    const auto snapshot = stats.snapshot();

    EXPECT_EQ(snapshot.count, 0U);
    EXPECT_EQ(snapshot.maximum, Duration::zero());
    EXPECT_EQ(snapshot.p99, Duration::zero());
}

TEST(EventLoopLatencyStats, recordsBoundariesAndRejectsNegativeSamples)
{
    Gui::EventLoopLatencyStats stats(4);

    EXPECT_TRUE(stats.record(Duration::zero()));
    EXPECT_FALSE(stats.record(Duration{-1}));
    EXPECT_TRUE(stats.record(Duration{42}));

    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.count, 2U);
    EXPECT_EQ(snapshot.maximum, Duration{42});
    EXPECT_EQ(snapshot.p99, Duration{42});
}

TEST(EventLoopLatencyStats, p99UsesDeterministicNearestRank)
{
    Gui::EventLoopLatencyStats stats(100);
    for (std::int64_t sample = 1; sample <= 100; ++sample) {
        ASSERT_TRUE(stats.record(Duration{sample}));
    }

    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.count, 100U);
    EXPECT_EQ(snapshot.maximum, Duration{100});
    EXPECT_EQ(snapshot.p99, Duration{99});
}

TEST(EventLoopLatencyStats, orderingDoesNotChangeStatistics)
{
    Gui::EventLoopLatencyStats stats(4);
    for (const auto sample : {Duration{40}, Duration{10}, Duration{30}, Duration{20}}) {
        ASSERT_TRUE(stats.record(sample));
    }

    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.count, 4U);
    EXPECT_EQ(snapshot.maximum, Duration{40});
    EXPECT_EQ(snapshot.p99, Duration{40});
}

TEST(EventLoopLatencyStats, retainsOnlyMostRecentSamplesAtCapacity)
{
    Gui::EventLoopLatencyStats stats(3);
    ASSERT_TRUE(stats.record(Duration{1}));
    ASSERT_TRUE(stats.record(Duration{2}));
    ASSERT_TRUE(stats.record(Duration{3}));
    ASSERT_TRUE(stats.record(Duration{100}));

    const auto snapshot = stats.snapshot();
    EXPECT_EQ(snapshot.count, 3U);
    EXPECT_EQ(snapshot.maximum, Duration{100});
    EXPECT_EQ(snapshot.p99, Duration{100});
}

TEST(EventLoopLatencyStats, zeroCapacityDoesNotRetainSamples)
{
    Gui::EventLoopLatencyStats stats(0);

    EXPECT_FALSE(stats.record(Duration::zero()));
    EXPECT_EQ(stats.capacity(), 0U);
    EXPECT_EQ(stats.snapshot().count, 0U);
}

TEST(EventLoopLatencyStats, snapshotsAreDetachedFromLaterRecords)
{
    Gui::EventLoopLatencyStats stats(2);
    ASSERT_TRUE(stats.record(Duration{10}));
    const auto before = stats.snapshot();

    ASSERT_TRUE(stats.record(Duration{100}));
    const auto after = stats.snapshot();

    EXPECT_EQ(before.count, 1U);
    EXPECT_EQ(before.maximum, Duration{10});
    EXPECT_EQ(before.p99, Duration{10});
    EXPECT_EQ(after.count, 2U);
    EXPECT_EQ(after.maximum, Duration{100});
}
