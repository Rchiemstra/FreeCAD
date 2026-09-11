// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include "GeometryArchive.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <unordered_map>

namespace App::Internal
{

using GeometryWorkerOperation =
    std::function<GeometryArchive(const GeometryArchive&, std::stop_token)>;

/**
 * Process-local registry of trusted native geometry worker operations.
 *
 * The callback surface is deliberately archive-only: a worker operation can
 * neither receive nor recover an App::Document or App::DocumentObject. Part
 * registers its native OCC codecs when the Part module is imported inside the
 * isolated FreeCADCmd process.
 */
class AppExport GeometryWorkerOperationRegistry
{
public:
    static GeometryWorkerOperationRegistry& instance();

    void registerOperation(std::string operationType,
                           GeometryWorkerOperation operation);
    [[nodiscard]] bool contains(const std::string& operationType) const;
    [[nodiscard]] GeometryArchive execute(const std::string& operationType,
                                          const GeometryArchive& input,
                                          std::stop_token stopToken) const;

private:
    mutable std::mutex _mutex;
    std::unordered_map<std::string, GeometryWorkerOperation> _operations;
};

}  // namespace App::Internal
