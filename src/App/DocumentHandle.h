// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"
#include "DocumentCommandHandle.h"

#include <FCGlobal.h>

#include <memory>
#include <type_traits>

namespace App
{

class Document;
class DocumentExecutionLane;

/**
 * Thread-safe, GUI-facing document reference.
 *
 * DocumentHandle is the only production document reference that may cross GUI
 * thread boundaries. It carries stable CollaborationRegistry identities and
 * never exposes a live App::Document pointer.
 */
class AppExport DocumentHandle
{
public:
    DocumentHandle();
    explicit DocumentHandle(DocumentRevisionIdentityBinding identity);

    DocumentHandle(const DocumentHandle&) = default;
    DocumentHandle& operator=(const DocumentHandle&) = default;
    DocumentHandle(DocumentHandle&&) noexcept = default;
    DocumentHandle& operator=(DocumentHandle&&) noexcept = default;
    ~DocumentHandle();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] DocumentInstanceId instanceId() const noexcept;
    [[nodiscard]] DocumentLifecycleEpoch lifecycleEpoch() const noexcept;
    [[nodiscard]] DocumentRevisionIdentityBinding identity() const noexcept;

    /**
     * Admit one model command without waiting.
     *
     * Returns Accepted, Busy, Closed, Conflict, or Unsupported. Only one model
     * command is admitted per document. Identical recompute commands may share
     * the active handle when their coalescing keys match.
     */
    [[nodiscard]] DocumentCommandSubmitOutcome trySubmit(DocumentCommand command);

private:
    friend class DocumentExecutionLane;

    struct State
    {
        DocumentRevisionIdentityBinding identity;
        std::weak_ptr<DocumentExecutionLane> lane;
    };

    explicit DocumentHandle(std::shared_ptr<State> state);

    std::shared_ptr<State> _state;
};

static_assert(!std::is_constructible_v<DocumentHandle, Document*>);
static_assert(!std::is_constructible_v<DocumentHandle, const Document*>);
static_assert(std::is_copy_constructible_v<DocumentHandle>);

}  // namespace App
