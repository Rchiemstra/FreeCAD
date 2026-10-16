// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/CollaborationRegistry.h>
#include <App/DocumentRevisionIndex.h>

#include <FCGlobal.h>

#include <cstdint>

namespace Gui
{

using PresentationSequence = std::uint64_t;

/**
 * Immutable identity for one committed or staged GUI presentation revision.
 *
 * A revision is bound to one document instance and lifecycle epoch, carries a
 * monotonic presentation sequence, and records the source model revision that
 * produced it.
 */
struct GuiExport PresentationRevision
{
    App::DocumentInstanceId documentInstanceId {0};
    App::DocumentLifecycleEpoch lifecycleEpoch {0};
    PresentationSequence sequence {0};
    App::DocumentRevision sourceModelRevision {0};

    [[nodiscard]] bool valid() const noexcept
    {
        return documentInstanceId != 0;
    }

    [[nodiscard]] App::DocumentRevisionIdentityBinding documentIdentity() const noexcept
    {
        return {documentInstanceId, lifecycleEpoch};
    }
};

inline bool operator==(const PresentationRevision& left,
                       const PresentationRevision& right) noexcept
{
    return left.documentInstanceId == right.documentInstanceId
        && left.lifecycleEpoch == right.lifecycleEpoch && left.sequence == right.sequence
        && left.sourceModelRevision == right.sourceModelRevision;
}

inline bool operator!=(const PresentationRevision& left,
                       const PresentationRevision& right) noexcept
{
    return !(left == right);
}

}  // namespace Gui
