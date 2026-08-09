// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include <QApplication>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWidget>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <Inventor/SoRenderManager.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>
#include <Base/Interpreter.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <src/App/InitApplication.h>

namespace App::Internal
{

class DocumentCollaborationConcurrencyTestAccess
{
public:
    static std::recursive_mutex& commitMutex(App::Document& document) noexcept
    {
        return document.collaborationCommitMutex();
    }
};

}  // namespace App::Internal

namespace
{

void initializeDomainGui()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char executable[] = "Gui_tests_run";
    static char* argv[] = {executable, nullptr};
    if (!QApplication::instance()) {
        new QApplication(argc, argv);
    }
    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
    if (!Gui::MainWindow::getInstance()) {
        new Gui::MainWindow();
    }
}

class CollaborationDomainIntegrationTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeDomainGui();
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        documentName = App::GetApplication().getUniqueDocumentName("collaborationDomains");
        document = App::GetApplication().newDocument(
            documentName.c_str(), "collaboration domains", flags);
        ASSERT_NE(document, nullptr);
        object = document->addObject("App::FeatureTest", "Target");
        ASSERT_NE(object, nullptr);
        document->recompute();
        guiDocument = Gui::Application::Instance->getDocument(document);
        ASSERT_NE(guiDocument, nullptr);
        guiDocument->setModified(false);
    }

    void TearDown() override
    {
        if (document && App::GetApplication().getDocument(documentName.c_str())) {
            App::GetApplication().closeDocument(documentName.c_str());
            QApplication::processEvents();
        }
    }

    App::Document* document {nullptr};
    App::DocumentObject* object {nullptr};
    Gui::Document* guiDocument {nullptr};
    std::string documentName;
};

}  // namespace

