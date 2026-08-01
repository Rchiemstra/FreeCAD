// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationRegistry.h"

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

void CollaborationRegistry::retainTombstoneLocked(const DocumentIdentity& identity)
{
    if (_tombstoneCapacity == 0) {
        _byInstance.erase(identity.instanceId);
        return;
    }

    _byInstance.at(identity.instanceId) = identity;
    _tombstoneOrder.push_back(identity.instanceId);
    while (_tombstoneOrder.size() > _tombstoneCapacity) {
        _byInstance.erase(_tombstoneOrder.front());
        _tombstoneOrder.pop_front();
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
    _byDocument.erase(found);
    retainTombstoneLocked(closed);
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
