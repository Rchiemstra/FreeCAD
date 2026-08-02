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

namespace App
{

class Document;

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
    UnknownModel
};

/** Pointer-free declaration for the short synchronous compatibility path. */
struct AppExport CollaborationCompatibilityMutation
{
    CollaborationCompatibilityScope scope {CollaborationCompatibilityScope::UnknownModel};
    std::string objectName;
    std::string stableObjectIdentity;
};

using CollaborationCompatibilityCallback = std::function<void()>;

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

    [[nodiscard]] CollaborationEditSnapshot snapshotForEdit(
        const std::string& sessionId,
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] PreparedEdit prepareEdit(const std::string& sessionId,
                                           std::string operationId,
                                           const CollaborativeOperationIntent& intent,
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
    [[nodiscard]] DocumentCommitResult serializeCompatibilityCallback(
        CollaborationCompatibilityCallback callback);

private:
    friend class Document;
    friend class Internal::DocumentCollaborationServiceTestAccess;

    class LifecyclePin;

    explicit DocumentCollaborationService(Document& document);

    [[nodiscard]] LifecyclePin pinDocumentAccess() const;
    [[nodiscard]] std::shared_ptr<Internal::CollaborationServiceLifetimeGate>
    lifetimeGate() const;

    [[nodiscard]] EditSession requireActiveSession(const std::string& sessionId) const;
    [[nodiscard]] CollaborationEditSnapshot snapshotForEditOnDocumentThread(
        const std::string& sessionId,
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] PreparedEdit prepareEditOnDocumentThread(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance);
    [[nodiscard]] PreparedEditExecutionId prepareEditAsyncOnDocumentThread(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance);
    [[nodiscard]] std::optional<CollaborationPreparedEditResult>
    takePreparedEditOnDocumentThread(const std::string& sessionId,
                                     PreparedEditExecutionId executionId);
    [[nodiscard]] DocumentCommitResult commitEditOnDocumentThread(
        const std::string& sessionId,
        const PreparedEdit& edit);
    [[nodiscard]] DocumentCommitResult commitCompatibilityMutationOnDocumentThread(
        CollaborationCompatibilityMutation mutation,
        CollaborationCompatibilityCallback callback);
    [[nodiscard]] DocumentCommitResult serializeCompatibilityCallbackOnDocumentThread(
        CollaborationCompatibilityCallback callback);

    Document& _document;
    DocumentCommitCoordinator _coordinator;

    struct PendingDetachedPreparation
    {
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