TEST_F(CollaborationDomainIntegrationTest,
       explicitPresentationCommitUsesAppBoundaryPublishesOnlyPresentation)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey presentationKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const auto appKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto appBefore = document->collaborationRevisions().current(appKey);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture({appKey});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({presentationKey});
    request.presentationWrites = {presentationKey};

    const bool appBeforeVisibility = object->Visibility.getValue();
    const bool guiBeforeVisibility = provider->Visibility.getValue();
    const bool targetVisibility = !appBeforeVisibility;
    int appObserverCalls = 0;
    bool observerSawPublishedPresentation = false;
    int guiObserverCalls = 0;
    bool everyGuiObserverSawPublishedPresentation = true;
    auto observer = document->signalChangedObject.connect(
        [&](const App::DocumentObject& changed, const App::Property& property) {
            if (&changed == object && std::string_view(property.getName()) == "Visibility") {
                ++appObserverCalls;
                observerSawPublishedPresentation =
                    guiDocument->sharedPresentationRevisions().current(presentationKey) == 1;
            }
        });
    auto guiObserver = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property& property) {
            if (&changed == provider && std::string_view(property.getName()) == "Visibility") {
                ++guiObserverCalls;
                everyGuiObserverSawPublishedPresentation =
                    everyGuiObserverSawPublishedPresentation
                    && guiDocument->sharedPresentationRevisions().current(presentationKey) == 1;
            }
        });
    bool competingThreadAcquiredMutex = false;
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        std::thread competing([&] {
            auto& mutex = App::Internal::DocumentCollaborationConcurrencyTestAccess::commitMutex(
                *document);
            if (mutex.try_lock()) {
                competingThreadAcquiredMutex = true;
                mutex.unlock();
            }
        });
        competing.join();
        object->Visibility.setValue(targetVisibility);
        EXPECT_EQ(appObserverCalls, 0);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(targetVisibility);
        EXPECT_EQ(appObserverCalls, 0);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [&] {
        return Gui::SharedPresentationStepResult {
            object->Visibility.getValue() == targetVisibility
                && provider->Visibility.getValue() == targetVisibility
                && appObserverCalls == 0,
            {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBeforeVisibility);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBeforeVisibility);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));
    ASSERT_TRUE(result.committed()) << result.diagnostic;
    ASSERT_TRUE(result.publishedPresentation.has_value());
    EXPECT_EQ(object->Visibility.getValue(), targetVisibility);
    EXPECT_EQ(provider->Visibility.getValue(), targetVisibility);
    EXPECT_FALSE(competingThreadAcquiredMutex);
    EXPECT_EQ(document->collaborationRevisions().current(appKey), appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(presentationKey), 1U);
    EXPECT_GE(appObserverCalls, 1);
    EXPECT_TRUE(observerSawPublishedPresentation);
    EXPECT_GE(guiObserverCalls, 1);
    EXPECT_TRUE(everyGuiObserverSawPublishedPresentation);
    observer.disconnect();
    guiObserver.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       failedRealPresentationCommitRestoresAppAndGuiWithoutObserverOrRevision)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey presentationKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const auto appKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto appRevisionBefore = document->collaborationRevisions().current(appKey);
    const auto presentationBefore =
        guiDocument->sharedPresentationRevisions().current(presentationKey);
    const bool appBefore = object->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();
    const bool target = !appBefore;
    int appObserverCalls = 0;
    int guiObserverCalls = 0;
    auto observer = document->signalChangedObject.connect(
        [&](const App::DocumentObject& changed, const App::Property& property) {
            if (&changed == object && std::string_view(property.getName()) == "Visibility") {
                ++appObserverCalls;
            }
        });
    auto guiObserver = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property& property) {
            if (&changed == provider && std::string_view(property.getName()) == "Visibility") {
                ++guiObserverCalls;
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture({appKey});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({presentationKey});
    request.presentationWrites = {presentationKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(target);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(target);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {false, "injected failure"};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));
    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::PostconditionFailed);
    EXPECT_EQ(object->Visibility.getValue(), appBefore);
    EXPECT_EQ(provider->Visibility.getValue(), guiBefore);
    EXPECT_EQ(appObserverCalls, 0);
    EXPECT_EQ(guiObserverCalls, 0);
    EXPECT_EQ(document->collaborationRevisions().current(appKey), appRevisionBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(presentationKey),
              presentationBefore);
    observer.disconnect();
    guiObserver.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       presentationBoundaryRejectsAndRollsBackModelMutationWithoutPublication)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey presentationKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const auto modelKey =
        App::DocumentRevisionKey::objectModel(object->getNameInDocument());
    const auto propertyKey = App::DocumentRevisionKey::objectProperty(
        object->getNameInDocument(), "Label");
    const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
    const auto appBefore =
        document->collaborationRevisions().capture({modelKey, propertyKey, wildcard});
    const auto presentationBefore =
        guiDocument->sharedPresentationRevisions().current(presentationKey);
    const std::string labelBefore = object->Label.getStrValue();
    const bool guiBefore = provider->Visibility.getValue();
    int appObserverCalls = 0;
    int guiObserverCalls = 0;
    auto appObserver = document->signalChangedObject.connect(
        [&](const App::DocumentObject& changed, const App::Property&) {
            if (&changed == object) {
                ++appObserverCalls;
            }
        });
    auto guiObserver = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property&) {
            if (&changed == provider) {
                ++guiObserverCalls;
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions =
        document->collaborationRevisions().capture({modelKey, propertyKey, wildcard});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({presentationKey});
    request.presentationWrites = {presentationKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Label.setValue("forbidden-model-write");
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(!guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Label.setValue(labelBefore.c_str());
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::AppCommitFailed)
        << result.diagnostic;
    EXPECT_EQ(object->Label.getStrValue(), labelBefore);
    EXPECT_EQ(provider->Visibility.getValue(), guiBefore);
    EXPECT_EQ(document->collaborationRevisions().capture({modelKey, propertyKey, wildcard}),
              appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(presentationKey),
              presentationBefore);
    EXPECT_EQ(appObserverCalls, 0);
    EXPECT_EQ(guiObserverCalls, 0);
    appObserver.disconnect();
    guiObserver.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       presentationBoundaryRejectsVisibilityForUndeclaredStableIdentity)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* other = document->addObject("App::FeatureTest", "OtherPresentationTarget");
    ASSERT_NE(other, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey declaredKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey undeclaredKey {
        document->collaborationObjectIdentity(*other), "Visibility"};
    const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
    const auto appBefore = document->collaborationRevisions().capture({wildcard});
    const auto declaredBefore =
        guiDocument->sharedPresentationRevisions().current(declaredKey);
    const auto undeclaredBefore =
        guiDocument->sharedPresentationRevisions().current(undeclaredKey);
    const bool otherBefore = other->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions =
        document->collaborationRevisions().capture({wildcard});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({declaredKey});
    request.presentationWrites = {declaredKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        other->Visibility.setValue(!otherBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(!guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        other->Visibility.setValue(otherBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::AppCommitFailed)
        << result.diagnostic;
    EXPECT_EQ(other->Visibility.getValue(), otherBefore);
    EXPECT_EQ(provider->Visibility.getValue(), guiBefore);
    EXPECT_EQ(document->collaborationRevisions().capture({wildcard}), appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(declaredKey),
              declaredBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(undeclaredKey),
              undeclaredBefore);
}

TEST_F(CollaborationDomainIntegrationTest,
       presentationBoundaryRejectsForeignDocumentMutationBeforeItChanges)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string foreignName =
        App::GetApplication().getUniqueDocumentName("collaborationForeignDomain");
    auto* foreignDocument = App::GetApplication().newDocument(
        foreignName.c_str(), "foreign collaboration domain", flags);
    ASSERT_NE(foreignDocument, nullptr);
    auto* foreignObject = foreignDocument->addObject("App::FeatureTest", "ForeignTarget");
    ASSERT_NE(foreignObject, nullptr);
    foreignObject->Label.setValue("ForeignBefore");
    foreignDocument->recompute();

    const auto closeForeign = qScopeGuard([&] {
        if (App::GetApplication().getDocument(foreignName.c_str())) {
            App::GetApplication().closeDocument(foreignName.c_str());
            QApplication::processEvents();
        }
    });
    const auto foreignKey = App::DocumentRevisionKey::objectProperty(
        foreignObject->getNameInDocument(), "Label");
    const auto foreignWildcard = App::DocumentRevisionKey::unknownModelMutation();
    const auto foreignRevisionsBefore = foreignDocument->collaborationRevisions().capture(
        {foreignKey, foreignWildcard});
    const std::string foreignLabelBefore = foreignObject->Label.getStrValue();

    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey presentationKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const auto presentationBefore =
        guiDocument->sharedPresentationRevisions().current(presentationKey);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({presentationKey});
    request.presentationWrites = {presentationKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        foreignObject->Label.setValue("MustNotEscape");
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::AppApplyFailed)
        << result.diagnostic;
    EXPECT_NE(result.diagnostic.find("cross-document mutation"), std::string::npos);
    EXPECT_EQ(foreignObject->Label.getStrValue(), foreignLabelBefore);
    EXPECT_EQ(foreignDocument->collaborationRevisions().capture(
                  {foreignKey, foreignWildcard}),
              foreignRevisionsBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(presentationKey),
              presentationBefore);
}

TEST_F(CollaborationDomainIntegrationTest,
       presentationBoundaryRejectsUndeclaredGuiPropertyWrite)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* other = document->addObject("App::FeatureTest", "OtherGuiPresentationTarget");
    ASSERT_NE(other, nullptr);
    auto* otherProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(other));
    ASSERT_NE(otherProvider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey declaredKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey undeclaredKey {
        document->collaborationObjectIdentity(*other), "Visibility"};
    const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
    const auto appBefore = document->collaborationRevisions().capture({wildcard});
    const auto declaredBefore =
        guiDocument->sharedPresentationRevisions().current(declaredKey);
    const auto undeclaredBefore =
        guiDocument->sharedPresentationRevisions().current(undeclaredKey);
    const bool appBeforeVisibility = object->Visibility.getValue();
    const bool guiBeforeVisibility = otherProvider->Visibility.getValue();

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions =
        document->collaborationRevisions().capture({wildcard});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({declaredKey});
    request.presentationWrites = {declaredKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(!appBeforeVisibility);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        otherProvider->Visibility.setValue(!guiBeforeVisibility);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        if (otherProvider->Visibility.getValue() != guiBeforeVisibility) {
            otherProvider->Visibility.setValue(guiBeforeVisibility);
        }
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBeforeVisibility);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::GuiApplyFailed)
        << result.diagnostic;
    EXPECT_EQ(object->Visibility.getValue(), appBeforeVisibility);
    EXPECT_EQ(otherProvider->Visibility.getValue(), guiBeforeVisibility);
    EXPECT_EQ(document->collaborationRevisions().capture({wildcard}), appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(declaredKey),
              declaredBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(undeclaredKey),
              undeclaredBefore);
}

TEST_F(CollaborationDomainIntegrationTest,
       deferredPresentationObserverCannotClosePinnedDocument)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const bool appBefore = object->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();
    const bool target = !appBefore;
    bool closeAttempted = false;
    bool closeSucceeded = true;
    auto observer = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property& property) {
            if (!closeAttempted && &changed == provider
                && std::string_view(property.getName()) == "Visibility") {
                closeAttempted = true;
                closeSucceeded =
                    App::GetApplication().closeDocument(documentName.c_str());
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::unknownModelMutation()});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(target);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(target);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(closeAttempted);
    EXPECT_FALSE(closeSucceeded);
    EXPECT_EQ(App::GetApplication().getDocument(documentName.c_str()), document);
    observer.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       AppObserverGuiMutationPublishesItsOwnPresentationRevision)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* other = document->addObject("App::FeatureTest", "AppObserverGuiTarget");
    ASSERT_NE(other, nullptr);
    auto* otherProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(other));
    ASSERT_NE(otherProvider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey observerKey {
        document->collaborationObjectIdentity(*other), "Visibility"};
    const auto keyBefore = guiDocument->sharedPresentationRevisions().current(key);
    const auto observerKeyBefore =
        guiDocument->sharedPresentationRevisions().current(observerKey);
    const bool appBefore = object->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();
    const bool observerBefore = otherProvider->Visibility.getValue();
    bool observerMutated = false;
    bool removalRejected = false;
    auto observer = document->signalChangedObject.connect(
        [&](const App::DocumentObject& changed, const App::Property& property) {
            if (!observerMutated && &changed == object
                && std::string_view(property.getName()) == "Visibility") {
                observerMutated = true;
                otherProvider->Visibility.setValue(!observerBefore);
                try {
                    document->removeObject(other->getNameInDocument());
                }
                catch (const Base::Exception&) {
                    removalRejected = true;
                }
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::unknownModelMutation()});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(!appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(!guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(observerMutated);
    EXPECT_TRUE(removalRejected);
    EXPECT_EQ(document->getObject("AppObserverGuiTarget"), other);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key), keyBefore + 1);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(observerKey),
              observerKeyBefore + 1);
    observer.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       GuiObserverNestedMutationPublishesItsOwnPresentationRevision)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* other = document->addObject("App::FeatureTest", "GuiObserverNestedTarget");
    ASSERT_NE(other, nullptr);
    auto* otherProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(other));
    ASSERT_NE(otherProvider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey observerKey {
        document->collaborationObjectIdentity(*other), "Visibility"};
    const auto keyBefore = guiDocument->sharedPresentationRevisions().current(key);
    const auto observerKeyBefore =
        guiDocument->sharedPresentationRevisions().current(observerKey);
    const bool appBefore = object->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();
    const bool observerBefore = otherProvider->Visibility.getValue();
    bool observerMutated = false;
    bool removalRejected = false;
    auto observer = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property& property) {
            if (!observerMutated && &changed == provider
                && std::string_view(property.getName()) == "Visibility") {
                observerMutated = true;
                otherProvider->Visibility.setValue(!observerBefore);
                try {
                    document->removeObject(other->getNameInDocument());
                }
                catch (const Base::Exception&) {
                    removalRejected = true;
                }
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::unknownModelMutation()});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(!appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(!guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(observerMutated);
    EXPECT_TRUE(removalRejected);
    EXPECT_EQ(document->getObject("GuiObserverNestedTarget"), other);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key), keyBefore + 1);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(observerKey),
              observerKeyBefore + 1);
    observer.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       GuiReplayObserverCannotDeletePropertyOwnedByLaterQueuedRecord)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* other = document->addObject("App::FeatureTest", "QueuedGuiPropertyTarget");
    ASSERT_NE(other, nullptr);
    auto* otherProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(other));
    ASSERT_NE(otherProvider, nullptr);
    auto* queuedProperty = dynamic_cast<App::PropertyBool*>(
        otherProvider->addDynamicProperty(
            "App::PropertyBool", "QueuedPresentation"));
    ASSERT_NE(queuedProperty, nullptr);

    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey primaryKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey queuedKey {
        document->collaborationObjectIdentity(*other), "QueuedPresentation"};
    const auto primaryBefore =
        guiDocument->sharedPresentationRevisions().current(primaryKey);
    const auto queuedBefore =
        guiDocument->sharedPresentationRevisions().current(queuedKey);
    const bool appBefore = object->Visibility.getValue();
    const bool guiBefore = provider->Visibility.getValue();
    bool removalAttempted = false;
    bool removalRejected = false;
    bool renameRejected = false;
    bool metadataChangeRejected = false;
    int queuedNotifications = 0;
    auto observer = Gui::Application::Instance->signalChangedObject.connect(
        [&](const Gui::ViewProvider& changed, const App::Property& property) {
            if (!removalAttempted && &changed == provider
                && std::string_view(property.getName()) == "Visibility") {
                removalAttempted = true;
                try {
                    static_cast<void>(
                        otherProvider->removeDynamicProperty("QueuedPresentation"));
                }
                catch (const Base::Exception&) {
                    removalRejected = true;
                }
                try {
                    static_cast<void>(otherProvider->renameDynamicProperty(
                        queuedProperty, "RenamedDuringReplay"));
                }
                catch (const Base::Exception&) {
                    renameRejected = true;
                }
                try {
                    static_cast<void>(otherProvider->changeDynamicProperty(
                        queuedProperty, "ChangedDuringReplay", "must not change"));
                }
                catch (const Base::Exception&) {
                    metadataChangeRejected = true;
                }
            }
            if (&changed == otherProvider
                && std::string_view(property.getName()) == "QueuedPresentation") {
                ++queuedNotifications;
            }
        });

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedAppRevisions = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::unknownModelMutation()});
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({primaryKey, queuedKey});
    request.presentationWrites = {primaryKey, queuedKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        object->Visibility.setValue(!appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->Visibility.setValue(!guiBefore);
        queuedProperty->setValue(true);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->Visibility.setValue(guiBefore);
        queuedProperty->setValue(false);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [&] {
        object->Visibility.setValue(appBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(removalAttempted);
    EXPECT_TRUE(removalRejected);
    EXPECT_TRUE(renameRejected);
    EXPECT_TRUE(metadataChangeRejected);
    EXPECT_EQ(otherProvider->getDynamicPropertyByName("QueuedPresentation"),
              queuedProperty);
    EXPECT_EQ(otherProvider->getDynamicPropertyByName("RenamedDuringReplay"),
              nullptr);
    EXPECT_GE(queuedNotifications, 1);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(primaryKey),
              primaryBefore + 1);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(queuedKey),
              queuedBefore + 1);
    observer.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest,
       ordinaryVisibilityPublishesPresentationWithoutModelRevision)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const auto model = App::DocumentRevisionKey::objectModel(object->getNameInDocument());
    const auto wildcard = App::DocumentRevisionKey::unknownModelMutation();
    const auto appBefore = document->collaborationRevisions().capture({model, wildcard});
    const auto presentationBefore = guiDocument->sharedPresentationRevisions().current(key);

    provider->Visibility.setValue(!provider->Visibility.getValue());

    EXPECT_EQ(document->collaborationRevisions().capture({model, wildcard}), appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              presentationBefore + 1);

    object->Visibility.setValue(!object->Visibility.getValue());

    EXPECT_EQ(document->collaborationRevisions().capture({model, wildcard}), appBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              presentationBefore + 2);
}

