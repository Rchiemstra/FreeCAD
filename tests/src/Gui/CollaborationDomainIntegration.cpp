// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>

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

class FailedSaveDialogProbe
{
public:
    void inspectNextDialog()
    {
        QTimer::singleShot(0, &timerContext, [this] { inspectActiveDialog(); });
    }

    int saveErrorDialogCount {0};
    bool inspectedCloseSafetyDialog {false};

private:
    void inspectActiveDialog()
    {
        auto* active = QApplication::activeModalWidget();
        auto* dialog = qobject_cast<QMessageBox*>(active);
        if (!dialog) {
            ADD_FAILURE() << "expected a modal failed-save dialog";
            if (auto* unexpected = qobject_cast<QDialog*>(active)) {
                unexpected->reject();
            }
            else if (active) {
                active->close();
            }
            return;
        }

        auto* discard = dialog->button(QMessageBox::Discard);
        auto* cancel = dialog->button(QMessageBox::Cancel);
        if (discard && cancel) {
            EXPECT_EQ(dialog->defaultButton(), cancel);
            EXPECT_EQ(dialog->escapeButton(), cancel);
            EXPECT_EQ(discard->text(), QStringLiteral("Close Without Saving"));
            inspectedCloseSafetyDialog = true;
            cancel->click();
            return;
        }

        // Gui::Document::save() can report the underlying save error as either
        // an OK-only critical dialog or a Yes/No "save elsewhere" question.
        // Dismiss either form before inspecting the close-safety dialog that
        // follows.  Arm the next inspection first so an unexpected dialog can
        // never be left blocking the GUI test after a nonfatal assertion.
        ++saveErrorDialogCount;
        inspectNextDialog();
        if (auto* no = dialog->button(QMessageBox::No)) {
            no->click();
        }
        else if (auto* ok = dialog->button(QMessageBox::Ok)) {
            ok->click();
        }
        else if (cancel) {
            cancel->click();
        }
        else {
            ADD_FAILURE() << "failed-save dialog had no safe dismissal button";
            dialog->reject();
        }
    }

    QObject timerContext;
};

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
        ASSERT_TRUE(baselineDirectory.isValid());
        const auto baselinePath = baselineDirectory.filePath(
            QString::fromStdString(documentName) + QStringLiteral(".FCStd"));
        ASSERT_TRUE(document->saveAs(baselinePath.toUtf8().constData()));
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
    QTemporaryDir baselineDirectory;
};

}  // namespace

