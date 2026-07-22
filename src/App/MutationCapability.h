// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "MutationKind.h"
#include <FCGlobal.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace App
{

class Document;

/**
 * Opaque in-process mutation grant bound to a document, fencing generation,
 * and non-reusable authority epoch.
 */
struct MutationCapability
{
    std::uint64_t id {0};
    Document* document {nullptr};
    std::uint64_t fencingGeneration {0};
    std::uint64_t authorityEpoch {0};
    MutationKindMask allowedKinds {0};
    MutationOrigin origin {MutationOrigin::Mcp};
    std::thread::id creatorThread {};
};

/**
 * RAII registration of an active capability on the creator thread.
 * Destruction from any thread removes the grant from the creator thread's
 * synchronized authorization registry.
 */
class AppExport MutationCapabilityScope
{
public:
    explicit MutationCapabilityScope(MutationCapability capability);
    MutationCapabilityScope(const MutationCapabilityScope&) = delete;
    MutationCapabilityScope& operator=(const MutationCapabilityScope&) = delete;
    MutationCapabilityScope(MutationCapabilityScope&& other) noexcept;
    MutationCapabilityScope& operator=(MutationCapabilityScope&& other) noexcept;
    ~MutationCapabilityScope();

    const MutationCapability& capability() const
    {
        return _capability;
    }

    bool valid() const
    {
        return _active && _capability.id != 0;
    }

private:
    MutationCapability _capability;
    bool _active {false};
};

/**
 * RAII internal grant that allows FreeCAD's own pipelines (recompute, undo apply,
 * restore) to mutate an owned document without an MCP capability.
 */
class AppExport MutationInternalScope
{
public:
    explicit MutationInternalScope(Document* document);
    MutationInternalScope(const MutationInternalScope&) = delete;
    MutationInternalScope& operator=(const MutationInternalScope&) = delete;
    MutationInternalScope(MutationInternalScope&& other) noexcept;
    MutationInternalScope& operator=(MutationInternalScope&& other) noexcept;
    ~MutationInternalScope();

    Document* document() const
    {
        return _document;
    }

private:
    Document* _document {nullptr};
    bool _active {false};
};

/** Synchronized capability / internal-grant accessors. */
namespace MutationAuthorityTLS
{
/** Capabilities active for the calling thread only. */
AppExport std::vector<MutationCapability> activeCapabilities();

/** True if the calling thread holds a matching capability for document+kind. */
AppExport bool hasMatchingCapability(const Document* document, MutationKind kind);

AppExport bool hasInternalGrant(const Document* document);

/** Activate/deactivate used by MutationCapabilityScope (any-thread deactivate). */
AppExport void activateCapability(const MutationCapability& capability);
AppExport void deactivateCapability(std::uint64_t capabilityId);

AppExport void activateInternalGrant(Document* document);
AppExport void deactivateInternalGrant(Document* document);
}  // namespace MutationAuthorityTLS

}  // namespace App
