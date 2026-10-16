// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationState.h"

namespace Gui
{

const char* documentPresentationStateName(const DocumentPresentationState state) noexcept
{
    switch (state) {
        case DocumentPresentationState::Committed:
            return "Committed";
        case DocumentPresentationState::Pending:
            return "Pending";
        case DocumentPresentationState::Preparing:
            return "Preparing";
        case DocumentPresentationState::Applying:
            return "Applying";
        case DocumentPresentationState::Stalled:
            return "Stalled";
        case DocumentPresentationState::Error:
            return "Error";
    }
    return "Unknown";
}

}  // namespace Gui