TEST_F(CollaborationDomainIntegrationTest,
       ViewProviderSchemaRenameAndRemoveRecreateInvalidatePresentationObservations)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    auto* property = provider->addDynamicProperty(
        "App::PropertyBool", "SchemaPresentation");
    ASSERT_NE(property, nullptr);

    const Gui::SharedPresentationRevisionKey oldKey {
        document->collaborationObjectIdentity(*object), "SchemaPresentation"};
    const Gui::SharedPresentationRevisionKey renamedKey {
        document->collaborationObjectIdentity(*object), "RenamedSchemaPresentation"};
    const auto oldBefore =
        guiDocument->sharedPresentationRevisions().current(oldKey);
    const auto renamedBefore =
        guiDocument->sharedPresentationRevisions().current(renamedKey);

    ASSERT_TRUE(provider->renameDynamicProperty(
        property, "RenamedSchemaPresentation"));
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(oldKey),
              oldBefore + 1);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(renamedKey),
              renamedBefore + 1);

    const auto staleObservation =
        guiDocument->sharedPresentationRevisions().capture({renamedKey});
    const auto revisionBeforeAba =
        guiDocument->sharedPresentationRevisions().current(renamedKey);
    ASSERT_TRUE(provider->removeDynamicProperty("RenamedSchemaPresentation"));
    ASSERT_NE(provider->addDynamicProperty(
                  "App::PropertyBool", "RenamedSchemaPresentation"),
              nullptr);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(renamedKey),
              revisionBeforeAba + 2);

    const auto identity = document->collaborationIdentity();
    Gui::SharedPresentationCommitRequest request;
    request.document = {identity.instanceId, identity.lifecycleEpoch};
    request.expectedPresentationRevisions = staleObservation;
    request.presentationWrites = {renamedKey};
    int callbackCalls = 0;
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [&] {
        ++callbackCalls;
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = callbacks.applyAppMutation;
    callbacks.checkPostcondition = callbacks.applyAppMutation;
    callbacks.rollbackGuiMutation = callbacks.applyAppMutation;
    callbacks.rollbackAppMutation = callbacks.applyAppMutation;

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::PresentationConflict)
        << result.diagnostic;
    EXPECT_EQ(callbackCalls, 0);
}

