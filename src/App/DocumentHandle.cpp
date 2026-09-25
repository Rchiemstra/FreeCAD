// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentHandle.h"

#include <utility>

namespace App
{

const char* documentCommandKindName(const DocumentCommandKind kind) noexcept
{
    switch (kind) {
        case DocumentCommandKind::Recompute:
            return "Recompute";
        case DocumentCommandKind::Edit:
            return "Edit";
        case DocumentCommandKind::Undo:
            return "Undo";
        case DocumentCommandKind::Redo:
            return "Redo";
        case DocumentCommandKind::Save:
            return "Save";
        case DocumentCommandKind::Close:
            return "Close";
    }
    return "Unknown";
}

const char* documentCommandSubmitResultName(const DocumentCommandSubmitResult result) noexcept
{
    switch (result) {
        case DocumentCommandSubmitResult::Accepted:
            return "Accepted";
        case DocumentCommandSubmitResult::Busy:
            return "Busy";
        case DocumentCommandSubmitResult::Closed:
            return "Closed";
        case DocumentCommandSubmitResult::Conflict:
            return "Conflict";
        case DocumentCommandSubmitResult::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

const char* documentCommandStateName(const DocumentCommandState state) noexcept
{
    switch (state) {
        case DocumentCommandState::Running:
            return "Running";
        case DocumentCommandState::Cancelling:
            return "Cancelling";
        case DocumentCommandState::Completed:
            return "Completed";
        case DocumentCommandState::Failed:
            return "Failed";
        case DocumentCommandState::Cancelled:
            return "Cancelled";
        case DocumentCommandState::Stalled:
            return "Stalled";
    }
    return "Unknown";
}

struct DocumentHandle::State
{
    DocumentRevisionIdentityBinding identity;
};

DocumentCommandHandle::DocumentCommandHandle(DocumentCommandId id,
                                             DocumentRevisionIdentityBinding document)
    : _id(id)
    , _document(document)
{}

bool DocumentCommandHandle::valid() const noexcept
{
    return _id != 0 && _document.documentInstanceId != 0;
}

DocumentCommandId DocumentCommandHandle::id() const noexcept
{
    return _id;
}

DocumentRevisionIdentityBinding DocumentCommandHandle::document() const noexcept
{
    return _document;
}

DocumentCommandSnapshot DocumentCommandHandle::status() const
{
    DocumentCommandSnapshot snapshot;
    snapshot.id = _id;
    snapshot.document = _document;
    if (!valid()) {
        snapshot.state = DocumentCommandState::Failed;
        snapshot.diagnostic = "invalid command handle";
        return snapshot;
    }

    snapshot.state = DocumentCommandState::Running;
    snapshot.diagnostic = "document execution lane not active";
    return snapshot;
}

bool DocumentCommandHandle::cancel(std::string reason)
{
    if (!valid()) {
        return false;
    }

    (void)reason;
    return false;
}

RecomputeCommandHandle::RecomputeCommandHandle(DocumentCommandHandle command,
                                               DocumentRecomputeId recomputeId)
    : _command(command)
    , _recomputeId(recomputeId)
{}

bool RecomputeCommandHandle::valid() const noexcept
{
    return _command.valid() && _recomputeId != 0;
}

const DocumentCommandHandle& RecomputeCommandHandle::command() const noexcept
{
    return _command;
}

DocumentRecomputeId RecomputeCommandHandle::recomputeId() const noexcept
{
    return _recomputeId;
}

DocumentCommandSnapshot RecomputeCommandHandle::status() const
{
    auto snapshot = _command.status();
    if (valid()) {
        DocumentRecomputeSnapshot recompute;
        recompute.id = _recomputeId;
        recompute.state = DocumentRecomputeState::Running;
        recompute.diagnostic = snapshot.diagnostic;
        snapshot.recompute = recompute;
    }
    return snapshot;
}

bool RecomputeCommandHandle::cancel(std::string reason)
{
    return _command.cancel(std::move(reason));
}

DocumentHandle::DocumentHandle()
    : _state(std::make_shared<State>())
{}

DocumentHandle::DocumentHandle(DocumentRevisionIdentityBinding identity)
    : _state(std::make_shared<State>(State {identity}))
{}

DocumentHandle::~DocumentHandle() = default;

bool DocumentHandle::valid() const noexcept
{
    return _state && _state->identity.documentInstanceId != 0;
}

DocumentInstanceId DocumentHandle::instanceId() const noexcept
{
    return _state ? _state->identity.documentInstanceId : 0;
}

DocumentLifecycleEpoch DocumentHandle::lifecycleEpoch() const noexcept
{
    return _state ? _state->identity.lifecycleEpoch : 0;
}

DocumentRevisionIdentityBinding DocumentHandle::identity() const noexcept
{
    return _state ? _state->identity : DocumentRevisionIdentityBinding {};
}

DocumentCommandSubmitOutcome DocumentHandle::trySubmit(DocumentCommand command)
{
    DocumentCommandSubmitOutcome outcome;
    if (!valid()) {
        outcome.result = DocumentCommandSubmitResult::Closed;
        outcome.diagnostic = "document handle is not bound to a live document";
        return outcome;
    }

    if (command.document != identity()) {
        outcome.result = DocumentCommandSubmitResult::Conflict;
        outcome.diagnostic = "command document identity does not match handle";
        return outcome;
    }

    outcome.result = DocumentCommandSubmitResult::Unsupported;
    outcome.diagnostic = "document execution lane is not active";
    return outcome;
}

}  // namespace App
