// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Sketcher/SketcherGlobal.h>

namespace SketcherGui
{

[[nodiscard]] SketcherGuiExport bool captureSketcherWorkbenchPresentationRenderBuffer(
    const Gui::ViewProvider& provider,
    const Gui::ViewProviderPresentationCaptureRequest& request,
    Gui::PresentationRenderBuffer& buffer);

}  // namespace SketcherGui
