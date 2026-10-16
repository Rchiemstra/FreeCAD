// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationBoundary.h"

#include "Document.h"

namespace App
{

namespace
{

DocumentPresentationBoundaryCallback gPresentationBoundaryCallback;

}  // namespace

void setDocumentPresentationBoundaryCallback(DocumentPresentationBoundaryCallback callback)
{
    gPresentationBoundaryCallback = std::move(callback);
}

bool hasDocumentPresentationBoundaryCallback() noexcept
{
    return static_cast<bool>(gPresentationBoundaryCallback);
}

void invokeDocumentPresentationBoundary(Document& document)
{
    if (gPresentationBoundaryCallback) {
        gPresentationBoundaryCallback(document);
    }
}

}  // namespace App
