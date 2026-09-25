// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PresentationRevision.h"

#include <FCGlobal.h>

#include <string>

namespace Gui
{

/** Lifecycle of one GUI-owned presentation revision stream. */
enum class DocumentPresentationState
{
    Committed,
    Pending,
    Preparing,
    Applying,
    Stalled,
    Error
};

/** Pointer-free observation of the current presentation lifecycle boundary. */
struct GuiExport DocumentPresentationStatus
{
    PresentationRevision revision;
    DocumentPresentationState state {DocumentPresentationState::Committed};
    std::string statusMessage;
    std::string errorMessage;

    [[nodiscard]] bool terminalError() const noexcept
    {
        return state == DocumentPresentationState::Error;
    }

    [[nodiscard]] bool inFlight() const noexcept
    {
        return state == DocumentPresentationState::Pending
            || state == DocumentPresentationState::Preparing
            || state == DocumentPresentationState::Applying
            || state == DocumentPresentationState::Stalled;
    }
};

GuiExport const char* documentPresentationStateName(
    DocumentPresentationState state) noexcept;

}  // namespace Gui
