// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionIngress.h"

#include "Application.h"
#include "Document.h"
#include "MainWindow.h"

#include <App/Application.h>
#include <App/DocumentCommandHandle.h>
#include <App/AutoTransaction.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <Base/Console.h>

#include <QApplication>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

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
    QTimer::singleShot(50, qApp, [documentIdentity, commandId, commandHandle = std::move(commandHandle)] {
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
    return command;
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
    QTimer::singleShot(
        50,
        qApp,
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

void scheduleSaveCommandCompletion(
    const char* appDocumentName,
    App::DocumentRevisionIdentityBinding documentIdentity,
    App::DocumentCommandId commandId)
{
    auto commandHandle = std::make_shared<App::DocumentCommandHandle>(commandId, documentIdentity);
    const std::string documentName = appDocumentName ? appDocumentName : std::string {};
    QTimer::singleShot(
        50,
        qApp,
        [documentName, documentIdentity, commandId, commandHandle = std::move(commandHandle)]() mutable {
            const auto snapshot = commandHandle->status();
            if (!snapshot.terminal()) {
                scheduleSaveCommandCompletion(
                    documentName.c_str(), documentIdentity, commandId);
                return;
            }
            auto* guiDocument = findGuiDocumentByName(documentName.c_str());
            if (snapshot.state == App::DocumentCommandState::Failed
                || snapshot.state == App::DocumentCommandState::Cancelled) {
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
                return;
            }
            if (guiDocument) {
                guiDocument->finishExecutionLaneSave(snapshot.state);
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

bool submitDocumentSave(App::Document& document)
{
    const auto outcome = submitDocumentKindCommand(document, App::DocumentCommandKind::Save);
    if (outcome.accepted()) {
        scheduleSaveCommandCompletion(
            document.getName(),
            document.executionHandle().identity(),
            outcome.commandId);
        reportDocumentSaveDeferred(document);
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
        Base::Console().warning("%s\n", message.toUtf8().constData());
    }
    else {
        Base::Console().message("%s\n", message.toUtf8().constData());
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
    Base::Console().message("%s\n", message.toUtf8().constData());
}

bool documentExecutionLaneBusy(const App::Document& document)
{
    const auto* lane = document.executionLane();
    return lane && !lane->isIdle();
}

bool shouldReadCommittedPresentation(const App::Document& document)
{
    if (!documentExecutionLaneBusy(document)) {
        return false;
    }
    auto* guiDocument = Application::Instance
        ? Application::Instance->getDocument(&document)
        : nullptr;
    if (!guiDocument) {
        return false;
    }
    return guiDocument->presentationCache().current().has_value();
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