TEST_F(CollaborationDomainIntegrationTest, successfulSaveAdvancesPresentationPersistedMarker)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    provider->Visibility.setValue(!provider->Visibility.getValue());
    const auto beforeSave = guiDocument->sharedPresentationRevisions().persistenceState();
    ASSERT_TRUE(beforeSave.hasUnpersistedChanges);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("presentation-marker.FCStd").toUtf8();
    ASSERT_TRUE(document->saveAs(path.constData()));

    const auto afterSave = guiDocument->sharedPresentationRevisions().persistenceState();
    EXPECT_FALSE(afterSave.poisoned);
    EXPECT_FALSE(afterSave.hasUnpersistedChanges);
    EXPECT_EQ(afterSave.persistedPublicationSequence,
              beforeSave.currentPublicationSequence);
}

TEST_F(CollaborationDomainIntegrationTest, restoredDocumentStartsAtPersistedPresentationBaseline)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    provider->Visibility.setValue(!provider->Visibility.getValue());

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("restored-presentation-marker.FCStd").toUtf8();
    ASSERT_TRUE(document->saveAs(path.constData()));
    App::GetApplication().closeDocument(documentName.c_str());
    QApplication::processEvents();
    document = nullptr;
    object = nullptr;
    guiDocument = nullptr;

    document = App::GetApplication().openDocument(path.constData());
    ASSERT_NE(document, nullptr);
    documentName = document->getName();
    guiDocument = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(guiDocument, nullptr);

    const auto restored = guiDocument->sharedPresentationRevisions().persistenceState();
    EXPECT_FALSE(restored.poisoned);
    EXPECT_FALSE(restored.hasUnpersistedChanges);
    EXPECT_EQ(restored.persistedPublicationSequence,
              restored.currentPublicationSequence);
}

