// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include <Mod/Inspection/App/PhotoInspectionEngine.h>

namespace
{

using Inspection::Photo::AnalysisInput;
using Inspection::Photo::analyzePhotoInspection;
using Inspection::Photo::canonicalizeProjection;
using Inspection::Photo::CanonicalProjection;
using Inspection::Photo::comparePolylineCycle;
using Inspection::Photo::ConformanceDecision;
using Inspection::Photo::DeviationSample;
using Inspection::Photo::evaluateMeasurement;
using Inspection::Photo::fitRigidTransform;
using Inspection::Photo::makeCanonicalFrame;
using Inspection::Photo::Measurement;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::PolylineCycle;
using Inspection::Photo::ProjectionSnapshot;
using Inspection::Photo::Vector2d;

PolylineCycle square(const bool hole = false, const double offset = 0.0)
{
    return {
        hole,
        {
            {0.0 - offset, 0.0 - offset},
            {10.0 + offset, 0.0 - offset},
            {10.0 + offset, 10.0 + offset},
            {0.0 - offset, 10.0 + offset},
        },
    };
}

CanonicalProjection squareProjection()
{
    ProjectionSnapshot snapshot;
    snapshot.frame = *makeCanonicalFrame({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0});
    snapshot.cycles = {square()};
    CanonicalProjection result;
    EXPECT_TRUE(canonicalizeProjection(snapshot, result).valid);
    return result;
}

TEST(PhotoInspectionComparisonTest, rigidFitRecoversRotationAndTranslationWithoutScale)
{
    const std::vector<Vector2d> source {{0.0, 0.0}, {10.0, 0.0}, {0.0, 5.0}};
    const double angle = 0.3;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    std::vector<Vector2d> target;
    for (const Vector2d& point : source) {
        target.push_back({
            cosine * point.x - sine * point.y + 3.0,
            sine * point.x + cosine * point.y - 2.0,
        });
    }

    const auto fitted = fitRigidTransform(source, target);
    ASSERT_TRUE(fitted.has_value());
    EXPECT_NEAR(fitted->cosine, cosine, 1.0e-12);
    EXPECT_NEAR(fitted->sine, sine, 1.0e-12);
    for (std::size_t index = 0; index < source.size(); ++index) {
        const Vector2d transformed = fitted->apply(source[index]);
        EXPECT_NEAR(transformed.x, target[index].x, 1.0e-12);
        EXPECT_NEAR(transformed.y, target[index].y, 1.0e-12);
    }

    std::vector<Vector2d> scaled = target;
    for (Vector2d& point : scaled) {
        point.x *= 1.02;
        point.y *= 1.02;
    }
    const auto scaleForbidden = fitRigidTransform(source, scaled);
    ASSERT_TRUE(scaleForbidden.has_value());
    const Vector2d transformed = scaleForbidden->apply(source[1]);
    EXPECT_GT(std::hypot(transformed.x - scaled[1].x, transformed.y - scaled[1].y), 0.01);
}

TEST(PhotoInspectionComparisonTest, rigidFitRejectsDegenerateInputs)
{
    EXPECT_FALSE(fitRigidTransform({}, {}).has_value());
    EXPECT_FALSE(fitRigidTransform({{1.0, 1.0}, {1.0, 1.0}}, {{2.0, 2.0}, {2.0, 2.0}}).has_value());
}

TEST(PhotoInspectionComparisonTest, outerCycleSignMeansOutsideIsExcessMaterial)
{
    const PolylineCycle measured {
        false,
        {
            {10.2, 5.0},
            {9.8, 5.0},
        },
    };
    std::vector<DeviationSample> deviations;
    const auto result = comparePolylineCycle(square(), measured, 0, deviations);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(deviations.size(), 2);
    EXPECT_NEAR(deviations[0].signedDeviationMm, 0.2, 1.0e-12);
    EXPECT_NEAR(deviations[1].signedDeviationMm, -0.2, 1.0e-12);
}

TEST(PhotoInspectionComparisonTest, holeCycleSignMeansSmallerHoleIsExcessMaterial)
{
    const PolylineCycle measured {
        true,
        {
            {0.2, 5.0},
            {-0.2, 5.0},
        },
    };
    std::vector<DeviationSample> deviations;
    const auto result = comparePolylineCycle(square(true), measured, 0, deviations);
    ASSERT_TRUE(result.valid);
    ASSERT_EQ(deviations.size(), 2);
    EXPECT_NEAR(deviations[0].signedDeviationMm, 0.2, 1.0e-12);
    EXPECT_NEAR(deviations[1].signedDeviationMm, -0.2, 1.0e-12);
}

TEST(PhotoInspectionDecisionTest, guardBandSeparatesPassFailAndInconclusive)
{
    Measurement measurement {"m", 10.0, 10.0, -0.5, 0.5, 0.2};
    EXPECT_EQ(evaluateMeasurement(measurement), ConformanceDecision::Pass);

    measurement.actualMm = 10.8;
    EXPECT_EQ(evaluateMeasurement(measurement), ConformanceDecision::Fail);

    measurement.actualMm = 10.4;
    EXPECT_EQ(evaluateMeasurement(measurement), ConformanceDecision::Inconclusive);
}

TEST(PhotoInspectionEngineTest, profileEvidenceGatesOtherwiseCompleteDecision)
{
    AnalysisInput input;
    input.generation = 42;
    input.nominalProjection = squareProjection();
    input.measuredCycles = {square(false, 0.1)};
    input.options.expandedUncertaintyMm = 0.1;

    auto result = analyzePhotoInspection(input);
    EXPECT_EQ(result.status, OperationStatus::Complete);
    EXPECT_EQ(result.decision, ConformanceDecision::Inconclusive);
    EXPECT_FALSE(result.diagnostics.empty());

    input.options.cameraProfileDecisionCapable = true;
    input.options.printerProfileDecisionCapable = true;
    result = analyzePhotoInspection(input);
    EXPECT_EQ(result.status, OperationStatus::Complete);
    EXPECT_EQ(result.decision, ConformanceDecision::Pass);
    EXPECT_EQ(result.generation, 42);
    EXPECT_FALSE(result.measurements.empty());
}

TEST(PhotoInspectionEngineTest, cancellationReturnsNoPartialResult)
{
    AnalysisInput input;
    input.generation = 7;
    input.nominalProjection = squareProjection();
    input.measuredCycles = {square()};

    int progressCalls = 0;
    const auto result = analyzePhotoInspection(
        input,
        []() { return true; },
        [&progressCalls](std::uint64_t, const std::string&, std::size_t, std::size_t) {
            ++progressCalls;
        }
    );
    EXPECT_EQ(result.status, OperationStatus::Cancelled);
    EXPECT_EQ(result.decision, ConformanceDecision::NotEvaluated);
    EXPECT_TRUE(result.deviations.empty());
    EXPECT_TRUE(result.measurements.empty());
    EXPECT_EQ(progressCalls, 0);
}

}  // namespace
