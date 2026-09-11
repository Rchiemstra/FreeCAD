// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationRegistry.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>

using namespace App;

namespace
{

std::atomic<DocumentInstanceId> nextDocumentInstanceId {1};
std::atomic<DocumentLifecycleEpoch> nextDocumentLifecycleEpoch {1};

template<typename Value>
Value allocateMonotonic(std::atomic<Value>& next, const char* exhaustedMessage)
{
    auto candidate = next.load(std::memory_order_relaxed);
    while (true) {
        if (candidate == 0 || candidate == std::numeric_limits<Value>::max()) {
            throw std::overflow_error(exhaustedMessage);
        }
        if (next.compare_exchange_weak(
                candidate,
                candidate + 1,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return candidate;
        }
    }
}

}  // namespace

CollaborationRegistry::CollaborationRegistry(std::size_t tombstoneCapacity)
    : _tombstoneCapacity(tombstoneCapacity)
{}

DocumentInstanceId CollaborationRegistry::allocateInstanceId()
{
    return allocateMonotonic(nextDocumentInstanceId, "document instance ID space exhausted");
}

DocumentLifecycleEpoch CollaborationRegistry::allocateLifecycleEpoch()
{
    return allocateMonotonic(nextDocumentLifecycleEpoch, "document lifecycle epoch space exhausted");
}

DocumentIdentity CollaborationRegistry::registerDocument(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (const auto found = _byDocument.find(&document); found != _byDocument.end()) {
        return found->second;
    }

    DocumentIdentity identity {
        allocateInstanceId(),
        allocateLifecycleEpoch(),
        DocumentLifecycleState::Live,
    };
    _byDocument.emplace(&document, identity);
    _byInstance.emplace(identity.instanceId, identity);
    return identity;
}

std::optional<DocumentIdentity> CollaborationRegistry::identity(const Document& document) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<DocumentIdentity>
CollaborationRegistry::identity(DocumentInstanceId instanceId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byInstance.find(instanceId);
    if (found == _byInstance.end()) {
        return std::nullopt;
    }
    return found->second;
}

void CollaborationRegistry::updateIdentityLocked(const Document* document,
                                                 const DocumentIdentity& identity)
{
    _byDocument.at(document) = identity;
    _byInstance.at(identity.instanceId) = identity;
}

std::optional<DocumentIdentity> CollaborationRegistry::advanceEpoch(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end() || found->second.state != DocumentLifecycleState::Live) {
        return std::nullopt;
    }

    auto advanced = found->second;
    advanced.lifecycleEpoch = allocateLifecycleEpoch();
    updateIdentityLocked(&document, advanced);
    return advanced;
}

DocumentLifecycleEpoch CollaborationRegistry::reserveLifecycleEpoch()
{
    return allocateLifecycleEpoch();
}

std::optional<DocumentIdentity> CollaborationRegistry::advanceEpoch(
    const Document& document,
    const DocumentLifecycleEpoch reservedEpoch)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end() || found->second.state != DocumentLifecycleState::Live) {
        return std::nullopt;
    }

    auto advanced = found->second;
    advanced.lifecycleEpoch = reservedEpoch;
    updateIdentityLocked(&document, advanced);
    return advanced;
}

std::optional<DocumentIdentity> CollaborationRegistry::markClosing(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end() || found->second.state != DocumentLifecycleState::Live) {
        return std::nullopt;
    }

    auto closing = found->second;
    closing.lifecycleEpoch = allocateLifecycleEpoch();
    closing.state = DocumentLifecycleState::Closing;
    updateIdentityLocked(&document, closing);
    return closing;
}

std::optional<CollaborationRegistry::PreparedDocumentClose>
CollaborationRegistry::prepareDocumentClose(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end() || found->second.state != DocumentLifecycleState::Live) {
        return std::nullopt;
    }
    const auto instance = _byInstance.find(found->second.instanceId);
    if (instance == _byInstance.end()) {
        return std::nullopt;
    }

    PreparedDocumentClose prepared;
    prepared.closingIdentity = found->second;
    prepared.closingIdentity.lifecycleEpoch = allocateLifecycleEpoch();
    prepared.closingIdentity.state = DocumentLifecycleState::Closing;
    prepared.closedEpoch = allocateLifecycleEpoch();

    // Reserve the only allocation needed to retain the eventual Closed
    // tombstone before publishing Closing. Once Closing is visible, the
    // Application close boundary is irrevocable and completion must not be
    // vulnerable to allocation failure.
    if (_tombstoneCapacity != 0) {
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
        if (const auto hook = _tombstonePreparationTestHook.load(
                std::memory_order_acquire)) {
            hook();
        }
#endif
        // Instance ID zero is reserved and acts as an allocation-only slot.
        // It cannot be evicted as a Closed tombstone while another document's
        // prepared close is still running observers.
        _tombstoneOrder.push_back(0);
        prepared.tombstoneQueued = true;
    }

    found->second = prepared.closingIdentity;
    instance->second = prepared.closingIdentity;
    return prepared;
}

