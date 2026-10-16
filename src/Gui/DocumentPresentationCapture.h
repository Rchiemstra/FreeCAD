// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PresentationDelta.h"

#include <FCGlobal.h>

namespace App
{
class Document;
class DocumentObject;
}  // namespace App

namespace Gui
{

/** Build one pointer-free presentation packet on the document owner thread. */
[[nodiscard]] GuiExport PresentationDelta
buildPresentationDeltaOnDocumentThread(const App::Document& document);

/** Capture one render buffer from a live App object (document thread only). */
[[nodiscard]] GuiExport bool captureDocumentObjectPresentationRenderBuffer(
    const App::Document& document,
    const App::DocumentObject& object,
    const std::string& stableObjectIdentity,
    PresentationRenderBuffer& buffer);

/** Register the App presentation-boundary hook for GUI enqueue. */
GuiExport void installDocumentPresentationBoundaryHook();

}  // namespace Gui
