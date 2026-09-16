// SPDX-License-Identifier: LGPL-2.1-or-later
// Regressions for the three re-review offset counterexamples.
// Compile: g++ -std=c++20 -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

#include "Mod/Part/Gui/ViewProviderGridExtensionInternal.h"

int main()
{
    using PartGui::GridExtensionInternal::planSketchGrid;
    using PartGui::GridExtensionInternal::recoverCameraExtent;
    using PartGui::GridExtensionInternal::tryGridOffsets;

    constexpr float cam = 100.0F;
    constexpr double spacing = 1.0;

    // Reviewer case 1: large finite X is accepted by line-count checks, not by offsets.
    const auto largeX = planSketchGrid(cam, spacing, 10000000000.0, 0.0);
    assert(!largeX.valid);
    assert(largeX.vlines == 0 || largeX.nlines == 0 || !largeX.valid);

    int offsetX = 0;
    int offsetY = 0;
    assert(!tryGridOffsets(static_cast<float>(10000000000.0) - 75.0F, 0.0F, spacing, 150, 300, offsetX, offsetY));

    // Reviewer case 2: representable cast, then X index add overflows.
    assert(!tryGridOffsets(2147483520.0F, 0.0F, spacing, 150, 300, offsetX, offsetY));

    // Reviewer case 3: representable Y cast, then subtract vlines overflows.
    const float minYNearMin =
        static_cast<float>(std::numeric_limits<int>::min() + 20);
    assert(!tryGridOffsets(0.0F, minYNearMin, spacing, 150, 300, offsetX, offsetY));

    // Recovery: Inf camera → fallback extent → valid offsets and a usable plan.
    const auto recovered = planSketchGrid(recoverCameraExtent(
            std::numeric_limits<float>::infinity(),
            200.0F
        ),
        10.0,
        0.0,
        0.0);
    assert(recovered.valid);
    assert(recovered.vlines >= 1);
    assert(recovered.nlines == 2 * recovered.vlines);
    assert(tryGridOffsets(
        recovered.minX,
        recovered.minY,
        10.0,
        recovered.vlines,
        recovered.nlines,
        offsetX,
        offsetY
    ));
    assert(offsetX == recovered.offsetX);
    assert(offsetY == recovered.offsetY);

    const auto origin = planSketchGrid(cam, spacing, 0.0, 0.0);
    assert(origin.valid);
    assert(origin.vlines == 150);
    assert(origin.nlines == 300);

    std::cout << "grid_offset_overflow ok origin_vlines=" << origin.vlines
              << " recovered_vlines=" << recovered.vlines << "\n";
    return 0;
}
