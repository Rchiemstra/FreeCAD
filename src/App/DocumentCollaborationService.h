// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperationRegistry.h"
#include "DocumentCommitCoordinator.h"
#include "EditSession.h"
#include "PreparedEdit.h"

#include <FCGlobal.h>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace App
{

class Document;

struct AppExport CollaborationEditSnapshot
{
    std::string sessionId;
    DocumentInstanceId documentInstanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    std::vector<DocumentRevisionObservation> revisions;
};

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
    DocumentCollaborationService(const DocumentCollaborationService&) = delete;
    DocumentCollaborationService& operator=(const DocumentCollaborationService&) = delete;

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
    [[nodiscard]] DocumentCommitResult commitEdit(const std::string& sessionId,
                                                  const PreparedEdit& edit);

private:
    friend class Document;

    explicit DocumentCollaborationService(Document& document);

    [[nodiscard]] EditSession requireActiveSession(const std::string& sessionId) const;
    [[nodiscard]] CollaborationEditSnapshot snapshotForEditOnDocumentThread(
        const std::string& sessionId,
        std::vector<DocumentRevisionKey> keys) const;
    [[nodiscard]] PreparedEdit prepareEditOnDocumentThread(
        const std::string& sessionId,
        std::string operationId,
        const CollaborativeOperationIntent& intent,
        std::string provenance);
    [[nodiscard]] DocumentCommitResult commitEditOnDocumentThread(
        const std::string& sessionId,
        const PreparedEdit& edit);

    Document& _document;
    DocumentCommitCoordinator _coordinator;
    mutable std::mutex _sessionMutex;
    std::unordered_map<std::string, EditSession> _sessions;
};

}  // namespace App
