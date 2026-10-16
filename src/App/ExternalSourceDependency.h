// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <set>
#include <FCGlobal.h>

namespace App
{
class Document;

// Unlike the persisted document graph, removal admission includes hidden and
// transient runtime links. Replay also includes expressions whose source was
// retired, since restoring that source reconnects their runtime dependencies.
AppExport std::set<Document*> externalSourceDependentDocuments(
    const Document& source, bool includeRetiredExpressions = false);
}
