// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

/** Part workbench pointer-free presentation capture entry points (Wave 3). */

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Part/PartGlobal.h>

namespace PartGui
{

[[nodiscard]] PartGuiExport bool capturePartWorkbenchPresentationRenderBuffer(
    const Gui::ViewProvider& provider,
    const Gui::ViewProviderPresentationCaptureRequest& request,
    Gui::PresentationRenderBuffer& buffer);

}  // namespace PartGui
