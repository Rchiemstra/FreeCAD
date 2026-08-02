// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <string_view>

namespace App
{

/** The registered intent type for conservative native scalar property edits. */
inline constexpr std::string_view CollaborativeSetPropertyOperationType =
    "App.CollaborativeSetProperty";

/**
 * Register the native scalar-property collaboration adapter exactly once.
 *
 * Registration remains an App-native extension boundary: this function uses
 * the private registrar internally and does not expose arbitrary adapter
 * registration to its caller.
 */
AppExport void ensureCollaborativeSetPropertyOperationRegistered();

}  // namespace App
