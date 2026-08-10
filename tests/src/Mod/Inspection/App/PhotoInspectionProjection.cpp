// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <Mod/Inspection/App/PhotoInspectionProjection.h>

namespace
{

using Inspection::Photo::compareSourceSnapshot;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::ProjectionInput;
using Inspection::Photo::projectPlanarFace;
using Inspection::Photo::SourceIdentitySnapshot;
using Inspection::Photo::SourceSnapshotState;

TopoDS_Wire rectangleWire(
    const double x,
    const double y,
    const double width,
    const double height,
    const double z = 0.0
)
{
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(gp_Pnt(x, y, z));
    polygon.Add(gp_Pnt(x + width, y, z));
    polygon.Add(gp_Pnt(x + width, y + height, z));
    polygon.Add(gp_Pnt(x, y + height, z));
    polygon.Close();
    return polygon.Wire();
}

TopoDS_Face rectangleFace(const double z = 0.0)
{
    return BRepBuilderAPI_MakeFace(rectangleWire(0.0, 0.0, 100.0, 50.0, z)).Face();
}

TEST(PhotoInspectionProjectionTest, planarPolygonProjectsToCanonicalSnapshot)
{
    ProjectionInput input;
    input.resolvedFace = rectangleFace(12.0);
    input.sourceIdentity = "source-1";

    const auto result = projectPlanarFace(input);
    ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
    ASSERT_EQ(result.projection.snapshot.cycles.size(), 1);
    EXPECT_FALSE(result.projection.snapshot.cycles.front().hole);
    EXPECT_EQ(result.projection.snapshot.cycles.front().points.size(), 4);
    EXPECT_EQ(result.projection.sha256.size(), 64);
    EXPECT_DOUBLE_EQ(result.projection.snapshot.frame.origin.z, 12.0);
}

TEST(PhotoInspectionProjectionTest, faceAndWireOrientationDoNotChangeCanonicalHash)
{
    ProjectionInput forward;
    forward.resolvedFace = rectangleFace();
    ProjectionInput reversed;
    reversed.resolvedFace = TopoDS::Face(forward.resolvedFace.Reversed());

    const auto first = projectPlanarFace(forward);
    const auto second = projectPlanarFace(reversed);
    ASSERT_EQ(first.status, OperationStatus::Complete) << first.diagnostic.message;
    ASSERT_EQ(second.status, OperationStatus::Complete) << second.diagnostic.message;
    EXPECT_EQ(first.projection.bytes, second.projection.bytes);
    EXPECT_EQ(first.projection.sha256, second.projection.sha256);
}

TEST(PhotoInspectionProjectionTest, innerPolygonIsClassifiedAsHole)
{
    BRepBuilderAPI_MakeFace builder(rectangleWire(0.0, 0.0, 100.0, 50.0));
    const TopoDS_Wire hole = TopoDS::Wire(rectangleWire(20.0, 10.0, 10.0, 10.0).Reversed());
    builder.Add(hole);
    ProjectionInput input;
    input.resolvedFace = builder.Face();

    const auto result = projectPlanarFace(input);
    ASSERT_EQ(result.status, OperationStatus::Complete) << result.diagnostic.message;
    ASSERT_EQ(result.projection.snapshot.cycles.size(), 2);
    EXPECT_FALSE(result.projection.snapshot.cycles[0].hole);
    EXPECT_TRUE(result.projection.snapshot.cycles[1].hole);
}

TEST(PhotoInspectionProjectionTest, nullAndCurvedEdgesRejectWithoutApproximation)
{
    ProjectionInput nullInput;
    auto result = projectPlanarFace(nullInput);
    EXPECT_EQ(result.status, OperationStatus::InvalidInput);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::InvalidGeometry);

    const gp_Circ circle(gp_Ax2(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(0.0, 0.0, 1.0)), 10.0);
    const TopoDS_Wire circularWire
        = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circle).Edge()).Wire();
    ProjectionInput curved;
    curved.resolvedFace = BRepBuilderAPI_MakeFace(circularWire).Face();
    result = projectPlanarFace(curved);
    EXPECT_EQ(result.status, OperationStatus::InvalidInput);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::UnsupportedGeometry);
    EXPECT_NE(result.diagnostic.message.find("curved edge"), std::string::npos);
}

SourceIdentitySnapshot sourceSnapshot()
{
    SourceIdentitySnapshot snapshot;
    snapshot.documentUuid = "document-uuid";
    snapshot.objectUuid = "object-uuid";
    snapshot.subelementPath = "Body/Pad/Face1";
    snapshot.resolvedPlacement = {
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
    snapshot.projectionSha256 = std::string(64, 'a');
    return snapshot;
}

TEST(PhotoInspectionSourceSnapshotTest, exactIdentityAndGeometryRemainCurrent)
{
    const auto frozen = sourceSnapshot();
    EXPECT_EQ(compareSourceSnapshot(frozen, frozen, 1.0e-9), SourceSnapshotState::Current);
}

TEST(PhotoInspectionSourceSnapshotTest, missingAndReboundSourcesAreDistinct)
{
    const auto frozen = sourceSnapshot();
    auto missing = frozen;
    missing.available = false;
    EXPECT_EQ(compareSourceSnapshot(frozen, missing, 1.0e-9), SourceSnapshotState::Missing);

    auto rebound = frozen;
    rebound.objectUuid = "different-object";
    EXPECT_EQ(compareSourceSnapshot(frozen, rebound, 1.0e-9), SourceSnapshotState::IdentityChanged);
}

TEST(PhotoInspectionSourceSnapshotTest, placementAndGeometryChangesDoNotRebind)
{
    const auto frozen = sourceSnapshot();
    auto moved = frozen;
    moved.resolvedPlacement[3] = 0.01;
    EXPECT_EQ(compareSourceSnapshot(frozen, moved, 1.0e-9), SourceSnapshotState::PlacementChanged);

    auto edited = frozen;
    edited.projectionSha256 = std::string(64, 'b');
    EXPECT_EQ(compareSourceSnapshot(frozen, edited, 1.0e-9), SourceSnapshotState::GeometryChanged);
}

TEST(PhotoInspectionSourceSnapshotTest, invalidToleranceOrSnapshotFailsClosed)
{
    auto frozen = sourceSnapshot();
    frozen.documentUuid.clear();
    EXPECT_EQ(compareSourceSnapshot(frozen, sourceSnapshot(), 1.0e-9), SourceSnapshotState::Invalid);
    EXPECT_EQ(
        compareSourceSnapshot(sourceSnapshot(), sourceSnapshot(), -1.0),
        SourceSnapshotState::Invalid
    );
}

}  // namespace
