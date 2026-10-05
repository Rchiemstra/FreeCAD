// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionIngress.h"

#include "Application.h"
#include "Document.h"
#include "MainWindow.h"
#include "Utilities.h"

#include <App/Application.h>
#include <App/DocumentCommandHandle.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentWouldBlock.h>
#include <App/Property.h>
#include <App/RecoverySnapshot.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/PyObjectBase.h>

#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

FC_LOG_LEVEL_INIT("Gui", true, true)

namespace Gui
{
namespace
{

DocumentRecomputeSubmitRequest makeRecomputeRequestFromObjects(
    const std::vector<App::DocumentObject*>& objects,
    const bool force,
    const int options)
{
    DocumentRecomputeSubmitRequest request;
    request.force = force;
    request.options = options;
    request.featureIds.reserve(objects.size());
    for (auto* object : objects) {
        if (object && object->isAttachedToDocument()) {
            request.featureIds.emplace_back(object->getNameInDocument());
        }
    }
    return request;
}

std::string buildRecomputeCoalescingKey(const DocumentRecomputeSubmitRequest& request)
{
    std::string key;
    key += request.force ? "force;" : "normal;";
    key += "options=" + std::to_string(request.options) + ";";
    if (!request.featureIds.empty()) {
        key += "features=";
        auto featureIds = request.featureIds;
        std::sort(featureIds.begin(), featureIds.end());
        for (const auto& featureId : featureIds) {
            key += featureId + ";";
        }
    }
    return key;
}

void scheduleDocumentCommandStatusRefresh(
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId)
{
    auto commandHandle = std::make_shared<App::DocumentCommandHandle>(commandId, documentIdentity);
    scheduleGuiSingleShot(50, [documentIdentity, commandId, commandHandle = std::move(commandHandle)] {
        const auto snapshot = commandHandle->status();
        if (!snapshot.terminal()) {
            scheduleDocumentCommandStatusRefresh(documentIdentity, commandId);
            return;
        }
        if (snapshot.state == App::DocumentCommandState::Failed) {
            FC_ERR("Document recompute "
                   << App::documentCommandStateName(snapshot.state) << ": "
                   << (snapshot.diagnostic.empty() ? "no diagnostic was provided"
                                                   : snapshot.diagnostic));
        }
    });
}

QString blockedReasonMessage(
    App::Document& document,
    const App::DocumentCommandSubmitOutcome& outcome)
{
    if (!outcome.diagnostic.empty()) {
        return QString::fromStdString(outcome.diagnostic);
    }
    const auto documentLabel = QString::fromUtf8(document.getName());
    switch (outcome.result) {
        case App::DocumentCommandSubmitResult::Busy:
            return QCoreApplication::translate(
                "Gui::DocumentExecutionIngress",
                "Document '%1' is busy executing model work. Try again when the active "
                "operation finishes.")
                .arg(documentLabel);
        case App::DocumentCommandSubmitResult::Closed:
            return QCoreApplication::translate(
                "Gui::DocumentExecutionIngress",
                "Document '%1' is closed.")
                .arg(documentLabel);
        case App::DocumentCommandSubmitResult::Conflict:
            return QCoreApplication::translate(
                "Gui::DocumentExecutionIngress",
                "Document '%1' identity conflict; reopen the document and retry.")
                .arg(documentLabel);
        case App::DocumentCommandSubmitResult::Unsupported:
            return QCoreApplication::translate(
                "Gui::DocumentExecutionIngress",
                "Recompute is not supported for document '%1' on this path.")
                .arg(documentLabel);
        case App::DocumentCommandSubmitResult::Accepted:
            break;
    }

    if (!outcome.diagnostic.empty()) {
        return QString::fromStdString(outcome.diagnostic);
    }
    return QCoreApplication::translate(
        "Gui::DocumentExecutionIngress",
        "Document '%1' rejected the recompute request (%2).")
        .arg(documentLabel)
        .arg(QString::fromUtf8(
            App::documentCommandSubmitResultName(outcome.result)));
}

}  // namespace

App::DocumentCommand makeDocumentRecomputeCommand(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    command.document = document.executionHandle().identity();
    command.recompute = App::DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = buildRecomputeCoalescingKey(request);
    command.recompute->options = request.options;
    command.recompute->featureIds = request.featureIds;
    // Public GUI/App facade recomputes declare snapshot-based foreign reads.
    command.recompute->declaresCrossDocumentSnapshots = true;
    return command;
}

App::DocumentCommand makeDocumentSaveAsCommand(
    App::Document& document,
    std::string targetPath,
    const bool overwrite,
    std::string expectedDestinationSha256)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Save;
    command.document = document.executionHandle().identity();
    command.save = App::DocumentCommandSavePayload {};
    command.save->targetPath = std::move(targetPath);
    command.save->overwrite = overwrite;
    command.save->expectedDestinationSha256 = std::move(expectedDestinationSha256);
    command.save->saveAs = true;
    return command;
}

