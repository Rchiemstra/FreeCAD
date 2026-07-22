// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

namespace Gui::View3DInventorViewerInternal
{

template<typename UpdateRequest>
inline bool requestDetachedNavigationUpdate(
    bool eventProcessed,
    bool mouseButtonPressed,
    bool isLocationEvent,
    bool isDetachedView,
    UpdateRequest&& requestUpdate
)
{
    if (!eventProcessed || !mouseButtonPressed || !isLocationEvent || !isDetachedView) {
        return false;
    }

    requestUpdate();
    return true;
}

}  // namespace Gui::View3DInventorViewerInternal
