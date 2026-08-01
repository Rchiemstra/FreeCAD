// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace App
{

class Document;

using DocumentInstanceId = std::uint64_t;
using DocumentLifecycleEpoch = std::uint64_t;

enum class DocumentLifecycleState
{
    Live,
    Closing,
    Closed
};

struct AppExport DocumentIdentity
{
    DocumentInstanceId instanceId {0};
    DocumentLifecycleEpoch lifecycleEpoch {0};
    DocumentLifecycleState state {DocumentLifecycleState::Closed};
};

constexpr bool operator==(const DocumentIdentity& left, const DocumentIdentity& right) noexcept
{
    return left.instanceId == right.instanceId && left.lifecycleEpoch == right.lifecycleEpoch
        && left.state == right.state;
}

constexpr bool operator!=(const DocumentIdentity& left, const DocumentIdentity& right) noexcept
{
    return !(left == right);
}

enum class DocumentIdentityValidation
{
    Valid,
    UnknownInstance,
    EpochMismatch,
    NotLive
};

/**
 * Tracks process-local document identities and lifecycle epochs.
 *
 * Closed documents remain addressable by instance ID for a bounded period so
 * prepared work can distinguish a closed document from an unknown one. Raw
 * document pointers are never retained after close, which prevents a reused
 * address from reviving the previous identity.
 */
class AppExport CollaborationRegistry
{
public:
    static constexpr std::size_t DefaultTombstoneCapacity = 1024;

    explicit CollaborationRegistry(std::size_t tombstoneCapacity = DefaultTombstoneCapacity);

    CollaborationRegistry(const CollaborationRegistry&) = delete;
    CollaborationRegistry& operator=(const CollaborationRegistry&) = delete;

    [[nodiscard]] DocumentIdentity registerDocument(const Document& document);

    [[nodiscard]] std::optional<DocumentIdentity> identity(const Document& document) const;
    [[nodiscard]] std::optional<DocumentIdentity> identity(DocumentInstanceId instanceId) const;

    [[nodiscard]] std::optional<DocumentIdentity> advanceEpoch(const Document& document);
    [[nodiscard]] std::optional<DocumentIdentity> markClosing(const Document& document);
    [[nodiscard]] std::optional<DocumentIdentity> closeDocument(const Document& document);

    [[nodiscard]] DocumentIdentityValidation validate(
        DocumentInstanceId instanceId,
        DocumentLifecycleEpoch lifecycleEpoch) const;

private:
    using IdentityByDocument = std::unordered_map<const Document*, DocumentIdentity>;
    using IdentityByInstance = std::unordered_map<DocumentInstanceId, DocumentIdentity>;

    [[nodiscard]] static DocumentInstanceId allocateInstanceId();
    [[nodiscard]] static DocumentLifecycleEpoch allocateLifecycleEpoch();

    void updateIdentityLocked(const Document* document, const DocumentIdentity& identity);
    void retainTombstoneLocked(const DocumentIdentity& identity);

    const std::size_t _tombstoneCapacity;
    mutable std::mutex _mutex;
    IdentityByDocument _byDocument;
    IdentityByInstance _byInstance;
    std::deque<DocumentInstanceId> _tombstoneOrder;
};

}  // namespace App
