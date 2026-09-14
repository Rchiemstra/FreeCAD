#pragma once

#include <cmath>
#include <limits>

namespace Gui::View3DInventorViewerInternal
{

inline bool volumeExtentsUsable(float volumeWidth, float volumeHeight, float volumeDepth)
{
    return std::isfinite(volumeWidth) && std::isfinite(volumeHeight)
        && std::isfinite(volumeDepth) && volumeWidth > 0.0F && volumeHeight > 0.0F
        && volumeDepth > 0.0F;
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
    out = recoverPositiveExtent(observed, sketchEditFallbackHeight);
    return out > 0.0F;
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
