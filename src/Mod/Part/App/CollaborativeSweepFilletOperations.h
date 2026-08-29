// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Mod/Part/PartGlobal.h>

#include <string_view>

namespace Part
{

inline constexpr std::string_view CollaborativeSweepOperationType =
    "Part.CollaborativeSweep";
inline constexpr std::string_view CollaborativeFilletOperationType =
    "Part.CollaborativeFillet";

/** Register the typed parent adapters and native archive-only worker handlers. */
PartExport void ensureCollaborativeSweepFilletOperationsRegistered();

}  // namespace Part