std::optional<DocumentIdentity> CollaborationRegistry::completePreparedDocumentClose(
    const Document& document,
    const PreparedDocumentClose& prepared)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end() || found->second != prepared.closingIdentity) {
        return std::nullopt;
    }
    const auto instance = _byInstance.find(prepared.closingIdentity.instanceId);
    if (instance == _byInstance.end() || instance->second != prepared.closingIdentity) {
        return std::nullopt;
    }

    auto closed = prepared.closingIdentity;
    closed.lifecycleEpoch = prepared.closedEpoch;
    closed.state = DocumentLifecycleState::Closed;
    auto reservedTombstone = _tombstoneOrder.end();
    if (prepared.tombstoneQueued) {
        reservedTombstone = std::find(_tombstoneOrder.begin(), _tombstoneOrder.end(), 0);
        if (reservedTombstone == _tombstoneOrder.end()) {
            return std::nullopt;
        }
    }

    instance->second = closed;
    _byDocument.erase(found);
    if (!prepared.tombstoneQueued) {
        _byInstance.erase(instance);
    }
    else {
        *reservedTombstone = closed.instanceId;
        evictClosedTombstonesLocked();
    }
    return closed;
}

void CollaborationRegistry::retainTombstoneLocked(const DocumentIdentity& identity)
{
    if (_tombstoneCapacity == 0) {
        _byInstance.erase(identity.instanceId);
        return;
    }

    _byInstance.at(identity.instanceId) = identity;
    _tombstoneOrder.push_back(identity.instanceId);
    evictClosedTombstonesLocked();
}

void CollaborationRegistry::evictClosedTombstonesLocked()
{
    auto retained = static_cast<std::size_t>(std::count_if(
        _tombstoneOrder.begin(), _tombstoneOrder.end(), [](const auto id) { return id != 0; }));
    while (retained > _tombstoneCapacity) {
        const auto oldestClosed = std::find_if(
            _tombstoneOrder.begin(), _tombstoneOrder.end(), [](const auto id) { return id != 0; });
        if (oldestClosed == _tombstoneOrder.end()) {
            break;
        }
        _byInstance.erase(*oldestClosed);
        _tombstoneOrder.erase(oldestClosed);
        --retained;
    }
}

std::optional<DocumentIdentity> CollaborationRegistry::closeDocument(const Document& document)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byDocument.find(&document);
    if (found == _byDocument.end()) {
        return std::nullopt;
    }

    auto closed = found->second;
    closed.lifecycleEpoch = allocateLifecycleEpoch();
    closed.state = DocumentLifecycleState::Closed;
    if (_tombstoneCapacity != 0) {
        // Preserve the legacy direct API's strong guarantee as well: reserve
        // deque storage before erasing the live pointer or changing identity.
        _tombstoneOrder.push_back(closed.instanceId);
    }
    _byInstance.at(closed.instanceId) = closed;
    _byDocument.erase(found);
    if (_tombstoneCapacity == 0) {
        _byInstance.erase(closed.instanceId);
    }
    else {
        evictClosedTombstonesLocked();
    }
    return closed;
}

DocumentIdentityValidation CollaborationRegistry::validate(
    DocumentInstanceId instanceId,
    DocumentLifecycleEpoch lifecycleEpoch) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _byInstance.find(instanceId);
    if (found == _byInstance.end()) {
        return DocumentIdentityValidation::UnknownInstance;
    }
    if (found->second.lifecycleEpoch != lifecycleEpoch) {
        return DocumentIdentityValidation::EpochMismatch;
    }
    if (found->second.state != DocumentLifecycleState::Live) {
        return DocumentIdentityValidation::NotLive;
    }
    return DocumentIdentityValidation::Valid;
}
