// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationCapability.h"

#include "ViewProvider.h"

using namespace Gui;

const char* Gui::viewProviderPresentationClassificationName(
    ViewProviderPresentationClassification classification) noexcept
{
    switch (classification) {
        case ViewProviderPresentationClassification::Adapted:
            return "adapted";
        case ViewProviderPresentationClassification::Unsupported:
            return "unsupported";
    }
    return "unsupported";
}

ViewProviderPresentationClassification ViewProvider::presentationClassification() const
{
    return ViewProviderPresentationClassification::Unsupported;
}

bool ViewProvider::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest&,
    PresentationRenderBuffer&) const
{
    return false;
}

bool Gui::capturePartPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::captureMeshPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::capturePointsPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::captureSketchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}
