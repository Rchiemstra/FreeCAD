// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "PresentationDelta.h"

#include <FCGlobal.h>

#include <string>

namespace Gui
{

class ViewProvider;

/** Wave 3 migration status for a production ViewProvider updateData provider. */
enum class ViewProviderPresentationClassification
{
    Adapted,
    Unsupported,
};

/**
 * Copied inputs for pointer-free presentation capture on the document thread.
 *
 * Must not carry live App::Property or DocumentObject pointers into GUI apply.
 */
struct GuiExport ViewProviderPresentationCaptureRequest
{
    std::string stableObjectIdentity;
    std::string propertyName;
};

[[nodiscard]] GuiExport const char*
viewProviderPresentationClassificationName(ViewProviderPresentationClassification classification) noexcept;

/**
 * Fill @p buffer with pointer-free tessellation data already resident on the
 * view provider without invoking updateData(App::Property*).
 */
[[nodiscard]] GuiExport bool capturePartPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer);

[[nodiscard]] GuiExport bool captureMeshPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer);

[[nodiscard]] GuiExport bool capturePointsPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer);

[[nodiscard]] GuiExport bool captureSketchPresentationRenderBuffer(
    const ViewProvider& provider,
    const ViewProviderPresentationCaptureRequest& request,
    PresentationRenderBuffer& buffer);

}  // namespace Gui