TEST_F(CollaborationDomainIntegrationTest,
       documentChangesUiTracksStateHistoryTitlesAndClosePrompt)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    Gui::Application::Instance->setActiveDocument(guiDocument);
    QApplication::processEvents();

    auto* dock = mainWindow->findChild<QDockWidget*>(QStringLiteral("Document Changes"));
    auto* status = mainWindow->findChild<QLabel*>(QStringLiteral("documentFileState"));
    auto* state = mainWindow->findChild<QLabel*>(QStringLiteral("documentFileStateValue"));
    auto* path = mainWindow->findChild<QLabel*>(QStringLiteral("documentCanonicalPath"));
    auto* categories =
        mainWindow->findChild<QLabel*>(QStringLiteral("documentPendingCategories"));
    auto* readiness =
        mainWindow->findChild<QLabel*>(QStringLiteral("documentMutationReadiness"));
    auto* history =
        mainWindow->findChild<QListWidget*>(QStringLiteral("documentChangeHistory"));
    auto* clearHistory =
        mainWindow->findChild<QPushButton*>(QStringLiteral("clearDocumentChangeHistory"));
    auto* pause = mainWindow->findChild<QCheckBox*>(QStringLiteral("pauseAgentWrites"));
    ASSERT_NE(dock, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(state, nullptr);
    ASSERT_NE(path, nullptr);
    ASSERT_NE(categories, nullptr);
    ASSERT_NE(readiness, nullptr);
    ASSERT_NE(history, nullptr);
    ASSERT_NE(clearHistory, nullptr);
    ASSERT_NE(pause, nullptr);

    EXPECT_EQ(state->text(), QStringLiteral("Saved"));
    EXPECT_EQ(categories->text(), QStringLiteral("None"));
    EXPECT_EQ(readiness->text(), QStringLiteral("Ready"));
    EXPECT_EQ(path->text(), QString::fromStdString(document->FileName.getStrValue()));
    EXPECT_TRUE(status->text().contains(QStringLiteral("Saved")));
    EXPECT_FALSE(pause->isVisible());

    auto* view = dynamic_cast<Gui::View3DInventor*>(
        guiDocument->createView(Gui::View3DInventor::getClassTypeId()));
    ASSERT_NE(view, nullptr);
    guiDocument->setActiveWindow(view);
    object->Label.setValue("Changed target");
    QApplication::processEvents();

    EXPECT_EQ(state->text(), QStringLiteral("Unsaved"));
    EXPECT_TRUE(categories->text().contains(QStringLiteral("Model")));
    EXPECT_TRUE(status->text().contains(QStringLiteral("Unsaved")));
    EXPECT_TRUE(view->windowTitle().contains(QStringLiteral("Unsaved")));
    EXPECT_EQ(view->toolTip(), QString::fromStdString(document->FileName.getStrValue()));
    EXPECT_GT(history->count(), 0);
    EXPECT_LE(history->count(), 100);

    bool inspectedPrompt = false;
    QTimer::singleShot(0, [&] {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        ASSERT_NE(box, nullptr);
        EXPECT_TRUE(box->informativeText().contains(QStringLiteral("Model")));
        EXPECT_TRUE(box->informativeText().contains(
            QString::fromStdString(document->FileName.getStrValue())));
        ASSERT_NE(box->button(QMessageBox::Save), nullptr);
        ASSERT_NE(box->button(QMessageBox::Discard), nullptr);
        ASSERT_NE(box->button(QMessageBox::Cancel), nullptr);
        EXPECT_EQ(box->button(QMessageBox::Save)->text(), QStringLiteral("Save Changes"));
        EXPECT_EQ(box->button(QMessageBox::Discard)->text(),
                  QStringLiteral("Close Without Saving"));
        inspectedPrompt = true;
        box->button(QMessageBox::Cancel)->click();
    });
    EXPECT_EQ(mainWindow->confirmSave(document), Gui::MainWindow::ConfirmSaveResult::Cancel);
    EXPECT_TRUE(inspectedPrompt);

    clearHistory->click();
    EXPECT_EQ(history->count(), 0);

    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    provider->Visibility.setValue(!provider->Visibility.getValue());
    QApplication::processEvents();
    EXPECT_EQ(categories->text(), QStringLiteral("Appearance"));
    EXPECT_FALSE(document->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));

    const auto blockedParent = baselineDirectory.filePath(QStringLiteral("not-a-directory"));
    QFile blocker(blockedParent);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("block"), 5);
    blocker.close();
    const auto failedPath = QDir(blockedParent).filePath(QStringLiteral("cannot-save.FCStd"));
    const auto failed = document->saveAsWithOutcome(failedPath.toUtf8().constData());
    EXPECT_EQ(failed.disposition, App::DocumentSaveDisposition::Failed);
    QApplication::processEvents();
    EXPECT_EQ(state->text(), QStringLiteral("Save failed"));
    EXPECT_TRUE(status->text().contains(QStringLiteral("Save failed")));
    EXPECT_FALSE(dock->isHidden());
}

TEST_F(CollaborationDomainIntegrationTest, documentChangesRetainsBackgroundSaveHistory)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    auto* history =
        mainWindow->findChild<QListWidget*>(QStringLiteral("documentChangeHistory"));
    ASSERT_NE(history, nullptr);

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto backgroundName =
        App::GetApplication().getUniqueDocumentName("backgroundChangeHistory");
    auto* background = App::GetApplication().newDocument(
        backgroundName.c_str(), "background change history", flags);
    ASSERT_NE(background, nullptr);
    const auto cleanup = qScopeGuard([&] {
        if (App::GetApplication().getDocument(backgroundName.c_str())) {
            App::GetApplication().closeDocument(backgroundName.c_str());
            QApplication::processEvents();
        }
    });
    auto* backgroundGui = Gui::Application::Instance->getDocument(background);
    ASSERT_NE(backgroundGui, nullptr);
    Gui::Application::Instance->setActiveDocument(guiDocument);
    QApplication::processEvents();
    const int activeHistoryCount = history->count();

    ASSERT_NE(background->addObject("App::FeatureTest", "BackgroundTarget"), nullptr);
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("background.FCStd").toUtf8();
    ASSERT_EQ(background->saveAsWithOutcome(path.constData()).disposition,
              App::DocumentSaveDisposition::Written);
    QApplication::processEvents();
    EXPECT_EQ(history->count(), activeHistoryCount)
        << "the panel still renders only the active document";

    Gui::Application::Instance->setActiveDocument(backgroundGui);
    QApplication::processEvents();
    EXPECT_GT(history->count(), 0);
    EXPECT_TRUE(history->item(history->count() - 1)->text().contains(QStringLiteral("Written")));

    Gui::Application::Instance->setActiveDocument(guiDocument);
}

