// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OffsetChain.h"
#include "BSplineOffset.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <GeomConvert.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Precision.hxx>

#include <Base/Exception.h>

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

namespace
{
using ChainStatus = OffsetChainStatus;
using V = Base::Vector3d;

struct GenerationFailure
{
    ChainStatus status;
    const char* reason;
};

V endpoint(const Part::GeomCurve& curve, bool last)
{
    return curve.value(last ? curve.getLastParameter() : curve.getFirstParameter());
}

V tangent(const Part::GeomCurve& curve, bool last)
{
    V direction;
    if (!curve.tangent(last ? curve.getLastParameter() : curve.getFirstParameter(), direction)) {
        throw GenerationFailure {ChainStatus::NotCertified, "Undefined source tangent."};
    }
    return direction.Normalize();
}

// Native trig endpoints at nonzero parameters cannot be algebraically equal to
// spline poles. Use the exact rational conic representation for welding, rather
// than relaxing the topology proof or exempting adjacent pieces from it.
std::unique_ptr<Part::GeomCurve> weldableCopy(const Part::GeomCurve& curve)
{
    if (dynamic_cast<const Part::GeomArcOfCircle*>(&curve)) {
        auto native = Handle(Geom_Curve)::DownCast(curve.handle());
        return std::make_unique<Part::GeomBSplineCurve>(
            GeomConvert::CurveToBSplineCurve(native, Convert_QuasiAngular)
        );
    }
    return std::unique_ptr<Part::GeomCurve>(static_cast<Part::GeomCurve*>(curve.copy()));
}

void checkWeldDisplacement(
    const Part::GeomCurve& curve, const V& first, const V& last, double tolerance
)
{
    const double a = (first - endpoint(curve, false)).Length();
    const double b = (last - endpoint(curve, true)).Length();
    if (!std::isfinite(a) || !std::isfinite(b) || a > tolerance || b > tolerance) {
        throw GenerationFailure {ChainStatus::NotCertified, "Join exceeds welding tolerance."};
    }
}

// A positive-weight, clamped spline moves by at most its largest pole movement.
// A line interpolates its endpoint movements. Never drop a native interval.
void weld(Part::GeomCurve& curve, const V& first, const V& last, double tolerance)
{
    checkWeldDisplacement(curve, first, last, tolerance);
    if (first == endpoint(curve, false) && last == endpoint(curve, true)) {
        return;
    }
    if (auto* line = dynamic_cast<Part::GeomLineSegment*>(&curve)) {
        line->setPoints(first, last);
    }
    else {
        auto native = Handle(Geom_BSplineCurve)::DownCast(curve.handle());
        native->Segment(native->FirstParameter(), native->LastParameter());
        native->SetPole(1, gp_Pnt(first.x, first.y, first.z));
        native->SetPole(native->NbPoles(), gp_Pnt(last.x, last.y, last.z));
    }
}

void weldChain(std::vector<OffsetChainOutputPiece>& chain, const OffsetChainOptions& options)
{
    std::vector<V> first, last;
    for (const auto& piece : chain) {
        first.push_back(endpoint(*piece.geometry, false));
        last.push_back(endpoint(*piece.geometry, true));
    }
    const size_t joins = options.closed ? chain.size() : chain.size() - 1;
    for (size_t i = 0; i < joins; ++i) {
        const size_t j = (i + 1) % chain.size();
        if ((last[i] - first[j]).Length() > options.joinTolerance) {
            throw GenerationFailure {
                ChainStatus::NotCertified, "Join requires trimming or extension."
            };
        }
        // Anchor to lines to preserve their analytic parameterization.
        V common = first[j] + (last[i] - first[j]) * 0.5;
        if (dynamic_cast<const Part::GeomLineSegment*>(chain[i].geometry.get())) {
            common = last[i];
        }
        else if (dynamic_cast<const Part::GeomLineSegment*>(chain[j].geometry.get())) {
            common = first[j];
        }
        last[i] = first[j] = common;
    }
    for (size_t i = 0; i < chain.size(); ++i) {
        weld(*chain[i].geometry, first[i], last[i], options.joinTolerance);
    }
}

}  // namespace

