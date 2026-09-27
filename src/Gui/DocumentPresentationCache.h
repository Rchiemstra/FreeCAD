// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentPresentationState.h"
#include "PresentationDelta.h"

#include <App/CollaborationRegistry.h>

#include <FCGlobal.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace Gui
{

class PresentationApplyScheduler;

/** Result of attempting to install one immutable presentation revision. */
enum class PresentationCommitResult
{
    Accepted,
    Unbound,
    StaleDocumentIdentity,
    StaleSequence,
    StaleSourceRevision,
    InvalidRevision
};

GuiExport const char* presentationCommitResultName(PresentationCommitResult result) noexcept;

/**
 * GUI-owned store for the last committed presentation revision.
 *
 * Committed tree, property, selection, and render-buffer data are swapped
 * atomically as one immutable snapshot. Observation APIs are thread-safe and
 * do not expose live model pointers.
 */
class GuiExport DocumentPresentationCache
{
public:
    DocumentPresentationCache();
    DocumentPresentationCache(const DocumentPresentationCache&) = delete;
    DocumentPresentationCache& operator=(const DocumentPresentationCache&) = delete;

    void bindDocumentIdentity(App::DocumentInstanceId documentInstanceId,
                              App::DocumentLifecycleEpoch lifecycleEpoch);

    [[nodiscard]] std::optional<App::DocumentRevisionIdentityBinding>
    documentIdentity() const;

    /**
     * Install @p delta when it is newer than the committed revision.
     * Rejects stale packets by document instance, lifecycle epoch, presentation
     * sequence, and source model revision.
     */
    [[nodiscard]] PresentationCommitResult tryCommit(PresentationDelta&& delta);

    /** Copy of the last committed presentation packet, if any. */
    [[nodiscard]] std::optional<PresentationDelta> current() const;

    /** Pointer-free lifecycle observation; does not advance work. */
    [[nodiscard]] DocumentPresentationStatus status() const;

    [[nodiscard]] PresentationSequence committedSequence() const noexcept;

private:
    friend class PresentationApplyScheduler;

    [[nodiscard]] bool acceptsPacket(const PresentationRevision& revision) const noexcept;

    void publishObservation(DocumentPresentationStatus observation);

    mutable std::mutex _mutex;
    std::optional<App::DocumentRevisionIdentityBinding> _documentIdentity;
    PresentationSequence _committedSequence {0};
    App::DocumentRevision _committedSourceRevision {0};
    std::shared_ptr<const PresentationDelta> _committed;
    DocumentPresentationStatus _status;
    std::atomic<PresentationSequence> _committedSequenceAtomic {0};
};

}  // namespace Gui