TEST_F(CollaborationDomainIntegrationTest,
       durableSaveAsOutcomeRepairsIdentityRefreshAfterLegacyGuiRelabelThrows)
{
    auto* view = dynamic_cast<Gui::View3DInventor*>(
        guiDocument->createView(Gui::View3DInventor::getClassTypeId()));
    ASSERT_NE(view, nullptr);
    guiDocument->setActiveWindow(view);
    const auto attemptedPath =
        baselineDirectory.filePath(QStringLiteral("resilient-identity.FCStd"));
    int throwingRelabelCalls = 0;
    auto throwingRelabelConnection =
        Gui::Application::Instance->signalRelabelDocument.connect(
            [&](const Gui::Document& changed) {
                if (&changed == guiDocument) {
                    ++throwingRelabelCalls;
                    throw Base::RuntimeError("legacy GUI relabel observer failure");
                }
            });

    const auto outcome = document->saveAsWithOutcome(
        attemptedPath.toUtf8().constData(), true);
    throwingRelabelConnection.disconnect();
    QApplication::processEvents();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(outcome.durabilityVerified);
    EXPECT_EQ(throwingRelabelCalls, 1);
    EXPECT_EQ(document->FileName.getStrValue(), attemptedPath.toStdString());
    EXPECT_STREQ(document->Label.getValue(), "resilient-identity");
    EXPECT_TRUE(view->windowTitle().contains(QStringLiteral("resilient-identity")));
    EXPECT_EQ(view->toolTip(), attemptedPath);
}

