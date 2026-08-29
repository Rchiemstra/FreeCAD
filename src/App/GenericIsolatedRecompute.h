// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperationRegistry.h"
#include "DocumentRecomputeCoordinator.h"

#include <FCGlobal.h>

#include <string_view>

namespace App
{

class Document;
class DocumentObject;

inline constexpr std::string_view GenericIsolatedRecomputeOperationType =
    "App.GenericIsolatedRecompute";

namespace Internal
{

/** Register both trusted sides of the generic isolated-feature protocol. */
AppExport void ensureGenericIsolatedRecomputeRegistered();

/** Build the immutable one-feature or downstream-recursive recompute plan. */
[[nodiscard]] AppExport DocumentRecomputeRequest makeGenericIsolatedRecomputeRequest(
    Document& document,
    DocumentObject& feature,
    bool recursive);

/** Narrow friend used only by the isolated worker implementation. */
class GenericIsolatedRecomputeAccess;

}  // namespace Internal
}  // namespace App
