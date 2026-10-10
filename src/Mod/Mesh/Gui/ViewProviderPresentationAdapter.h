// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Mesh/MeshGlobal.h>

namespace MeshGui
{

[[nodiscard]] MeshGuiExport bool captureMeshWorkbenchPresentationRenderBuffer(
    const Gui::ViewProvider& provider,
    const Gui::ViewProviderPresentationCaptureRequest& request,
    Gui::PresentationRenderBuffer& buffer);

}  // namespace MeshGui