TEST_F(CollaborationDomainIntegrationTest,
       failedSingleDocumentSaveDefaultsToKeepingUnsavedChanges)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    Gui::Application::Instance->setActiveDocument(guiDocument);

    const auto blockedParent = baselineDirectory.filePath(QStringLiteral("blocked-save-parent"));
    QFile blocker(blockedParent);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("block"), 5);
    blocker.close();
    const auto failedPath = QDir(blockedParent).filePath(QStringLiteral("cannot-save.FCStd"));
    document->FileName.setValue(failedPath.toUtf8().constData());
    object->Label.setValue("unsaved close protection");

    FailedSaveDialogProbe failureDialogs;
    QObject initialDialogTimerContext;
    QTimer::singleShot(0, &initialDialogTimerContext, [&] {
        auto* initial = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!initial) {
            ADD_FAILURE() << "expected the initial save confirmation dialog";
            return;
        }
        auto* save = initial->button(QMessageBox::Save);
        if (!save) {
            ADD_FAILURE() << "initial close confirmation had no Save button";
            initial->reject();
            return;
        }
        failureDialogs.inspectNextDialog();
        save->click();
    });

    EXPECT_FALSE(guiDocument->canClose(true, false));
    EXPECT_EQ(failureDialogs.saveErrorDialogCount, 1);
    EXPECT_TRUE(failureDialogs.inspectedCloseSafetyDialog);
    EXPECT_TRUE(document->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest,
       failedMultiDocumentSaveDefaultsToKeepingUnsavedChanges)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    Gui::Application::Instance->setActiveDocument(guiDocument);

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto secondName = App::GetApplication().getUniqueDocumentName("closeFailureSecond");
    auto* second = App::GetApplication().newDocument(
        secondName.c_str(), "close failure second", flags);
    ASSERT_NE(second, nullptr);
    const auto cleanup = qScopeGuard([&] {
        if (App::GetApplication().getDocument(secondName.c_str())) {
            App::GetApplication().closeDocument(secondName.c_str());
            QApplication::processEvents();
        }
    });
    auto* secondObject = second->addObject("App::FeatureTest", "SecondTarget");
    ASSERT_NE(secondObject, nullptr);
    second->recompute();
    const auto secondBaseline =
        baselineDirectory.filePath(QStringLiteral("close-failure-second-baseline.FCStd"));
    ASSERT_TRUE(second->saveAs(secondBaseline.toUtf8().constData()));

    const auto blockedParent = baselineDirectory.filePath(QStringLiteral("blocked-save-all"));
    QFile blocker(blockedParent);
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    ASSERT_EQ(blocker.write("block"), 5);
    blocker.close();
    document->FileName.setValue(
        QDir(blockedParent).filePath(QStringLiteral("first.FCStd")).toUtf8().constData());
    second->FileName.setValue(
        QDir(blockedParent).filePath(QStringLiteral("second.FCStd")).toUtf8().constData());
    object->Label.setValue("first unsaved close protection");
    secondObject->Label.setValue("second unsaved close protection");

    auto confirmAllGroup = App::GetApplication()
                               .GetUserParameter()
                               .GetGroup("BaseApp")
                               ->GetGroup("Preferences")
                               ->GetGroup("General");
    const auto confirmAllValues = confirmAllGroup->GetBoolMap();
    const bool hadConfirmAll = std::ranges::any_of(
        confirmAllValues,
        [](const auto& entry) { return entry.first == "ConfirmAll"; });
    const bool previousConfirmAll = confirmAllGroup->GetBool("ConfirmAll", false);
    const auto restoreConfirmAll = qScopeGuard([confirmAllGroup, hadConfirmAll, previousConfirmAll] {
        if (hadConfirmAll) {
            confirmAllGroup->SetBool("ConfirmAll", previousConfirmAll);
        }
        else {
            confirmAllGroup->RemoveBool("ConfirmAll");
        }
    });
    confirmAllGroup->SetBool("ConfirmAll", false);

    FailedSaveDialogProbe failureDialogs;
    QObject initialDialogTimerContext;
    QTimer::singleShot(0, &initialDialogTimerContext, [&] {
        auto* initial = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!initial) {
            ADD_FAILURE() << "expected the initial save-all confirmation dialog";
            return;
        }
        auto* save = initial->button(QMessageBox::Save);
        if (!save) {
            ADD_FAILURE() << "initial close-all confirmation had no Save button";
            initial->reject();
            return;
        }
        auto* confirmAll = initial->findChild<QCheckBox*>();
        if (!confirmAll) {
            ADD_FAILURE() << "initial close-all confirmation had no Apply to all checkbox";
            initial->reject();
            return;
        }
        confirmAll->setChecked(true);
        failureDialogs.inspectNextDialog();
        save->click();
    });

    EXPECT_FALSE(mainWindow->closeAllDocuments(false));
    EXPECT_EQ(failureDialogs.saveErrorDialogCount, 2);
    EXPECT_TRUE(failureDialogs.inspectedCloseSafetyDialog);
    EXPECT_TRUE(document->hasPendingFileChanges());
    EXPECT_TRUE(second->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest, touchOnlyActivityDoesNotDirtyCanonicalFileState)
{
    ASSERT_FALSE(document->hasPendingFileChanges());
    ASSERT_FALSE(guiDocument->isModified());

    int fileStateNotifications = 0;
    auto connection = document->signalFileChangeStateChanged().connect(
        [&](const App::Document&) { ++fileStateNotifications; });
    object->touch();
    QApplication::processEvents();
    connection.disconnect();

    EXPECT_EQ(fileStateNotifications, 0);
    EXPECT_FALSE(document->hasPendingFileChanges());
    EXPECT_FALSE(guiDocument->isModified());
    EXPECT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Unchanged);
}

