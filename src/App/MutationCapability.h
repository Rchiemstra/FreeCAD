// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "MutationKind.h"
#include <FCGlobal.h>

#include <cstdint>
#include <vector>

namespace App
{

class Document;

/** Opaque in-process mutation grant bound to a document and fencing generation. */
struct MutationCapability
{
    std::uint64_t id {0};
    Document* document {nullptr};
    std::uint64_t fencingGeneration {0};
    MutationKindMask allowedKinds {0};
    MutationOrigin origin {MutationOrigin::Mcp};
};

/**
 * RAII registration of an active capability on the calling thread.
 * While alive, authorize() may ALLOW matching mutations for the bound document.
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

/** Thread-local accessors used by DocumentMutationAuthority. */
namespace MutationAuthorityTLS
{
AppExport const std::vector<MutationCapability>& activeCapabilities();
AppExport bool hasInternalGrant(const Document* document);
}  // namespace MutationAuthorityTLS

}  // namespace App
