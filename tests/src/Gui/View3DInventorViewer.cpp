// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include <array>

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
    EXPECT_TRUE(Gui::View3DInventorViewerInternal::isUsablePickVolume(true, 1024, 768, 1.0F, 1.0F, 1.0F));
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
