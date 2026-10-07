// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Inventor/SbTime.h>
#include <Inventor/SoDB.h>

#include <QPoint>
#include <QRect>

namespace Gui::MainWindowInternal
{

inline bool isTabDetachGesture(
    const QPoint& pressPosition,
    const QPoint& releasePosition,
    const QRect& tabBarRect,
    int startDragDistance
)
{
    return startDragDistance >= 0 && !tabBarRect.contains(releasePosition)
        && (releasePosition - pressPosition).manhattanLength() >= startDragDistance;
}

/// Pauses Coin's realtime sensor while the main window is inactive.
///
/// Only real transitions touch the sensor: Coin warns when it is switched to
/// the state it is already in, and the first activation arrives while the
/// sensor still runs.
class RealTimeSensorPause
{
public:
    void windowActivated()
    {
        if (!paused) {
            return;
        }
        paused = false;
        SoDB::enableRealTimeSensor(true);
        SoDB::setRealTimeInterval(savedInterval);
    }

    void windowDeactivated()
    {
        if (paused) {
            return;
        }
        paused = true;
        savedInterval = SoDB::getRealTimeInterval();
        SoDB::enableRealTimeSensor(false);
    }

private:
    SbTime savedInterval;
    bool paused {false};
};

}  // namespace Gui::MainWindowInternal
