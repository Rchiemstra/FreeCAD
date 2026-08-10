// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>

#include <Mod/Inspection/App/OpenCVPhotoInspectionCompat.h>
#include <Mod/Inspection/App/PhotoInspectionTransform.h>

namespace
{

using Inspection::Photo::AffineTransform2d;
using Inspection::Photo::BinaryGrid;
using Inspection::Photo::OpenCVPhotoInspectionCompat;
using Inspection::Photo::Point2d;
using Inspection::Photo::printerCommandFromPhysical;

TEST(PhotoInspectionCapabilityTest, reportsTruthfulCompileAndRuntimeCapability)
{
    const auto capability = OpenCVPhotoInspectionCompat::capability();
    std::string runtimeReason;
    const bool runtimeAvailable = OpenCVPhotoInspectionCompat::runtimeSmokeTest(runtimeReason);

    EXPECT_EQ(runtimeAvailable, capability.available);
    if (capability.available) {
        EXPECT_TRUE(capability.requested);
        EXPECT_GE(capability.versionMajor, 4);
        EXPECT_LT(capability.versionMajor, 5);
        EXPECT_FALSE(capability.version.empty());
        EXPECT_FALSE(capability.components.empty());
        EXPECT_FALSE(capability.compatibilityBranch.empty());
        EXPECT_FALSE(capability.buildInformation.empty());
        EXPECT_TRUE(runtimeReason.empty());
    }
    else {
        EXPECT_FALSE(capability.reason.empty());
        EXPECT_FALSE(runtimeReason.empty());
        EXPECT_TRUE(capability.buildInformation.empty());
    }
}

TEST(PhotoInspectionCapabilityTest, markerGridIsBoundedAndTruthful)
{
    const auto capability = OpenCVPhotoInspectionCompat::capability();
    BinaryGrid marker;
    std::string reason;
    const bool generated = OpenCVPhotoInspectionCompat::markerGrid(7, 920, marker, reason);
    EXPECT_EQ(generated, capability.available);
    if (generated) {
        EXPECT_TRUE(marker.valid());
        EXPECT_EQ(marker.rows, 7);
        EXPECT_EQ(marker.columns, 7);
        EXPECT_TRUE(marker.black(0, 0));
        EXPECT_TRUE(marker.black(6, 6));
        EXPECT_TRUE(reason.empty());
    }
    else {
        EXPECT_FALSE(marker.valid());
        EXPECT_FALSE(reason.empty());
    }
}

TEST(PhotoInspectionCapabilityTest, qrGridEnforcesPayloadBounds)
{
    BinaryGrid qr;
    std::string reason;
    EXPECT_FALSE(OpenCVPhotoInspectionCompat::qrGrid("", qr, reason));
    EXPECT_FALSE(reason.empty());

    reason.clear();
    EXPECT_FALSE(OpenCVPhotoInspectionCompat::qrGrid(std::string(321, 'x'), qr, reason));
    EXPECT_FALSE(reason.empty());

    const auto capability = OpenCVPhotoInspectionCompat::capability();
    reason.clear();
    const bool generated
        = OpenCVPhotoInspectionCompat::qrGrid("{\"v\":[1,0],\"sid\":\"test\"}", qr, reason);
    EXPECT_EQ(generated, capability.available);
    if (generated) {
        EXPECT_TRUE(qr.valid());
        EXPECT_GE(qr.rows, 21);
        EXPECT_EQ(qr.rows, qr.columns);
        EXPECT_TRUE(reason.empty());
    }
}

TEST(PhotoInspectionTransformTest, identityLeavesPointsUnchanged)
{
    const Point2d source {12.5, -4.25};
    const Point2d result = AffineTransform2d::identity().apply(source);

    EXPECT_DOUBLE_EQ(result.x, source.x);
    EXPECT_DOUBLE_EQ(result.y, source.y);
}

TEST(PhotoInspectionTransformTest, inverseRecoversAnisotropicShearedTranslatedPoint)
{
    const AffineTransform2d targetFromSource({1.02, 0.01, -0.015, 0.98, 3.5, -2.25});
    const auto sourceFromTarget = targetFromSource.inverse();
    ASSERT_TRUE(sourceFromTarget.has_value());

    const Point2d source {123.4, 56.7};
    const Point2d target = targetFromSource.apply(source);
    const Point2d recovered = sourceFromTarget->apply(target);

    EXPECT_NEAR(recovered.x, source.x, 1.0e-12);
    EXPECT_NEAR(recovered.y, source.y, 1.0e-12);
}

TEST(PhotoInspectionTransformTest, printerPlusTwoPercentScaleIsPreCompensated)
{
    // The measured printer profile maps a 100 mm X print command to 102 mm
    // physically. The inspection sheet must command 100/1.02 mm so the
    // physical reference remains 100 mm. Treating ideal marker coordinates as
    // physical would retain the 2 mm error.
    const AffineTransform2d physicalFromCommand({1.02, 0.0, 0.0, 1.0, 0.0, 0.0});
    const auto commandFromPhysical = printerCommandFromPhysical(physicalFromCommand);
    ASSERT_TRUE(commandFromPhysical.has_value());

    const Point2d intendedPhysical {100.0, 40.0};
    const Point2d correctedCommand = commandFromPhysical->apply(intendedPhysical);
    const Point2d correctedPhysical = physicalFromCommand.apply(correctedCommand);
    const Point2d uncorrectedPhysical = physicalFromCommand.apply(intendedPhysical);

    EXPECT_NEAR(correctedCommand.x, 100.0 / 1.02, 1.0e-12);
    EXPECT_NEAR(correctedPhysical.x, intendedPhysical.x, 1.0e-12);
    EXPECT_NEAR(correctedPhysical.y, intendedPhysical.y, 1.0e-12);
    EXPECT_NEAR(uncorrectedPhysical.x, 102.0, 1.0e-12);
}

TEST(PhotoInspectionTransformTest, singularOrNonFiniteProfileIsRejected)
{
    const AffineTransform2d singular({1.0, 2.0, 2.0, 4.0, 0.0, 0.0});
    EXPECT_FALSE(printerCommandFromPhysical(singular).has_value());

    const AffineTransform2d nonFinite(
        {1.0, 0.0, 0.0, 1.0, std::numeric_limits<double>::infinity(), 0.0}
    );
    EXPECT_FALSE(printerCommandFromPhysical(nonFinite).has_value());
}

}  // namespace
