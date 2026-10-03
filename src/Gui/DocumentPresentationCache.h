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

class SoSeparator;

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
 * atomically as one immutable snapshot. The companion Coin presentation root
 * is activated in the same critical section so readers never observe a split
 * cache/root pair. Observation APIs are thread-safe and do not expose live
 * model pointers.
 */
class GuiExport DocumentPresentationCache
{
public:
    DocumentPresentationCache();
    ~DocumentPresentationCache();
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

    /**
     * Atomically install @p delta and activate @p coinRoot as the committed
     * presentation scene root. Ownership of @p coinRoot transfers to the cache
     * (ref taken). The previous committed root is unref'd after the swap.
     */
    [[nodiscard]] PresentationCommitResult tryCommitWithCoinRoot(
        PresentationDelta&& delta,
        SoSeparator* coinRoot);

    /** Copy of the last committed presentation packet, if any. */
    [[nodiscard]] std::optional<PresentationDelta> current() const;

    /** Detached Coin presentation root for the committed revision, or null. */
    [[nodiscard]] SoSeparator* committedCoinRoot() const noexcept;

    /** Pointer-free lifecycle observation; does not advance work. */
    [[nodiscard]] DocumentPresentationStatus status() const;

    [[nodiscard]] PresentationSequence committedSequence() const noexcept;

private:
    friend class PresentationApplyScheduler;

    [[nodiscard]] bool acceptsPacket(const PresentationRevision& revision) const noexcept;

    void publishObservation(DocumentPresentationStatus observation);
    PresentationCommitResult tryCommitLocked(PresentationDelta&& delta);
    void activateCoinRootLocked(SoSeparator* coinRoot);

    mutable std::mutex _mutex;
    std::optional<App::DocumentRevisionIdentityBinding> _documentIdentity;
    PresentationSequence _committedSequence {0};
    App::DocumentRevision _committedSourceRevision {0};
    std::shared_ptr<const PresentationDelta> _committed;
    SoSeparator* _committedCoinRoot {nullptr};
    DocumentPresentationStatus _status;
    std::atomic<PresentationSequence> _committedSequenceAtomic {0};
};

}  // namespace Gui