TEST_F(CollaborationDomainIntegrationTest,
       personalContextIsTransientRevisionNeutralAndRestoresRenderer)
{
    Gui::PersonalViewContext actor;
    actor.camera = "actor-camera";
    actor.projection = "Perspective";
    actor.selectionPaths = {"Target.Face1"};
    actor.expandedTreePaths = {"Target"};
    actor.activeDocument = documentName;
    actor.activeView = "view-1";
    actor.activeWorkbench = "Part";
    actor.editFocus = "Target";
    actor.temporaryOverlays = {{"preview", "shape", "opaque-payload"}};

    const auto modelBefore = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::objectModel(object->getNameInDocument()),
         App::DocumentRevisionKey::unknownModelMutation()});
    ASSERT_FALSE(guiDocument->isModified());
    guiDocument->storePersonalViewContext("actor-a", actor);
    ASSERT_EQ(guiDocument->personalViewContext("actor-a"), actor);

    Gui::PersonalViewContext rendererState;
    rendererState.camera = "human-camera";
    bool renderedActor = false;
    Gui::PersonalViewRendererCallbacks renderer;
    renderer.captureState = [&] { return rendererState; };
    renderer.applyState = [&](const auto& context) { rendererState = context; };
    renderer.render = [&] { renderedActor = rendererState == actor; };
    renderer.restoreState = Gui::PersonalViewRestoreCallback(
        [&](const auto& context) noexcept { rendererState = context; });

    EXPECT_EQ(guiDocument->applyPersonalViewContext("actor-a", renderer),
              Gui::PersonalViewRenderStatus::Rendered);
    EXPECT_TRUE(renderedActor);
    EXPECT_EQ(rendererState.camera, "human-camera");
    EXPECT_FALSE(guiDocument->isModified());
    EXPECT_EQ(document->collaborationRevisions().capture(
                  {App::DocumentRevisionKey::objectModel(object->getNameInDocument()),
                   App::DocumentRevisionKey::unknownModelMutation()}),
              modelBefore);
}

