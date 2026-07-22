// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

namespace Gui::View3DInventorViewerInternal
{

template<typename RedrawRequest>
inline bool requestDetachedNavigationRedraw(
    bool eventProcessed,
    bool mouseButtonPressed,
    bool isLocationEvent,
    bool isDetachedView,
    RedrawRequest&& requestRedraw
)
{
    if (!eventProcessed || !mouseButtonPressed || !isLocationEvent || !isDetachedView) {
        return false;
    }

    requestRedraw();
    return true;
}

}  // namespace Gui::View3DInventorViewerInternal