App::DocumentCommandSubmitOutcome submitDocumentSaveAs(
    App::Document& document,
    std::string targetPath,
    const bool overwrite,
    std::string expectedDestinationSha256)
{
    return document.executionHandle().trySubmit(makeDocumentSaveAsCommand(
        document, std::move(targetPath), overwrite, std::move(expectedDestinationSha256)));
}

App::DocumentCommand makeDocumentKindCommand(
    App::Document& document,
    const App::DocumentCommandKind kind,
    const int steps)
{
    App::DocumentCommand command;
    command.kind = kind;
    command.document = document.executionHandle().identity();
    if (kind == App::DocumentCommandKind::Undo || kind == App::DocumentCommandKind::Redo) {
        command.transaction = App::DocumentCommandTransactionPayload {};
        command.transaction->steps = steps > 0 ? steps : 1;
    }
    return command;
}

App::DocumentCommandSubmitOutcome submitDocumentKindCommand(
    App::Document& document,
    const App::DocumentCommandKind kind,
    const int steps)
{
    return document.executionHandle().trySubmit(makeDocumentKindCommand(document, kind, steps));
}

App::DocumentCommand makeDocumentPropertyEditCommand(
    App::Document& document,
    std::vector<App::DocumentCommandPropertyValue> propertyValues)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Edit;
    command.document = document.executionHandle().identity();
    command.edit = App::DocumentCommandEditPayload {};
    command.edit->operationId = "gui.property-editor";
    command.edit->provenance = "Gui::PropertyItem::setPropertyValue";
    command.edit->propertyValues = std::move(propertyValues);
    return command;
}

std::optional<std::string> copyPropertyValueFromPythonRhs(
    const App::Property& schema,
    const std::string& pythonRhs)
{
    if (pythonRhs.empty()) {
        return std::nullopt;
    }

    std::unique_ptr<App::Property> staged(
        static_cast<App::Property*>(schema.getTypeId().createInstance()));
    if (!staged) {
        return std::nullopt;
    }

    try {
        Base::PyGILStateLocker gil;
        std::string code = "__freecad_property_editor_rhs = ";
        code += pythonRhs;
        Base::Interpreter().runString(code.c_str());
        Py::Module mainModule(PyImport_AddModule("__main__"));
        Py::Dict mainDict(mainModule.getDict());
        Py::Object rhs = mainDict.getItem("__freecad_property_editor_rhs");
        staged->setPyObject(rhs.ptr());
    }
    catch (const Base::Exception& exception) {
        FC_ERR("Failed to copy property value for lane edit: " << exception.what());
        return std::nullopt;
    }
    catch (const std::exception& exception) {
        FC_ERR("Failed to copy property value for lane edit: " << exception.what());
        return std::nullopt;
    }
    catch (...) {
        FC_ERR("Failed to copy property value for lane edit: unknown exception");
        return std::nullopt;
    }

    std::ostringstream stream(std::ios::out | std::ios::binary);
    staged->dumpToStream(stream, 1);
    return stream.str();
}

namespace
{

void schedulePropertyEditCommandCompletion(
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId)
{
    auto commandHandle = std::make_shared<App::DocumentCommandHandle>(commandId, documentIdentity);
    scheduleGuiSingleShot(50, [documentIdentity, commandId, commandHandle = std::move(commandHandle)] {
        const auto snapshot = commandHandle->status();
        if (!snapshot.terminal()) {
            schedulePropertyEditCommandCompletion(documentIdentity, commandId);
            return;
        }
        if (snapshot.state == App::DocumentCommandState::Failed) {
            FC_ERR("Document Edit "
                   << App::documentCommandStateName(snapshot.state) << ": "
                   << (snapshot.diagnostic.empty() ? "no diagnostic was provided"
                                                   : snapshot.diagnostic));
        }
    });
}

}  // namespace

