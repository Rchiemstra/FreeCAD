// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPresentationAdapter.h"

#include "ViewProvider.h"

#include <App/DocumentWouldBlock.h>
#include <Gui/ViewProviderPresentationCapability.h>

#include <Base/Vector3D.h>

#include <Mod/Mesh/App/MeshFeature.h>
#include <Mod/Mesh/App/Core/Elements.h>

using namespace Gui;

bool MeshGui::captureMeshWorkbenchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer)
{
    return provider.capturePresentationRenderBuffer(request, buffer);
}

ViewProviderPresentationClassification MeshGui::ViewProviderMesh::presentationClassification() const
{
    return ViewProviderPresentationClassification::Adapted;
}

bool MeshGui::ViewProviderMesh::capturePresentationRenderBuffer(
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer) const
{
    if (App::DocumentWouldBlock::isGuiThread()) {
        return false;
    }
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

    const MeshCore::MeshFacetArray& facets = kernel.GetFacets();
    buffer.indices.reserve(facets.size() * 3U);
    buffer.normals.assign(buffer.vertices.size(), 0.0F);
    for (const MeshCore::MeshFacet& facet : facets) {
        const std::uint32_t i0 = static_cast<std::uint32_t>(facet._aulPoints[0]);
        const std::uint32_t i1 = static_cast<std::uint32_t>(facet._aulPoints[1]);
        const std::uint32_t i2 = static_cast<std::uint32_t>(facet._aulPoints[2]);
        buffer.indices.push_back(i0);
        buffer.indices.push_back(i1);
        buffer.indices.push_back(i2);

        const Base::Vector3f& p0 = points[facet._aulPoints[0]];
        const Base::Vector3f& p1 = points[facet._aulPoints[1]];
        const Base::Vector3f& p2 = points[facet._aulPoints[2]];
        const Base::Vector3f normal = (p1 - p0).Cross(p2 - p0).Normalize();
        for (const auto index : {i0, i1, i2}) {
            const std::size_t offset = static_cast<std::size_t>(index) * 3U;
            if (offset + 2U < buffer.normals.size()) {
                buffer.normals[offset] += normal.x;
                buffer.normals[offset + 1U] += normal.y;
                buffer.normals[offset + 2U] += normal.z;
            }
        }
    }
    for (std::size_t index = 0; index + 2U < buffer.normals.size(); index += 3U) {
        Base::Vector3f normal(
            buffer.normals[index],
            buffer.normals[index + 1U],
            buffer.normals[index + 2U]);
        if (normal.Sqr() > 0.0F) {
            normal.Normalize();
            buffer.normals[index] = normal.x;
            buffer.normals[index + 1U] = normal.y;
            buffer.normals[index + 2U] = normal.z;
        }
    }
    return !buffer.vertices.empty() || !request.stableObjectIdentity.empty();
}
