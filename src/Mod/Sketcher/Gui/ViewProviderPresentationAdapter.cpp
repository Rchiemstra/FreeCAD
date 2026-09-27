// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationAdapter.h"

#include "ViewProviderSketch.h"

#include <Gui/ViewProviderPresentationCapability.h>

using namespace Gui;

bool SketcherGui::captureSketcherWorkbenchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

ViewProviderPresentationClassification ViewProviderSketch::presentationClassification() const
{
    return ViewProviderPresentationClassification::Adapted;
}

bool ViewProviderSketch::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer) const
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = request.stableObjectIdentity;
    // Sketch geometry remains on Coin edit nodes until a dedicated buffer path lands.
    return !request.stableObjectIdentity.empty();
}
