// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Mod/Part/PartGlobal.h>

#include <string_view>

namespace Part
{

inline constexpr std::string_view CollaborativeBooleanOperationType =
    "Part.CollaborativeBoolean";

/**
 * Register the native Part Boolean collaboration adapter exactly once.
 *
 * Its pointer-free intent accepts exactly the string arguments `base`, `tool`,
 * `result`, and `kind`; kind is one of `cut`, `fuse`, or `common`. All three
 * object arguments name pre-existing Part::Feature objects; result must be an
 * exact Part::Feature so recompute cannot replace the committed shape through
 * a subclass execute implementation.
 */
PartExport void ensureCollaborativeBooleanOperationRegistered();

}  // namespace Part