App::DocumentCommandSubmitOutcome submitDocumentPropertyEdit(
    App::Document& document,
    std::vector<App::DocumentCommandPropertyValue> propertyValues)
{
    if (propertyValues.empty()) {
        App::DocumentCommandSubmitOutcome outcome;
        outcome.result = App::DocumentCommandSubmitResult::Unsupported;
        outcome.diagnostic = "property edit command has no values";
        return outcome;
    }

    const auto outcome = document.executionHandle().trySubmit(
        makeDocumentPropertyEditCommand(document, std::move(propertyValues)));
    if (outcome.accepted() && outcome.commandId != 0) {
        schedulePropertyEditCommandCompletion(
            document.executionHandle().identity(),
            outcome.commandId);
    }
    return outcome;
}

namespace
{

Gui::Document* findGuiDocumentByName(const char* appDocumentName)
{
    if (!appDocumentName || appDocumentName[0] == '\0') {
        return nullptr;
    }
    auto* appDocument = App::GetApplication().getDocument(appDocumentName);
    if (!appDocument) {
        return nullptr;
    }
    return Application::Instance->getDocument(appDocument);
}

}  // namespace

void reportGroupedUndoRedoUnsupported(App::Document& document, const bool undo)
{
    App::DocumentCommandSubmitOutcome outcome;
    outcome.result = App::DocumentCommandSubmitResult::Unsupported;
    outcome.diagnostic = undo
        ? "grouped undo across linked documents is not supported on the execution lane"
        : "grouped redo across linked documents is not supported on the execution lane";
    reportDocumentCommandSubmitBlocked(document, outcome);
}

void scheduleUndoRedoCommandCompletion(
    const char* appDocumentName,
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId,
    const App::DocumentCommandKind kind,
    std::shared_ptr<UndoRedoCompletionAnchor> anchor)
{
    auto commandHandle = std::make_shared<App::DocumentCommandHandle>(commandId, documentIdentity);
    const std::string documentName = appDocumentName ? appDocumentName : std::string {};
    scheduleGuiSingleShot(
        50,
        [documentName,
         documentIdentity,
         commandId,
         kind,
         anchor = std::move(anchor),
         commandHandle = std::move(commandHandle)]() mutable {
            if (anchor && !anchor->active.load(std::memory_order_acquire)) {
                return;
            }
            const auto snapshot = commandHandle->status();
            if (!snapshot.terminal()) {
                scheduleUndoRedoCommandCompletion(
                    documentName.c_str(),
                    documentIdentity,
                    commandId,
                    kind,
                    std::move(anchor));
                return;
            }
            if (anchor && !anchor->active.load(std::memory_order_acquire)) {
                return;
            }
            if (snapshot.state == App::DocumentCommandState::Failed) {
                FC_ERR("Document "
                       << App::documentCommandKindName(kind) << " "
                       << App::documentCommandStateName(snapshot.state) << ": "
                       << (snapshot.diagnostic.empty() ? "no diagnostic was provided"
                                                       : snapshot.diagnostic));
            }
            if (anchor
                && anchor->inFlightCommandId.load(std::memory_order_acquire) != commandId) {
                return;
            }
            if (auto* guiDocument = findGuiDocumentByName(documentName.c_str())) {
                guiDocument->finishExecutionLaneUndoRedo(kind, snapshot.state, commandId);
            }
        });
}

namespace
{

void resumePendingClose(Gui::Document* guiDocument,
                        Gui::Document::PendingLaneCloseKind kind,
                        bool discardUnsaved)
{
    if (!guiDocument || kind == Gui::Document::PendingLaneCloseKind::None) {
        return;
    }
    if (kind == Gui::Document::PendingLaneCloseKind::Document) {
        guiDocument->reissueDocumentClose();
        return;
    }
    if (discardUnsaved) {
        guiDocument->markSkipSaveOnClose();
    }
    if (auto* window = getMainWindow()) {
        window->close();
    }
}

}  // namespace

