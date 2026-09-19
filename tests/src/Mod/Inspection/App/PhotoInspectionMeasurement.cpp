// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include <Mod/Inspection/App/PhotoInspectionMeasurement.h>

namespace
{

using namespace Inspection::Photo;

constexpr double pi = 3.141592653589793238462643383279502884;

TEST(PhotoInspectionMeasurementTest, independentAndCorrelatedComponentsCombineConservatively)
{
    UncertaintyBudget budget;
    budget.components = {
        {"camera", 0.1, ""},
        {"marker", 0.2, "image"},
        {"segmentation", 0.3, "image"},
        {"printer", 0.4, ""},
    };
    const auto validation = combineUncertaintyBudget(budget);
    ASSERT_TRUE(validation.valid) << validation.diagnostic.message;
    EXPECT_NEAR(budget.combinedStandardUncertaintyMm, std::sqrt(0.01 + 0.25 + 0.16), 1.0e-12);
    EXPECT_NEAR(budget.expandedUncertaintyMm, 2.0 * std::sqrt(0.42), 1.0e-12);
}

TEST(PhotoInspectionMeasurementTest, invalidUncertaintyCannotReachDecision)
{
    UncertaintyBudget budget;
    budget.components = {{"camera", std::numeric_limits<double>::quiet_NaN(), ""}};
    const auto validation = combineUncertaintyBudget(budget);
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(validation.diagnostic.code, DiagnosticCode::InvalidSchema);
}

TEST(PhotoInspectionMeasurementTest, fullCircleRecoversCenterRadiusAndResidual)
{
    std::vector<Vector2d> points;
    for (int index = 0; index < 72; ++index) {
        const double angle = 2.0 * pi * index / 72.0;
        points.push_back({4.0 + 10.0 * std::cos(angle), -2.0 + 10.0 * std::sin(angle)});
    }
    const auto result = fitCircleFeature(points, 1.5 * pi);
    ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
    EXPECT_NEAR(result.center.x, 4.0, 1.0e-10);
    EXPECT_NEAR(result.center.y, -2.0, 1.0e-10);
    EXPECT_NEAR(result.diameterMm, 20.0, 1.0e-10);
    EXPECT_NEAR(result.rmsResidualMm, 0.0, 1.0e-10);
}

TEST(PhotoInspectionMeasurementTest, partialArcIsNeverRelabeledAsHole)
{
    std::vector<Vector2d> points;
    for (int index = 0; index < 20; ++index) {
        const double angle = pi * index / 38.0;
        points.push_back({10.0 * std::cos(angle), 10.0 * std::sin(angle)});
    }
    const auto result = fitCircleFeature(points, pi);
    EXPECT_EQ(result.status, OperationStatus::Inconclusive);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::UnsupportedGeometry);
}

TEST(PhotoInspectionMeasurementTest, extentsIgnoreHoles)
{
    const std::vector<PolylineCycle> cycles {
        {false, {{-5.0, 2.0}, {15.0, 2.0}, {15.0, 12.0}, {-5.0, 12.0}}},
        {true, {{100.0, 100.0}, {101.0, 100.0}, {101.0, 101.0}}},
    };
    const auto result = measureExtents(cycles);
    ASSERT_EQ(result.status, OperationStatus::Complete);
    EXPECT_DOUBLE_EQ(result.widthMm, 20.0);
    EXPECT_DOUBLE_EQ(result.heightMm, 10.0);
}

TEST(PhotoInspectionMeasurementTest, extentsRejectNonFiniteData)
{
    const std::vector<PolylineCycle> cycles {
        {false, {{0.0, 0.0}, {1.0, std::numeric_limits<double>::infinity()}, {1.0, 1.0}}},
    };
    const auto result = measureExtents(cycles);
    EXPECT_EQ(result.status, OperationStatus::InvalidInput);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::NonFiniteValue);
}

}  // namespace