OffsetChainResult makeOffsetChain(
    const std::vector<OffsetChainPiece>& source,
    double distance,
    const OffsetChainOptions& options
)
{
    OffsetChainResult result;
    // Keep all provisional geometry local, so every exit on failure is atomic.
    std::vector<OffsetChainOutputPiece> output;
    try {
        const auto& n = options.sketchNormal;
        if (!std::isfinite(distance) || !std::isfinite(options.tolerance)
            || options.tolerance < Precision::Confusion()
            || std::abs(distance) <= 2 * options.tolerance || !std::isfinite(n.x)
            || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(n.Length())
            || n.Length() <= Precision::Confusion() || source.empty() || source.size() > 256
            || !std::isfinite(options.joinTolerance) || options.joinTolerance < 0
            || options.joinTolerance > Precision::Confusion() || options.maxWork < 1
            || options.maxWork > 10000000 || options.maxDepth < 1 || options.maxDepth > 24) {
            throw GenerationFailure {ChainStatus::InvalidInput, "Invalid offset options."};
        }
        OffsetChainTopologyOptions topologyOptions;
        topologyOptions.closed = options.closed;
        topologyOptions.joinTolerance = options.joinTolerance;
        topologyOptions.maxWork = options.maxWork;
        topologyOptions.maxDepth = options.maxDepth;
        const auto consume = [&]() {
            if (result.work >= options.maxWork) {
                throw GenerationFailure {
                    ChainStatus::BudgetExceeded, "Offset work budget exhausted."
                };
            }
            ++result.work;
        };
        std::set<std::size_t> identities;
        std::vector<OffsetChainOutputPiece> joinedSource;
        for (const auto& piece : source) {
            consume();
            if (!piece.geometry || !identities.insert(piece.identity).second) {
                throw GenerationFailure {
                    ChainStatus::InvalidInput, "Null geometry or duplicate source identity."
                };
            }
            if (!dynamic_cast<const Part::GeomLineSegment*>(piece.geometry)
                && !dynamic_cast<const Part::GeomArcOfCircle*>(piece.geometry)
                && !dynamic_cast<const Part::GeomBSplineCurve*>(piece.geometry)) {
                throw GenerationFailure {
                    ChainStatus::UnsupportedCurve, "Unsupported source curve."
                };
            }
            const auto native = Handle(Geom_Curve)::DownCast(piece.geometry->handle());
            if (native.IsNull() || !std::isfinite(native->FirstParameter())
                || !std::isfinite(native->LastParameter())
                || native->FirstParameter() >= native->LastParameter()) {
                throw GenerationFailure {
                    ChainStatus::InvalidInput, "Invalid source interval."
                };
            }
            if (const auto* spline = dynamic_cast<const Part::GeomBSplineCurve*>(piece.geometry)) {
                BSplineOffsetOptions splineOptions;
                splineOptions.distance = distance;
                splineOptions.tolerance = options.tolerance;
                splineOptions.sketchNormal = n;
                const auto status = checkBSplineOffsetInput(*spline, splineOptions);
                if (status != BSplineOffsetStatus::Success) {
                    throw GenerationFailure {
                        ChainStatus::UnsupportedCurve, "Unsupported source spline."
                    };
                }
            }
            if (dynamic_cast<const Part::GeomArcOfCircle*>(piece.geometry)
                && (native->FirstParameter() < -8 || native->LastParameter() > 8)) {
                throw GenerationFailure {
                    ChainStatus::UnsupportedCurve, "Unsupported arc interval."
                };
            }
            joinedSource.push_back({weldableCopy(*piece.geometry), piece.identity});
        }
        weldChain(joinedSource, options);
        std::vector<OffsetChainPiece> sourcePieces;
        for (size_t i = 0; i < joinedSource.size(); ++i) {
            const auto& piece = joinedSource[i];
            checkWeldDisplacement(*piece.geometry, endpoint(*source[i].geometry, false),
                                  endpoint(*source[i].geometry, true), options.joinTolerance);
            sourcePieces.push_back({piece.geometry.get(), piece.identity});
        }
        consume();
        if (result.work == options.maxWork) {
            throw GenerationFailure {ChainStatus::BudgetExceeded, "Offset work budget exhausted."};
        }
        topologyOptions.maxWork = options.maxWork - result.work;
        const auto sourceTopology = certifyOffsetChainTopology(sourcePieces, topologyOptions);
        result.work += sourceTopology.work;
        if (sourceTopology.status != ChainStatus::Success) {
            result.status = sourceTopology.status;
            result.reason = sourceTopology.message;
            return result;
        }
        const V normal = V(n).Normalize();
        const V origin = endpoint(*source.front().geometry, false);
        const size_t joins = options.closed ? source.size() : source.size() - 1;
        for (size_t i = 0; i < joins; ++i) {
            consume();
            const auto& a = *source[i].geometry;
            const auto& b = *source[(i + 1) % source.size()].geometry;
            const V ta = tangent(a, true), tb = tangent(b, false);
            if (ta.Dot(tb) <= 0 || ta.Cross(tb).Length() > Precision::Angular()) {
                throw GenerationFailure {ChainStatus::NotCertified, "Non-G1 source junction."};
            }
        }
        std::vector<const Part::GeomCurve*> sourceView;
        for (const auto& piece : source) {
            consume();
            const auto& curve = *piece.geometry;
            const auto inPlane = [&](const V& p) {
                return std::abs((p - origin).Dot(normal)) <= Precision::Confusion();
            };
            if (const auto* line = dynamic_cast<const Part::GeomLineSegment*>(&curve)) {
                const V direction = tangent(curve, false);
                if (!inPlane(line->getStartPoint()) || !inPlane(line->getEndPoint())
                    || std::abs(direction.Dot(normal)) > Precision::Angular()) {
                    throw GenerationFailure {
                        ChainStatus::UnsupportedCurve, "Nonplanar source line."
                    };
                }
                const V shift = direction.Cross(normal) * distance;
                auto offset = std::make_unique<Part::GeomLineSegment>();
                offset->setPoints(line->getStartPoint() + shift, line->getEndPoint() + shift);
                output.push_back({std::move(offset), piece.identity});
            }
            else if (const auto* arc = dynamic_cast<const Part::GeomArcOfCircle*>(&curve)) {
                auto trimmed = Handle(Geom_TrimmedCurve)::DownCast(arc->handle());
                auto circle = Handle(Geom_Circle)::DownCast(trimmed->BasisCurve());
                const auto axis = circle->Axis().Direction();
                const V axisVector(axis.X(), axis.Y(), axis.Z());
                const auto center = circle->Location();
                if (axisVector.Cross(normal).Length() > Precision::Angular()
                    || !inPlane(V(center.X(), center.Y(), center.Z()))) {
                    throw GenerationFailure {
                        ChainStatus::UnsupportedCurve, "Nonplanar source arc."
                    };
                }
                const double radius = circle->Radius() + distance * axisVector.Dot(normal);
                if (!std::isfinite(radius) || radius <= Precision::Confusion()) {
                    throw GenerationFailure {ChainStatus::NotCertified, "Offset arc collapses."};
                }
                auto offset = std::unique_ptr<Part::GeomArcOfCircle>(
                    static_cast<Part::GeomArcOfCircle*>(arc->copy())
                );
                offset->setRadius(radius);
                output.push_back({std::move(offset), piece.identity});
            }
            else {
                const auto& spline = static_cast<const Part::GeomBSplineCurve&>(curve);
                auto native = Handle(Geom_BSplineCurve)::DownCast(spline.handle());
                for (int i = 1; i <= native->NbPoles(); ++i) {
                    const auto p = native->Pole(i);
                    if (!inPlane(V(p.X(), p.Y(), p.Z()))) {
                        throw GenerationFailure {
                            ChainStatus::UnsupportedCurve, "Nonplanar source spline."
                        };
                    }
                }
                consume();
                BSplineOffsetOptions splineOptions;
                splineOptions.distance = distance;
                splineOptions.tolerance = options.tolerance;
                splineOptions.sketchNormal = normal;
                splineOptions.maxValidationSpans = std::min(
                    splineOptions.maxValidationSpans, options.maxWork - result.work - 1
                );
                splineOptions.maxRefinementDepth = std::min(
                    splineOptions.maxRefinementDepth, options.maxDepth
                );
                if (splineOptions.maxValidationSpans < 16) {
                    throw GenerationFailure {
                        ChainStatus::BudgetExceeded, "Offset work budget exhausted."
                    };
                }
                auto fitted = makeBSplineOffset(spline, splineOptions);
                result.work += fitted.complexity.validationSpans;
                if (fitted.status != BSplineOffsetStatus::Success) {
                    result.status = fitted.status == BSplineOffsetStatus::ComplexityExceeded
                        ? ChainStatus::BudgetExceeded : ChainStatus::NotCertified;
                    result.reason = fitted.message;
                    return result;
                }
                output.push_back({std::move(fitted.curve), piece.identity});
            }
            sourceView.push_back(piece.geometry);
        }
        std::vector<V> nominalFirst, nominalLast;
        for (auto& piece : output) {
            nominalFirst.push_back(endpoint(*piece.geometry, false));
            nominalLast.push_back(endpoint(*piece.geometry, true));
            if (dynamic_cast<const Part::GeomArcOfCircle*>(piece.geometry.get())) {
                piece.geometry = weldableCopy(*piece.geometry);
            }
        }
        weldChain(output, options);
        std::vector<const Part::GeomCurve*> candidate;
        std::vector<OffsetChainPiece> finalPieces;
        for (size_t i = 0; i < output.size(); ++i) {
            consume();
            const auto& piece = output[i];
            // Bound conversion plus welding together, against the analytic offset.
            checkWeldDisplacement(*piece.geometry, nominalFirst[i], nominalLast[i],
                                  options.joinTolerance);
            candidate.push_back(piece.geometry.get());
            finalPieces.push_back({piece.geometry.get(), piece.identity});
        }
        consume();
        OffsetChainDistanceOptions distanceOptions;
        distanceOptions.distance = distance;
        distanceOptions.tolerance = options.tolerance;
        distanceOptions.sketchNormal = normal;
        if (result.work == options.maxWork) {
            throw GenerationFailure {ChainStatus::BudgetExceeded, "Offset work budget exhausted."};
        }
        distanceOptions.maxWork = options.maxWork - result.work;
        distanceOptions.maxDepth = options.maxDepth;
        result.distanceCertificate = certifyOffsetChainDistance(
            sourceView, candidate, distanceOptions
        );
        result.work += result.distanceCertificate.work;
        if (result.distanceCertificate.status != ChainStatus::Success) {
            result.status = result.distanceCertificate.status;
            result.reason = result.distanceCertificate.message;
            return result;
        }
        consume();
        if (result.work == options.maxWork) {
            throw GenerationFailure {ChainStatus::BudgetExceeded, "Offset work budget exhausted."};
        }
        topologyOptions.maxWork = options.maxWork - result.work;
        result.topologyCertificate = certifyOffsetChainTopology(finalPieces, topologyOptions);
        result.work += result.topologyCertificate.work;
        if (result.topologyCertificate.status != ChainStatus::Success) {
            result.status = result.topologyCertificate.status;
            result.reason = result.topologyCertificate.message;
            return result;
        }
        result.pieces = std::move(output);
        result.status = ChainStatus::Success;
        result.reason = "Offset chain certified.";
    }
    catch (const GenerationFailure& failure) {
        result.status = failure.status;
        result.reason = failure.reason;
    }
    catch (const Standard_Failure&) {
        result.status = ChainStatus::NotCertified;
        result.reason = "Offset kernel failure.";
    }
    catch (const Base::Exception&) {
        result.status = ChainStatus::InvalidInput;
        result.reason = "Invalid offset geometry.";
    }
    return result;
}

}  // namespace Sketcher
