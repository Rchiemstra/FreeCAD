// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OffsetChain.h"
#include "BSplineOffset.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

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

bool offsetChainEndpointsWithinTolerance(
    const Part::GeomCurve& a, bool lastA,
    const Part::GeomCurve& b, bool lastB, double tolerance
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
    const Part::GeomCurve& before, const Part::GeomCurve& after, double tolerance
)
{
    if (!offsetChainEndpointsWithinTolerance(before, false, after, false, tolerance)
        || !offsetChainEndpointsWithinTolerance(before, true, after, true, tolerance)) {
        throw GenerationFailure {ChainStatus::NotCertified, "Join exceeds welding tolerance."};
    }
}

// A positive-weight, clamped spline moves by at most its largest pole movement.
// A line interpolates its endpoint movements. Never drop a native interval.
void weld(Part::GeomCurve& curve, const V& first, const V& last, double tolerance)
{
    if (first == endpoint(curve, false) && last == endpoint(curve, true)) {
        return;
    }
    const std::unique_ptr<Part::GeomCurve> before(
        static_cast<Part::GeomCurve*>(curve.copy())
    );
    if (auto* line = dynamic_cast<Part::GeomLineSegment*>(&curve)) {
        line->setPoints(first, last);
    }
    else {
        auto native = Handle(Geom_BSplineCurve)::DownCast(curve.handle());
        native->Segment(native->FirstParameter(), native->LastParameter());
        native->SetPole(1, gp_Pnt(first.x, first.y, first.z));
        native->SetPole(native->NbPoles(), gp_Pnt(last.x, last.y, last.z));
    }
    checkWeldDisplacement(*before, curve, tolerance);
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
        if (!offsetChainEndpointsWithinTolerance(
                *chain[i].geometry, true, *chain[j].geometry, false, options.joinTolerance)) {
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

OffsetChainSelectionResult orderOffsetChain(const std::vector<OffsetChainPiece>& input)
{
    OffsetChainSelectionResult result;
    try {
        const auto refuse = [](const char* reason) { throw std::runtime_error(reason); };
        if (input.size() < 2 || input.size() > 256) {
            refuse("Select between 2 and 256 mixed edges.");
        }
        // Canonicalize before finding endpoints or choosing a cycle seam. In an
        // orientation tie, the smallest stable source identity decides the side.
        auto selection = input;
        std::sort(selection.begin(), selection.end(), [](const auto& a, const auto& b) {
            return a.identity < b.identity;
        });
        bool spline = false, analytic = false;
        std::set<std::size_t> identities;
        std::vector<V> endpoints;
        for (const auto& piece : selection) {
            const auto* curve = piece.geometry;
            if (!curve || !identities.insert(piece.identity).second) {
                refuse("Null geometry or duplicate selection identity.");
            }
            if (const auto* bspline = dynamic_cast<const Part::GeomBSplineCurve*>(curve)) {
                spline = true;
                if (checkBSplineOffsetInput(*bspline) != BSplineOffsetStatus::Success) {
                    refuse("Unsupported source spline.");
                }
            }
            else if (dynamic_cast<const Part::GeomLineSegment*>(curve)
                     || dynamic_cast<const Part::GeomArcOfCircle*>(curve)) {
                analytic = true;
            }
            else {
                refuse("Unsupported selected geometry.");
            }
            for (bool last : {false, true}) {
                const V point = endpoint(*curve, last);
                if (!std::isfinite(point.x) || !std::isfinite(point.y)
                    || !std::isfinite(point.z)) {
                    refuse("Nonfinite selection endpoint.");
                }
                endpoints.push_back(point);
            }
        }
        if (!spline || !analytic) {
            refuse("Select a mixed chain containing B-splines and lines or arcs.");
        }
        std::vector<int> neighbor(endpoints.size(), -1);
        for (size_t i = 0; i < endpoints.size(); ++i) {
            for (size_t j = i + 1; j < endpoints.size(); ++j) {
                if ((endpoints[i] - endpoints[j]).Length() > Precision::Confusion()) {
                    continue;
                }
                if (i / 2 == j / 2 || neighbor[i] >= 0 || neighbor[j] >= 0) {
                    refuse("Branched or ambiguous selection endpoints.");
                }
                neighbor[i] = static_cast<int>(j);
                neighbor[j] = static_cast<int>(i);
            }
        }
        const auto openEnds = std::count(neighbor.begin(), neighbor.end(), -1);
        if (openEnds != 0 && openEnds != 2) {
            refuse("Disconnected selection.");
        }
        result.closed = openEnds == 0;
        size_t start = result.closed ? 0 : std::find(neighbor.begin(), neighbor.end(), -1)
            - neighbor.begin();
        const size_t alternative = result.closed ? 1
            : std::find(neighbor.begin() + start + 1, neighbor.end(), -1) - neighbor.begin();
        const auto reversals = [&](size_t entry) {
            const size_t first = entry;
            size_t count = 0;
            for (size_t i = 0; i < selection.size(); ++i) {
                count += entry % 2;
                const int next = neighbor[entry ^ 1];
                if (next < 0 || static_cast<size_t>(next) == first) {
                    break;
                }
                entry = static_cast<size_t>(next);
            }
            return count;
        };
        // Preserve the prevailing input orientation, independently of selection
        // order. Unnecessary kernel arc reversals can also move the native seam.
        if (reversals(alternative) < reversals(start)) {
            start = alternative;
        }
        size_t current = start;
        std::vector<bool> visited(selection.size(), false);
        std::vector<OffsetChainOutputPiece> ordered;
        while (!visited[current / 2]) {
            const auto index = current / 2;
            visited[index] = true;
            auto copy = std::unique_ptr<Part::GeomCurve>(
                static_cast<Part::GeomCurve*>(selection[index].geometry->clone())
            );
            if (current % 2) {
                copy->reverse();
            }
            ordered.push_back({std::move(copy), selection[index].identity});
            const int next = neighbor[current ^ 1];
            if (next < 0) {
                break;
            }
            current = static_cast<size_t>(next);
        }
        if (ordered.size() != selection.size() || (result.closed && current != start)) {
            refuse("Disconnected selection or ambiguous cycle.");
        }
        result.pieces = std::move(ordered);
        result.status = OffsetChainStatus::Success;
    }
    catch (const Standard_Failure& e) {
        result.reason = e.GetMessageString();
    }
    catch (const Base::Exception& e) {
        result.reason = e.what();
    }
    catch (const std::exception& e) {
        result.reason = e.what();
    }
    catch (...) {
        result.reason = "Unable to order the selected chain.";
    }
    return result;
}

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
        }
        consume();
        if (result.work == options.maxWork) {
            throw GenerationFailure {ChainStatus::BudgetExceeded, "Offset work budget exhausted."};
        }
        topologyOptions.maxWork = options.maxWork - result.work;
        const auto sourceTopology = certifyOffsetChainTopology(source, topologyOptions);
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
        std::vector<std::unique_ptr<Part::GeomCurve>> nominal;
        for (auto& piece : output) {
            nominal.emplace_back(static_cast<Part::GeomCurve*>(piece.geometry->copy()));
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
            checkWeldDisplacement(*nominal[i], *piece.geometry, options.joinTolerance);
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
