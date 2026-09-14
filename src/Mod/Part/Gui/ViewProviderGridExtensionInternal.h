// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cmath>
#include <limits>

namespace PartGui::GridExtensionInternal
{

struct SketchGridPlan
{
    bool valid {false};
    int vlines {0};
    int nlines {0};
    float gridDimension {0.0F};
    float minX {0.0F};
    float minY {0.0F};
    float maxX {0.0F};
    float maxY {0.0F};
};

inline float recoverCameraExtent(float observed, float fallback)
{
    if (std::isfinite(observed) && observed > 0.0F) {
        return observed;
    }
    if (std::isfinite(fallback) && fallback > 0.0F) {
        return fallback;
    }
    return -1.0F;
}

inline SketchGridPlan planSketchGrid(
    float camMaxDimension,
    double computedGridValue,
    double centerX,
    double centerY
)
{
    SketchGridPlan plan;
    if (!std::isfinite(camMaxDimension) || camMaxDimension <= 0.0F
        || !std::isfinite(computedGridValue) || computedGridValue <= 0.0
        || !std::isfinite(centerX) || !std::isfinite(centerY)) {
        return plan;
    }

    const float gridDimension = 1.5F * camMaxDimension;
    if (!std::isfinite(gridDimension) || gridDimension <= 0.0F) {
        return plan;
    }

    const double vlinesD = static_cast<double>(gridDimension) / computedGridValue;
    if (!std::isfinite(vlinesD) || vlinesD < 1.0 || vlinesD > 1000.0) {
        return plan;
    }

    plan.vlines = static_cast<int>(vlinesD);
    plan.nlines = 2 * plan.vlines;
    if (plan.vlines < 1 || plan.nlines < 2) {
        return plan;
    }

    float minX = static_cast<float>(centerX) - (gridDimension / 2.0F);
    float minY = static_cast<float>(centerY) - (gridDimension / 2.0F);
    float maxX = minX + gridDimension;
    float maxY = minY + gridDimension;
    if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(maxX)
        || !std::isfinite(maxY)) {
        return plan;
    }

    plan.valid = true;
    plan.gridDimension = gridDimension;
    plan.minX = minX;
    plan.minY = minY;
    plan.maxX = maxX;
    plan.maxY = maxY;
    return plan;
}

inline bool canWriteEditedField(const void* startEditingPointer)
{
    return startEditingPointer != nullptr;
}

}  // namespace PartGui::GridExtensionInternal
