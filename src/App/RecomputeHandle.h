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

/**
 * Pointer-safe public observation and control handle for one document recompute.
 *
 * The handle never owns a document and becomes a terminal cancelled view when
 * the document closes. poll() and wait() are owner-thread compatibility pumps:
 * detached work runs outside the document, while capture and commit remain on
 * the caller/GUI thread through DocumentRecomputeCoordinator.
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
    [[nodiscard]] DocumentRecomputeSnapshot wait(
        std::chrono::milliseconds timeout = std::chrono::minutes(6));

private:
    [[nodiscard]] Document* document() const noexcept;
    DocumentRecomputeSnapshot closedDocumentSnapshot() const;
    void finalizeIfTerminal(Document& document,
                            const DocumentRecomputeSnapshot& snapshot);

    std::unique_ptr<DocumentWeakPtrT> _document;
    DocumentRecomputeId _id {0};
};

AppExport const char* documentRecomputeStateName(DocumentRecomputeState state) noexcept;
AppExport const char* documentRecomputeFeatureStateName(
    DocumentRecomputeFeatureState state) noexcept;

}  // namespace App
