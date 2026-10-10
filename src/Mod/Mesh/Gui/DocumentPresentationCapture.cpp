// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Gui/PresentationCaptureRegistry.h>

#include <App/DocumentObject.h>
#include <Mod/Mesh/App/Core/Elements.h>
#include <Mod/Mesh/App/MeshFeature.h>

namespace MeshGui
{
namespace
{

bool captureMeshFeaturePresentationRenderBuffer(const App::DocumentObject& object,
                                                const std::string& stableObjectIdentity,
                                                Gui::PresentationRenderBuffer& buffer)
{
    const auto* feature = dynamic_cast<const Mesh::Feature*>(&object);
    if (!feature) {
        return false;
    }

    buffer = Gui::PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const Mesh::MeshObject& mesh = feature->Mesh.getValue();
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
    return !buffer.vertices.empty() || !stableObjectIdentity.empty();
}

const Gui::DocumentPresentationCaptureRegistrar meshPresentationCaptureRegistrar {
    "Mesh::Feature",
    captureMeshFeaturePresentationRenderBuffer,
};

}  // namespace
}  // namespace MeshGui
