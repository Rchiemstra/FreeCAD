// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Mod/Inspection/App/PhotoInspectionSession.h>

namespace
{

using namespace Inspection::Photo;

AnalysisResult completeResult(const std::uint64_t generation)
{
    AnalysisResult result;
    result.generation = generation;
    result.status = OperationStatus::Complete;
    result.decision = ConformanceDecision::Inconclusive;
    return result;
}

TEST(PhotoInspectionSessionTest, newerGenerationRejectsLateOlderResult)
{
    AnalysisSession session;
    const auto first = session.begin();
    const auto second = session.begin();
    EXPECT_FALSE(session.publish(completeResult(first)));
    EXPECT_TRUE(session.publish(completeResult(second)));
    EXPECT_FALSE(session.result(first).has_value());
    EXPECT_TRUE(session.result(second).has_value());
}

TEST(PhotoInspectionSessionTest, cancellationPreventsLatePublication)
{
    AnalysisSession session;
    const auto generation = session.begin();
    EXPECT_TRUE(session.requestCancellation(generation));
    EXPECT_TRUE(session.cancellationRequested(generation));
    EXPECT_FALSE(session.publish(completeResult(generation)));
    EXPECT_FALSE(session.result(generation).has_value());
}

TEST(PhotoInspectionSessionTest, wrongGenerationCannotCancelCurrentWork)
{
    AnalysisSession session;
    const auto generation = session.begin();
    EXPECT_FALSE(session.requestCancellation(generation + 1));
    EXPECT_FALSE(session.cancellationRequested(generation));
    EXPECT_TRUE(session.publish(completeResult(generation)));
}

TEST(PhotoInspectionSessionTest, closeInvalidatesGenerationAndResult)
{
    AnalysisSession session;
    const auto generation = session.begin();
    EXPECT_TRUE(session.publish(completeResult(generation)));
    ASSERT_TRUE(session.result(generation).has_value());
    session.close();
    const auto snapshot = session.snapshot();
    EXPECT_EQ(snapshot.state, SessionState::Closed);
    EXPECT_GT(snapshot.generation, generation);
    EXPECT_FALSE(snapshot.hasResult);
    EXPECT_FALSE(session.result(generation).has_value());
}

TEST(PhotoInspectionSessionTest, beginAfterCloseCreatesCleanGeneration)
{
    AnalysisSession session;
    session.close();
    const auto generation = session.begin();
    const auto snapshot = session.snapshot();
    EXPECT_EQ(snapshot.generation, generation);
    EXPECT_EQ(snapshot.state, SessionState::Running);
    EXPECT_FALSE(snapshot.cancellationRequested);
    EXPECT_FALSE(snapshot.hasResult);
}

}  // namespace