TEST_F(CollaborationDomainIntegrationTest,
       transientViewProviderDynamicSchemaTracksAppearanceButNoPersistStaysClean)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);

    auto* property = provider->addDynamicProperty(
        "App::PropertyString", "TransientViewSchema", "Group", "Documentation",
        App::Prop_Transient);
    ASSERT_NE(property, nullptr);
    EXPECT_FALSE(document->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    ASSERT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(provider->renameDynamicProperty(property, "RenamedTransientViewSchema"));
    ASSERT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(provider->changeDynamicProperty(
        property, "Changed group", "Changed documentation"));
    ASSERT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    ASSERT_TRUE(provider->changeDynamicProperty(
        property, "Changed group", "Changed documentation"));
    EXPECT_FALSE(document->hasPendingFileChanges());
    ASSERT_TRUE(provider->changeDynamicProperty(property, nullptr, nullptr));
    EXPECT_FALSE(document->hasPendingFileChanges());

    document->openTransaction("view dynamic metadata is not undo payload");
    ASSERT_TRUE(provider->changeDynamicProperty(
        property, "Aborted group", "Aborted documentation"));
    document->abortTransaction();
    EXPECT_STREQ(provider->getPropertyGroup(property), "Aborted group");
    EXPECT_STREQ(provider->getPropertyDocumentation(property), "Aborted documentation");
    EXPECT_FALSE(document->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    EXPECT_TRUE(document->hasPendingFileChanges());

    ASSERT_TRUE(provider->removeDynamicProperty("RenamedTransientViewSchema"));
    ASSERT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    auto* noPersist = provider->addDynamicProperty(
        "App::PropertyString", "NoPersistViewSchema", "Group", "Documentation",
        App::Prop_NoPersist);
    ASSERT_NE(noPersist, nullptr);
    EXPECT_FALSE(document->hasPendingFileChanges());
    ASSERT_TRUE(provider->renameDynamicProperty(noPersist, "RenamedNoPersistViewSchema"));
    ASSERT_TRUE(provider->changeDynamicProperty(
        noPersist, "Changed group", "Changed documentation"));
    ASSERT_TRUE(provider->removeDynamicProperty("RenamedNoPersistViewSchema"));
    EXPECT_FALSE(document->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest,
       staticTransientViewPropertyStatusTracksAppearance)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "ShowInTree"};
    const auto revisionBefore = guiDocument->sharedPresentationRevisions().current(key);

    // ShowInTree is a static ViewProvider property. After it acquires the
    // transient status, its serialized Hidden transition must still dirty the
    // presentation archive; this is the case the old transient filter lost.
    provider->ShowInTree.setStatus(App::Property::Transient, true);
    ASSERT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              revisionBefore + 1);
    ASSERT_EQ(document->saveWithOutcome().disposition,
              App::DocumentSaveDisposition::Written);

    provider->ShowInTree.setStatus(App::Property::Hidden, true);
    EXPECT_FALSE(document->getPendingFileChanges().testFlag(App::DocumentFileChange::Model));
    EXPECT_TRUE(
        document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              revisionBefore + 2);
}

TEST_F(CollaborationDomainIntegrationTest,
       declaredViewPropertyStatusCommitPublishesExactlyOnce)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "ShowInTree"};
    const auto revisionBefore = guiDocument->sharedPresentationRevisions().current(key);
    const bool hiddenBefore = provider->ShowInTree.testStatus(App::Property::Hidden);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->ShowInTree.setStatus(App::Property::Hidden, !hiddenBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [&] {
        return Gui::SharedPresentationStepResult {
            provider->ShowInTree.testStatus(App::Property::Hidden) != hiddenBefore,
            {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->ShowInTree.setStatus(App::Property::Hidden, hiddenBefore);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    ASSERT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_NE(provider->ShowInTree.testStatus(App::Property::Hidden), hiddenBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              revisionBefore + 1);
}

TEST_F(CollaborationDomainIntegrationTest,
       declaredBulkViewPropertyStatusCommitPublishesExactlyOnce)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);

    // Make ShowInTree the only property changed by the bulk operation. This
    // keeps the declared write set exact while still exercising the direct
    // PropertyContainer::setPropertyStatus() storage path.
    std::vector<App::Property*> properties;
    provider->getPropertyList(properties);
    ASSERT_FALSE(properties.empty());
    for (auto* property : properties) {
        ASSERT_NE(property, nullptr);
        property->setStatus(App::Property::Hidden,
                            property != &provider->ShowInTree);
    }
    ASSERT_FALSE(provider->ShowInTree.testStatus(App::Property::Hidden));
    const auto baselineSave = document->saveWithOutcome();
    ASSERT_TRUE(baselineSave.succeeded()) << baselineSave.message;
    ASSERT_FALSE(document->hasPendingFileChanges());

    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "ShowInTree"};
    const auto revisionBefore = guiDocument->sharedPresentationRevisions().current(key);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->setPropertyStatus(
            static_cast<unsigned char>(App::Property::Hidden), true);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.checkPostcondition = [&] {
        return Gui::SharedPresentationStepResult {
            provider->ShowInTree.testStatus(App::Property::Hidden), {}};
    };
    callbacks.rollbackGuiMutation = [&] {
        provider->ShowInTree.setStatus(App::Property::Hidden, false);
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    ASSERT_TRUE(result.committed()) << result.diagnostic;
    EXPECT_TRUE(provider->ShowInTree.testStatus(App::Property::Hidden));
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key),
              revisionBefore + 1);
}

