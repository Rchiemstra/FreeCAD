// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <string>

#include <TopoDS_Face.hxx>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

struct InspectionExport ProjectionInput
{
    TopoDS_Face resolvedFace;
    std::array<double, 16> resolvedPlacement {
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    std::string sourceIdentity;
    double planarityToleranceMm {1.0e-6};
};

struct InspectionExport ProjectionResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    CanonicalProjection projection;
};

struct InspectionExport SourceIdentitySnapshot
{
    std::string documentUuid;
    std::string objectUuid;
    std::string subelementPath;
    std::array<double, 16> resolvedPlacement {};
    std::string projectionSha256;
    bool available {true};
};

enum class SourceSnapshotState
{
    Current,
    Missing,
    IdentityChanged,
    PlacementChanged,
    GeometryChanged,
    Invalid
};

// The first production increment accepts exact planar polygonal wires. Curved
// edges reject as UnsupportedGeometry until their exact canonical encodings
// are enabled; they are never tessellated into decision-capable geometry.
InspectionExport ProjectionResult projectPlanarFace(const ProjectionInput& input);

InspectionExport SourceSnapshotState compareSourceSnapshot(
    const SourceIdentitySnapshot& frozen,
    const SourceIdentitySnapshot& current,
    double placementTolerance
);

}  // namespace Inspection::Photo
