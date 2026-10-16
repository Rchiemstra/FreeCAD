// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Points/PointsGlobal.h>

namespace PointsGui
{

[[nodiscard]] PointsGuiExport bool capturePointsWorkbenchPresentationRenderBuffer(
    const Gui::ViewProvider& provider,
    const Gui::ViewProviderPresentationCaptureRequest& request,
    Gui::PresentationRenderBuffer& buffer);

}  // namespace PointsGui
