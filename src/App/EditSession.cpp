// SPDX-License-Identifier: LGPL-2.1-or-later

#include "EditSession.h"

#include <stdexcept>
#include <utility>

using namespace App;

EditSession::EditSession(std::string sessionId,
                         std::string actorId,
                         DocumentInstanceId documentInstanceId)
    : _sessionId(std::move(sessionId))
    , _actorId(std::move(actorId))
    , _documentInstanceId(documentInstanceId)
{
    if (_sessionId.empty() || _actorId.empty() || _documentInstanceId == 0) {
        throw std::invalid_argument("edit session identity fields must be nonempty");
    }
}

const std::string& EditSession::sessionId() const noexcept
{
    return _sessionId;
}

const std::string& EditSession::actorId() const noexcept
{
    return _actorId;
}

DocumentInstanceId EditSession::documentInstanceId() const noexcept
{
    return _documentInstanceId;
}

EditSessionStatus EditSession::status() const noexcept
{
    return _status;
}

const std::optional<std::string>& EditSession::cancellationReason() const noexcept
{
    return _cancellationReason;
}
