// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperationRegistry.h"
#include "DocumentCommitCoordinator.h"
#include "EditSession.h"
#include "PreparedEdit.h"
#include "PreparedEditExecutor.h"

#include <FCGlobal.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Gui
{
class Document;
}

namespace App
{

class Document;
class Application;
class DocumentRecomputeCoordinator;
struct RecoverySnapshotSaveOptions;

namespace Internal
{
class DocumentCollaborationServiceTestAccess;

struct CollaborationServiceLifetimeGate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::size_t activeAccesses {0};
    std::unordered_map<std::thread::id, std::size_t> accessOwners;
    bool sealed {false};
};
}  // namespace Internal

struct AppExport CollaborationEditSnapshot
{
    std::string sessionId;
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    std::vector<DocumentRevisionObservation> revisions;
};

struct AppExport CollaborationPreparedEditResult
{
    PreparedEditExecutionId executionId {0};
    PreparedEditExecutionStatus status {PreparedEditExecutionStatus::Failed};
    std::unique_ptr<PreparedEdit> preparedEdit;
    std::string diagnostic;
};

/** App-owned scope for one synchronous legacy model mutation. */
enum class CollaborationCompatibilityScope
{
    ObjectModel,
    UnknownModel,
    Structural
};

/** Recompute policy for the synchronous native compatibility boundary. */
enum class CollaborationCompatibilityRecomputePolicy
{
    Eager,
    Deferred
};

/** Pointer-free declaration for the short synchronous compatibility path. */
struct AppExport CollaborationCompatibilityMutation
{
    CollaborationCompatibilityScope scope {CollaborationCompatibilityScope::UnknownModel};
    std::string objectName;
    std::string stableObjectIdentity;
    std::string propertyName;
};

using CollaborationCompatibilityCallback = std::function<void()>;
using CollaborationCompatibilityPostcondition = std::function<bool()>;

/**
 * Options for the extended synchronous compatibility boundary.
 *
 * This is deliberately separate from CollaborationCompatibilityMutation so
 * the layout and exported call shape of the legacy declaration remain ABI
 * stable.
 */
struct AppExport CollaborationCompatibilityMutationOptions
{
    CollaborationCompatibilityRecomputePolicy recomputePolicy {
        CollaborationCompatibilityRecomputePolicy::Eager};
    CollaborationCompatibilityPostcondition postcondition;
};

/** Result of the service-owned native transaction commit point. */
struct AppExport CollaborationAtomicCommitPointResult
{
    bool committed {false};
    std::string diagnostic;
};

/**
 * Runs synchronously inside one native transaction and notification barrier.
 * The trusted Gui integration invokes the private exact-once commit step;
 * no transaction capability is handed to or retainable by this callback.
 */
using CollaborationAtomicCompatibilityCallback =
    std::function<void()>;

/**
 * The single Document-owned native collaboration facade.
 *
 * Sessions are advisory metadata. Callers submit pointer-free intent; only a
 * registered native adapter can derive a PreparedEdit and only this service
 * can construct or commit it.
 */
class AppExport DocumentCollaborationService
{
public:
    ~DocumentCollaborationService();

    DocumentCollaborationService(const DocumentCollaborationService&) = delete;
    DocumentCollaborationService& operator=(const DocumentCollaborationService&) = delete;

    // Borrowed owner access. The caller must already hold an external
    // Document lifetime guarantee and must not retain the reference across
    // closeDocument(); operational facade methods below are internally pinned.
    [[nodiscard]] Document& document() const noexcept;
    [[nodiscard]] EditSession beginEditSession(std::string actorId);
    [[nodiscard]] std::optional<EditSession> sessionStatus(
        const std::string& sessionId) const;
    [[nodiscard]] bool cancelEdit(const std::string& sessionId,
                                  std::string reason = "cancelled by caller");