TEST_F(CollaborationDomainIntegrationTest,
       failedViewPropertyStatusApplyRestoresExactBitsFileStateAndRevision)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey key {
        document->collaborationObjectIdentity(*object), "ShowInTree"};
    const auto revisionBefore = guiDocument->sharedPresentationRevisions().current(key);
    const auto statusBefore = provider->ShowInTree.getStatus();
    const auto fileChangesBefore = document->getPendingFileChanges().toUnderlyingType();
    const auto targetStatus = statusBefore
        ^ (1UL << App::Property::Hidden)
        ^ (1UL << App::Property::ReadOnly);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({key});
    request.presentationWrites = {key};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->ShowInTree.setStatusValue(targetStatus);
        return Gui::SharedPresentationStepResult {false, "forced status apply failure"};
    };
    callbacks.checkPostcondition = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    // Deliberately omit status restoration here. The integration-owned ledger
    // must restore it even when a legacy callback reports successful rollback.
    callbacks.rollbackGuiMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.rollbackAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };

    const auto result =
        guiDocument->commitSharedPresentation(std::move(request), std::move(callbacks));

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::GuiApplyFailed)
        << result.diagnostic;
    EXPECT_EQ(provider->ShowInTree.getStatus(), statusBefore);
    EXPECT_EQ(document->getPendingFileChanges().toUnderlyingType(), fileChangesBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(key), revisionBefore);
}

