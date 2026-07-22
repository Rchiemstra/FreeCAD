// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MutationCapability.h"
#include "Document.h"

#include <iterator>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace App;

namespace
{

struct ThreadCapabilityBucket
{
    std::vector<MutationCapability> capabilities;
    std::vector<Document*> internalGrants;
};

std::mutex gRegistryMutex;
std::unordered_map<std::thread::id, ThreadCapabilityBucket> gByThread;
std::unordered_map<std::uint64_t, std::thread::id> gCapabilityOwners;

ThreadCapabilityBucket& bucketFor(std::thread::id tid)
{
    return gByThread[tid];
}

}  // namespace

void MutationAuthorityTLS::activateCapability(const MutationCapability& capability)
{
    if (capability.id == 0 || !capability.document) {
        return;
    }
    const auto tid = capability.creatorThread;
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto& bucket = bucketFor(tid);
    bucket.capabilities.push_back(capability);
    gCapabilityOwners[capability.id] = tid;
}

void MutationAuthorityTLS::deactivateCapability(std::uint64_t capabilityId)
{
    if (capabilityId == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto ownerIt = gCapabilityOwners.find(capabilityId);
    if (ownerIt == gCapabilityOwners.end()) {
        return;
    }
    const auto tid = ownerIt->second;
    gCapabilityOwners.erase(ownerIt);
    auto bucketIt = gByThread.find(tid);
    if (bucketIt == gByThread.end()) {
        return;
    }
    auto& caps = bucketIt->second.capabilities;
    for (auto it = caps.begin(); it != caps.end(); ++it) {
        if (it->id == capabilityId) {
            caps.erase(it);
            break;
        }
    }
    if (bucketIt->second.capabilities.empty() && bucketIt->second.internalGrants.empty()) {
        gByThread.erase(bucketIt);
    }
}

void MutationAuthorityTLS::activateInternalGrant(Document* document)
{
    if (!document) {
        return;
    }
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    bucketFor(std::this_thread::get_id()).internalGrants.push_back(document);
}

void MutationAuthorityTLS::deactivateInternalGrant(Document* document)
{
    if (!document) {
        return;
    }
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    const auto tid = std::this_thread::get_id();
    auto bucketIt = gByThread.find(tid);
    if (bucketIt == gByThread.end()) {
        return;
    }
    auto& grants = bucketIt->second.internalGrants;
    for (auto it = grants.rbegin(); it != grants.rend(); ++it) {
        if (*it == document) {
            grants.erase(std::next(it).base());
            break;
        }
    }
    if (bucketIt->second.capabilities.empty() && bucketIt->second.internalGrants.empty()) {
        gByThread.erase(bucketIt);
    }
}

std::vector<MutationCapability> MutationAuthorityTLS::activeCapabilities()
{
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gByThread.find(std::this_thread::get_id());
    if (it == gByThread.end()) {
        return {};
    }
    return it->second.capabilities;
}

bool MutationAuthorityTLS::hasMatchingCapability(const Document* document, MutationKind kind)
{
    if (!document) {
        return false;
    }
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gByThread.find(std::this_thread::get_id());
    if (it == gByThread.end()) {
        return false;
    }
    for (const auto& cap : it->second.capabilities) {
        if (cap.document != document) {
            continue;
        }
        if ((cap.allowedKinds & mutationKindBit(kind)) == 0) {
            continue;
        }
        return true;
    }
    return false;
}

bool MutationAuthorityTLS::hasInternalGrant(const Document* document)
{
    if (!document) {
        return false;
    }
    std::lock_guard<std::mutex> lock(gRegistryMutex);
    auto it = gByThread.find(std::this_thread::get_id());
    if (it == gByThread.end()) {
        return false;
    }
    for (auto* doc : it->second.internalGrants) {
        if (doc == document) {
            return true;
        }
    }
    return false;
}

MutationCapabilityScope::MutationCapabilityScope(MutationCapability capability)
    : _capability(std::move(capability))
    , _active(_capability.id != 0 && _capability.document != nullptr)
{
    if (_active) {
        if (_capability.creatorThread == std::thread::id {}) {
            _capability.creatorThread = std::this_thread::get_id();
        }
        MutationAuthorityTLS::activateCapability(_capability);
    }
}

MutationCapabilityScope::MutationCapabilityScope(MutationCapabilityScope&& other) noexcept
    : _capability(std::move(other._capability))
    , _active(other._active)
{
    other._active = false;
    other._capability = {};
}

MutationCapabilityScope& MutationCapabilityScope::operator=(MutationCapabilityScope&& other) noexcept
{
    if (this != &other) {
        this->~MutationCapabilityScope();
        new (this) MutationCapabilityScope(std::move(other));
    }
    return *this;
}

MutationCapabilityScope::~MutationCapabilityScope()
{
    if (!_active) {
        return;
    }
    MutationAuthorityTLS::deactivateCapability(_capability.id);
    _active = false;
}

MutationInternalScope::MutationInternalScope(Document* document)
    : _document(document)
    , _active(document != nullptr)
{
    if (_active) {
        MutationAuthorityTLS::activateInternalGrant(_document);
    }
}

MutationInternalScope::MutationInternalScope(MutationInternalScope&& other) noexcept
    : _document(other._document)
    , _active(other._active)
{
    other._active = false;
    other._document = nullptr;
}

MutationInternalScope& MutationInternalScope::operator=(MutationInternalScope&& other) noexcept
{
    if (this != &other) {
        this->~MutationInternalScope();
        new (this) MutationInternalScope(std::move(other));
    }
    return *this;
}

MutationInternalScope::~MutationInternalScope()
{
    if (!_active) {
        return;
    }
    MutationAuthorityTLS::deactivateInternalGrant(_document);
    _active = false;
}
