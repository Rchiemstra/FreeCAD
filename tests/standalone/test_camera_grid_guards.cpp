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
    using Gui::View3DInventorViewerInternal::volumeExtentsUsable;
    using PartGui::GridExtensionInternal::canWriteEditedField;
    using PartGui::GridExtensionInternal::planSketchGrid;
    using PartGui::GridExtensionInternal::recoverCameraExtent;

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    assert(!volumeExtentsUsable(inf, 1.0F, 1.0F));
    assert(!volumeExtentsUsable(1.0F, inf, 1.0F));
    assert(!volumeExtentsUsable(1.0F, 1.0F, 0.0F));
    assert(volumeExtentsUsable(1.0F, 1.0F, 1.0F));

    assert(recoverPositiveExtent(inf, 200.0F) == 200.0F);
    assert(recoverPositiveExtent(40.0F, 200.0F) == 40.0F);
    assert(recoverPositiveExtent(inf, inf) == -1.0F);
    assert(recoverCameraExtent(inf, 10.0F) == 10.0F);

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

    int dummyVertices[] = {2, 2};
    assert(canWriteEditedField(dummyVertices));
    assert(!canWriteEditedField(nullptr));

    std::cout << "camera_grid_guards ok"
              << " recovered_vlines=" << valid.vlines
              << " recovered_nlines=" << valid.nlines << "\n";
    return 0;
}
