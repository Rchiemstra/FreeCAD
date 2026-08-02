// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

#include <string>
#include <vector>

namespace App
{

class Document;
class DocumentCollaborationService;
class PreparedEdit;

/** The terminal outcome of one prepared-edit commit attempt. */
enum class DocumentCommitStatus
{
    Committed,
    StaleDocument,
    InvalidPreparedEdit,
    Conflict,
    Cancelled,
    Unsupported,
    Busy,
    ApplyFailed,
    RecomputeFailed,
    PostconditionFailed,
    PublicationFailed,
    RollbackFailed
};

[[nodiscard]] AppExport const char* documentCommitStatusName(DocumentCommitStatus status) noexcept;

/** Pointer-free structured result returned by DocumentCommitCoordinator. */
struct AppExport DocumentCommitResult
{
    DocumentCommitStatus status {DocumentCommitStatus::InvalidPreparedEdit};
    std::string operationId;
    std::string message;
    std::vector<DocumentRevisionConflict> conflicts;
    std::vector<DocumentRevisionObservation> publishedRevisions;

    [[nodiscard]] bool committed() const noexcept
    {
        return status == DocumentCommitStatus::Committed;
    }
};

/**
 * The serialized commit point for one live App::Document.
 *
 * Prepared work may be produced concurrently, but this class never applies
 * more than one mutation batch at a time.  Lifecycle and semantic revisions
 * are revalidated under that per-document serialization boundary immediately
 * before the native transaction starts.
 */
class AppExport DocumentCommitCoordinator
{
private:
    friend class DocumentCollaborationService;

    explicit DocumentCommitCoordinator(Document& document) noexcept;

    DocumentCommitCoordinator(const DocumentCommitCoordinator&) = delete;
    DocumentCommitCoordinator& operator=(const DocumentCommitCoordinator&) = delete;

    [[nodiscard]] Document& document() const noexcept;
    [[nodiscard]] DocumentCommitResult commit(const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitOnDocumentThread(const PreparedEdit& edit);

    Document& _document;
};

}  // namespace App
