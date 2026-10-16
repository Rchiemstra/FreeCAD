// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Gui/PresentationCaptureRegistry.h>

#include <App/DocumentObject.h>
#include <Mod/Points/App/PointsFeature.h>
#include <Mod/Points/App/PropertyPointKernel.h>

namespace PointsGui
{
namespace
{

bool capturePointsFeaturePresentationRenderBuffer(const App::DocumentObject& object,
                                                  const std::string& stableObjectIdentity,
                                                  Gui::PresentationRenderBuffer& buffer)
{
    const auto* feature = dynamic_cast<const Points::Feature*>(&object);
    if (!feature) {
        return false;
    }

    buffer = Gui::PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const Points::PointKernel points = feature->Points.getValue();
    buffer.vertices.reserve(points.size() * 3U);
    for (const Base::Vector3d& point : points) {
        buffer.vertices.push_back(static_cast<float>(point.x));
        buffer.vertices.push_back(static_cast<float>(point.y));
        buffer.vertices.push_back(static_cast<float>(point.z));
    }
    return !buffer.vertices.empty() || !stableObjectIdentity.empty();
}

const Gui::DocumentPresentationCaptureRegistrar pointsPresentationCaptureRegistrar {
    "Points::Feature",
    capturePointsFeaturePresentationRenderBuffer,
};

}  // namespace
}  // namespace PointsGui
