// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OffsetChain.h"

#include <cmath>
#include <set>

#include <Precision.hxx>

namespace Sketcher
{

// Implemented beside the original homogeneous interval helpers in BSplineOffset.cpp.
OffsetChainDistanceResult certifyOffsetChainIntervals(
    const std::vector<const Part::GeomCurve*>& source,
    const std::vector<const Part::GeomCurve*>& candidate,
    const OffsetChainDistanceOptions& options
);

OffsetChainTopologyResult certifyOffsetChainTopologyIntervals(
    const std::vector<OffsetChainPiece>& chain,
    const OffsetChainTopologyOptions& options
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

OffsetChainTopologyResult certifyOffsetChainTopology(
    const std::vector<OffsetChainPiece>& chain,
    const OffsetChainTopologyOptions& options
)
{
    using Status = OffsetChainTopologyStatus;
    if (chain.empty() || chain.size() > 256 || !std::isfinite(options.joinTolerance)
        || options.joinTolerance < 0 || options.joinTolerance > Precision::Confusion()
        || !std::isfinite(options.clearance) || options.clearance < 0
        || options.maxWork < 1 || options.maxWork > 10000000
        || options.maxDepth < 1 || options.maxDepth > 24) {
        return {Status::InvalidInput, "Invalid topology certificate input or options."};
    }
    std::set<std::size_t> identities;
    for (const auto& piece : chain) {
        if (!piece.geometry || !identities.insert(piece.identity).second) {
            return {Status::InvalidInput, "Null geometry or duplicate piece identity."};
        }
        if (!dynamic_cast<const Part::GeomLineSegment*>(piece.geometry)
            && !dynamic_cast<const Part::GeomArcOfCircle*>(piece.geometry)
            && !dynamic_cast<const Part::GeomBSplineCurve*>(piece.geometry)) {
            return {Status::UnsupportedCurve, "Unsupported topology certificate geometry."};
        }
    }
    return certifyOffsetChainTopologyIntervals(chain, options);
}

}  // namespace Sketcher
