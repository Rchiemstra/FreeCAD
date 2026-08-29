// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerOperationRegistry.h"

#include <stdexcept>
#include <utility>

using namespace App::Internal;

GeometryWorkerOperationRegistry& GeometryWorkerOperationRegistry::instance()
{
    static GeometryWorkerOperationRegistry registry;
    return registry;
}

void GeometryWorkerOperationRegistry::registerOperation(
    std::string operationType,
    GeometryWorkerOperation operation)
{
    if (operationType.empty() || !operation) {
        throw std::invalid_argument(
            "geometry worker operation type and callback are required");
    }
    std::lock_guard lock(_mutex);
    if (_operations.contains(operationType)) {
        throw std::invalid_argument(
            "geometry worker operation type is already registered");
    }
    _operations.emplace(std::move(operationType), std::move(operation));
}

bool GeometryWorkerOperationRegistry::contains(
    const std::string& operationType) const
{
    std::lock_guard lock(_mutex);
    return _operations.contains(operationType);
}

App::GeometryArchive GeometryWorkerOperationRegistry::execute(
    const std::string& operationType,
    const App::GeometryArchive& input,
    const std::stop_token stopToken) const
{
    GeometryWorkerOperation operation;
    {
        std::lock_guard lock(_mutex);
        const auto found = _operations.find(operationType);
        if (found == _operations.end()) {
            throw std::invalid_argument("unsupported isolated geometry operation");
        }
        operation = found->second;
    }
    return operation(input, stopToken);
}
