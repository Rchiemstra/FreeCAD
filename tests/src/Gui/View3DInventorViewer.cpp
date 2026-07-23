// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <array>

#include <Gui/View3DInventorViewerInternal.h>

namespace
{

TEST(DetachedNavigationUpdate, requestsViewportUpdateForCameraDrag)
{
    int updateRequests = 0;

    EXPECT_TRUE(Gui::View3DInventorViewerInternal::requestDetachedNavigationUpdate(
        true,
        true,
        true,
        true,
        [&updateRequests] {
            ++updateRequests;
        }
    ));
    EXPECT_EQ(updateRequests, 1);
}

TEST(DetachedNavigationUpdate, ignoresEventsThatAreNotDetachedCameraDrags)
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
        int updateRequests = 0;
        EXPECT_FALSE(Gui::View3DInventorViewerInternal::requestDetachedNavigationUpdate(
            event.eventProcessed,
            event.cameraNavigationActive,
            event.isLocationEvent,
            event.isDetachedView,
            [&updateRequests] {
                ++updateRequests;
            }
        ));
        EXPECT_EQ(updateRequests, 0);
    }
}

}  // namespace
