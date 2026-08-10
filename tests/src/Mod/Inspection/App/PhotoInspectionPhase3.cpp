// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <limits>
#include <string>

#include <Mod/Inspection/App/PhotoInspectionProfiles.h>

namespace
{

using Inspection::Photo::AffineTransform2d;
using Inspection::Photo::CameraProfile;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::maximumProfileBytes;
using Inspection::Photo::parseCameraProfile;
using Inspection::Photo::parsePrinterProfile;
using Inspection::Photo::PrinterProfile;
using Inspection::Photo::validateCameraProfile;
using Inspection::Photo::validatePrinterProfile;

CameraProfile cameraProfile()
{
    CameraProfile profile;
    profile.uuid = "camera-profile-018f";
    profile.imageWidth = 1280;
    profile.imageHeight = 960;
    profile.cameraMatrix = {1000.0, 0.0, 640.0, 0.0, 1001.0, 480.0, 0.0, 0.0, 1.0};
    profile.distortion = {-0.01, 0.001, 0.0, 0.0, 0.0};
    profile.rmsReprojectionErrorPx = 0.18;
    profile.acceptedFrames = 24;
    profile.boardHash = "board-sha256-0123456789";
    profile.cameraFingerprint = "camera-fingerprint-42";
    profile.policyVersion = "camera-policy-1";
    profile.validated = true;
    return profile;
}

PrinterProfile printerProfile()
{
    PrinterProfile profile;
    profile.uuid = "printer-profile-018f";
    profile.printerFingerprint = "printer-driver-media-42";
    profile.media = "A4";
    profile.physicalFromCommand = AffineTransform2d({1.001, 0.0002, -0.0001, 0.999, 0.04, -0.03});
    profile.fitResidualMm = 0.08;
    profile.repeatabilityMm = 0.12;
    profile.calibrationPoints = 12;
    profile.heldOutSpans = 3;
    profile.repeatedPrints = 5;
    profile.policyVersion = "printer-policy-1";
    profile.validated = true;
    return profile;
}

TEST(PhotoInspectionProfileTest, cameraCanonicalRoundTripPreservesHash)
{
    const auto validated = validateCameraProfile(cameraProfile());
    ASSERT_TRUE(validated.valid) << validated.diagnostic.message;
    EXPECT_TRUE(validated.decisionCapable);
    EXPECT_EQ(validated.sha256.size(), 64);

    CameraProfile parsed;
    const auto reparsed = parseCameraProfile(validated.canonicalJson, parsed);
    ASSERT_TRUE(reparsed.valid) << reparsed.diagnostic.message;
    EXPECT_EQ(reparsed.canonicalJson, validated.canonicalJson);
    EXPECT_EQ(reparsed.sha256, validated.sha256);
    EXPECT_EQ(parsed.cameraMatrix, cameraProfile().cameraMatrix);
}

TEST(PhotoInspectionProfileTest, duplicateAndUnknownJsonKeysAreRejected)
{
    CameraProfile parsed;
    auto result = parseCameraProfile("{\"uuid\":\"first\",\"uuid\":\"second\"}", parsed);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.diagnostic.message.find("duplicate"), std::string::npos);

    const auto valid = validateCameraProfile(cameraProfile());
    std::string unknown = valid.canonicalJson;
    unknown.insert(unknown.size() - 1, ",\"unexpected\":1");
    result = parseCameraProfile(unknown, parsed);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.diagnostic.message.find("schema"), std::string::npos);
}

TEST(PhotoInspectionProfileTest, cameraRejectsInvalidMatrixAndNonFiniteResidual)
{
    CameraProfile invalid = cameraProfile();
    invalid.cameraMatrix[0] = 0.0;
    auto result = validateCameraProfile(invalid);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidSchema);

    invalid = cameraProfile();
    invalid.rmsReprojectionErrorPx = std::numeric_limits<double>::quiet_NaN();
    result = validateCameraProfile(invalid);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidSchema);
}

TEST(PhotoInspectionProfileTest, printerEvidenceControlsDecisionCapability)
{
    PrinterProfile profile = printerProfile();
    auto result = validatePrinterProfile(profile);
    ASSERT_TRUE(result.valid);
    EXPECT_TRUE(result.decisionCapable);

    profile.repeatedPrints = 2;
    result = validatePrinterProfile(profile);
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.decisionCapable);

    profile.validated = false;
    profile.repeatedPrints = 5;
    result = validatePrinterProfile(profile);
    EXPECT_TRUE(result.valid);
    EXPECT_FALSE(result.decisionCapable);
}

TEST(PhotoInspectionProfileTest, printerRoundTripAndSingularTransformHandling)
{
    const auto validated = validatePrinterProfile(printerProfile());
    ASSERT_TRUE(validated.valid);

    PrinterProfile parsed;
    const auto reparsed = parsePrinterProfile(validated.canonicalJson, parsed);
    ASSERT_TRUE(reparsed.valid) << reparsed.diagnostic.message;
    EXPECT_EQ(reparsed.sha256, validated.sha256);
    EXPECT_EQ(
        parsed.physicalFromCommand.coefficients(),
        printerProfile().physicalFromCommand.coefficients()
    );

    PrinterProfile singular = printerProfile();
    singular.physicalFromCommand = AffineTransform2d({1.0, 2.0, 2.0, 4.0, 0.0, 0.0});
    EXPECT_FALSE(validatePrinterProfile(singular).valid);
}

TEST(PhotoInspectionProfileTest, malformedAndOversizedProfilesAreRejected)
{
    CameraProfile parsed;
    auto result = parseCameraProfile("{not-json", parsed);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidSchema);

    result = parseCameraProfile(std::string(maximumProfileBytes + 1, 'x'), parsed);
    EXPECT_FALSE(result.valid);
    EXPECT_NE(result.diagnostic.message.find("byte limit"), std::string::npos);
}

}  // namespace
