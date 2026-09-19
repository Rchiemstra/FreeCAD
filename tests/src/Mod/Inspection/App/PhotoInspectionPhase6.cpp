// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include <Mod/Inspection/App/PhotoInspectionReport.h>

namespace
{

using Inspection::Photo::AnalysisResult;
using Inspection::Photo::buildResultScene;
using Inspection::Photo::ConformanceDecision;
using Inspection::Photo::DeviationSample;
using Inspection::Photo::Diagnostic;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::DiagnosticSeverity;
using Inspection::Photo::Measurement;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::renderPhotoInspectionSvg;
using Inspection::Photo::toCanonicalJson;
using Inspection::Photo::toCsvMeasurements;

AnalysisResult result()
{
    AnalysisResult value;
    value.generation = 42;
    value.status = OperationStatus::Complete;
    value.decision = ConformanceDecision::Inconclusive;
    value.projectionGeometrySha256 = std::string(64, 'a');
    value.diagnostics = {
        Diagnostic {
            DiagnosticCode::MissingCalibration,
            DiagnosticSeverity::Warning,
            "printer evidence required",
        },
    };
    value.measurements = {
        Measurement {"width", 100.0, 100.1, -0.5, 0.5, 0.2, ConformanceDecision::Pass},
    };
    value.deviations = {
        DeviationSample {0, 0, {10.1, 5.0}, {10.0, 5.0}, 0.1},
    };
    return value;
}

TEST(PhotoInspectionReportTest, canonicalJsonIsDeterministicAndSeparatesStatusDecision)
{
    const auto first = toCanonicalJson(result());
    const auto second = toCanonicalJson(result());
    ASSERT_TRUE(first.valid);
    ASSERT_TRUE(second.valid);
    EXPECT_EQ(first.content, second.content);
    EXPECT_EQ(first.sha256, second.sha256);
    EXPECT_EQ(first.sha256.size(), 64);
    EXPECT_NE(first.content.find("\"status\":\"Complete\""), std::string::npos);
    EXPECT_NE(first.content.find("\"decision\":\"Inconclusive\""), std::string::npos);
    EXPECT_NE(first.content.find("\"photo_inspection_schema_version\":[1,0]"), std::string::npos);
}

TEST(PhotoInspectionReportTest, nonFiniteMeasurementsAreRejected)
{
    AnalysisResult invalid = result();
    invalid.measurements.front().actualMm = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(toCanonicalJson(invalid).valid);
    EXPECT_FALSE(toCsvMeasurements(invalid).valid);
}

TEST(PhotoInspectionReportTest, csvUsesLocaleIndependentNumbersAndFormulaProtection)
{
    AnalysisResult value = result();
    value.measurements.front().id = "=HYPERLINK(\"bad\")\nnext";
    const auto csv = toCsvMeasurements(value);
    ASSERT_TRUE(csv.valid);
    EXPECT_NE(csv.content.find("\"'=HYPERLINK(\"\"bad\"\") next\""), std::string::npos);
    EXPECT_NE(csv.content.find(",100.1,"), std::string::npos);
    EXPECT_EQ(csv.content.find(",100,1,"), std::string::npos);
    EXPECT_NE(csv.content.find("\r\n"), std::string::npos);
}

TEST(PhotoInspectionReportTest, resultSceneContainsOnlyOwnedVectorPrimitives)
{
    const auto scene = buildResultScene(result());
    ASSERT_EQ(scene.primitives.size(), 2);
    EXPECT_EQ(scene.primitives.front().layer, "deviations");
    EXPECT_EQ(scene.primitives.back().layer, "decision");

    const std::string svg = renderPhotoInspectionSvg(scene);
    EXPECT_NE(svg.find("data-layer=\"deviations\""), std::string::npos);
    EXPECT_NE(svg.find("data-layer=\"decision\""), std::string::npos);
    EXPECT_EQ(svg.find("<image"), std::string::npos);
}

}  // namespace
