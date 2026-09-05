// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

#include <atomic>
#include <string>
#include <vector>

namespace App
{

class Document;
class DocumentCollaborationService;
class PreparedEdit;
struct CollaborationRollbackResult;
struct TransactionName;
enum class CollaborationCompatibilityRecomputePolicy;
namespace Internal
{
class DocumentCommitCoordinatorTestAccess;
}

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
    friend class Internal::DocumentCommitCoordinatorTestAccess;

    explicit DocumentCommitCoordinator(Document& document) noexcept;

    DocumentCommitCoordinator(const DocumentCommitCoordinator&) = delete;
    DocumentCommitCoordinator& operator=(const DocumentCommitCoordinator&) = delete;

    [[nodiscard]] Document& document() const noexcept;
    [[nodiscard]] int openCompatibilityTransaction(TransactionName name, int transactionId);
    [[nodiscard]] int openMutationTransaction(std::string name, int transactionId);
    [[nodiscard]] int setActiveCompatibilityTransaction(TransactionName name, int transactionId);
    void commitCompatibilityTransaction();
    void abortCompatibilityTransaction();
    [[nodiscard]] bool undoCompatibilityTransaction(int transactionId);
    [[nodiscard]] bool redoCompatibilityTransaction(int transactionId);
    void clearCompatibilityTransactionHistory();
    [[nodiscard]] bool commitApplicationTransaction();
    void abortApplicationTransaction();
    [[nodiscard]] int openNativeCommitTransaction(std::string name,
                                                  bool retainUndoHistory);
    [[nodiscard]] bool commitNativeCommitTransaction(bool retainUndoHistory);
    [[nodiscard]] CollaborationRollbackResult rollbackNativeCommitTransaction(
        bool preservePendingRecompute) noexcept;
    [[nodiscard]] DocumentCommitResult commit(const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitRecompute(const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitCompatibility(
        const PreparedEdit& edit,
        bool structural);
    [[nodiscard]] DocumentCommitResult commitCompatibilityWithPolicy(
        const PreparedEdit& edit,
        bool structural,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitCompatibilityWithOptions(
        const PreparedEdit& edit,
        bool structural,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitWithPreparationPolicy(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility);
    [[nodiscard]] DocumentCommitResult commitWithPreparationPolicyAndRecompute(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitWithPreparationPolicyAndOptions(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy recomputePolicy,
        bool retainUndoHistory = true);
    [[nodiscard]] DocumentCommitResult commitOnDocumentThread(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility);
    [[nodiscard]] DocumentCommitResult commitOnDocumentThreadWithRecompute(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitOnDocumentThreadWithOptions(
        const PreparedEdit& edit,
        bool requireDetachedPreparationSupport,
        bool structuralCompatibility,
        CollaborationCompatibilityRecomputePolicy recomputePolicy,
        bool retainUndoHistory);
    [[nodiscard]] DocumentCommitResult commitDerivedRecomputeInActiveTransaction(
        const PreparedEdit& edit);

    using PostReservationTestHook = void (*)();
    static std::atomic<PostReservationTestHook> _postReservationTestHook;

    Document& _document;
};

}  // namespace App