void scheduleSaveCommandCompletion(
    const char* appDocumentName,
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId)
{
    auto commandHandle = std::make_shared<App::DocumentCommandHandle>(commandId, documentIdentity);
    const std::string documentName = appDocumentName ? appDocumentName : std::string {};
    scheduleGuiSingleShot(
        50,
        [documentName, documentIdentity, commandId, commandHandle = std::move(commandHandle)]() mutable {
            const auto snapshot = commandHandle->status();
            // Stalled is not terminal, but leaving the pending flag set keeps
            // closeAllDocuments stuck forever (N2). Treat it as a failed close.
            if (!snapshot.terminal()
                && snapshot.state != App::DocumentCommandState::Stalled) {
                scheduleSaveCommandCompletion(
                    documentName.c_str(), documentIdentity, commandId);
                return;
            }
            auto* guiDocument = findGuiDocumentByName(documentName.c_str());
            const auto pendingKind = guiDocument ? guiDocument->consumePendingLaneClose()
                                                 : Gui::Document::PendingLaneCloseKind::None;
            if (snapshot.state == App::DocumentCommandState::Failed
                || snapshot.state == App::DocumentCommandState::Cancelled
                || snapshot.state == App::DocumentCommandState::Stalled) {
                FC_ERR("Document Save "
                       << App::documentCommandStateName(snapshot.state) << ": "
                       << (snapshot.diagnostic.empty() ? "no diagnostic was provided"
                                                       : snapshot.diagnostic));
                if (auto* window = getMainWindow()) {
                    window->showMessage(
                        QCoreApplication::translate(
                            "Gui::DocumentExecutionIngress",
                            "Save of document '%1' failed.")
                            .arg(QString::fromStdString(documentName)),
                        5000);
                }
                // Pending close was already cleared. Offer discard of this document
                // only, or of the application close, matching what was requested.
                if (pendingKind != Gui::Document::PendingLaneCloseKind::None) {
                    const QString docName =
                        QString::fromStdString(documentName);
                    const QString text = QCoreApplication::translate(
                        "Gui::DocumentExecutionIngress",
                        "Failed to save document '%1'. Close without saving?")
                        .arg(docName);
                    QMessageBox box(
                        QMessageBox::Warning,
                        QCoreApplication::translate(
                            "Gui::DocumentExecutionIngress",
                            "Save failed"),
                        text,
                        QMessageBox::Discard | QMessageBox::Cancel,
                        getMainWindow());
                    box.setDefaultButton(QMessageBox::Cancel);
                    box.setEscapeButton(QMessageBox::Cancel);
                    if (auto* discard = box.button(QMessageBox::Discard)) {
                        discard->setText(QCoreApplication::translate(
                            "Gui::DocumentExecutionIngress",
                            "Close Without Saving"));
                    }
                    if (box.exec() == QMessageBox::Discard) {
                        resumePendingClose(guiDocument, pendingKind, /*discardUnsaved=*/true);
                    }
                }
                return;
            }
            if (guiDocument) {
                guiDocument->finishExecutionLaneSave(snapshot.state);
                if (pendingKind != Gui::Document::PendingLaneCloseKind::None) {
                    resumePendingClose(guiDocument, pendingKind, /*discardUnsaved=*/false);
                    return;
                }
            }
            if (auto* window = getMainWindow()) {
                window->showMessage(
                    QCoreApplication::translate(
                        "Gui::DocumentExecutionIngress",
                        "Document '%1' saved.")
                        .arg(QString::fromStdString(documentName)),
                    3000);
            }
        });
}

namespace
{

bool writeRecoverySnapshotOnLaneOwnerThread(
    App::Document& document,
    const App::RecoverySnapshotSaveOptions& options)
{
    if (auto* lane = document.executionLane()) {
        return lane->dispatchToOwner([&] {
            return App::writeRecoverySnapshotToTransientDir(document, options);
        });
    }
    return App::writeRecoverySnapshotToTransientDir(document, options);
}

}  // namespace

