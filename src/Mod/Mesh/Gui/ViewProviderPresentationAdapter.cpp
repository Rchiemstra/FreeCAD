// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationAdapter.h"

#include "ViewProvider.h"

#include <Gui/ViewProviderPresentationCapability.h>

#include <Mod/Mesh/App/MeshFeature.h>

using namespace Gui;

bool MeshGui::captureMeshWorkbenchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

ViewProviderPresentationClassification ViewProviderMesh::presentationClassification() const
{
    return ViewProviderPresentationClassification::Adapted;
}

bool ViewProviderMesh::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer) const
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = request.stableObjectIdentity;
    if (!pcObject) {
        return !request.stableObjectIdentity.empty();
    }

    const auto* meshFeature = dynamic_cast<const Mesh::Feature*>(pcObject);
    if (!meshFeature) {
        return !request.stableObjectIdentity.empty();
    }

    const Mesh::MeshObject& mesh = meshFeature->Mesh.getValue();
    const MeshCore::MeshKernel& kernel = mesh.getKernel();
    const MeshCore::MeshPointArray& points = kernel.GetPoints();
    buffer.vertices.reserve(points.size() * 3U);
    for (const MeshCore::MeshPoint& point : points) {
        buffer.vertices.push_back(static_cast<float>(point.x));
        buffer.vertices.push_back(static_cast<float>(point.y));
        buffer.vertices.push_back(static_cast<float>(point.z));
    }
    return !buffer.vertices.empty() || !request.stableObjectIdentity.empty();
}
