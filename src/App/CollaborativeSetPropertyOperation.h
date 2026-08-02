// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <string_view>

namespace App
{

namespace Internal
{

struct CollaborativeSetPropertyIndependenceFacts
{
    bool exactBaseObject = false;
    bool exactDynamicProperty = false;
    bool hasExtensions = true;
    bool hasExpressions = true;
    bool noRecompute = false;
    bool hasReverseDependents = true;
};

[[nodiscard]] constexpr bool hasCollaborativeSetPropertyIndependenceProof(
    const CollaborativeSetPropertyIndependenceFacts& facts) noexcept
{
    return facts.exactBaseObject && facts.exactDynamicProperty
        && !facts.hasExtensions && !facts.hasExpressions && facts.noRecompute
        && !facts.hasReverseDependents;
}

}  // namespace Internal

/** The registered intent type for native scalar property edits. */
inline constexpr std::string_view CollaborativeSetPropertyOperationType =
    "App.CollaborativeSetProperty";

/**
 * Register the native scalar-property collaboration adapter exactly once.
 *
 * Registration remains an App-native extension boundary: this function uses
 * the private registrar internally and does not expose arbitrary adapter
 * registration to its caller. The adapter remains object-conservative unless
 * it can prove that an exact native DocumentObject property write is isolated.
 */
AppExport void ensureCollaborativeSetPropertyOperationRegistered();

}  // namespace App
