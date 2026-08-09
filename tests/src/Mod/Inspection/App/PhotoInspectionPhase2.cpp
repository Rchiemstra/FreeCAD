// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>

#include <Mod/Inspection/App/OpenCVPhotoInspectionCompat.h>
#include <Mod/Inspection/App/PhotoInspectionSheet.h>

namespace
{

using Inspection::Photo::AffineTransform2d;
using Inspection::Photo::buildPhotoInspectionSheet;
using Inspection::Photo::canonicalizeProjection;
using Inspection::Photo::CanonicalProjection;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::makeCanonicalFrame;
using Inspection::Photo::OpenCVPhotoInspectionCompat;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::PolylineCycle;
using Inspection::Photo::ProjectionSnapshot;
using Inspection::Photo::renderPhotoInspectionSvg;
using Inspection::Photo::ScenePrimitive;
using Inspection::Photo::ScenePrimitiveKind;
using Inspection::Photo::SheetIdentity;
using Inspection::Photo::SheetOptions;
using Inspection::Photo::VectorScene;

CanonicalProjection rectangleProjection(const double width = 100.0, const double height = 50.0)
{
    ProjectionSnapshot snapshot;
    snapshot.frame = *makeCanonicalFrame({0.0, 0.0, 0.0}, {0.0, 0.0, 1.0});
    snapshot.cycles = {
        PolylineCycle {
            false,
            {
                {0.0, 0.0},
                {width, 0.0},
                {width, height},
                {0.0, height},
            },
        },
    };
    CanonicalProjection projection;
    const auto validation = canonicalizeProjection(snapshot, projection);
    EXPECT_TRUE(validation.valid) << validation.diagnostic.message;
    return projection;
}

SheetIdentity identity()
{
    return {
        "series-018f5f7b",
        "revision-018f5f7b",
        "source-token-2e93",
        1,
    };
}

TEST(PhotoInspectionSheetTest, buildsDeterministicA4VectorSceneWhenCapabilityExists)
{
    const auto first = buildPhotoInspectionSheet(rectangleProjection(), identity(), {});
    const auto second = buildPhotoInspectionSheet(rectangleProjection(), identity(), {});
    const bool available = OpenCVPhotoInspectionCompat::capability().available;

    if (!available) {
        EXPECT_EQ(first.status, OperationStatus::Unavailable);
        EXPECT_EQ(first.diagnostic.code, DiagnosticCode::OpenCVUnavailable);
        EXPECT_EQ(second.status, OperationStatus::Unavailable);
        return;
    }

    ASSERT_EQ(first.status, OperationStatus::Complete) << first.diagnostic.message;
    ASSERT_EQ(second.status, OperationStatus::Complete) << second.diagnostic.message;
    EXPECT_EQ(first.projectionGeometrySha256.size(), 64);
    EXPECT_EQ(first.qrContentSha256.size(), 64);
    EXPECT_EQ(first.sheetContentSha256.size(), 64);
    EXPECT_NE(first.qrContentSha256, first.sheetContentSha256);
    EXPECT_NE(first.qrPayload.find(first.qrContentSha256), std::string::npos);
    EXPECT_EQ(first.qrContentSha256, second.qrContentSha256);
    EXPECT_EQ(first.sheetContentSha256, second.sheetContentSha256);
    EXPECT_EQ(first.qrPayload, second.qrPayload);
    EXPECT_EQ(renderPhotoInspectionSvg(first.scene), renderPhotoInspectionSvg(second.scene));
}

TEST(PhotoInspectionSheetTest, svgUsesExactMillimetresAndVectorOnlyCriticalLayers)
{
    const auto draft = buildPhotoInspectionSheet(rectangleProjection(), identity(), {});
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_EQ(draft.status, OperationStatus::Unavailable);
        return;
    }
    ASSERT_EQ(draft.status, OperationStatus::Complete);

