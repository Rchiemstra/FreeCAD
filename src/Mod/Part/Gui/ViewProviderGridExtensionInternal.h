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
    int offsetX {0};
    int offsetY {0};
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

inline bool tryCastToInt(double value, int& out)
{
    if (!std::isfinite(value)) {
        return false;
    }
    constexpr double minI = static_cast<double>(std::numeric_limits<int>::min());
    constexpr double maxI = static_cast<double>(std::numeric_limits<int>::max());
    if (value < minI || value > maxI) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

inline bool tryAddInt(int left, int right, int& out)
{
    if (right > 0 && left > std::numeric_limits<int>::max() - right) {
        return false;
    }
    if (right < 0 && left < std::numeric_limits<int>::min() - right) {
        return false;
    }
    out = left + right;
    return true;
}

inline bool trySubInt(int left, int right, int& out)
{
    if (right < 0 && left > std::numeric_limits<int>::max() + right) {
        return false;
    }
    if (right > 0 && left < std::numeric_limits<int>::min() + right) {
        return false;
    }
    out = left - right;
    return true;
}

inline bool tryGridOffsets(
    float minX,
    float minY,
    double gridValue,
    int vlines,
    int nlines,
    int& offsetX,
    int& offsetY
)
{
    if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(gridValue)
        || gridValue <= 0.0 || vlines < 1 || nlines < 2) {
        return false;
    }

    int fromMinX = 0;
    int fromMinY = 0;
    if (!tryCastToInt(static_cast<double>(minX) / gridValue, fromMinX)
        || !tryCastToInt(static_cast<double>(minY) / gridValue, fromMinY)) {
        return false;
    }

    int lastX = 0;
    int lastY = 0;
    int firstY = 0;
    if (!tryAddInt(fromMinX, vlines - 1, lastX) || !trySubInt(fromMinY, vlines, offsetY)
        || !tryAddInt(offsetY, vlines, firstY) || !tryAddInt(offsetY, nlines - 1, lastY)) {
        return false;
    }

    const auto finiteStepCoord = [gridValue](int step) {
        const double coord = static_cast<double>(step) * gridValue;
        return std::isfinite(coord) && std::isfinite(static_cast<float>(coord));
    };
    if (!finiteStepCoord(fromMinX) || !finiteStepCoord(lastX) || !finiteStepCoord(firstY)
        || !finiteStepCoord(lastY)) {
        return false;
    }

    offsetX = fromMinX;
    return true;
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

    if (!tryGridOffsets(
            minX,
            minY,
            computedGridValue,
            plan.vlines,
            plan.nlines,
            plan.offsetX,
            plan.offsetY
        )) {
        return {};
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

// Rejected builds must not allocate. Failed builds after allocate must drop refs.
template<typename Node>
struct ScopedCoinRef
{
    Node* ptr {nullptr};
    explicit ScopedCoinRef(Node* node)
        : ptr(node)
    {
        if (ptr) {
            ptr->ref();
        }
    }
    ScopedCoinRef(const ScopedCoinRef&) = delete;
    ScopedCoinRef& operator=(const ScopedCoinRef&) = delete;
    ~ScopedCoinRef()
    {
        if (ptr) {
            ptr->unref();
        }
    }
};

template<typename Allocate>
bool allocateGridNodesAfterPlan(bool planValid, Allocate&& allocate)
{
    if (!planValid) {
        return false;
    }
    allocate();
    return true;
}

}  // namespace PartGui::GridExtensionInternal
