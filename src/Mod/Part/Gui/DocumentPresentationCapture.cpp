// SPDX-License-Identifier: LGPL-2.1-or-later

#include <Gui/PresentationCaptureRegistry.h>

#include <App/Application.h>
#include <App/DocumentObject.h>
#include <Base/Parameter.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/Tools.h>

#include "SoBrepEdgeSet.h"
#include "SoBrepFaceSet.h"
#include "SoBrepPointSet.h"
#include "ViewProviderExt.h"

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoNormal.h>

#include <utility>
#include <vector>

namespace PartGui
{
namespace
{

void appendCoinVec3Field(const SoMFVec3f& field, std::vector<float>& target)
{
    const int count = field.getNum();
    target.reserve(target.size() + static_cast<std::size_t>(count) * 3U);
    for (int index = 0; index < count; ++index) {
        const SbVec3f& value = field[index];
        target.push_back(value[0]);
        target.push_back(value[1]);
        target.push_back(value[2]);
    }
}

void appendCoinInt32Field(const SoMFInt32& field, std::vector<std::uint32_t>& target)
{
    const int count = field.getNum();
    target.reserve(target.size() + static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        target.push_back(static_cast<std::uint32_t>(field[index]));
    }
}

std::pair<double, double> defaultPartTessellationDeviation()
{
    ParameterGrp::handle hGrp =
        App::GetApplication().GetUserParameter().GetGroup("BaseApp")->GetGroup("Preferences")->GetGroup(
            "View");
    hGrp = hGrp->GetGroup("Geometry");
    const double deviation = hGrp->GetFloat("Deviation", 0.2);
    const double angularDeflection = hGrp->GetFloat("MeshAngularDeflection", 28.65);
    return {deviation, angularDeflection};
}

bool capturePartFeaturePresentationRenderBuffer(const App::DocumentObject& object,
                                                const std::string& stableObjectIdentity,
                                                Gui::PresentationRenderBuffer& buffer)
{
    const auto* feature = dynamic_cast<const Part::Feature*>(&object);
    if (!feature) {
        return false;
    }

    buffer = Gui::PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const TopoDS_Shape shape = feature->Shape.getValue();
    if (Part::Tools::isShapeEmpty(shape)) {
        return !stableObjectIdentity.empty();
    }

    const auto [deviation, angularDeflection] = defaultPartTessellationDeviation();

    auto* tempCoords = new SoCoordinate3;
    auto* tempFaces = new SoBrepFaceSet;
    auto* tempNorm = new SoNormal;
    auto* tempLines = new SoBrepEdgeSet;
    auto* tempNodes = new SoBrepPointSet;
    tempCoords->ref();
    tempFaces->ref();
    tempNorm->ref();
    tempLines->ref();
    tempNodes->ref();
    ViewProviderPartExt::setupCoinGeometry(
        shape,
        tempCoords,
        tempFaces,
        tempNorm,
        tempLines,
        tempNodes,
        deviation,
        angularDeflection,
        false);

    appendCoinVec3Field(tempCoords->point, buffer.vertices);
    appendCoinVec3Field(tempNorm->vector, buffer.normals);
    appendCoinInt32Field(tempFaces->coordIndex, buffer.indices);
    appendCoinInt32Field(tempFaces->partIndex, buffer.topology);
    tempNodes->unref();
    tempLines->unref();
    tempNorm->unref();
    tempFaces->unref();
    tempCoords->unref();
    return !buffer.vertices.empty() || !stableObjectIdentity.empty();
}

const Gui::DocumentPresentationCaptureRegistrar partPresentationCaptureRegistrar {
    "Part::Feature",
    capturePartFeaturePresentationRenderBuffer,
};

}  // namespace
}  // namespace PartGui
