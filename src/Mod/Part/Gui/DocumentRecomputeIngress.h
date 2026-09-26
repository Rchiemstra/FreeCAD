// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommand.h>
#include <App/DocumentHandle.h>
#include <Base/Console.h>

#include <QMessageBox>
#include <QObject>
#include <QString>
#include <QWidget>

#include <string>
#include <utility>

namespace PartGui
{

inline QString documentRecomputeSubmitUserMessage(const App::DocumentCommandSubmitOutcome& outcome)
{
    switch (outcome.result) {
        case App::DocumentCommandSubmitResult::Accepted:
            return {};
        case App::DocumentCommandSubmitResult::Busy:
            return QObject::tr("Document recompute is already in progress.");
        case App::DocumentCommandSubmitResult::Closed:
            return QObject::tr("Document is closed or unavailable.");
        case App::DocumentCommandSubmitResult::Conflict:
            return QObject::tr("Document recompute was rejected due to a revision conflict.");
        case App::DocumentCommandSubmitResult::Unsupported:
            return QObject::tr("Document recompute is not supported in the current state.");
    }
    return QObject::tr("Document recompute could not be submitted.");
}

inline bool reportDocumentRecomputeSubmitOutcome(
    const App::DocumentCommandSubmitOutcome& outcome,
    QWidget* parent = nullptr)
{
    if (outcome.accepted()) {
        return true;
    }

    QString message = documentRecomputeSubmitUserMessage(outcome);
    if (!outcome.diagnostic.empty()) {
        message += QLatin1Char('\n') + QString::fromStdString(outcome.diagnostic);
    }

    Base::Console().warning("%s\n", message.toUtf8().constData());
    if (parent) {
        QMessageBox::warning(parent, QObject::tr("Recompute"), message);
    }
    return false;
}

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::Document& document,
    std::string coalescingKey = {})
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    auto handle = document.executionHandle();
    command.document = handle.identity();
    command.recompute = App::DocumentCommandRecomputePayload {};
    if (coalescingKey.empty()) {
        coalescingKey = std::string("gui-recompute:") + document.getName();
    }
    command.recompute->coalescingKey = std::move(coalescingKey);
    return handle.trySubmit(std::move(command));
}

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::DocumentObject& object,
    std::string coalescingKey = {})
{
    return trySubmitDocumentRecompute(*object.getDocument(), std::move(coalescingKey));
}

inline App::DocumentCommandSubmitOutcome trySubmitActiveDocumentRecompute(
    std::string coalescingKey = {})
{
    App::Document* document = App::GetApplication().getActiveDocument();
    if (!document) {
        App::DocumentCommandSubmitOutcome outcome;
        outcome.result = App::DocumentCommandSubmitResult::Closed;
        return outcome;
    }
    return trySubmitDocumentRecompute(*document, std::move(coalescingKey));
}

inline bool submitDocumentRecomputeOrReport(
    App::Document& document,
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(document, std::move(coalescingKey)),
        parent);
}

inline bool submitDocumentRecomputeOrReport(
    App::DocumentObject& object,
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(object, std::move(coalescingKey)),
        parent);
}

inline bool submitActiveDocumentRecomputeOrReport(
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitActiveDocumentRecompute(std::move(coalescingKey)),
        parent);
}

}  // namespace PartGui
