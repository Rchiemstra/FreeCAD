// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationCapture.h"

#include "Application.h"
#include "Document.h"
#include "GuiPythonGate.h"
#include "MainWindow.h"
#include "Tree.h"

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentPresentationBoundary.h>
#include <App/PropertyStandard.h>
#include <Base/Parameter.h>

#include <Mod/Mesh/App/MeshFeature.h>
#include <Mod/Mesh/App/Core/Elements.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/Tools.h>
#include <Mod/Part/Gui/ViewProviderExt.h>
#include <Mod/Points/App/PointsFeature.h>
#include <Mod/Points/App/PropertyPointKernel.h>

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoNormal.h>

#include <Gui/Inventor/SoBrepEdgeSet.h>
#include <Gui/Inventor/SoBrepFaceSet.h>
#include <Gui/Inventor/SoBrepPointSet.h>

#include <QCoreApplication>
#include <QMetaObject>

#include <utility>

using namespace Gui;

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

bool capturePartFeaturePresentationRenderBuffer(
    const Part::Feature& feature,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer)
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const TopoDS_Shape shape = feature.Shape.getValue();
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
    PartGui::ViewProviderPartExt::setupCoinGeometry(
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

bool captureMeshFeaturePresentationRenderBuffer(
    const Mesh::Feature& feature,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer)
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const Mesh::MeshObject& mesh = feature.Mesh.getValue();
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

bool capturePointsFeaturePresentationRenderBuffer(
    const Points::Feature& feature,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer)
{
    buffer = PresentationRenderBuffer {};
    buffer.stableObjectIdentity = stableObjectIdentity;

    const Points::PointKernel points = feature.Points.getValue();
    buffer.vertices.reserve(points.size() * 3U);
    for (const Base::Vector3d& point : points) {
        buffer.vertices.push_back(static_cast<float>(point.x));
        buffer.vertices.push_back(static_cast<float>(point.y));
        buffer.vertices.push_back(static_cast<float>(point.z));
    }
    return !buffer.vertices.empty() || !stableObjectIdentity.empty();
}

bool objectSupportsDocumentThreadPresentationCapture(const App::DocumentObject& object)
{
    return object.isDerivedFrom<Part::Feature>() || object.isDerivedFrom<Mesh::Feature>()
        || object.isDerivedFrom<Points::Feature>();
}

}  // namespace

bool Gui::captureDocumentObjectPresentationRenderBuffer(
    const App::Document& document,
    const App::DocumentObject& object,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer)
{
    if (!objectSupportsDocumentThreadPresentationCapture(object)) {
        return false;
    }
    const auto featureAdmission = GuiPythonGate::verifyFeaturePythonExecution(object);
    if (!featureAdmission.executed()) {
        return false;
    }
    if (const auto* partFeature = dynamic_cast<const Part::Feature*>(&object)) {
        return capturePartFeaturePresentationRenderBuffer(*partFeature, stableObjectIdentity, buffer);
    }
    if (const auto* meshFeature = dynamic_cast<const Mesh::Feature*>(&object)) {
        return captureMeshFeaturePresentationRenderBuffer(*meshFeature, stableObjectIdentity, buffer);
    }
    if (const auto* pointsFeature = dynamic_cast<const Points::Feature*>(&object)) {
        return capturePointsFeaturePresentationRenderBuffer(
            *pointsFeature,
            stableObjectIdentity,
            buffer);
    }
    return false;
}

PresentationDelta Gui::buildPresentationDeltaOnDocumentThread(const App::Document& document)
{
    const auto identity = document.collaborationIdentity();

    PresentationDelta delta;
    delta.revision.documentInstanceId = identity.instanceId;
    delta.revision.lifecycleEpoch = identity.lifecycleEpoch;
    delta.status.revision = delta.revision;
    delta.status.state = DocumentPresentationState::Applying;
    delta.status.statusMessage =
        "presentation revision captured on document thread at recompute boundary";

    // Sequence is assigned on the GUI thread when the packet is enqueued.
    delta.revision.sequence = 0;
    delta.revision.sourceModelRevision =
        document.collaborationRevisions().current(App::DocumentRevisionKey::documentStructure());
    delta.status.revision = delta.revision;

    const std::vector<App::DocumentObject*> objects = document.getObjects();
    for (const App::DocumentObject* object : objects) {
        if (!object) {
            continue;
        }
        const std::string stableIdentity = document.collaborationObjectIdentity(*object);

        PresentationTreeNode node;
        node.stableObjectIdentity = stableIdentity;
        node.label = object->Label.getValue();
        node.visible = object->Visibility.getValue();
        delta.tree.push_back(std::move(node));

        const auto captureDisplayProperty =
            [&](const char* propertyName, const std::string& displayValue) {
                if (propertyName == nullptr || displayValue.empty()) {
                    return;
                }
                PresentationPropertyValue propertyValue;
                propertyValue.stableObjectIdentity = stableIdentity;
                propertyValue.propertyName = propertyName;
                propertyValue.displayValue = displayValue;
                delta.properties.push_back(std::move(propertyValue));
            };
        captureDisplayProperty("Label", object->Label.getValue());
        captureDisplayProperty("Label2", object->Label2.getValue());
        captureDisplayProperty(
            "Visibility",
            object->Visibility.getValue() ? std::string("true") : std::string("false"));

        if (!objectSupportsDocumentThreadPresentationCapture(*object)) {
            continue;
        }

        PresentationRenderBuffer buffer;
        if (captureDocumentObjectPresentationRenderBuffer(
                document,
                *object,
                stableIdentity,
                buffer)) {
            delta.renderBuffers.push_back(std::move(buffer));
        }
    }

    return delta;
}

void Gui::installDocumentPresentationBoundaryHook()
{
    App::setDocumentPresentationBoundaryCallback([](App::Document& appDocument) {
        PresentationDelta delta = buildPresentationDeltaOnDocumentThread(appDocument);
        const std::string documentName = appDocument.getName();
        if (!QCoreApplication::instance()) {
            return;
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [documentName, delta = std::move(delta)]() mutable {
                auto* applicationDocument = App::GetApplication().getDocument(documentName.c_str());
                if (!applicationDocument || !Application::Instance) {
                    return;
                }
                Gui::Document* guiDocument =
                    Application::Instance->getDocument(applicationDocument);
                if (!guiDocument || guiDocument->isAboutToClose()) {
                    return;
                }
                auto& cache = guiDocument->presentationCache();
                delta.revision.sequence = cache.committedSequence() + 1;
                if (delta.revision.sourceModelRevision == 0) {
                    delta.revision.sourceModelRevision = delta.revision.sequence;
                }
                delta.status.revision = delta.revision;
                guiDocument->enqueuePresentationDelta(std::move(delta));
                if (auto* window = getMainWindow()) {
                    window->updateActions();
                }
                TreeWidget::updateStatus();
            },
            Qt::QueuedConnection);
    });
}
