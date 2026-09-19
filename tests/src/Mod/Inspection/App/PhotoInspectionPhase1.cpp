// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <Mod/Inspection/App/PhotoInspectionTypes.h>

namespace
{

using Inspection::Photo::canonicalizeProjection;
using Inspection::Photo::CanonicalProjection;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::makeCanonicalFrame;
using Inspection::Photo::PolylineCycle;
using Inspection::Photo::ProjectionSnapshot;
using Inspection::Photo::projectToFrame;
using Inspection::Photo::Vector2d;
using Inspection::Photo::Vector3d;

ProjectionSnapshot rectangleWithHole()
{
    ProjectionSnapshot snapshot;
    snapshot.frame = *makeCanonicalFrame({0.0, 0.0, 12.0}, {0.0, 0.0, 1.0});
    snapshot.cycles = {
        PolylineCycle {
            false,
            {
                {0.0, 0.0},
                {100.0, 0.0},
                {100.0, 50.0},
                {0.0, 50.0},
            },
        },
        PolylineCycle {
            true,
            {
                {20.0, 20.0},
                {20.0, 30.0},
                {30.0, 30.0},
                {30.0, 20.0},
            },
        },
    };
    return snapshot;
}

TEST(PhotoInspectionCanonicalFrameTest, normalSignDoesNotChangeFrame)
{
    const auto positive = makeCanonicalFrame({2.0, -3.0, 12.0}, {0.0, 0.0, 1.0});
    const auto negative = makeCanonicalFrame({2.0, -3.0, 12.0}, {0.0, 0.0, -1.0});
    ASSERT_TRUE(positive.has_value());
    ASSERT_TRUE(negative.has_value());

    EXPECT_DOUBLE_EQ(positive->origin.x, negative->origin.x);
    EXPECT_DOUBLE_EQ(positive->origin.y, negative->origin.y);
    EXPECT_DOUBLE_EQ(positive->origin.z, negative->origin.z);
    EXPECT_DOUBLE_EQ(positive->xAxis.x, negative->xAxis.x);
    EXPECT_DOUBLE_EQ(positive->xAxis.y, negative->xAxis.y);
    EXPECT_DOUBLE_EQ(positive->xAxis.z, negative->xAxis.z);
    EXPECT_DOUBLE_EQ(positive->yAxis.x, negative->yAxis.x);
    EXPECT_DOUBLE_EQ(positive->yAxis.y, negative->yAxis.y);
    EXPECT_DOUBLE_EQ(positive->yAxis.z, negative->yAxis.z);
}

TEST(PhotoInspectionCanonicalFrameTest, originIsProjectionOfWorldOriginOntoPlane)
{
    const auto frame = makeCanonicalFrame({0.0, 0.0, 12.0}, {0.0, 0.0, 2.0});
    ASSERT_TRUE(frame.has_value());

    EXPECT_DOUBLE_EQ(frame->origin.x, 0.0);
    EXPECT_DOUBLE_EQ(frame->origin.y, 0.0);
    EXPECT_DOUBLE_EQ(frame->origin.z, 12.0);

    const Vector2d projected = projectToFrame(*frame, {5.0, 7.0, 12.0});
    EXPECT_DOUBLE_EQ(projected.x, 5.0);
    EXPECT_DOUBLE_EQ(projected.y, 7.0);
}

TEST(PhotoInspectionCanonicalFrameTest, rejectsDegenerateAndNonFiniteInputs)
{
    EXPECT_FALSE(makeCanonicalFrame({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}).has_value());
    EXPECT_FALSE(
        makeCanonicalFrame({std::numeric_limits<double>::infinity(), 0.0, 0.0}, {0.0, 0.0, 1.0})
            .has_value()
    );
}

TEST(PhotoInspectionCanonicalProjectionTest, hashIsInvariantToCycleStartWindingAndOrder)
{
    ProjectionSnapshot first = rectangleWithHole();
    CanonicalProjection canonicalFirst;
    const auto firstResult = canonicalizeProjection(first, canonicalFirst);
    ASSERT_TRUE(firstResult.valid) << firstResult.diagnostic.message;

    ProjectionSnapshot reordered = first;
    std::reverse(reordered.cycles.begin(), reordered.cycles.end());
    for (PolylineCycle& cycle : reordered.cycles) {
        std::reverse(cycle.points.begin(), cycle.points.end());
        std::rotate(cycle.points.begin(), cycle.points.begin() + 1, cycle.points.end());
        cycle.points.push_back(cycle.points.front());
    }

    CanonicalProjection canonicalReordered;
    const auto reorderedResult = canonicalizeProjection(reordered, canonicalReordered);
    ASSERT_TRUE(reorderedResult.valid) << reorderedResult.diagnostic.message;
    EXPECT_EQ(canonicalFirst.bytes, canonicalReordered.bytes);
    EXPECT_EQ(canonicalFirst.sha256, canonicalReordered.sha256);
    EXPECT_EQ(canonicalFirst.sha256.size(), 64);
}

TEST(PhotoInspectionCanonicalProjectionTest, meaningfulGeometryChangeChangesHash)
{
    ProjectionSnapshot first = rectangleWithHole();
    ProjectionSnapshot changed = first;
    changed.cycles.front().points[1].x += 0.001;

    CanonicalProjection canonicalFirst;
    CanonicalProjection canonicalChanged;
    const auto firstResult = canonicalizeProjection(first, canonicalFirst);
    const auto changedResult = canonicalizeProjection(changed, canonicalChanged);
    ASSERT_TRUE(firstResult.valid) << firstResult.diagnostic.message;
    ASSERT_TRUE(changedResult.valid) << changedResult.diagnostic.message;

    EXPECT_NE(canonicalFirst.sha256, canonicalChanged.sha256);
}

TEST(PhotoInspectionCanonicalProjectionTest, rejectsCollapsedNonFiniteAndEmptyGeometry)
{
    ProjectionSnapshot empty = rectangleWithHole();
    empty.cycles.clear();
    CanonicalProjection output;
    auto result = canonicalizeProjection(empty, output);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidGeometry);

    ProjectionSnapshot collapsed = rectangleWithHole();
    collapsed.cycles = {
        PolylineCycle {false, {{0.0, 0.0}, {1.0, 0.0}, {2.0, 0.0}}},
    };
    result = canonicalizeProjection(collapsed, output);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidGeometry);

    ProjectionSnapshot nonFinite = rectangleWithHole();
    nonFinite.cycles.front().points.front().x = std::numeric_limits<double>::quiet_NaN();
    result = canonicalizeProjection(nonFinite, output);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::NonFiniteValue);
}

TEST(PhotoInspectionCanonicalProjectionTest, quantizationIgnoresSubGridNoise)
{
    ProjectionSnapshot first = rectangleWithHole();
    ProjectionSnapshot noisy = first;
    noisy.cycles.front().points[1].x += 0.2e-6;

    CanonicalProjection canonicalFirst;
    CanonicalProjection canonicalNoisy;
    const auto firstResult = canonicalizeProjection(first, canonicalFirst);
    const auto noisyResult = canonicalizeProjection(noisy, canonicalNoisy);
    ASSERT_TRUE(firstResult.valid) << firstResult.diagnostic.message;
    ASSERT_TRUE(noisyResult.valid) << noisyResult.diagnostic.message;
    EXPECT_EQ(canonicalFirst.sha256, canonicalNoisy.sha256);
}

}  // namespace
