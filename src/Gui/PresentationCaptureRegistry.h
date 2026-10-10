// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PresentationDelta.h"

#include <Base/Type.h>

#include <FCGlobal.h>

namespace App
{
class DocumentObject;
}  // namespace App

namespace Gui
{

/** Document-thread capture of pointer-free render buffers from live App objects. */
using DocumentObjectPresentationCaptureFn = bool (*)(const App::DocumentObject& object,
                                                     const std::string& stableObjectIdentity,
                                                     PresentationRenderBuffer& buffer);

class GuiExport PresentationCaptureRegistry
{
public:
    static void registerCapture(const char* objectTypeName, DocumentObjectPresentationCaptureFn capture);

    static void registerCapture(Base::Type objectType, DocumentObjectPresentationCaptureFn capture);

    [[nodiscard]] static bool hasCapture(const App::DocumentObject& object);

    [[nodiscard]] static bool tryCapture(const App::DocumentObject& object,
                                         const std::string& stableObjectIdentity,
                                         PresentationRenderBuffer& buffer);
};

/** Static registration helper for module Gui libraries. */
struct GuiExport DocumentPresentationCaptureRegistrar
{
    DocumentPresentationCaptureRegistrar(const char* objectTypeName,
                                         DocumentObjectPresentationCaptureFn capture);
};

}  // namespace Gui