void scheduleRecoverySnapshotWrite(
    App::Document& document,
    const App::RecoverySnapshotSaveOptions& options,
    std::function<void(bool written, std::exception_ptr failure)> onFinished)
{
    // L8: for lane documents, capture the lane shared_ptr at schedule time so the
    // worker thread never calls getDocument(name). This prevents:
    //   (a) a data race with document close on the name lookup,
    //   (b) a reused document name clearing another lane's recovery flag.
    // endRecoverySnapshotOwnerWork() is always called on the capturing lane, not
    // on whichever lane happens to own the name at that point later.
    //
    // For no-lane documents: the legacy path still looks up the document by name
    // from the worker (same behaviour as before). No lane recovery flag exists
    // in this case so there is no flag-identity bug.
    const App::RecoverySnapshotSaveOptions optionsCopy = options;
    App::DocumentExecutionLane* laneRaw = document.executionLane();
    std::shared_ptr<App::DocumentExecutionLane> lane =
        laneRaw ? laneRaw->shared_from_this() : nullptr;
    const std::string documentName = lane ? std::string {} : document.getName();
    if (lane) {
        lane->beginRecoverySnapshotOwnerWork();
    }

    std::thread([lane = std::move(lane),
                 documentName,
                 optionsCopy,
                 onFinished = std::move(onFinished)]() mutable {
        std::optional<bool> result;
        std::exception_ptr failure;
        try {
            if (lane) {
                // L8 fast path: dispatch the actual write to the lane owner thread.
                // dispatchToOwner holds a reference to the document through the
                // lane; the document is only closed from the GUI thread so it
                // remains valid for the duration of the dispatch.
                result = lane->dispatchToOwner(
                    [&lane, &optionsCopy]() -> bool {
                        return App::writeRecoverySnapshotToTransientDir(
                            lane->document(), optionsCopy);
                    },
                    /*releaseGilWhileWaiting=*/true,
                    /*allowWhileCommandActive=*/false);
            }
            else {
                // No-lane legacy path: look up document by name (as before).
                if (auto* doc = App::GetApplication().getDocument(documentName.c_str())) {
                    result = App::writeRecoverySnapshotToTransientDir(*doc, optionsCopy);
                }
                else {
                    result = false;
                }
            }
        }
        catch (...) {
            failure = std::current_exception();
        }
        if (lane) {
            lane->endRecoverySnapshotOwnerWork();
        }
        const bool written = result.value_or(false);
        scheduleGuiSingleShot(0, [onFinished = std::move(onFinished), written, failure]() mutable {
            onFinished(written, failure);
        });
    }).detach();
}

