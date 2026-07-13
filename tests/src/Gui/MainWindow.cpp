// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Gui/MainWindowInternal.h>

namespace
{

TEST(MDITabDrag, detachesOutsideTheTabBarPastDragThreshold)
{
    const QRect tabBarRect(0, 0, 500, 30);

    EXPECT_TRUE(Gui::MainWindowInternal::isTabDetachGesture(
        QPoint(200, 15),
        QPoint(200, -20),
        tabBarRect,
        10
    ));
}

TEST(MDITabDrag, remainsDockedInsideTheTabBar)
{
    const QRect tabBarRect(0, 0, 500, 30);

    EXPECT_FALSE(Gui::MainWindowInternal::isTabDetachGesture(
        QPoint(100, 15),
        QPoint(400, 15),
        tabBarRect,
        10
    ));
}

TEST(MDITabDrag, ignoresMovementBelowDragThreshold)
{
    const QRect tabBarRect(0, 0, 500, 30);

    EXPECT_FALSE(Gui::MainWindowInternal::isTabDetachGesture(
        QPoint(100, 1),
        QPoint(100, -1),
        tabBarRect,
        10
    ));
}

}  // namespace
