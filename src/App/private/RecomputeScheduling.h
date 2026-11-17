// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "../Application.h"

namespace App::Internal
{
inline bool useOptimizedRecomputeScheduling()
{
    return GetApplication()
        .GetParameterGroupByPath("User parameter:BaseApp/Preferences/Document")
        ->GetBool("OptimizeRecomputeScheduling", false);
}
}  // namespace App::Internal
