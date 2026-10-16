// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentPresentationCache.h"
#include "PresentationDelta.h"

#include <FCGlobal.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace Gui
{

/** Record of one GUI-thread apply pump turn. */
struct GuiExport PresentationApplyPumpResult
{
    std::size_t slicesApplied {0};
    std::vector<std::uint64_t> sliceDurationsMicros;
    bool stagingComplete {false};
    bool committedRevision {false};
};

/**
 * Incrementally applies pointer-free presentation packets on the GUI thread.
 *
 * Superseded staging revisions are coalesced so only the committed revision and
 * the newest staging revision are retained. Work is divided into bounded CPU
 * slices (approximately four milliseconds per pump by default).
 */
class GuiExport PresentationApplyScheduler
{
public:
    explicit PresentationApplyScheduler(DocumentPresentationCache& cache);
    ~PresentationApplyScheduler();

    PresentationApplyScheduler(const PresentationApplyScheduler&) = delete;
    PresentationApplyScheduler& operator=(const PresentationApplyScheduler&) = delete;

    /**
     * Queue one presentation packet for incremental application.
     * Stale packets are rejected using the same rules as the cache commit path.
     */
    void enqueue(PresentationDelta&& delta);

    /**
     * Apply as many staging slices as fit within @p budgetMs of CPU time.
     * Returns the number of slices applied and per-slice durations for tests.
     */
    [[nodiscard]] PresentationApplyPumpResult pump(int budgetMs = 4);

    [[nodiscard]] bool hasStagingWork() const;

    [[nodiscard]] std::optional<PresentationRevision> stagingRevision() const;

private:
    enum class ApplySliceKind
    {
        Tree,
        Properties,
        Selection,
        RenderBuffer
    };

    struct ApplySlicePlan
    {
        ApplySliceKind kind;
        std::size_t renderBufferIndex {0};
    };

    void rebuildSlicePlan();
    void applySlice(const ApplySlicePlan& slice);
    void touchBuffer(const PresentationRenderBuffer& buffer) const;

    DocumentPresentationCache& _cache;
    std::optional<PresentationDelta> _stagingPacket;
    PresentationDelta _stagingBuild;
    class SoSeparator* _stagingCoinRoot {nullptr};
    std::vector<ApplySlicePlan> _slicePlan;
    std::size_t _nextSlice {0};
    mutable std::mutex _mutex;
};

}  // namespace Gui
