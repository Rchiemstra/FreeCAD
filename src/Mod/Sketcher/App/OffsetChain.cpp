// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OffsetChain.h"

#include <cmath>

#include <Precision.hxx>

namespace Sketcher
{

// Implemented beside the original homogeneous interval helpers in BSplineOffset.cpp.
OffsetChainDistanceResult certifyOffsetChainIntervals(
    const std::vector<const Part::GeomCurve*>& source,
    const std::vector<const Part::GeomCurve*>& candidate,
    const OffsetChainDistanceOptions& options
);

OffsetChainDistanceResult certifyOffsetChainDistance(
    const std::vector<const Part::GeomCurve*>& source,
    const std::vector<const Part::GeomCurve*>& candidate,
    const OffsetChainDistanceOptions& options
)
{
    using Status = OffsetChainDistanceStatus;
    const auto& n = options.sketchNormal;
    if (!std::isfinite(options.distance) || !std::isfinite(options.tolerance)
        || options.tolerance < Precision::Confusion()
        || std::abs(options.distance) <= 2 * options.tolerance || !std::isfinite(n.x)
        || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(n.Length())
        || n.Length() <= Precision::Confusion() || options.maxWork < 1 || options.maxWork > 10000000
        || options.maxDepth < 1 || options.maxDepth > 24 || source.empty() || candidate.empty()
        || source.size() > 256 || candidate.size() > 256) {
        return {Status::InvalidInput, "Invalid distance certificate input or options."};
    }
    for (const auto* chain : {&source, &candidate}) {
        for (const auto* curve : *chain) {
            if (!curve) {
                return {Status::InvalidInput, "Null chain geometry."};
            }
            if (!dynamic_cast<const Part::GeomLineSegment*>(curve)
                && !dynamic_cast<const Part::GeomArcOfCircle*>(curve)
                && !dynamic_cast<const Part::GeomBSplineCurve*>(curve)) {
                return {Status::UnsupportedCurve, "Unsupported distance certificate geometry."};
            }
        }
    }
    return certifyOffsetChainIntervals(source, candidate, options);
}

}  // namespace Sketcher
