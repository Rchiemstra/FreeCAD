// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <string>

#include <App/Application.h>
#include <src/App/InitApplication.h>

#include <Mod/Inspection/App/PhotoInspectionObject.h>

namespace
{

using Inspection::Photo::AnalysisResult;
using Inspection::Photo::ConformanceDecision;
using Inspection::Photo::Measurement;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::PhotoInspectionResult;
using Inspection::Photo::PhotoInspectionSheet;
using Inspection::Photo::SheetDraft;

SheetDraft draft()
{
    SheetDraft value;
    value.status = OperationStatus::Complete;
    value.identity.seriesUuid = "series-object-1";
    value.identity.revisionUuid = "revision-object-1";
    value.identity.sourceToken = "source-object-1";
    value.identity.revision = 2;
    value.projection.bytes = {0x01, 0xa2};
    value.projection.sha256 = std::string(64, 'a');
    value.projectionGeometrySha256 = value.projection.sha256;
    value.qrContentSha256 = std::string(64, 'c');
    value.sheetContentSha256 = std::string(64, 'b');
    value.scene.widthMm = 210.0;
    value.scene.heightMm = 297.0;
    value.qrPayload = "{\"v\":[1,0]}";
    return value;
}

AnalysisResult analysis()
{
    AnalysisResult value;
    value.status = OperationStatus::Complete;
    value.decision = ConformanceDecision::Pass;
    value.projectionGeometrySha256 = std::string(64, 'a');
    value.measurements = {
        Measurement {"m", 0.0, 0.1, -0.5, 0.5, 0.2, ConformanceDecision::Pass},
    };
    return value;
}

void ensureObjectTypesRegistered()
{
    static const bool registered = [] {
        if (App::Application::GetARGC() == 0) {
            tests::initApplication();
        }
        PhotoInspectionSheet::init();
        PhotoInspectionResult::init();
        return true;
    }();
    (void)registered;
}

TEST(PhotoInspectionObjectTest, sheetInitializesOnceAndSealsImmutableProperties)
{
    ensureObjectTypesRegistered();
    PhotoInspectionSheet object;
    std::string reason;
    ASSERT_TRUE(object.initializeFromDraft(draft(), reason)) << reason;
    EXPECT_TRUE(reason.empty());
    EXPECT_TRUE(object.Sealed.getValue());
    EXPECT_EQ(object.SheetSeriesUUID.getStrValue(), "series-object-1");
    EXPECT_EQ(object.SheetRevisionUUID.getStrValue(), "revision-object-1");
    EXPECT_EQ(object.Revision.getValue(), 2);
    EXPECT_EQ(object.CanonicalProjectionHex.getStrValue(), "01a2");
    EXPECT_TRUE(object.SheetContentSha256.testStatus(App::Property::ReadOnly));
    EXPECT_EQ(object.QrContentSha256.getStrValue(), "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    EXPECT_TRUE(object.QrContentSha256.testStatus(App::Property::ReadOnly));
    EXPECT_TRUE(object.SceneSvg.testStatus(App::Property::ReadOnly));

    EXPECT_FALSE(object.initializeFromDraft(draft(), reason));
    EXPECT_NE(reason.find("already sealed"), std::string::npos);
}

TEST(PhotoInspectionObjectTest, invalidDraftCannotSealSheet)
{
    ensureObjectTypesRegistered();
    PhotoInspectionSheet object;
    SheetDraft invalid = draft();
    invalid.sheetContentSha256.clear();
    std::string reason;
    EXPECT_FALSE(object.initializeFromDraft(invalid, reason));
    EXPECT_FALSE(object.Sealed.getValue());
    EXPECT_FALSE(reason.empty());

    PhotoInspectionSheet objectMissingQr;
    invalid = draft();
    invalid.qrContentSha256.clear();
    reason.clear();
    EXPECT_FALSE(objectMissingQr.initializeFromDraft(invalid, reason));
    EXPECT_FALSE(objectMissingQr.Sealed.getValue());
    EXPECT_FALSE(reason.empty());
}

TEST(PhotoInspectionObjectTest, resultPersistsSeparateStatusDecisionAndCanonicalReport)
{
    ensureObjectTypesRegistered();
    PhotoInspectionResult object;
    std::string reason;
    ASSERT_TRUE(
        object.initializeFromResult(analysis(), "result-object-1", "revision-object-1", reason)
    ) << reason;
    EXPECT_TRUE(object.Sealed.getValue());
    EXPECT_EQ(object.OperationStatus.getStrValue(), "Complete");
    EXPECT_EQ(object.Decision.getStrValue(), "Pass");
    EXPECT_EQ(object.ReportSha256.getStrValue().size(), 64);
    EXPECT_NE(object.ReportJson.getStrValue().find("\"status\":\"Complete\""), std::string::npos);
    EXPECT_NE(object.ReportJson.getStrValue().find("\"decision\":\"Pass\""), std::string::npos);
    EXPECT_TRUE(object.ReportJson.testStatus(App::Property::ReadOnly));

    EXPECT_FALSE(
        object.initializeFromResult(analysis(), "result-object-2", "revision-object-1", reason)
    );
}

}  // namespace