TEST_F(CollaborationDomainIntegrationTest, personalRendererRejectsOffGuiThread)
{
    Gui::PersonalViewContext actor;
    actor.camera = "actor-camera";
    guiDocument->storePersonalViewContext("actor-a", actor);
    bool callbackEntered = false;
    bool rejected = false;
    Gui::PersonalViewRendererCallbacks renderer;
    renderer.captureState = [&] {
        callbackEntered = true;
        return Gui::PersonalViewContext {};
    };
    renderer.applyState = [&](const auto&) { callbackEntered = true; };
    renderer.render = [&] { callbackEntered = true; };
    renderer.restoreState = Gui::PersonalViewRestoreCallback(
        [&](const auto&) noexcept { callbackEntered = true; });

    std::thread worker([&] {
        try {
            static_cast<void>(guiDocument->applyPersonalViewContext("actor-a", renderer));
        }
        catch (const Base::Exception&) {
            rejected = true;
        }
    });
    worker.join();

    EXPECT_TRUE(rejected);
    EXPECT_FALSE(callbackEntered);
}

TEST_F(CollaborationDomainIntegrationTest,
       nativePersonalRenderReturnsPngAndPreservesHumanGuiAndRevisionState)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    const bool mainWindowWasVisible = mainWindow->isVisible();
    if (!mainWindowWasVisible) {
        mainWindow->resize(640, 480);
        mainWindow->show();
    }
    auto restoreMainWindowVisibility = qScopeGuard([mainWindow, mainWindowWasVisible] {
        if (!mainWindowWasVisible) {
            mainWindow->hide();
        }
    });

    auto* view = dynamic_cast<Gui::View3DInventor*>(
        guiDocument->createView(Gui::View3DInventor::getClassTypeId()));
    ASSERT_NE(view, nullptr);
    view->resize(128, 128);
    view->show();
    guiDocument->setActiveWindow(view);
    QApplication::processEvents();

    const QByteArray qtPlatform = qgetenv("QT_QPA_PLATFORM");
    const bool offscreenPlatform =
        qtPlatform.isEmpty() || qtPlatform == "offscreen";

    auto* viewer = view->getViewer();
    ASSERT_NE(viewer, nullptr);
    auto* glViewport = dynamic_cast<QOpenGLWidget*>(viewer->viewport());
    if (!glViewport) {
        if (offscreenPlatform) {
            GTEST_SKIP() << "OpenGL viewport unavailable for native personal render";
        }
        FAIL() << "OpenGL viewport unavailable for native personal render";
    }
    glViewport->makeCurrent();
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (!glContext || !glContext->isValid()) {
        if (offscreenPlatform) {
            GTEST_SKIP() << "OpenGL context unavailable (use X11/xcb or Xvfb; "
                            "QT_QPA_PLATFORM=offscreen cannot capture FBOs)";
        }
        FAIL() << "OpenGL context unavailable (use X11/xcb or Xvfb; "
                  "QT_QPA_PLATFORM=offscreen cannot capture FBOs)";
    }
    QOpenGLFramebufferObjectFormat fboFormat;
    fboFormat.setSamples(0);
    QOpenGLFramebufferObject fboProbe(8, 8, fboFormat);
    if (!fboProbe.isValid()) {
        if (offscreenPlatform) {
            GTEST_SKIP() << "OpenGL framebuffer capture unavailable in this environment";
        }
        FAIL() << "OpenGL framebuffer capture unavailable in this environment";
    }

    Gui::Selection().clearSelection(documentName.c_str());
    Gui::Selection().rmvPreselect();
    const std::string cameraBefore = view->getCamera();
    Gui::PersonalViewContext actor;
    actor.camera = cameraBefore;
    actor.projection = cameraBefore.find("PerspectiveCamera") != std::string::npos
        ? "Perspective"
        : "Orthographic";
    actor.selectionPaths = {"Target"};
    actor.activeDocument = documentName;
    guiDocument->storePersonalViewContext("actor-native", actor);

    const auto modelBefore = document->collaborationRevisions().capture(
        {App::DocumentRevisionKey::objectModel(object->getNameInDocument()),
         App::DocumentRevisionKey::unknownModelMutation()});
    const auto presentationBefore =
        guiDocument->sharedPresentationRevisions().latestPublicationSequence();
    guiDocument->setModified(false);
    int globalSelectionNotifications = 0;
    auto selectionObserver = Gui::Selection().signalSelectionChanged.connect(
        [&](const Gui::SelectionChanges&) { ++globalSelectionNotifications; });

    Gui::PersonalViewImageOptions options;
    options.width = 64;
    options.height = 64;
    options.samples = 0;
    std::optional<std::vector<std::uint8_t>> png;
    try {
        png = guiDocument->renderPersonalViewContext("actor-native", options);
    }
    catch (const Base::Exception& exception) {
        FAIL() << "native personal render failed: " << exception.what();
    }

    ASSERT_TRUE(png.has_value());
    ASSERT_GE(png->size(), 8U);
    const std::array<std::uint8_t, 8> pngSignature {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    EXPECT_TRUE(std::equal(pngSignature.begin(), pngSignature.end(), png->begin()));
    EXPECT_EQ(view->getCamera(), cameraBefore);
    EXPECT_FALSE(Gui::Selection().isSelected(documentName.c_str(), "Target"));
    EXPECT_EQ(globalSelectionNotifications, 0);
    EXPECT_FALSE(guiDocument->isModified());
    EXPECT_EQ(document->collaborationRevisions().capture(
                  {App::DocumentRevisionKey::objectModel(object->getNameInDocument()),
                   App::DocumentRevisionKey::unknownModelMutation()}),
              modelBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().latestPublicationSequence(),
              presentationBefore);
    EXPECT_EQ(guiDocument->personalViewContext("actor-native"), actor);

    auto overlayActor = actor;
    overlayActor.temporaryOverlays.push_back(
        {"axis-line",
         "coin-v1",
         "#Inventor V2.1 ascii\n"
         "Separator {\n"
         "  BaseColor { rgb 1 0 0 }\n"
         "  Coordinate3 { point [ 0 0 0, 1 0 0 ] }\n"
         "  LineSet { numVertices [ 2 ] }\n"
         "}\n"});
    guiDocument->storePersonalViewContext("actor-overlay", overlayActor);
    const auto overlayPng =
        guiDocument->renderPersonalViewContext("actor-overlay", options);
    ASSERT_TRUE(overlayPng.has_value());
    EXPECT_EQ(guiDocument->personalViewContext("actor-overlay"), overlayActor);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().latestPublicationSequence(),
              presentationBefore);
    EXPECT_FALSE(guiDocument->isModified());

    auto disallowedOverlay = actor;
    disallowedOverlay.temporaryOverlays.push_back(
        {"cube", "coin-v1", "#Inventor V2.1 ascii\nCube { width 1 }\n"});
    guiDocument->storePersonalViewContext("actor-disallowed-overlay", disallowedOverlay);
    EXPECT_THROW(guiDocument->renderPersonalViewContext(
                     "actor-disallowed-overlay", options),
                 Base::ValueError);

    auto invalidSelection = actor;
    invalidSelection.selectionPaths = {"MissingObject.Face1"};
    guiDocument->storePersonalViewContext("actor-invalid-selection", invalidSelection);
    const int notificationsBeforeInvalidSelection = globalSelectionNotifications;
    EXPECT_THROW(guiDocument->renderPersonalViewContext(
                     "actor-invalid-selection", options),
                 Base::ValueError);
    EXPECT_EQ(globalSelectionNotifications, notificationsBeforeInvalidSelection);
    EXPECT_FALSE(Gui::Selection().isSelected(documentName.c_str(), "Target"));

    auto mismatchedProjection = actor;
    mismatchedProjection.projection = actor.projection == "Perspective"
        ? "Orthographic"
        : "Perspective";
    guiDocument->storePersonalViewContext(
        "actor-mismatched-projection", mismatchedProjection);
    EXPECT_THROW(guiDocument->renderPersonalViewContext(
                     "actor-mismatched-projection", options),
                 Base::ValueError);

    auto* renderManager = view->getViewer()->getSoRenderManager();
    ASSERT_NE(renderManager, nullptr);
    const SbVec2s viewportBefore = renderManager->getSize();
    renderManager->setSize(SbVec2s(5000, 64));
    Gui::PersonalViewImageOptions mixedDefaultOptions;
    mixedDefaultOptions.width = -1;
    mixedDefaultOptions.height = 64;
    mixedDefaultOptions.samples = 0;
    EXPECT_THROW(guiDocument->renderPersonalViewContext(
                     "actor-native", mixedDefaultOptions),
                 Base::ValueError);
    renderManager->setSize(viewportBefore);

    auto oversizedCamera = actor;
    oversizedCamera.camera.assign(1024 * 1024 + 1, 'x');
    guiDocument->storePersonalViewContext("actor-oversized-camera", oversizedCamera);
    EXPECT_THROW(guiDocument->renderPersonalViewContext(
                     "actor-oversized-camera", options),
                 Base::ValueError);
    selectionObserver.disconnect();
}

