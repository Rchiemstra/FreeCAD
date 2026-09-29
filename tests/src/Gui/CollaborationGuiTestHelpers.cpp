// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationGuiTestHelpers.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentHandle.h>
#include <App/PropertyLinks.h>
#include <App/DocumentObject.h>
#include <App/RecoverySnapshot.h>
#include <Base/Exception.h>
#include <Base/Parameter.h>
#include <Gui/Application.h>
#include <Gui/AutoSaver.h>
#include <Gui/Document.h>
#include <Gui/DocumentExecutionIngress.h>
#include <Gui/MDIView.h>
#include <Gui/MainWindow.h>

#include <QAbstractButton>
#include <QMessageBox>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace Gui::Test
{

class SharedPresentationSerializeTestAccess
{
public:
    static App::DocumentCommitResult serializeAtomic(
        App::DocumentCollaborationService& service,
        std::vector<App::CollaborationAtomicPresentationWrite> allowedWrites,
        App::CollaborationAtomicCompatibilityCallback callback)
    {
        return service.serializeAtomicCompatibilityCallback(
            std::move(allowedWrites), std::move(callback));
    }
};

namespace
{

void waitForExecutionLaneIdle(App::Document& document)
{
    const auto* lane = document.executionLane();
    if (!lane) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        if (lane->isIdle()) {
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL() << "document execution lane did not become idle before timeout";
}

void waitForCompletedCommand(App::DocumentCommandHandle& commandHandle)
{
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    App::DocumentCommandSnapshot snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        snapshot = commandHandle.status();
        if (snapshot.terminal()) {
            ASSERT_EQ(snapshot.state, App::DocumentCommandState::Completed)
                << snapshot.diagnostic;
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL() << "document command did not finish before timeout";
}

bool askIfSavingFailedForGuiTest(Gui::Document& guiDocument, const QString& error)
{
    const int ret = QMessageBox::question(
        Gui::getMainWindow(),
        QObject::tr("Could not save document"),
        QObject::tr(
            "There was an issue trying to save the file. "
            "This may be because some of the parent folders do not exist, "
            "or you do not have sufficient permissions, "
            "or for other reasons. Error details:\n\n\"%1\"\n\n"
            "Would you like to save the file with a different name?")
            .arg(error),
        QMessageBox::Yes,
        QMessageBox::No);
    if (ret == QMessageBox::No) {
        if (auto* window = Gui::getMainWindow()) {
            window->showMessage(QObject::tr("Saving aborted"), 2000);
        }
        return false;
    }
    if (ret == QMessageBox::Yes) {
        return guiDocument.saveAs();
    }
    return false;
}

bool saveModifiedDocumentForCloseWithoutBlockingGui(Gui::Document& guiDocument)
{
    App::Document* const document = guiDocument.getDocument();
    if (!document->executionLane()) {
        return guiDocument.save();
    }
    std::string saveFailureDiagnostic;
    if (submitDocumentSaveAwaitingCompletion(*document, &saveFailureDiagnostic)) {
        return true;
    }
    if (!saveFailureDiagnostic.empty()
        && askIfSavingFailedForGuiTest(
            guiDocument,
            QString::fromStdString(saveFailureDiagnostic))) {
        return true;
    }
    return false;
}

void submitAndWait(App::Document& document, App::DocumentCommand&& command)
{
    waitForExecutionLaneIdle(document);
    auto handle = document.executionHandle();
    command.document = handle.identity();
    const auto outcome = handle.trySubmit(std::move(command));
    if (outcome.result != App::DocumentCommandSubmitResult::Accepted) {
        FAIL() << "document command trySubmit failed: "
               << App::documentCommandSubmitResultName(outcome.result);
    }

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    waitForCompletedCommand(commandHandle);
    waitForExecutionLaneIdle(document);
}

}  // namespace

class AutoSaverRecoveryTestAccess
{
public:
    static void flushPendingSaveWithoutBlockingGui(App::Document& document)
    {
        auto* const saver = AutoSaver::instance();
        const auto found = saver->saverMap.find(document.getName());
        if (found == saver->saverMap.end()) {
            return;
        }

        AutoSaveProperty& property = *found->second;
        if (!property.beginSaveAttempt()) {
            return;
        }

        if (!document.canWriteRecoverySnapshot()) {
            property.deferSaveUntilStable();
            return;
        }

        if (auto* app = Application::Instance) {
            if (auto* guiDocument = app->getDocument(&document)) {
                if (guiDocument->isPerformingTransaction()) {
                    property.deferSaveUntilStable();
                    return;
                }
            }
        }

        ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        App::RecoverySnapshotSaveOptions options;
        options.compressed = saver->compressed;
        options.saveBinaryBrep = !saver->compressed || hGrp->GetBool("SaveBinaryBrep", true);
        options.saveThumbnail = false;

        try {
            // Prefer the production GUI hop helper so tests exercise the same
            // owner-thread path AutoSaver uses (worker + pump, never GUI dispatchToOwner).
            const bool written = writeRecoverySnapshotAwaitingOwnerThread(document, options);
            if (!written) {
                property.restoreFailedSaveAttempt();
                document.reportRecoverySaveOutcome(
                    document.TransientDir.getStrValue(),
                    false,
                    "Recovery snapshot was not stable");
                return;
            }
        }
        catch (...) {
            // Match AutoSaver::flushPendingSaveForIdentity / timerEvent: retain dirty
            // work for retry and do not let the exception escape the flush helper.
            property.restoreFailedSaveAttempt();
            document.reportRecoverySaveOutcome(
                document.TransientDir.getStrValue(),
                false,
                "Recovery snapshot write threw an exception");
            return;
        }

        property.finishSuccessfulSaveAttempt();
        document.reportRecoverySaveOutcome(document.TransientDir.getStrValue(), true);
    }
};

bool canCloseWithoutBlockingGui(Gui::Document& guiDocument,
                                const bool checkModify,
                                const bool checkLink)
{
    App::Document* const document = guiDocument.getDocument();
    if (document->testStatus(App::Document::TempDoc)) {
        return true;
    }

    // Do not pump the event loop before confirmSave(). Tests arm a single-shot
    // timer that must fire only after the modal confirmation is already up;
    // pumping here steals that timer and leaves confirmSave() hung forever.

    if (!document->isClosable()) {
        QMessageBox::warning(
            guiDocument.getActiveView(),
            QObject::tr("Document not closable"),
            QObject::tr("The document is not closable for the moment."));
        return false;
    }

    if (checkLink && !App::PropertyXLink::getDocumentInList(document).empty()) {
        return true;
    }

    bool ok = true;
    if (checkModify && guiDocument.isModified() && !document->testStatus(App::Document::PartialDoc)) {
        auto* const mainWindow = getMainWindow();
        if (!mainWindow) {
            return false;
        }
        const int res = mainWindow->confirmSave(document, guiDocument.getActiveView());
        switch (res) {
            case MainWindow::ConfirmSaveResult::Cancel:
                ok = false;
                break;
            case MainWindow::ConfirmSaveResult::SaveAll:
            case MainWindow::ConfirmSaveResult::Save:
                waitForExecutionLaneIdle(*document);
                ok = saveModifiedDocumentForCloseWithoutBlockingGui(guiDocument);
                if (!ok) {
                    const QString docName =
                        QString::fromStdString(document->Label.getStrValue());
                    const QString text =
                        (!docName.isEmpty()
                             ? QObject::tr(
                                   "Failed to save document '%1'. Would you like to cancel the closure?")
                                   .arg(docName)
                             : QObject::tr(
                                   "Document saving failed. Would you like to cancel the closure?"));
                    QMessageBox box(
                        QMessageBox::Warning,
                        QObject::tr("Unable to save document"),
                        text,
                        QMessageBox::Discard | QMessageBox::Cancel,
                        guiDocument.getActiveView());
                    box.setDefaultButton(QMessageBox::Cancel);
                    box.setEscapeButton(QMessageBox::Cancel);
                    if (auto* discard = box.button(QMessageBox::Discard)) {
                        discard->setText(QObject::tr("Close Without Saving"));
                    }
                    const int ret = box.exec();
                    if (ret == QMessageBox::Discard) {
                        ok = true;
                    }
                }
                break;
            case MainWindow::ConfirmSaveResult::DiscardAll:
            case MainWindow::ConfirmSaveResult::Discard:
                ok = true;
                break;
        }
    }

    return ok;
}

void flushAutoSaverWithoutBlockingGui(App::Document& document)
{
    AutoSaverRecoveryTestAccess::flushPendingSaveWithoutBlockingGui(document);
}

bool closeAllDocumentsWithoutBlockingGui(const bool close)
{
    auto* const mainWindow = getMainWindow();
    if (!mainWindow) {
        return true;
    }

    auto docs = App::GetApplication().getDocuments();
    try {
        docs = App::Document::getDependentDocuments(docs, true);
    }
    catch (const Base::Exception& exception) {
        exception.reportException();
    }

    bool checkModify = true;
    bool saveAll = false;
    int failedSaves = 0;

    MDIView* activeView = mainWindow->activeWindow();
    App::Document* activeDoc = (activeView ? activeView->getAppDocument() : nullptr);
    if (activeDoc) {
        for (auto it = ++docs.begin(); it != docs.end(); it++) {
            if (*it == activeDoc) {
                docs.erase(it);
                docs.insert(docs.begin(), activeDoc);
            }
        }
    }

    for (auto* doc : docs) {
        auto* gdoc = Application::Instance->getDocument(doc);
        if (!gdoc) {
            continue;
        }
        if (!gdoc->canClose(false)) {
            return false;
        }
        if (!gdoc->isModified() || doc->testStatus(App::Document::PartialDoc)
            || doc->testStatus(App::Document::TempDoc)) {
            continue;
        }
        bool save = saveAll;
        if (!save && checkModify) {
            const int res = mainWindow->confirmSave(doc, mainWindow, docs.size() > 1);
            switch (res) {
                case MainWindow::ConfirmSaveResult::Cancel:
                    return false;
                case MainWindow::ConfirmSaveResult::SaveAll:
                    saveAll = true;
                    /* FALLTHRU */
                case MainWindow::ConfirmSaveResult::Save:
                    save = true;
                    break;
                case MainWindow::ConfirmSaveResult::DiscardAll:
                    checkModify = false;
                    break;
                case MainWindow::ConfirmSaveResult::Discard:
                    break;
            }
        }

        if (save && !saveModifiedDocumentForCloseWithoutBlockingGui(*gdoc)) {
            failedSaves++;
        }
    }

    if (failedSaves > 0) {
        QMessageBox box(
            QMessageBox::Warning,
            QObject::tr("%1 Document(s) not saved").arg(QString::number(failedSaves)),
            QObject::tr("Some documents could not be saved. Cancel closing?"),
            QMessageBox::Discard | QMessageBox::Cancel,
            mainWindow);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setEscapeButton(QMessageBox::Cancel);
        if (auto* discard = box.button(QMessageBox::Discard)) {
            discard->setText(QObject::tr("Close Without Saving"));
        }
        const int ret = box.exec();
        if (ret == QMessageBox::Cancel) {
            return false;
        }
    }

    if (close) {
        App::GetApplication().closeAllDocuments();
    }

    return true;
}

void recomputeWithoutBlockingGui(App::Document& document, const char* coalescingKey)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    command.recompute = App::DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = coalescingKey;
    command.recompute->options = 0;
    submitAndWait(document, std::move(command));
}

void saveAsWithoutBlockingGui(App::Document& document, const char* path)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Save;
    command.save = App::DocumentCommandSavePayload {};
    command.save->targetPath = path;
    command.save->saveAs = true;
    submitAndWait(document, std::move(command));
}

App::DocumentSaveOutcome saveWithOutcomeWithoutBlockingGui(App::Document& document)
{
    waitForExecutionLaneIdle(document);
    return invokeOnOwnerWorkerWhilePumpingGui(
        [&] { return document.saveWithOutcome(); });
}

App::DocumentSaveOutcome saveAsWithOutcomeWithoutBlockingGui(App::Document& document,
                                                             const char* path,
                                                             const bool overwrite)
{
    waitForExecutionLaneIdle(document);
    return invokeOnOwnerWorkerWhilePumpingGui(
        [&] { return document.saveAsWithOutcome(path, overwrite); });
}

SharedPresentationCommitResult commitSharedPresentationWithoutBlockingGui(
    Document& guiDocument,
    SharedPresentationCommitRequest request,
    SharedPresentationCommitCallbacks callbacks)
{
    App::Document* const appDocument = guiDocument.getDocument();
    auto allowedGuiWrites = request.presentationWrites;
    std::sort(allowedGuiWrites.begin(), allowedGuiWrites.end());
    allowedGuiWrites.erase(
        std::unique(allowedGuiWrites.begin(), allowedGuiWrites.end()),
        allowedGuiWrites.end());
    std::vector<App::CollaborationAtomicPresentationWrite> allowedAppWrites;
    allowedAppWrites.reserve(allowedGuiWrites.size());
    for (const auto& write : allowedGuiWrites) {
        allowedAppWrites.push_back({write.stableObjectIdentity, write.propertyName});
    }
    callbacks.serialize =
        [appDocument, allowedAppWrites = std::move(allowedAppWrites)](
            SharedPresentationCommitWork&& work,
            SharedPresentationCommitCompletion&& complete) mutable {
            invokeOnOwnerWorkerWhilePumpingGui([&] {
                return SharedPresentationSerializeTestAccess::serializeAtomic(
                    appDocument->collaborationService(),
                    allowedAppWrites,
                    [&] {
                        work();
                        complete({true, {}});
                    });
            });
        };
    return guiDocument.commitSharedPresentation(std::move(request), std::move(callbacks));
}

CollaborationCompatibilityMutationOutcome executeCompatibilityMutationWithoutBlockingGui(
    Document& guiDocument,
    CollaborationCompatibilityMutationDeclaration declaration,
    CollaborationCompatibilityMutationCallback callback)
{
    App::Document* const appDocument = guiDocument.getDocument();
    return runOnDocumentOwnerWhilePumpingGui(
        *appDocument,
        [&] {
            return guiDocument.executeCompatibilityMutation(
                std::move(declaration),
                std::move(callback));
        });
}

App::DocumentCommitResult commitCompatibilityMutationWithoutBlockingGui(
    App::Document& document,
    App::CollaborationCompatibilityMutation mutation,
    std::function<void()> callback)
{
    return runOnDocumentOwnerWhilePumpingGui(
        document,
        [&] {
            return document.collaborationService().commitCompatibilityMutation(
                std::move(mutation),
                std::move(callback));
        });
}

}  // namespace Gui::Test
