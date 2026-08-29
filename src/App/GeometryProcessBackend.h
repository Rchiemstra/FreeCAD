// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>

namespace App
{
class GeometryJobManager;
}

namespace App::Internal
{

struct GeometryProcessBackendOptions
{
    std::filesystem::path executable;
    std::filesystem::path workspaceRoot;
    std::chrono::milliseconds pollInterval {10};
    std::chrono::milliseconds startupHeartbeatTimeout {30'000};
    std::chrono::milliseconds heartbeatTimeout {2'000};
    std::chrono::milliseconds terminationGrace {500};
    std::chrono::hours completedArtifactRetention {24};
};

/** Private process owner used only by the App-owned GeometryJobManager. */
class GeometryJobProcessBackend
{
public:
    GeometryJobProcessBackend(GeometryJobManager& manager,
                              GeometryProcessBackendOptions options);
    ~GeometryJobProcessBackend();

    GeometryJobProcessBackend(const GeometryJobProcessBackend&) = delete;
    GeometryJobProcessBackend& operator=(const GeometryJobProcessBackend&) = delete;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace App::Internal
