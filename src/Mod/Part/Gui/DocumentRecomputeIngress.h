// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommand.h>
#include <App/DocumentObject.h>
#include <Base/Console.h>

#include <Gui/DocumentExecutionIngress.h>

#include <QMessageBox>
#include <QObject>
#include <QString>
#include <QWidget>

#include <utility>
#include <vector>

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

/**
 * Admit recompute through Gui::DocumentExecutionIngress so coalescing keys
 * include force, options, and sorted feature ids (contract: identical join
 * only when key AND options match).
 */
inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects = {},
    const bool force = false,
    const int options = 0)
{
    Gui::DocumentRecomputeSubmitRequest request;
    request.force = force;
    request.options = options;
    request.featureIds.reserve(objects.size());
    for (auto* object : objects) {
        if (object && object->isAttachedToDocument() && object->getDocument() == &document) {
            request.featureIds.emplace_back(object->getNameInDocument());
        }
    }
    return Gui::submitDocumentRecompute(document, request);
}

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::DocumentObject& object,
    const bool force = false,
    const int options = 0)
{
    return trySubmitDocumentRecompute(*object.getDocument(), {&object}, force, options);
}

inline App::DocumentCommandSubmitOutcome trySubmitActiveDocumentRecompute(
    const std::vector<App::DocumentObject*>& objects = {},
    const bool force = false,
    const int options = 0)
{
    App::Document* document = App::GetApplication().getActiveDocument();
    if (!document) {
        App::DocumentCommandSubmitOutcome outcome;
        outcome.result = App::DocumentCommandSubmitResult::Closed;
        return outcome;
    }
    return trySubmitDocumentRecompute(*document, objects, force, options);
}

inline bool submitDocumentRecomputeOrReport(
    App::Document& document,
    QWidget* parent = nullptr,
    const std::vector<App::DocumentObject*>& objects = {},
    const bool force = false,
    const int options = 0)
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(document, objects, force, options),
        parent);
}

inline bool submitDocumentRecomputeOrReport(
    App::DocumentObject& object,
    QWidget* parent = nullptr,
    const bool force = false,
    const int options = 0)
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(object, force, options),
        parent);
}

inline bool submitActiveDocumentRecomputeOrReport(
    QWidget* parent = nullptr,
    const std::vector<App::DocumentObject*>& objects = {},
    const bool force = false,
    const int options = 0)
{
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitActiveDocumentRecompute(objects, force, options),
        parent);
}

}  // namespace PartGui