TEST_F(CollaborationDomainIntegrationTest,
       undeclaredViewPropertyStatusIsRejectedBeforeBitsChange)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const Gui::SharedPresentationRevisionKey declaredKey {
        document->collaborationObjectIdentity(*object), "Visibility"};
    const Gui::SharedPresentationRevisionKey undeclaredKey {
        document->collaborationObjectIdentity(*object), "ShowInTree"};
    const auto statusBefore = provider->ShowInTree.getStatus();
    const auto revisionBefore =
        guiDocument->sharedPresentationRevisions().current(undeclaredKey);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({declaredKey});
    request.presentationWrites = {declaredKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->ShowInTree.setStatus(
            App::Property::Hidden,
            !provider->ShowInTree.testStatus(App::Property::Hidden));
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

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::GuiApplyFailed)
        << result.diagnostic;
    EXPECT_NE(result.diagnostic.find("undeclared"), std::string::npos);
    EXPECT_EQ(provider->ShowInTree.getStatus(), statusBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(undeclaredKey),
              revisionBefore);
    EXPECT_FALSE(document->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest,
       undeclaredBulkViewPropertyStatusPreflightsBeforeAnyBitsChange)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);

    std::vector<App::Property*> properties;
    provider->getPropertyList(properties);
    std::vector<App::Property*> orderedPersistentProperties;
    for (auto* property : properties) {
        ASSERT_NE(property, nullptr);
        const char* name = property->getName();
        if (name && *name != '\0'
            && !property->testStatus(App::Property::PropNoPersist)
            && std::ranges::find(orderedPersistentProperties, property)
                == orderedPersistentProperties.end()) {
            orderedPersistentProperties.push_back(property);
        }
        property->setStatus(App::Property::Hidden, true);
    }
    ASSERT_GE(orderedPersistentProperties.size(), 2U);
    auto* admittedProperty = orderedPersistentProperties[0];
    auto* rejectedProperty = orderedPersistentProperties[1];
    admittedProperty->setStatus(App::Property::Hidden, false);
    rejectedProperty->setStatus(App::Property::Hidden, false);
    ASSERT_FALSE(admittedProperty->testStatus(App::Property::Hidden));
    ASSERT_FALSE(rejectedProperty->testStatus(App::Property::Hidden));
    const auto baselineSave = document->saveWithOutcome();
    ASSERT_TRUE(baselineSave.succeeded()) << baselineSave.message;
    ASSERT_FALSE(document->hasPendingFileChanges());

    const auto identity = document->collaborationIdentity();
    const App::DocumentRevisionIdentityBinding binding {
        identity.instanceId, identity.lifecycleEpoch};
    const std::string stableIdentity = document->collaborationObjectIdentity(*object);
    const Gui::SharedPresentationRevisionKey admittedKey {
        stableIdentity, admittedProperty->getName()};
    const Gui::SharedPresentationRevisionKey rejectedKey {
        stableIdentity, rejectedProperty->getName()};
    const auto admittedRevisionBefore =
        guiDocument->sharedPresentationRevisions().current(admittedKey);
    const auto rejectedRevisionBefore =
        guiDocument->sharedPresentationRevisions().current(rejectedKey);

    Gui::SharedPresentationCommitRequest request;
    request.document = binding;
    request.expectedPresentationRevisions =
        guiDocument->sharedPresentationRevisions().capture({admittedKey});
    request.presentationWrites = {admittedKey};
    Gui::SharedPresentationCommitCallbacks callbacks;
    callbacks.applyAppMutation = [] {
        return Gui::SharedPresentationStepResult {true, {}};
    };
    callbacks.applyGuiMutation = [&] {
        provider->setPropertyStatus(
            static_cast<unsigned char>(App::Property::Hidden), true);
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

    EXPECT_EQ(result.status, Gui::SharedPresentationCommitStatus::GuiApplyFailed)
        << result.diagnostic;
    EXPECT_NE(result.diagnostic.find("undeclared"), std::string::npos);
    EXPECT_FALSE(admittedProperty->testStatus(App::Property::Hidden));
    EXPECT_FALSE(rejectedProperty->testStatus(App::Property::Hidden));
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(admittedKey),
              admittedRevisionBefore);
    EXPECT_EQ(guiDocument->sharedPresentationRevisions().current(rejectedKey),
              rejectedRevisionBefore);
    EXPECT_FALSE(document->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest,
       readOnlyAndForeignViewPropertyStatusAdmissionRejectBeforeBitsChange)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const auto statusBefore = provider->ShowInTree.getStatus();

    document->beginCollaborationReadOnlyPostconditionAudit();
    auto endReadOnly = qScopeGuard(
        [&] { document->endCollaborationAtomicPresentationAudit(); });
    EXPECT_THROW(
        provider->ShowInTree.setStatus(
            App::Property::Hidden,
            !provider->ShowInTree.testStatus(App::Property::Hidden)),
        Base::RuntimeError);
    EXPECT_EQ(provider->ShowInTree.getStatus(), statusBefore);
    document->endCollaborationAtomicPresentationAudit();
    endReadOnly.dismiss();

    App::DocumentInitFlags flags;
    flags.createView = false;
    const std::string foreignName =
        App::GetApplication().getUniqueDocumentName("foreignStatusAdmission");
    auto* foreignDocument = App::GetApplication().newDocument(
        foreignName.c_str(), "foreign status admission", flags);
    ASSERT_NE(foreignDocument, nullptr);
    const auto closeForeign = qScopeGuard([&] {
        if (App::GetApplication().getDocument(foreignName.c_str())) {
            App::GetApplication().closeDocument(foreignName.c_str());
        }
    });
    auto* foreignObject = foreignDocument->addObject("App::FeatureTest", "Foreign");
    ASSERT_NE(foreignObject, nullptr);
    auto* foreignGuiDocument = Gui::Application::Instance->getDocument(foreignDocument);
    ASSERT_NE(foreignGuiDocument, nullptr);
    auto* foreignProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        foreignGuiDocument->getViewProvider(foreignObject));
    ASSERT_NE(foreignProvider, nullptr);
    const auto foreignStatusBefore = foreignProvider->ShowInTree.getStatus();

    document->beginCollaborationAtomicPresentationAudit({});
    const auto endAtomic = qScopeGuard(
        [&] { document->endCollaborationAtomicPresentationAudit(); });
    EXPECT_THROW(
        foreignProvider->ShowInTree.setStatus(
            App::Property::Hidden,
            !foreignProvider->ShowInTree.testStatus(App::Property::Hidden)),
        Base::RuntimeError);
    EXPECT_EQ(foreignProvider->ShowInTree.getStatus(), foreignStatusBefore);
}

