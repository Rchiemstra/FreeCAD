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

#include <algorithm>
#include <string>
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

/** Match Gui::DocumentExecutionIngress recompute coalescing (force/options/features). */
inline std::string buildDocumentRecomputeCoalescingKey(
    const std::vector<App::DocumentObject*>& objects,
    const bool force,
    const int options)
{
    std::string key;
    key += force ? "force;" : "normal;";
    key += "options=" + std::to_string(options) + ";";
    if (!objects.empty()) {
        key += "features=";
        std::vector<std::string> featureIds;
        featureIds.reserve(objects.size());
        for (auto* object : objects) {
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

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::Document& document,
    const std::vector<App::DocumentObject*>& objects,
    const bool force,
    const int options)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    auto handle = document.executionHandle();
    command.document = handle.identity();
    command.recompute = App::DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey =
        buildDocumentRecomputeCoalescingKey(objects, force, options);
    command.recompute->options = options;
    command.recompute->featureIds.reserve(objects.size());
    for (auto* object : objects) {
        if (object && object->isAttachedToDocument()) {
            command.recompute->featureIds.emplace_back(object->getNameInDocument());
        }
    }
    return handle.trySubmit(std::move(command));
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
        coalescingKey = buildDocumentRecomputeCoalescingKey({}, false, 0);
    }
    command.recompute->coalescingKey = std::move(coalescingKey);
    return handle.trySubmit(std::move(command));
}

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::DocumentObject& object,
    std::string coalescingKey = {})
{
    static_cast<void>(coalescingKey);
    return trySubmitDocumentRecompute(*object.getDocument(), {&object}, false, 0);
}

inline App::DocumentCommandSubmitOutcome trySubmitActiveDocumentRecompute(
    std::string coalescingKey = {})
{
    static_cast<void>(coalescingKey);
    App::Document* document = App::GetApplication().getActiveDocument();
    if (!document) {
        App::DocumentCommandSubmitOutcome outcome;
        outcome.result = App::DocumentCommandSubmitResult::Closed;
        return outcome;
    }
    return trySubmitDocumentRecompute(*document);
}

inline bool submitDocumentRecomputeOrReport(
    App::Document& document,
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    static_cast<void>(coalescingKey);
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(document),
        parent);
}

inline bool submitDocumentRecomputeOrReport(
    App::DocumentObject& object,
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    static_cast<void>(coalescingKey);
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitDocumentRecompute(object),
        parent);
}

inline bool submitActiveDocumentRecomputeOrReport(
    QWidget* parent = nullptr,
    std::string coalescingKey = {})
{
    static_cast<void>(coalescingKey);
    return reportDocumentRecomputeSubmitOutcome(
        trySubmitActiveDocumentRecompute(),
        parent);
}

}  // namespace PartGui
