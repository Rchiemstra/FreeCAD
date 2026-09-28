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

ViewProviderPresentationClassification
SketcherGui::ViewProviderSketch::presentationClassification() const
{
    // Sketch geometry remains on Coin edit nodes; do not claim Adapted until a
    // real pointer-free buffer path exists.
    return ViewProviderPresentationClassification::Unsupported;
}

bool SketcherGui::ViewProviderSketch::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest&,
    PresentationRenderBuffer&) const
{
    return false;
}
