// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationAdapter.h"

#include "ViewProvider.h"

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Points/App/PropertyPointKernel.h>

using namespace Gui;

bool PointsGui::capturePointsWorkbenchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

ViewProviderPresentationClassification ViewProviderPoints::presentationClassification() const
{
    return ViewProviderPresentationClassification::Adapted;
}

bool ViewProviderPoints::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer) const
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = request.stableObjectIdentity;
    if (!pcObject) {
        return !request.stableObjectIdentity.empty();
    }

    const auto* pointsProperty = dynamic_cast<Points::Feature*>(pcObject);
    if (!pointsProperty) {
        return !request.stableObjectIdentity.empty();
    }

    // Capture runs at the stable recompute boundary on the GUI thread after the
    // App model is quiescent; values are copied into locals before buffer fill.
    const Points::PointKernel points = pointsProperty->Points.getValue();
    buffer.vertices.reserve(points.size() * 3U);
    for (const Base::Vector3d& point : points) {
        buffer.vertices.push_back(static_cast<float>(point.x));
        buffer.vertices.push_back(static_cast<float>(point.y));
        buffer.vertices.push_back(static_cast<float>(point.z));
    }
    return !buffer.vertices.empty() || !request.stableObjectIdentity.empty();
}
