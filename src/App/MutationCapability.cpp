// SPDX-License-Identifier: LGPL-2.1-or-later

#include "MutationCapability.h"
#include "Document.h"

#include <algorithm>
#include <utility>
#include <vector>

using namespace App;

namespace
{
thread_local std::vector<MutationCapability> tlsCapabilities;
thread_local std::vector<Document*> tlsInternalGrants;
}  // namespace

MutationCapabilityScope::MutationCapabilityScope(MutationCapability capability)
    : _capability(std::move(capability))
    , _active(_capability.id != 0 && _capability.document != nullptr)
{
    if (_active) {
        tlsCapabilities.push_back(_capability);
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
    for (auto it = tlsCapabilities.rbegin(); it != tlsCapabilities.rend(); ++it) {
        if (it->id == _capability.id) {
            tlsCapabilities.erase(std::next(it).base());
            break;
        }
    }
    _active = false;
}

MutationInternalScope::MutationInternalScope(Document* document)
    : _document(document)
    , _active(document != nullptr)
{
    if (_active) {
        tlsInternalGrants.push_back(_document);
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
    for (auto it = tlsInternalGrants.rbegin(); it != tlsInternalGrants.rend(); ++it) {
        if (*it == _document) {
            tlsInternalGrants.erase(std::next(it).base());
            break;
        }
    }
    _active = false;
}

const std::vector<MutationCapability>& MutationAuthorityTLS::activeCapabilities()
{
    return tlsCapabilities;
}

bool MutationAuthorityTLS::hasInternalGrant(const Document* document)
{
    if (!document) {
        return false;
    }
    for (auto* doc : tlsInternalGrants) {
        if (doc == document) {
            return true;
        }
    }
    return false;
}
