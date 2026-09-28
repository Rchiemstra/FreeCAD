// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <functional>

namespace App
{

class Document;

/** Optional hook invoked on the document owner thread at a stable recompute boundary. */
using DocumentPresentationBoundaryCallback = std::function<void(Document&)>;

/** Register the GUI presentation capture hook (nullptr clears). */
AppExport void setDocumentPresentationBoundaryCallback(
    DocumentPresentationBoundaryCallback callback);

[[nodiscard]] AppExport bool hasDocumentPresentationBoundaryCallback() noexcept;

/** Invoke the hook when registered; no-op otherwise. */
AppExport void invokeDocumentPresentationBoundary(Document& document);

}  // namespace App