    [[nodiscard]] CollaborationEditSnapshot captureSemanticRevisions(
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] CollaborationEditSnapshot snapshotForEdit(
        const std::string& sessionId,
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] PreparedEdit prepareEdit(const std::string& sessionId,
                                           std::string operationId,
                                           const CollaborativeOperationIntent& intent,
                                           std::string provenance);
    [[nodiscard]] PreparedEdit prepareEditWithExpectedRevisions(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::vector<DocumentRevisionObservation> expectedRevisions,
        std::string provenance);
    [[nodiscard]] PreparedEditExecutionId prepareEditAsync(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance);
    [[nodiscard]] std::optional<PreparedEditExecutionSnapshot> preparedEditStatus(
        PreparedEditExecutionId executionId) const;
    [[nodiscard]] bool cancelPreparedEdit(PreparedEditExecutionId executionId);
    [[nodiscard]] std::optional<CollaborationPreparedEditResult> takePreparedEdit(
        const std::string& sessionId,
        PreparedEditExecutionId executionId);
    [[nodiscard]] DocumentCommitResult commitEdit(const std::string& sessionId,
                                                  const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutation(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationWithPolicy(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationWithOptions(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback,
        CollaborationCompatibilityMutationOptions options);
    [[nodiscard]] DocumentCommitResult serializeCompatibilityCallback(
        CollaborationCompatibilityCallback callback);

private:
    friend class DocumentRecomputeCoordinator;
    friend class Gui::Document;
    friend class Application;
    friend class Document;
    friend class Internal::DocumentCollaborationServiceTestAccess;
    friend AppExport bool writeRecoverySnapshotToTransientDir(
        const Document& doc,
        const RecoverySnapshotSaveOptions& options);

    class AppExport LifecyclePin
    {
    public:
        explicit LifecyclePin(const DocumentCollaborationService& service);
        ~LifecyclePin();

        LifecyclePin(const LifecyclePin&) = delete;
        LifecyclePin& operator=(const LifecyclePin&) = delete;

        explicit operator bool() const noexcept;

    private:
        std::shared_ptr<Internal::CollaborationServiceLifetimeGate> _gate;
        bool _pinned {false};
    };

    explicit DocumentCollaborationService(Document& document);

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

    [[nodiscard]] LifecyclePin pinDocumentAccess() const;
    [[nodiscard]] std::shared_ptr<Internal::CollaborationServiceLifetimeGate>
    lifetimeGate() const;

    [[nodiscard]] EditSession requireActiveSession(const std::string& sessionId) const;
    [[nodiscard]] CollaborationEditSnapshot captureSemanticRevisionsOnDocumentThread(
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] CollaborationEditSnapshot snapshotForEditOnDocumentThread(
        const std::string& sessionId,
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] PreparedEdit prepareEditOnDocumentThread(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance,
        const std::vector<DocumentRevisionObservation>* expectedRevisionFence = nullptr);
    [[nodiscard]] PreparedEditExecutionId prepareEditAsyncOnDocumentThread(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance);
    [[nodiscard]] std::optional<CollaborationPreparedEditResult>
    takePreparedEditOnDocumentThread(const std::string& sessionId,
                                     PreparedEditExecutionId executionId,
                                     bool allowPendingRecompute = false);
    [[nodiscard]] std::optional<CollaborationPreparedEditResult>
    takeRecomputePreparedEdit(const std::string& sessionId,
                              PreparedEditExecutionId executionId);
    [[nodiscard]] DocumentCommitResult commitEditOnDocumentThread(
        const std::string& sessionId,
        const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitRecomputeEdit(
        const std::string& sessionId,
        const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitRecomputeEditOnDocumentThread(
        const std::string& sessionId,
        const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationOnDocumentThread(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationWithPolicyOnDocumentThread(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback,
        CollaborationCompatibilityRecomputePolicy recomputePolicy);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationWithOptionsOnDocumentThread(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback,
        CollaborationCompatibilityMutationOptions options);
    [[nodiscard]] DocumentCommitResult serializeCompatibilityCallbackOnDocumentThread(
        CollaborationCompatibilityCallback callback);
    [[nodiscard]] DocumentCommitResult serializeAtomicCompatibilityCallback(
        std::vector<CollaborationAtomicPresentationWrite> allowedWrites,
        CollaborationAtomicCompatibilityCallback callback);
    [[nodiscard]] DocumentCommitResult serializeAtomicCompatibilityCallbackOnDocumentThread(
        std::vector<CollaborationAtomicPresentationWrite> allowedWrites,
        CollaborationAtomicCompatibilityCallback callback);
    [[nodiscard]] CollaborationAtomicCommitPointResult
    commitAtomicCompatibilityTransaction();
    /**
     * Cancel every session and detach every executor job while the caller owns
     * the document lifecycle serialization boundary. Executor abandonment is
     * deliberately split out so no executor lock is taken under that boundary.
     */
    [[nodiscard]] std::vector<PreparedEditExecutionId>
    cancelAllForLifecycle(std::string reason);
    void abandonLifecyclePreparations(
        const std::vector<PreparedEditExecutionId>& executionIds) noexcept;

    Document& _document;
    DocumentCommitCoordinator _coordinator;
    bool _atomicCompatibilityActive {false};
    bool _atomicCompatibilityCommitInvoked {false};
    bool _atomicCompatibilityCommitted {false};
    std::thread::id _atomicCompatibilityOwner;
    std::vector<std::pair<std::string, std::string>> _atomicCompatibilityObjectFingerprint;

    struct PendingDetachedPreparation
    {
        enum class Backend
        {
            InProcess,
            IsolatedProcess
        };

        std::string sessionId;
        std::uint64_t adapterRegistrationId {0};
        std::string operationId;
        DocumentInstanceId documentInstanceId {0};
        DocumentLifecycleEpoch lifecycleEpoch {0};
        std::string operationType;
        std::vector<DocumentRevisionObservation> expectedRevisions;
        std::vector<DocumentRevisionKey> readSet;
        std::vector<DocumentRevisionKey> writeSet;
        std::vector<DocumentRevisionPublicationRequest> publicationEffects;
        std::string provenance;
        Backend backend {Backend::InProcess};
        CollaborativeOperationPreparation::IsolatedResultDecoder isolatedResultDecoder;
        CollaborativeOperationPreparation::IsolatedPublicationEffectDecoder
            isolatedPublicationEffectDecoder;
        bool collecting {false};
    };

    mutable std::mutex _sessionMutex;
    std::unordered_map<std::string, EditSession> _sessions;
    mutable std::mutex _preparationMutex;
    std::unordered_map<PreparedEditExecutionId, PendingDetachedPreparation>
        _pendingDetachedPreparations;

    using AsyncTestHook = void (*)();
    inline static std::atomic<AsyncTestHook> _postSubmitTestHook {nullptr};
    inline static std::atomic<AsyncTestHook> _postTakeResultTestHook {nullptr};
    inline static std::atomic<AsyncTestHook> _postCancelSessionTestHook {nullptr};
    inline static std::atomic<AsyncTestHook> _postLifecycleAdmissionTestHook {nullptr};
};

}  // namespace App
