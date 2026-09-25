// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionIngress.h"

#include "MainWindow.h"

#include <App/Document.h>
#include <Base/Console.h>

#include <QApplication>
#include <QApplication>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace Gui
{
namespace
{

std::string buildRecomputeCoalescingKey(const DocumentRecomputeSubmitRequest& request)
{
    std::string key;
    key += request.force ? "force;" : "normal;";
    key += "options=" + std::to_string(request.options) + ";";
    if (!request.objects.empty()) {
        key += "features=";
        std::vector<std::string> featureIds;
        featureIds.reserve(request.objects.size());
        for (auto* object : request.objects) {
            if (object && object->isAttachedToDocument()) {
                featureIds.emplace_back(object->getNameInDocument());
            }
        }
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
    command.recompute->featureIds.reserve(request.objects.size());
    for (auto* object : request.objects) {
        if (object && object->isAttachedToDocument()) {
            command.recompute->featureIds.emplace_back(object->getNameInDocument());
        }
    }
    return command;
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

bool trySubmitDocumentRecompute(
    App::Document& document,
    const DocumentRecomputeSubmitRequest& request)
{
    auto command = makeDocumentRecomputeCommand(document, request);
    const auto outcome = document.executionHandle().trySubmit(std::move(command));
    if (outcome.accepted() && outcome.commandId != 0) {
        scheduleDocumentCommandStatusRefresh(document.executionHandle().identity(), outcome.commandId);
        return true;
    }
    return false;
}

bool requestDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects,
    const bool force,
    const int options,
    const bool quiet)
{
    DocumentRecomputeSubmitRequest request;
    request.objects = objects;
    request.force = force;
    request.options = options;

    auto command = makeDocumentRecomputeCommand(document, request);
    const auto outcome = document.executionHandle().trySubmit(std::move(command));
    if (outcome.accepted()) {
        if (outcome.commandId != 0) {
            scheduleDocumentCommandStatusRefresh(
                document.executionHandle().identity(),
                outcome.commandId);
        }
        return true;
    }

    reportDocumentCommandSubmitBlocked(document, outcome, quiet);
    return false;
}

}  // namespace Gui
