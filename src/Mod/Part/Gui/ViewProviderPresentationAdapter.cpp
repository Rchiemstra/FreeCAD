// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationAdapter.h"

#include <Gui/ViewProvider.h>

bool PartGui::capturePartWorkbenchPresentationRenderBuffer(
    const Gui::ViewProvider& provider,
    const Gui::ViewProviderPresentationCaptureRequest& request,
    Gui::PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}
