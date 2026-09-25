// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommand.h>
#include <App/DocumentHandle.h>

#include <string>
#include <utility>

namespace PartGui
{

inline App::DocumentCommandSubmitOutcome trySubmitDocumentRecompute(
    App::Document& document,
    std::string coalescingKey = {})
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    const auto handle = document.executionHandle();
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

}  // namespace PartGui