TEST_F(CollaborationDomainIntegrationTest,
       newObjectViewMetadataAndStatusDirtinessAbortWithOwningTransaction)
{
    ASSERT_FALSE(document->hasPendingFileChanges());
    document->openTransaction("new ViewProvider presentation schema");
    auto* added = document->addObject("App::FeatureTest", "TransactionOwnedViewObject");
    ASSERT_NE(added, nullptr);
    auto* addedProvider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(added));
    ASSERT_NE(addedProvider, nullptr);
    auto* dynamic = addedProvider->addDynamicProperty(
        "App::PropertyString", "TransactionOwnedMetadata", "Initial", "Initial");
    ASSERT_NE(dynamic, nullptr);
    ASSERT_TRUE(addedProvider->changeDynamicProperty(dynamic, "Changed", "Changed"));
    addedProvider->ShowInTree.setStatus(
        App::Property::Hidden,
        !addedProvider->ShowInTree.testStatus(App::Property::Hidden));
    ASSERT_TRUE(document->getPendingFileChanges().testFlag(
        App::DocumentFileChange::Appearance));

    document->abortTransaction();

    EXPECT_EQ(document->getObject("TransactionOwnedViewObject"), nullptr);
    EXPECT_FALSE(document->hasPendingFileChanges());
}

TEST_F(CollaborationDomainIntegrationTest,
       existingObjectViewPropertyStatusRemainsStickyAcrossOrdinaryAbort)
{
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    const bool hiddenBefore = provider->ShowInTree.testStatus(App::Property::Hidden);

    document->openTransaction("existing ViewProvider status");
    provider->ShowInTree.setStatus(App::Property::Hidden, !hiddenBefore);
    document->abortTransaction();

    EXPECT_NE(provider->ShowInTree.testStatus(App::Property::Hidden), hiddenBefore);
    EXPECT_TRUE(document->getPendingFileChanges().testFlag(
        App::DocumentFileChange::Appearance));
}

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

TEST(CollaborationDomainIntegrationStandalone,
     throwingLegacyFinishObserverCannotStarveGuiSaveOutcomeBookkeeping)
{
    initializeDomainGui();
    auto throwingObserver = App::GetApplication().signalFinishSaveDocument.connect(
        [](const App::Document&, const std::string&) {
            throw std::runtime_error("injected legacy finish observer failure");
        });

    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto documentName =
        App::GetApplication().getUniqueDocumentName("resilientGuiSaveOutcome");
    auto* document = App::GetApplication().newDocument(documentName.c_str(), nullptr, flags);
    ASSERT_NE(document, nullptr);
    const auto cleanup = qScopeGuard([&] {
        throwingObserver.disconnect();
        if (App::GetApplication().getDocument(documentName.c_str())) {
            App::GetApplication().closeDocument(documentName.c_str());
            QApplication::processEvents();
        }
    });
    auto* object = document->addObject("App::FeatureTest", "Target");
    ASSERT_NE(object, nullptr);
    auto* guiDocument = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(guiDocument, nullptr);
    auto* provider = dynamic_cast<Gui::ViewProviderDocumentObject*>(
        guiDocument->getViewProvider(object));
    ASSERT_NE(provider, nullptr);
    provider->Visibility.setValue(!provider->Visibility.getValue());
    const auto beforeSave = guiDocument->sharedPresentationRevisions().persistenceState();
    ASSERT_TRUE(beforeSave.hasUnpersistedChanges);

    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto path = temporary.filePath("resilient-gui-outcome.FCStd").toUtf8();
    const auto outcome = document->saveAsWithOutcome(path.constData());
    ASSERT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written);

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
