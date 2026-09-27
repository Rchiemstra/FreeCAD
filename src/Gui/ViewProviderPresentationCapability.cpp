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
    // Dispatch only when the concrete provider already overrides capture.
    // Calling this from a default/base override that forwards here recurses.
    if (provider.presentationClassification()
        != ViewProviderPresentationClassification::Adapted) {
        buffer = PresentationRenderBuffer {};
        buffer.stableObjectIdentity = request.stableObjectIdentity;
        return false;
    }
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::captureMeshPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    if (provider.presentationClassification()
        != ViewProviderPresentationClassification::Adapted) {
        buffer = PresentationRenderBuffer {};
        buffer.stableObjectIdentity = request.stableObjectIdentity;
        return false;
    }
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::capturePointsPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    if (provider.presentationClassification()
        != ViewProviderPresentationClassification::Adapted) {
        buffer = PresentationRenderBuffer {};
        buffer.stableObjectIdentity = request.stableObjectIdentity;
        return false;
    }
    return provider.capturePresentationRenderBuffer(request, buffer);
}

bool Gui::captureSketchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    if (provider.presentationClassification()
        != ViewProviderPresentationClassification::Adapted) {
        buffer = PresentationRenderBuffer {};
        buffer.stableObjectIdentity = request.stableObjectIdentity;
        return false;
    }
    return provider.capturePresentationRenderBuffer(request, buffer);
}
