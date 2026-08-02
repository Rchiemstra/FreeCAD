// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborationRegistry.h"

#include <FCGlobal.h>

#include <optional>
#include <string>

namespace App
{

class DocumentCollaborationService;

enum class EditSessionStatus
{
    Active,
    Cancelled
};

/** Pointer-free advisory session metadata; never an ownership lease. */
class AppExport EditSession
{
public:
    EditSession(std::string sessionId,
                std::string actorId,
                DocumentInstanceId documentInstanceId);

    [[nodiscard]] const std::string& sessionId() const noexcept;
    [[nodiscard]] const std::string& actorId() const noexcept;
    [[nodiscard]] DocumentInstanceId documentInstanceId() const noexcept;
    [[nodiscard]] EditSessionStatus status() const noexcept;
    [[nodiscard]] const std::optional<std::string>& cancellationReason() const noexcept;

private:
    friend class DocumentCollaborationService;

    std::string _sessionId;
    std::string _actorId;
    DocumentInstanceId _documentInstanceId;
    EditSessionStatus _status {EditSessionStatus::Active};
    std::optional<std::string> _cancellationReason;
};

}  // namespace App