TEST_F(CollaborationDomainIntegrationTest, pythonPersonalContextStorageApiIsCallable)
{
    Base::PyGILStateLocker gil;
    PyObject* guiModule = PyImport_ImportModule("FreeCADGui");
    ASSERT_NE(guiModule, nullptr);
    PyObject* documentObject = guiDocument->getPyObject();
    ASSERT_NE(documentObject, nullptr);
    for (const char* method : {"storePersonalViewContext",
                               "getPersonalViewContext",
                               "removePersonalViewContext",
                               "renderPersonalViewContext"}) {
        EXPECT_EQ(PyObject_HasAttrString(documentObject, method), 1) << method;
        EXPECT_EQ(PyObject_HasAttrString(guiModule, method), 1) << method;
    }

    PyObject* pythonNamespace = PyDict_New();
    ASSERT_NE(pythonNamespace, nullptr);
    ASSERT_EQ(PyDict_SetItemString(pythonNamespace, "gui", guiModule), 0);
    ASSERT_EQ(PyDict_SetItemString(pythonNamespace, "doc", documentObject), 0);
    PyObject* pythonDocumentName = PyUnicode_FromString(documentName.c_str());
    ASSERT_NE(pythonDocumentName, nullptr);
    ASSERT_EQ(PyDict_SetItemString(pythonNamespace, "document_name", pythonDocumentName), 0);
    Py_DECREF(pythonDocumentName);

    constexpr const char* script = R"PY(
context = {
    "camera": "actor-camera",
    "projection": "Perspective",
    "selection_paths": ["Target.Face1"],
    "preselection_path": None,
    "expanded_tree_paths": ["Target"],
    "tree_horizontal_scroll": 3,
    "tree_vertical_scroll": 7,
    "active_document": document_name,
    "active_view": "view-1",
    "active_workbench": "Part",
    "edit_focus": "Target",
    "temporary_overlays": [
        {"identifier": "preview", "kind": "shape", "payload": "opaque"}
    ],
}
doc.storePersonalViewContext("actor-a", context)
assert doc.getPersonalViewContext("actor-a") == context

gui.storePersonalViewContext(document_name, "actor-b", context)
assert gui.getPersonalViewContext(document_name, "actor-b") == context
assert gui.removePersonalViewContext(document_name, "actor-b") is True
assert gui.getPersonalViewContext(document_name, "actor-b") is None
assert doc.renderPersonalViewContext("missing-actor", 16, 16, "Current", 0) is None
assert gui.renderPersonalViewContext(
    document_name, "missing-actor", 16, 16, "Current", 0
) is None
)PY";
    PyObject* scriptResult =
        PyRun_String(script, Py_file_input, pythonNamespace, pythonNamespace);
    if (!scriptResult) {
        PyErr_Print();
    }
    ASSERT_NE(scriptResult, nullptr);
    Py_DECREF(scriptResult);
    Py_DECREF(pythonNamespace);
    Py_DECREF(documentObject);
    Py_DECREF(guiModule);
}
