// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationCapture.h"

#include "Application.h"
#include "Document.h"
#include "GuiPythonGate.h"
#include "MainWindow.h"
#include "PresentationCaptureRegistry.h"
#include "Tree.h"
#include "ViewProviderPresentationCapability.h"

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentPresentationBoundary.h>
#include <App/PropertyStandard.h>

#include <QCoreApplication>
#include <QMetaObject>

#include <utility>

using namespace Gui;

bool Gui::captureDocumentObjectPresentationRenderBuffer(
    const App::Document& document,
    const App::DocumentObject& object,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer)
{
    const auto featureAdmission = GuiPythonGate::verifyFeaturePythonExecution(object);
    if (!featureAdmission.executed()) {
        return false;
    }
    return PresentationCaptureRegistry::tryCapture(object, stableObjectIdentity, buffer);
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

        if (!PresentationCaptureRegistry::hasCapture(*object)) {
            continue;
        }

        PresentationRenderBuffer buffer;
        if (captureDocumentObjectPresentationRenderBuffer(
                document,
                *object,
                stableIdentity,
                buffer)) {
            delta.renderBuffers.push_back(std::move(buffer));
            continue;
        }

        PresentationPropertyValue captureStatus;
        captureStatus.stableObjectIdentity = stableIdentity;
        captureStatus.propertyName = "PresentationCapture";
        captureStatus.displayValue = viewProviderPresentationClassificationName(
            ViewProviderPresentationClassification::Unsupported);
        captureStatus.statusAnnotation =
            "document-thread presentation capture missing or failed";
        delta.properties.push_back(std::move(captureStatus));
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
