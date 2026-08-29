// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <filesystem>
#include <string>

namespace App::Internal
{

/** True only for the private FCG/1 worker invocation created by the process backend. */
AppExport bool geometryWorkerRequested() noexcept;

/** Resolve the running binary without requiring a QCoreApplication instance. */
AppExport std::filesystem::path currentExecutablePath();

/** SHA-256 binding of the exact FreeCADCmd launcher and FreeCADApp runtime. */
AppExport std::string geometryWorkerBuildFingerprint(
    const std::filesystem::path& workerExecutable);

/** Execute one validated geometry worker request and return a stable process exit code. */
AppExport int runGeometryWorkerMain() noexcept;

}  // namespace App::Internal
