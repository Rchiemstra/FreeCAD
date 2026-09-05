// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <App/CollaborativeOperationRegistry.h>

namespace App::Internal
{

/** Native-tree-only adapter registrar; this definition is not installed. */
class AppExport CollaborativeOperationRegistrar
{
public:
    static std::uint64_t registerAdapter(std::string operationType,
                                         CollaborativeOperationAdapter adapter);
};

}  // namespace App::Internal
