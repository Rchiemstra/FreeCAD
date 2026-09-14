// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cmath>

namespace Gui::View3DInventorViewerInternal
{

inline bool isUsablePickVolume(
    bool hasCamera,
    int viewportWidthPx,
    int viewportHeightPx,
    float volumeWidth,
    float volumeHeight,
    float volumeDepth
)
{
    return hasCamera && viewportWidthPx > 0 && viewportHeightPx > 0 && std::isfinite(volumeWidth)
        && std::isfinite(volumeHeight) && std::isfinite(volumeDepth) && volumeWidth > 0.0F
        && volumeHeight > 0.0F && volumeDepth > 0.0F;
}

template<typename RedrawRequest>
inline bool requestDetachedNavigationRedraw(
    bool eventProcessed,
    bool cameraNavigationActive,
    bool isLocationEvent,
    bool isDetachedView,
    RedrawRequest&& requestRedraw
)
{
    if (!eventProcessed || !cameraNavigationActive || !isLocationEvent || !isDetachedView) {
        return false;
    }

    requestRedraw();
    return true;
}

}  // namespace Gui::View3DInventorViewerInternal
