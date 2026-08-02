// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperation.h"
#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

class DocumentCollaborationService;

/** Canonical pointer-free fields produced by PreparedEdit contract validation. */
struct AppExport PreparedEditCanonicalContract
{
    std::vector<DocumentRevisionObservation> expectedRevisions;
    std::vector<DocumentRevisionKey> readSet;
    std::vector<DocumentRevisionKey> writeSet;
    std::vector<DocumentRevisionPublicationRequest> publicationEffects;
};

/**
 * Validate and canonicalize the pointer-free metadata captured before a
 * detached operation is submitted to the executor.
 */
[[nodiscard]] AppExport PreparedEditCanonicalContract validatePreparedEditMetadata(
    std::string_view operationId,
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::string_view operationType,
    std::vector<DocumentRevisionObservation> expectedRevisions,
    std::vector<DocumentRevisionKey> readSet,
    std::vector<DocumentRevisionKey> writeSet,
    std::vector<DocumentRevisionPublicationRequest> publicationEffects,
    std::string_view provenance);

/**
 * Validate the immutable portion of a proposed prepared edit and return its
 * dependency and publication fields in deterministic key order.
 *
 * This function does not construct a PreparedEdit or grant commit authority.
 */
[[nodiscard]] AppExport PreparedEditCanonicalContract validatePreparedEditContract(
    std::string_view operationId,
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::string_view operationType,
    std::vector<DocumentRevisionObservation> expectedRevisions,
    std::vector<DocumentRevisionKey> readSet,
    std::vector<DocumentRevisionKey> writeSet,
    std::vector<DocumentRevisionPublicationRequest> publicationEffects,
    std::string_view provenance,
    const CollaborativeOperation& operation);

/**
 * Immutable prepared work bound to one document instance and lifecycle epoch.
 *
 * The dependency sets and publication effects are frozen by the App-owned
 * preparation boundary before this value is created. They are not supplied or
 * re-derived by the final mutation batch. This value stores no live document
 * or object pointer.
 */
class AppExport PreparedEdit
{
public:
    class ConstructionKey
    {
    private:
        ConstructionKey() = default;
        ConstructionKey(const ConstructionKey&) = delete;
        ConstructionKey(ConstructionKey&&) = delete;
        ConstructionKey& operator=(const ConstructionKey&) = delete;
        ConstructionKey& operator=(ConstructionKey&&) = delete;
        friend class DocumentCollaborationService;
    };

    PreparedEdit(ConstructionKey,
                 std::uint64_t adapterRegistrationId,
                 std::string operationId,
                 DocumentInstanceId documentInstanceId,
                 DocumentLifecycleEpoch lifecycleEpoch,
                 std::string operationType,
                 std::vector<DocumentRevisionObservation> expectedRevisions,
                 std::vector<DocumentRevisionKey> readSet,
                 std::vector<DocumentRevisionKey> writeSet,
                 std::vector<DocumentRevisionPublicationRequest> publicationEffects,
                 std::string provenance,
                 std::unique_ptr<const CollaborativeOperation> operation);

    PreparedEdit() = delete;
    PreparedEdit(const PreparedEdit&) = delete;
    PreparedEdit(PreparedEdit&&) noexcept = default;
    PreparedEdit& operator=(const PreparedEdit&) = delete;
    PreparedEdit& operator=(PreparedEdit&&) = delete;

    [[nodiscard]] const std::string& operationId() const noexcept;
    [[nodiscard]] DocumentInstanceId documentInstanceId() const noexcept;
    [[nodiscard]] DocumentLifecycleEpoch lifecycleEpoch() const noexcept;
    [[nodiscard]] const std::string& operationType() const noexcept;
    [[nodiscard]] const std::vector<DocumentRevisionObservation>&
    expectedRevisions() const noexcept;
    [[nodiscard]] const std::vector<DocumentRevisionKey>& readSet() const noexcept;
    [[nodiscard]] const std::vector<DocumentRevisionKey>& writeSet() const noexcept;
    [[nodiscard]] const std::vector<DocumentRevisionPublicationRequest>&
    publicationEffects() const noexcept;
    /** Opaque diagnostic origin; provenance never authorizes a mutation. */
    [[nodiscard]] const std::string& provenance() const noexcept;
    [[nodiscard]] const CollaborativeOperation& operation() const noexcept;

private:
    friend class DocumentCollaborationService;

    [[nodiscard]] std::uint64_t adapterRegistrationId() const noexcept;

    std::uint64_t _adapterRegistrationId;
    std::string _operationId;
    DocumentInstanceId _documentInstanceId;
    DocumentLifecycleEpoch _lifecycleEpoch;
    std::string _operationType;
    std::vector<DocumentRevisionObservation> _expectedRevisions;
    std::vector<DocumentRevisionKey> _readSet;
    std::vector<DocumentRevisionKey> _writeSet;
    std::vector<DocumentRevisionPublicationRequest> _publicationEffects;
    std::string _provenance;
    std::unique_ptr<const CollaborativeOperation> _operation;
};

}  // namespace App