    const std::string svg = renderPhotoInspectionSvg(draft.scene);
    EXPECT_NE(svg.find("width=\"210mm\""), std::string::npos);
    EXPECT_NE(svg.find("height=\"297mm\""), std::string::npos);
    EXPECT_NE(svg.find("viewBox=\"0 0 210 297\""), std::string::npos);
    EXPECT_NE(svg.find("data-layer=\"cad-boundary\""), std::string::npos);
    EXPECT_NE(svg.find("data-layer=\"markers\""), std::string::npos);
    EXPECT_NE(svg.find("data-layer=\"identity\""), std::string::npos);
    EXPECT_EQ(svg.find("<image"), std::string::npos);
    EXPECT_EQ(svg.find("transform=\"scale("), std::string::npos);
}

TEST(PhotoInspectionSheetTest, identityLabelClearsBottomLeftMarkerQuietZone)
{
    const auto draft = buildPhotoInspectionSheet(rectangleProjection(), identity(), {});
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_EQ(draft.status, OperationStatus::Unavailable);
        return;
    }
    ASSERT_EQ(draft.status, OperationStatus::Complete);

    constexpr double marginMm = 5.0;
    constexpr double markerQuietZone = 2.0;
    constexpr double markerSize = 12.0;
    constexpr double fontSizeMm = 3.0;
    const double bottomMarkerTopY = draft.scene.heightMm - marginMm - markerQuietZone - markerSize;
    const double minimumLabelBaselineY = bottomMarkerTopY - markerQuietZone - fontSizeMm;

    const auto label = std::find_if(
        draft.scene.primitives.begin(),
        draft.scene.primitives.end(),
        [](const ScenePrimitive& primitive) { return primitive.id == "sheet-identity-label"; }
    );
    ASSERT_NE(label, draft.scene.primitives.end());
    ASSERT_FALSE(label->points.empty());
    EXPECT_LE(label->points.front().y, minimumLabelBaselineY);
}

TEST(PhotoInspectionSheetTest, rejectsOversizeProjectionBeforeMarkerGeneration)
{
    const auto draft = buildPhotoInspectionSheet(rectangleProjection(200.0, 280.0), identity(), {});
    EXPECT_EQ(draft.status, OperationStatus::InvalidInput);
    EXPECT_EQ(draft.diagnostic.code, DiagnosticCode::InvalidGeometry);
    EXPECT_NE(draft.diagnostic.message.find("does not fit"), std::string::npos);
}

TEST(PhotoInspectionSheetTest, appliesInversePrinterScaleToCadCommands)
{
    SheetOptions options;
    options.physicalFromCommand = AffineTransform2d({1.02, 0.0, 0.0, 1.0, 0.0, 0.0});
    const auto draft = buildPhotoInspectionSheet(rectangleProjection(), identity(), options);
    if (!OpenCVPhotoInspectionCompat::capability().available) {
        EXPECT_EQ(draft.status, OperationStatus::Unavailable);
        return;
    }
    ASSERT_EQ(draft.status, OperationStatus::Complete);

    const auto boundary = std::find_if(
        draft.scene.primitives.begin(),
        draft.scene.primitives.end(),
        [](const ScenePrimitive& primitive) { return primitive.id == "cad-cycle-0"; }
    );
    ASSERT_NE(boundary, draft.scene.primitives.end());
    ASSERT_GE(boundary->points.size(), 2);
    const double commandWidth = std::abs(boundary->points[1].x - boundary->points[0].x);
    EXPECT_NEAR(commandWidth, 100.0 / 1.02, 1.0e-9);
}

TEST(PhotoInspectionSheetTest, svgEscapesUntrustedTextAndIdentifiers)
{
    VectorScene scene;
    scene.widthMm = 10.0;
    scene.heightMm = 10.0;
    ScenePrimitive text;
    text.id = "label<&";
    text.layer = "annotations<&";
    text.kind = ScenePrimitiveKind::Text;
    text.points = {{1.0, 2.0}};
    text.text = "A<&\"'";
    scene.primitives.push_back(text);

    const std::string svg = renderPhotoInspectionSvg(scene);
    EXPECT_NE(svg.find("label&lt;&amp;"), std::string::npos);
    EXPECT_NE(svg.find("annotations&lt;&amp;"), std::string::npos);
    EXPECT_NE(svg.find("A&lt;&amp;&quot;&apos;"), std::string::npos);
    EXPECT_EQ(svg.find("A<&"), std::string::npos);
}

}  // namespace
