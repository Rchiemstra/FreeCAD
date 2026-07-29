// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionProjection.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <exception>
#include <utility>
#include <vector>

namespace Inspection::Photo
{
namespace
{

ProjectionResult failure(const DiagnosticCode code, std::string message)
{
    return {
        OperationStatus::InvalidInput,
        {code, DiagnosticSeverity::Error, std::move(message)},
        {},
    };
}

double distance(const Vector2d& lhs, const Vector2d& rhs)
{
    return std::hypot(lhs.x - rhs.x, lhs.y - rhs.y);
}

Vector3d vector(const gp_Pnt& point)
{
    return {point.X(), point.Y(), point.Z()};
}

Vector3d vector(const gp_Dir& direction)
{
    return {direction.X(), direction.Y(), direction.Z()};
}

ValidationResult extractPolygonalWire(
    const TopoDS_Wire& wire,
    const TopoDS_Face& face,
    const CanonicalFrame& frame,
    const bool hole,
    const double tolerance,
    PolylineCycle& output
)
{
    if (wire.IsNull()) {
        return ValidationResult::failure(DiagnosticCode::TopologyFailure, "face contains a null wire");
    }

    std::vector<Vector2d> points;
    Vector2d previousEnd;
    bool havePrevious = false;
    for (BRepTools_WireExplorer explorer(wire, face); explorer.More(); explorer.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
        if (edge.IsNull()) {
            return ValidationResult::failure(
                DiagnosticCode::TopologyFailure,
                "wire contains a null edge"
            );
        }
        BRepAdaptor_Curve curve(edge);
        if (curve.GetType() != GeomAbs_Line) {
            return ValidationResult::failure(
                DiagnosticCode::UnsupportedGeometry,
                "exact canonical support for this curved edge is not enabled"
            );
        }

        gp_Pnt first = curve.Value(curve.FirstParameter());
        gp_Pnt last = curve.Value(curve.LastParameter());
        if (edge.Orientation() == TopAbs_REVERSED) {
            std::swap(first, last);
        }
        const Vector2d start = projectToFrame(frame, vector(first));
        const Vector2d end = projectToFrame(frame, vector(last));
        if (!havePrevious) {
            points.push_back(start);
            havePrevious = true;
        }
        else if (distance(previousEnd, start) > tolerance) {
            return ValidationResult::failure(
                DiagnosticCode::TopologyFailure,
                "wire edges are open or not ordered continuously"
            );
        }
        if (distance(start, end) <= tolerance) {
            return ValidationResult::failure(
                DiagnosticCode::TopologyFailure,
                "wire contains a zero-length edge"
            );
        }
        points.push_back(end);
        previousEnd = end;
    }

    if (points.size() < 4 || distance(points.front(), points.back()) > tolerance) {
        return ValidationResult::failure(
            DiagnosticCode::TopologyFailure,
            "wire is open or contains fewer than three edges"
        );
    }
    points.pop_back();
    output = {hole, std::move(points)};
    return ValidationResult::success();
}

}  // namespace

ProjectionResult projectPlanarFace(const ProjectionInput& input)
{
    if (input.resolvedFace.IsNull()) {
        return failure(DiagnosticCode::InvalidGeometry, "resolved face is null");
    }
    if (!std::isfinite(input.planarityToleranceMm) || input.planarityToleranceMm <= 0.0) {
        return failure(DiagnosticCode::InvalidSchema, "planarity tolerance is invalid");
    }

    try {
        BRepAdaptor_Surface surface(input.resolvedFace, true);
        if (surface.GetType() != GeomAbs_Plane) {
            return failure(
                DiagnosticCode::UnsupportedGeometry,
                "photo inspection requires a planar face"
            );
        }
        const gp_Pln plane = surface.Plane();
        const auto frame
            = makeCanonicalFrame(vector(plane.Location()), vector(plane.Axis().Direction()));
        if (!frame) {
            return failure(DiagnosticCode::NumericalFailure, "cannot construct canonical frame");
        }

        const TopoDS_Wire outerWire = BRepTools::OuterWire(input.resolvedFace);
        if (outerWire.IsNull()) {
            return failure(DiagnosticCode::TopologyFailure, "face has no unambiguous outer wire");
        }

        ProjectionSnapshot snapshot;
        snapshot.frame = *frame;
        snapshot.sourcePlacement = input.resolvedPlacement;
        snapshot.planarityToleranceMm = input.planarityToleranceMm;

        bool foundOuter = false;
        for (TopExp_Explorer explorer(input.resolvedFace, TopAbs_WIRE); explorer.More();
             explorer.Next()) {
            const TopoDS_Wire wire = TopoDS::Wire(explorer.Current());
            const bool isOuter = wire.IsSame(outerWire);
            PolylineCycle cycle;
            const ValidationResult wireResult = extractPolygonalWire(
                wire,
                input.resolvedFace,
                *frame,
                !isOuter,
                input.planarityToleranceMm,
                cycle
            );
            if (!wireResult.valid) {
                return failure(wireResult.diagnostic.code, wireResult.diagnostic.message);
            }
            foundOuter = foundOuter || isOuter;
            snapshot.cycles.push_back(std::move(cycle));
        }
        if (!foundOuter) {
            return failure(DiagnosticCode::TopologyFailure, "outer wire was not enumerated");
        }

        CanonicalProjection projection;
        const ValidationResult canonical = canonicalizeProjection(snapshot, projection);
        if (!canonical.valid) {
            return failure(canonical.diagnostic.code, canonical.diagnostic.message);
        }
        return {OperationStatus::Complete, {}, std::move(projection)};
    }
    catch (const Standard_Failure& error) {
        return failure(
            DiagnosticCode::TopologyFailure,
            std::string("OCCT projection failure: ") + error.GetMessageString()
        );
    }
    catch (const std::exception& error) {
        return failure(
            DiagnosticCode::NumericalFailure,
            std::string("projection failure: ") + error.what()
        );
    }
    catch (...) {
        return failure(DiagnosticCode::NumericalFailure, "unknown projection failure");
    }
}

SourceSnapshotState compareSourceSnapshot(
    const SourceIdentitySnapshot& frozen,
    const SourceIdentitySnapshot& current,
    const double placementTolerance
)
{
    if (!std::isfinite(placementTolerance) || placementTolerance < 0.0
        || frozen.documentUuid.empty() || frozen.objectUuid.empty() || frozen.subelementPath.empty()
        || frozen.projectionSha256.size() != 64) {
        return SourceSnapshotState::Invalid;
    }
    if (!current.available) {
        return SourceSnapshotState::Missing;
    }
    if (frozen.documentUuid != current.documentUuid || frozen.objectUuid != current.objectUuid
        || frozen.subelementPath != current.subelementPath) {
        return SourceSnapshotState::IdentityChanged;
    }
    for (std::size_t index = 0; index < frozen.resolvedPlacement.size(); ++index) {
        if (!std::isfinite(frozen.resolvedPlacement[index])
            || !std::isfinite(current.resolvedPlacement[index])) {
            return SourceSnapshotState::Invalid;
        }
        if (std::abs(frozen.resolvedPlacement[index] - current.resolvedPlacement[index])
            > placementTolerance) {
            return SourceSnapshotState::PlacementChanged;
        }
    }
    if (frozen.projectionSha256 != current.projectionSha256) {
        return SourceSnapshotState::GeometryChanged;
    }
    return SourceSnapshotState::Current;
}

}  // namespace Inspection::Photo
