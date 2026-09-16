#pragma once

#include <cmath>
#include <limits>

namespace Gui::View3DInventorViewerInternal
{

constexpr float minUsablePickExtent = 1.0e-6F;

// Sketch-edit collapsed-camera heuristic only (skip-bbox / 81856 traces).
// Do not use this for generic viewer picking.
constexpr float maxUsablePickExtent = 1.0e6F;

inline bool isUsablePickExtent(float extent)
{
    return std::isfinite(extent) && extent > minUsablePickExtent;
}

inline bool isSketchCollapsedCameraExtent(float extent)
{
    return !std::isfinite(extent) || extent <= minUsablePickExtent
        || extent >= maxUsablePickExtent;
}

inline bool volumeExtentsUsable(float volumeWidth, float volumeHeight, float volumeDepth)
{
    // Depth is only required to be a positive finite span. Near/far can be
    // large while the ortho height is still a usable pick scale.
    return isUsablePickExtent(volumeWidth) && isUsablePickExtent(volumeHeight)
        && std::isfinite(volumeDepth) && volumeDepth > 0.0F;
}

inline bool isUsablePickVolume(
    bool hasCamera,
    int viewportWidthPx,
    int viewportHeightPx,
    float volumeWidth,
    float volumeHeight,
    float volumeDepth
)
{
    return hasCamera && viewportWidthPx > 0 && viewportHeightPx > 0
        && volumeExtentsUsable(volumeWidth, volumeHeight, volumeDepth);
}

inline float recoverPositiveExtent(float observed, float fallback)
{
    if (std::isfinite(observed) && observed > 0.0F) {
        return observed;
    }
    if (std::isfinite(fallback) && fallback > 0.0F) {
        return fallback;
    }
    return -1.0F;
}

// Sketch edit uses this when Coin's ortho height is Inf/NaN/<=0 (gdb WP 358).
constexpr float sketchEditFallbackHeight = 200.0F;

inline bool recoveredOrthographicHeight(float observed, float& out)
{
    if (!isSketchCollapsedCameraExtent(observed)) {
        out = observed;
        return true;
    }
    out = sketchEditFallbackHeight;
    return isUsablePickExtent(out);
}

inline bool recoveredCameraDistances(
    float height,
    float& nearDist,
    float& farDist,
    float& focalDist
)
{
    if (isSketchCollapsedCameraExtent(height)) {
        height = sketchEditFallbackHeight;
    }
    bool changed = false;
    if (!std::isfinite(nearDist) || nearDist <= 0.0F) {
        const float fromHeight = height * 0.005F;
        nearDist = fromHeight > 0.1F ? fromHeight : 0.1F;
        changed = true;
    }
    if (!std::isfinite(farDist) || farDist <= nearDist) {
        const float fromHeight = height * 10.0F;
        const float fromNear = nearDist * 10.0F;
        farDist = fromHeight > fromNear ? fromHeight : fromNear;
        changed = true;
    }
    if (!std::isfinite(focalDist) || focalDist < nearDist || focalDist > farDist) {
        focalDist = 0.5F * (nearDist + farDist);
        changed = true;
    }
    return changed;
}

inline bool finiteNormalizedToShortPixel(float normalized, int pixels, short& out)
{
    if (pixels <= 0 || !std::isfinite(normalized)) {
        return false;
    }
    const float scaled = std::roundf(normalized * static_cast<float>(pixels));
    if (!std::isfinite(scaled)) {
        return false;
    }
    const auto minShort = static_cast<float>(std::numeric_limits<short>::min());
    const auto maxShort = static_cast<float>(std::numeric_limits<short>::max());
    if (scaled < minShort || scaled > maxShort) {
        return false;
    }
    out = static_cast<short>(scaled);
    return true;
}

template<typename RedrawRequest>
inline bool requestDetachedNavigationRedraw(
    bool eventProcessed,
    bool cameraNavigationActive,
    bool isLocationEvent,
    bool isDetachedView,
    RedrawRequest&& requestRedraw
)
{
    if (!eventProcessed || !cameraNavigationActive || !isLocationEvent || !isDetachedView) {
        return false;
    }

    requestRedraw();
    return true;
}

}  // namespace Gui::View3DInventorViewerInternal
