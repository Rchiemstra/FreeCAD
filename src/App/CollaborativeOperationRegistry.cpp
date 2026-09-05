// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborativeOperationRegistry.h"

#include "Document.h"
#include "private/CollaborativeOperationRegistryInternal.h"

#include <stdexcept>
#include <utility>

using namespace App;

CollaborativeOperationRegistry& CollaborativeOperationRegistry::instance()
{
    static CollaborativeOperationRegistry registry;
    return registry;
}

CollaborativeOperationRegistry::RegistrationId
CollaborativeOperationRegistry::registerAdapter(std::string operationType,
                                                CollaborativeOperationAdapter adapter)
{
    if (operationType.empty() || !adapter) {
        throw std::invalid_argument("collaborative adapter type and callback are required");
    }

    std::lock_guard lock(_mutex);
    if (_adapters.contains(operationType)) {
        throw std::invalid_argument("collaborative adapter type is already registered");
    }
    if (_nextRegistrationId == 0) {
        throw std::overflow_error("collaborative adapter registration identity exhausted");
    }
    const RegistrationId id = _nextRegistrationId++;
    _adapters.emplace(std::move(operationType), Entry {id, std::move(adapter)});
    return id;
}

CollaborativeOperationPreparation CollaborativeOperationRegistry::prepare(
    const Document& document,
    const CollaborativeOperationIntent& intent) const
{
    CollaborativeOperationAdapter adapter;
    RegistrationId foundRegistrationId = 0;
    {
        std::lock_guard lock(_mutex);
        const auto found = _adapters.find(intent.operationType);
        if (found == _adapters.end()) {
            throw std::invalid_argument("collaborative operation type is not registered");
        }
        adapter = found->second.adapter;
        foundRegistrationId = found->second.id;
    }

    auto preparation = adapter(document, intent);
    if (static_cast<bool>(preparation.operation)
        == static_cast<bool>(preparation.detachedTask)) {
        throw std::invalid_argument(
            "registered adapter must return exactly one synchronous operation or detached task");
    }
    if (preparation.operation
        && preparation.operation->typeId() != intent.operationType) {
        throw std::invalid_argument("registered adapter returned a mismatched operation type");
    }
    preparation.registrationId = foundRegistrationId;
    return preparation;
}

bool CollaborativeOperationRegistry::contains(const std::string& operationType) const
{
    std::lock_guard lock(_mutex);
    return _adapters.contains(operationType);
}

bool CollaborativeOperationRegistry::matches(RegistrationId registrationId,
                                             const std::string& operationType) const
{
    std::lock_guard lock(_mutex);
    const auto found = _adapters.find(operationType);
    return found != _adapters.end() && found->second.id == registrationId;
}

std::uint64_t App::Internal::CollaborativeOperationRegistrar::registerAdapter(
    std::string operationType,
    CollaborativeOperationAdapter adapter)
{
    return CollaborativeOperationRegistry::instance().registerAdapter(
        std::move(operationType), std::move(adapter));
}
