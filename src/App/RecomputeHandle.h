// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentRecomputeCoordinator.h"

#include <FCGlobal.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace App
{

class Document;
class DocumentWeakPtrT;

namespace Internal
{
struct CollaborationServiceLifetimeGate;
}  // namespace Internal

/**
 * Pointer-safe public observation and control handle for one document recompute.
 *
 * The handle never owns a document and becomes a terminal cancelled view when
 * the document closes. poll() and status() are observation-only on every thread.
 * wait() advances work on the document owner thread only. Lane-owned plans
 * retain the lane's submission, publication and presentation lifecycle.
 */
class AppExport RecomputeHandle
{
public:
    RecomputeHandle(Document& document, DocumentRecomputeId id);
    ~RecomputeHandle();

    RecomputeHandle(const RecomputeHandle&) = delete;
    RecomputeHandle& operator=(const RecomputeHandle&) = delete;
    RecomputeHandle(RecomputeHandle&&) = delete;
    RecomputeHandle& operator=(RecomputeHandle&&) = delete;

    [[nodiscard]] DocumentRecomputeId id() const noexcept;
    [[nodiscard]] DocumentRecomputeSnapshot status();
    [[nodiscard]] bool poll();
    [[nodiscard]] bool cancel(
        std::string reason = "recompute cancelled by caller");

    /**
     * Advance the recompute until terminal publication or the timeout expires.
     *
     * For a lane-owned command, also finish its presentation before returning a
     * terminal result. A recursive wait from that command's submission, commit
     * or presentation callback throws DocumentWouldBlock; use status() there.
     *
     * The timeout bounds the number of polling rounds, not the duration of one
     * round. A plan whose snapshot reports ownerThreadFeatures > 0 runs those
     * features' execute() synchronously inside this call, and a single such
     * feature can exceed the timeout on its own. Check the snapshot before
     * relying on wait() for bounded latency.
     */
    [[nodiscard]] DocumentRecomputeSnapshot wait(
        std::chrono::milliseconds timeout = std::chrono::minutes(6));

private:
    [[nodiscard]] Document* document() const noexcept;
    DocumentRecomputeSnapshot closedDocumentSnapshot() const;
    void finalizeIfTerminal(Document& document,
                            const DocumentRecomputeSnapshot& snapshot);

    std::unique_ptr<DocumentWeakPtrT> _document;
    std::shared_ptr<Internal::CollaborationServiceLifetimeGate> _lifetimeGate;
    DocumentRecomputeId _id {0};
};

AppExport const char* documentRecomputeStateName(DocumentRecomputeState state) noexcept;
AppExport const char* documentRecomputeFeatureStateName(
    DocumentRecomputeFeatureState state) noexcept;

}  // namespace App