bool writeRecoverySnapshotAwaitingOwnerThread(
    App::Document& document,
    const App::RecoverySnapshotSaveOptions& options)
{
    App::DocumentExecutionLane* lane = document.executionLane();
    if (lane) {
        lane->beginRecoverySnapshotOwnerWork();
    }
    const auto endRecoveryOwnerWork = [&] {
        if (lane) {
            lane->endRecoverySnapshotOwnerWork();
        }
    };
    std::exception_ptr failure;
    std::atomic<bool> finished {false};
    std::optional<bool> result;
    std::thread worker([&] {
        try {
            result = writeRecoverySnapshotOnLaneOwnerThread(document, options);
        }
        catch (...) {
            failure = std::current_exception();
        }
        endRecoveryOwnerWork();
        finished.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (!finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(1ms);
    }
    if (!finished.load(std::memory_order_acquire)) {
        // L8: deadline exceeded — detach rather than block forever on join().
        // The worker will still call endRecoveryOwnerWork() when it eventually
        // exits (via dispatchToOwner timeout or lane shutdown), so the lane
        // recovery-work counter stays consistent.
        worker.detach();
        throw Base::RuntimeError(
            "recovery snapshot write did not finish before timeout");
    }
    worker.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    return result.value_or(false);
}

bool submitDocumentSaveAwaitingCompletion(
    App::Document& document,
    std::string* failureDiagnostic)
{
    const auto outcome = submitDocumentKindCommand(document, App::DocumentCommandKind::Save);
    if (!outcome.accepted()) {
        reportDocumentCommandSubmitBlocked(document, outcome);
        if (failureDiagnostic) {
            failureDiagnostic->clear();
        }
        return false;
    }

    App::DocumentCommandHandle commandHandle(
        outcome.commandId,
        document.executionHandle().identity());
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        const auto snapshot = commandHandle.status();
        if (!snapshot.terminal()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (snapshot.state == App::DocumentCommandState::Completed) {
            if (auto* guiDocument = findGuiDocumentByName(document.getName())) {
                guiDocument->finishExecutionLaneSave(snapshot.state);
            }
            if (failureDiagnostic) {
                failureDiagnostic->clear();
            }
            return true;
        }
        if (failureDiagnostic) {
            *failureDiagnostic = snapshot.diagnostic;
        }
        FC_ERR("Document Save "
               << App::documentCommandStateName(snapshot.state) << ": "
               << (snapshot.diagnostic.empty() ? "no diagnostic was provided"
                                               : snapshot.diagnostic));
        return false;
    }

    if (failureDiagnostic) {
        *failureDiagnostic = "document save did not finish before timeout";
    }
    FC_ERR("Document Save timed out waiting for completion");
    return false;
}

bool submitDocumentSave(App::Document& document)
{
    const auto outcome = submitDocumentKindCommand(document, App::DocumentCommandKind::Save);
    if (outcome.accepted()) {
        scheduleSaveCommandCompletion(
            document.getName(),
            document.executionHandle().identity(),
            outcome.commandId);
        reportDocumentSaveAdmitted(document);
        // Admission is not completion — callers must not claim the write finished.
        return false;
    }
    reportDocumentCommandSubmitBlocked(document, outcome);
    return false;
}

void reportDocumentCommandSubmitBlocked(
    App::Document& document,
    const App::DocumentCommandSubmitOutcome& outcome,
    const bool quiet)
{
    if (outcome.result == App::DocumentCommandSubmitResult::Accepted) {
        return;
    }

    const auto message = blockedReasonMessage(document, outcome);
    if (auto* window = getMainWindow()) {
        window->showMessage(message, 5000);
    }
    if (quiet) {
        Base::Console().warning("{}\n", message.toUtf8().constData());
    }
    else {
        Base::Console().message("{}\n", message.toUtf8().constData());
    }
}

void reportDocumentSaveDeferred(App::Document& document)
{
    const auto message = QCoreApplication::translate(
        "Gui::DocumentExecutionIngress",
        "Save of document '%1' was deferred because a recompute is still running.")
        .arg(QString::fromUtf8(document.getName()));
    if (auto* window = getMainWindow()) {
        window->showMessage(message, 5000);
    }
    Base::Console().message("{}\n", message.toUtf8().constData());
}

void reportDocumentSaveAdmitted(App::Document& document)
{
    const auto message = QCoreApplication::translate(
        "Gui::DocumentExecutionIngress",
        "Save of document '%1' was admitted and is in progress.")
        .arg(QString::fromUtf8(document.getName()));
    if (auto* window = getMainWindow()) {
        window->showMessage(message, 5000);
    }
    Base::Console().message("{}\n", message.toUtf8().constData());
}

bool submitDocumentClose(App::Document& document, const bool allowApplicationClose)
{
    if (!App::DocumentWouldBlock::isGuiThread() || !document.executionLane()) {
        return App::GetApplication().closeDocument(&document);
    }
    if (!document.isClosable()) {
        return false;
    }
    const auto outcome =
        submitDocumentKindCommand(document, App::DocumentCommandKind::Close);
    if (!outcome.accepted()) {
        reportDocumentCommandSubmitBlocked(document, outcome);
        // Never terminate the lane thread. When the watchdog has marked the
        // lane Stalled, the user can skip this document. Automatic orphan
        // cleanup must not pop a modal or start an application close (H3).
        const auto* lane = document.executionLane();
        const bool stalled = lane && lane->isWatchdogStalled();
        const auto action =
            App::DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(stalled);
        if (action
            == App::DocumentExecutionClosePolicy::UnresponsiveLaneAction::RequestProcessExit) {
            if (!allowApplicationClose) {
                Base::Console().warning(
                    "Document '{}' execution is stalled; automatic cleanup will not "
                    "close the application.\n",
                    document.getName());
                return false;
            }
            if (auto* window = getMainWindow()) {
                const auto choice = QMessageBox::question(
                    window,
                    QCoreApplication::translate(
                        "Gui::DocumentExecutionIngress",
                        "Document execution stalled"),
                    QCoreApplication::translate(
                        "Gui::DocumentExecutionIngress",
                        "%1\n\n"
                        "Unsaved work in this stalled document may be lost if you "
                        "skip it now. Every other open document will still receive "
                        "a normal save prompt.\n\n"
                        "Skip this document and close everything else normally?")
                        .arg(QString::fromUtf8(
                            App::DocumentExecutionClosePolicy::unresponsiveLaneGuidance())),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No);
                if (choice == QMessageBox::Yes) {
                    if (auto* guiDocument = Application::Instance
                            ? Application::Instance->getDocument(&document)
                            : nullptr) {
                        guiDocument->markSkipSaveOnClose();
                    }
                    window->close();
                }
            }
        }
    }
    return outcome.accepted();
}

bool documentExecutionLaneBusy(const App::Document& document)
{
    const auto* lane = document.executionLane();
    return lane && !lane->isIdle();
}

bool shouldReadCommittedPresentation(const App::Document& document)
{
    auto* guiDocument = Application::Instance
        ? Application::Instance->getDocument(&document)
        : nullptr;
    if (!guiDocument) {
        return false;
    }
    if (!guiDocument->presentationCache().current().has_value()) {
        return false;
    }
    return documentExecutionLaneBusy(document)
        || guiDocument->prefersCommittedPresentation();
}

std::optional<std::string> committedPresentationPropertyDisplayValue(
    const App::Document& document,
    const App::DocumentObject& object,
    const char* propertyName)
{
    if (!propertyName || propertyName[0] == '\0'
        || !shouldReadCommittedPresentation(document)) {
        return std::nullopt;
    }
    auto* guiDocument = Application::Instance
        ? Application::Instance->getDocument(&document)
        : nullptr;
    if (!guiDocument) {
        return std::nullopt;
    }
    const auto presentation = guiDocument->presentationCache().current();
    if (!presentation) {
        return std::nullopt;
    }
    const std::string stableIdentity = document.collaborationObjectIdentity(object);
    for (const auto& propertyValue : presentation->properties) {
        if (propertyValue.stableObjectIdentity == stableIdentity
            && propertyValue.propertyName == propertyName) {
            return propertyValue.displayValue;
        }
    }
    return std::nullopt;
}

bool prepareDocumentForImmediateSave(App::Document& document, const bool skipRecomputeIfAlreadyFlagged)
{
    if (documentExecutionLaneBusy(document)) {
        App::DocumentCommandSubmitOutcome outcome;
        outcome.result = App::DocumentCommandSubmitResult::Busy;
        outcome.diagnostic = "document execution lane is busy";
        reportDocumentCommandSubmitBlocked(document, outcome);
        return false;
    }

    if (!skipRecomputeIfAlreadyFlagged && document.mustExecute()) {
        App::AutoTransaction trans(&document, "Recompute");
        const auto outcome = submitDocumentRecompute(document);
        if (outcome.result == App::DocumentCommandSubmitResult::Busy) {
            reportDocumentCommandSubmitBlocked(document, outcome);
            return false;
        }
        if (!outcome.accepted()) {
            reportDocumentCommandSubmitBlocked(document, outcome);
            return false;
        }
        reportDocumentSaveDeferred(document);
        return false;
    }

    return true;
}

App::DocumentCommandSubmitOutcome submitDocumentRecompute(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request)
{
    auto command = makeDocumentRecomputeCommand(document, request);
    const auto outcome = document.executionHandle().trySubmit(std::move(command));
    if (outcome.accepted() && outcome.commandId != 0) {
        scheduleDocumentCommandStatusRefresh(
            document.executionHandle().identity(),
            outcome.commandId);
    }
    return outcome;
}

bool requestDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects,
    const bool force,
    const int options,
    const bool quiet)
{
    const auto outcome =
        submitDocumentRecompute(document, makeRecomputeRequestFromObjects(objects, force, options));
    if (outcome.accepted()) {
        return true;
    }

    reportDocumentCommandSubmitBlocked(document, outcome, quiet);
    return false;
}

}  // namespace Gui
