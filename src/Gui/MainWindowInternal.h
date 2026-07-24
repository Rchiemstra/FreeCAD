// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

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

}  // namespace Gui::MainWindowInternal
