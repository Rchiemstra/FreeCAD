// SPDX-License-Identifier: LGPL-2.1-or-later
// Standalone guards: compile with g++ -fsanitize=undefined. No FreeCAD binary.

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "Gui/View3DInventorViewerInternal.h"
#include "Mod/Part/Gui/ViewProviderGridExtensionInternal.h"

int main()
{
    using Gui::View3DInventorViewerInternal::finiteNormalizedToShortPixel;
    using Gui::View3DInventorViewerInternal::recoverPositiveExtent;
    using Gui::View3DInventorViewerInternal::recoveredOrthographicHeight;
    using Gui::View3DInventorViewerInternal::sketchEditFallbackHeight;
    using Gui::View3DInventorViewerInternal::volumeExtentsUsable;
    using PartGui::GridExtensionInternal::canWriteEditedField;
    using PartGui::GridExtensionInternal::planSketchGrid;
    using PartGui::GridExtensionInternal::recoverCameraExtent;

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // First invalid numeric state (UBSan, sibling of gdb WP 358):
    // roundf(Inf * 1024) stays Inf; static_cast<short> was 32767; UBSan did not trap.
    const float oldScaled = std::roundf(inf * 1024.0F);
    assert(!std::isfinite(oldScaled));

    assert(!volumeExtentsUsable(inf, 1.0F, 1.0F));
    assert(!volumeExtentsUsable(1.0F, inf, 1.0F));
    assert(!volumeExtentsUsable(1.0F, 1.0F, 0.0F));
    assert(volumeExtentsUsable(1.0F, 1.0F, 1.0F));

    assert(recoverPositiveExtent(inf, 200.0F) == 200.0F);
    assert(recoverPositiveExtent(40.0F, 200.0F) == 40.0F);
    assert(recoverPositiveExtent(inf, inf) == -1.0F);
    assert(recoverCameraExtent(inf, 10.0F) == 10.0F);

    float restoredHeight = 0.0F;
    assert(recoveredOrthographicHeight(inf, restoredHeight));
    assert(restoredHeight == sketchEditFallbackHeight);
    float pathologicalHeight = 0.0F;
    assert(recoveredOrthographicHeight(1.0e10F, pathologicalHeight));
    assert(pathologicalHeight == sketchEditFallbackHeight);
    assert(volumeExtentsUsable(1.1e6F, 1.1e6F, 1.0F));
    assert(volumeExtentsUsable(1.0e10F, 1.0e10F, 1.0F));
    float nearDist = nan;
    float farDist = inf;
    float focalDist = nan;
    assert(Gui::View3DInventorViewerInternal::recoveredCameraDistances(
        200.0F,
        nearDist,
        farDist,
        focalDist
    ));
    assert(std::isfinite(nearDist) && nearDist > 0.0F);
    assert(std::isfinite(farDist) && farDist > nearDist);
    assert(std::isfinite(focalDist));
    const auto afterViewObjects = recoverCameraExtent(inf, restoredHeight);
    const auto recoveredAfterRewrite = planSketchGrid(afterViewObjects, 10.0, 0.0, 0.0);
    assert(recoveredAfterRewrite.valid);
    assert(recoveredAfterRewrite.nlines == 2 * recoveredAfterRewrite.vlines);

    short pixel = 0;
    assert(!finiteNormalizedToShortPixel(inf, 1024, pixel));
    assert(!finiteNormalizedToShortPixel(nan, 1024, pixel));
    assert(!finiteNormalizedToShortPixel(0.5F, 0, pixel));
    assert(finiteNormalizedToShortPixel(0.5F, 1024, pixel));
    assert(pixel == 512);

    const auto invalid = planSketchGrid(inf, 10.0, 0.0, 0.0);
    assert(!invalid.valid);
    assert(invalid.nlines == 0);

    const auto recoveredExtent = recoverCameraExtent(inf, 200.0F);
    const auto valid = planSketchGrid(recoveredExtent, 10.0, 0.0, 0.0);
    assert(valid.valid);
    assert(valid.vlines >= 1);
    assert(valid.nlines == 2 * valid.vlines);
    assert(std::isfinite(valid.minX) && std::isfinite(valid.maxY));
    int ox = 0;
    int oy = 0;
    assert(PartGui::GridExtensionInternal::tryGridOffsets(
        valid.minX,
        valid.minY,
        10.0,
        valid.vlines,
        valid.nlines,
        ox,
        oy
    ));
    assert(ox == valid.offsetX);
    assert(oy == valid.offsetY);

    int dummyVertices[] = {2, 2};
    assert(canWriteEditedField(dummyVertices));
    assert(!canWriteEditedField(nullptr));

    std::cout << "camera_grid_guards ok"
              << " recovered_vlines=" << valid.vlines
              << " recovered_nlines=" << valid.nlines << "\n";
    return 0;
}
