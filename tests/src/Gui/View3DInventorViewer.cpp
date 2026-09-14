// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include <array>
#include <cmath>
#include <limits>

#include <Gui/View3DInventorViewerInternal.h>

namespace
{

TEST(PickVolume, rejectsNullCameraAndEmptyFrustum)
{
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(
        false,
        1024,
        768,
        1.0F,
        1.0F,
        1.0F
    ));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 0, 768, 1.0F, 1.0F, 1.0F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 0, 1.0F, 1.0F, 1.0F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 768, 0.0F, 1.0F, 1.0F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 768, 1.0F, 0.0F, 1.0F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 768, 1.0F, 1.0F, 0.0F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(
        true,
        1024,
        768,
        std::numeric_limits<float>::infinity(),
        1.0F,
        1.0F
    ));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickExtent(1.0e10F));
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::isUsablePickVolume(
        true,
        1024,
        768,
        1.0e10F,
        1.0e10F,
        1.0F
    ));
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::isUsablePickExtent(200.0F));
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 768, 1.0F, 1.0F, 1.0F));
}

TEST(PickVolume, infNormalizedPixelConversionIsRejected)
{
    short pixel = 99;
    EXPECT_FALSE(Gui::View3DInventorViewerInternal::finiteNormalizedToShortPixel(
        std::numeric_limits<float>::infinity(),
        1024,
        pixel
    ));
    EXPECT_EQ(pixel, 99);
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::finiteNormalizedToShortPixel(0.5F, 1024, pixel));
    EXPECT_EQ(pixel, 512);
}

TEST(PickVolume, infExtentRecoversToFiniteFallback)
{
    EXPECT_FLOAT_EQ(
        Gui::View3DInventorViewerInternal::recoverPositiveExtent(
            std::numeric_limits<float>::infinity(),
            200.0F
        ),
        200.0F
    );
    EXPECT_FLOAT_EQ(Gui::View3DInventorViewerInternal::recoverPositiveExtent(40.0F, 200.0F), 40.0F);
}

TEST(PickVolume, infOrthographicHeightRecoversToSketchEditFallback)
{
    float recovered = 0.0F;
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::recoveredOrthographicHeight(
        std::numeric_limits<float>::infinity(),
        recovered
    ));
    EXPECT_FLOAT_EQ(recovered, Gui::View3DInventorViewerInternal::sketchEditFallbackHeight);
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::recoveredOrthographicHeight(40.0F, recovered));
    EXPECT_FLOAT_EQ(recovered, 40.0F);
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::recoveredOrthographicHeight(1.0e10F, recovered));
    EXPECT_FLOAT_EQ(recovered, Gui::View3DInventorViewerInternal::sketchEditFallbackHeight);
}

TEST(PickVolume, nanNearDistanceRecoversWithFiniteHeight)
{
    float nearDist = std::numeric_limits<float>::quiet_NaN();
    float farDist = std::numeric_limits<float>::infinity();
    float focalDist = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::recoveredCameraDistances(
        200.0F,
        nearDist,
        farDist,
        focalDist
    ));
    EXPECT_TRUE(std::isfinite(nearDist) && nearDist > 0.0F);
    EXPECT_TRUE(std::isfinite(farDist) && farDist > nearDist);
    EXPECT_TRUE(std::isfinite(focalDist) && focalDist >= nearDist && focalDist <= farDist);
}

TEST(DetachedNavigationRedraw, dockedViewFailsDetachedRequirementAsExpected)
{
    EXPECT_NONFATAL_FAILURE(
        EXPECT_TRUE(Gui::View3DInventorViewerInternal::requestDetachedNavigationRedraw(
            true,
            true,
            true,
            false,
            [] {}
        )),
        ""
    );
}

TEST(DetachedNavigationRedraw, requestsGuardedRedrawForCameraDrag)
{
    int redrawRequests = 0;

    EXPECT_TRUE(Gui::View3DInventorViewerInternal::requestDetachedNavigationRedraw(
        true,
        true,
        true,
        true,
        [&redrawRequests] {
            ++redrawRequests;
        }
    ));
    EXPECT_EQ(redrawRequests, 1);
}

TEST(DetachedNavigationRedraw, ignoresEventsThatAreNotDetachedCameraDrags)
{
    struct EventState
    {
        bool eventProcessed;
        bool cameraNavigationActive;
        bool isLocationEvent;
        bool isDetachedView;
    };

    constexpr std::array<EventState, 4> ignoredEvents {{
        {false, true, true, true},
        {true, false, true, true},
        {true, true, false, true},
        {true, true, true, false},
    }};

    for (const auto& event : ignoredEvents) {
        int redrawRequests = 0;
        EXPECT_FALSE(Gui::View3DInventorViewerInternal::requestDetachedNavigationRedraw(
            event.eventProcessed,
            event.cameraNavigationActive,
            event.isLocationEvent,
            event.isDetachedView,
            [&redrawRequests] {
                ++redrawRequests;
            }
        ));
        EXPECT_EQ(redrawRequests, 0);
    }
}

}  // namespace
