// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <Gui/PresentationDelta.h>
#include <Mod/Part/PartGlobal.h>

#include <cstddef>
#include <cstdint>
#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
#include <functional>
#endif

class TopoDS_Shape;
namespace App
{
class Document;
class DocumentObject;
}

namespace PartGui
{

// Process-wide byte bound counts packed buffers only. Shapes retained for
// identity comparison retain OCC data too; that retention is limited by the entry cap.
inline constexpr std::size_t PresentationCaptureCacheMaxBytes = 64U * 1024U * 1024U;
inline constexpr std::size_t PresentationCaptureCacheMaxEntries = 512;

/** Document-owner-thread capture; copies cached packed geometry on an exact hit.
 * Concurrent misses may mesh twice, using independent OCC copies. Shape property
 * changes invalidate the key generation. In-place edits without a property change
 * follow the live path's unchanged-shape assumption (ViewProviderPartExt::updateVisual).
 */
[[nodiscard]] PartGuiExport bool capturePartPresentationRenderBuffer(
    const App::Document& document,
    const std::string& stableObjectIdentity,
    const TopoDS_Shape& shape,
    double deviation,
    double angularDeflection,
    bool normalsFromUV,
    Gui::PresentationRenderBuffer& buffer);

/** Preserve the stored shape's stable location when getRenderedShape rebuilt
 * an equivalent OCC location; otherwise capture the supplied rendered shape. */
[[nodiscard]] PartGuiExport bool capturePartPresentationRenderBuffer(
    const App::DocumentObject& object,
    const std::string& stableObjectIdentity,
    const TopoDS_Shape& renderedShape,
    double deviation,
    double angularDeflection,
    bool normalsFromUV,
    Gui::PresentationRenderBuffer& buffer);

#if defined(FREECAD_TEST_PART_CAPTURE_CACHE)
// Runs after the private OCC copy, before tessellation, without the cache lock.
using PresentationCaptureMeshHook = std::function<void(const std::string&)>;
PartGuiExport void setPresentationCaptureMeshHook(PresentationCaptureMeshHook hook);
#endif

struct PresentationCaptureCacheStatistics
{
    std::size_t entries;
    std::size_t packedBytes;
    std::uint64_t tessellations;
};

/** Thread-safe diagnostics; tessellations counts actual tessellation calls. */
[[nodiscard]] PartGuiExport PresentationCaptureCacheStatistics
presentationCaptureCacheStatistics();

}  // namespace PartGui
