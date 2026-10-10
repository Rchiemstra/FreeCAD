// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Inventor/SoDB.h>
#include <Inventor/errors/SoDebugError.h>

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

namespace
{

int coinWarnings = 0;

void countCoinWarning(const SoError* /*error*/, void* /*data*/)
{
    ++coinWarnings;
}

class RealTimeSensorPauseTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        SoDB::init();
    }

    void SetUp() override
    {
        coinWarnings = 0;
        previousHandler = SoDebugError::getHandlerCallback();
        previousData = SoDebugError::getHandlerData();
        SoDebugError::setHandlerCallback(countCoinWarning, nullptr);
    }

    void TearDown() override
    {
        SoDebugError::setHandlerCallback(previousHandler, previousData);
        SoDB::enableRealTimeSensor(true);
    }

    SoErrorCB* previousHandler {};
    void* previousData {};
};

// The first activation arrives while Coin's sensor still runs; Coin's debug
// build warned "realtime sensor already on" at every FreeCAD start.
TEST_F(RealTimeSensorPauseTest, firstActivationLeavesTheRunningSensorAlone)
{
    Gui::MainWindowInternal::RealTimeSensorPause pause;

    pause.windowActivated();

    EXPECT_EQ(coinWarnings, 0);
}

TEST_F(RealTimeSensorPauseTest, pauseAndResumeRestoreTheInterval)
{
    Gui::MainWindowInternal::RealTimeSensorPause pause;
    const SbTime interval(1.0 / 25.0);
    SoDB::setRealTimeInterval(interval);

    pause.windowDeactivated();
    pause.windowDeactivated();
    SoDB::setRealTimeInterval(SbTime(1.0));
    pause.windowActivated();
    pause.windowActivated();

    EXPECT_EQ(SoDB::getRealTimeInterval(), interval);
    EXPECT_EQ(coinWarnings, 0);
}

}  // namespace
