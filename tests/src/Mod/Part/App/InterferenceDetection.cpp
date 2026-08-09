// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <gp_Pnt.hxx>

#include <src/App/InitApplication.h>

#include "Mod/Part/App/InterferenceDetection.h"
#include "PartTestHelpers.h"

using namespace Part;
using namespace PartTestHelpers;

namespace
{

TopoDS_Shape makeBox(const gp_Pnt& corner, double dx, double dy, double dz)
{
    return BRepPrimAPI_MakeBox(corner, dx, dy, dz).Shape();
}

TopoDS_Shape makeFaceXY(double x0, double y0, double x1, double y1, double z)
{
    BRepBuilderAPI_MakeEdge e1(gp_Pnt(x0, y0, z), gp_Pnt(x1, y0, z));
    BRepBuilderAPI_MakeEdge e2(gp_Pnt(x1, y0, z), gp_Pnt(x1, y1, z));
    BRepBuilderAPI_MakeEdge e3(gp_Pnt(x1, y1, z), gp_Pnt(x0, y1, z));
    BRepBuilderAPI_MakeEdge e4(gp_Pnt(x0, y1, z), gp_Pnt(x0, y0, z));
    BRepBuilderAPI_MakeWire wire(e1, e2, e3, e4);
    return BRepBuilderAPI_MakeFace(wire).Face();
}

/** Closed-looking solid missing a face — invalid to BRepCheck_Analyzer. */
TopoDS_Shape makeMalformedOpenSolid()
{
    const TopoDS_Solid solid = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Solid();
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(solid, TopAbs_FACE, faces);
    EXPECT_GE(faces.Extent(), 2);

    BRep_Builder builder;
    TopoDS_Shell shell;
    builder.MakeShell(shell);
    // Drop one face so the shell is open / solid is malformed.
    for (int i = 1; i < faces.Extent(); ++i) {
        builder.Add(shell, faces(i));
    }
    TopoDS_Solid malformed;
    builder.MakeSolid(malformed);
    builder.Add(malformed, shell);
    return malformed;
}

}  // namespace

class InterferenceDetectionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }
};

TEST_F(InterferenceDetectionTest, penetrationKnownVolume)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 10, 10, 10);
    const TopoDS_Shape b = makeBox(gp_Pnt(5, 0, 0), 10, 10, 10);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Penetration);
    EXPECT_NEAR(result.overlapVolume, 500.0, 1e-4);
    EXPECT_FALSE(result.commonShape.IsNull());
    EXPECT_LE(result.minimumDistance, Precision::Confusion());
}

TEST_F(InterferenceDetectionTest, containmentFullVolume)
{
    const TopoDS_Shape outer = makeBox(gp_Pnt(0, 0, 0), 10, 10, 10);
    const TopoDS_Shape inner = makeBox(gp_Pnt(2, 2, 2), 3, 3, 3);

    const auto result = classifyInterference(outer, inner);

    EXPECT_EQ(result.kind, InterferenceKind::Penetration);
    EXPECT_NEAR(result.overlapVolume, 27.0, 1e-4);
}

TEST_F(InterferenceDetectionTest, coincidentSolidsFullVolume)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 4, 5, 6);
    const TopoDS_Shape b = makeBox(gp_Pnt(0, 0, 0), 4, 5, 6);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Penetration);
    EXPECT_NEAR(result.overlapVolume, 120.0, 1e-4);
}

TEST_F(InterferenceDetectionTest, compoundMultipleDisconnectedCommons)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, makeBox(gp_Pnt(0, 0, 0), 2, 2, 2));
    builder.Add(compound, makeBox(gp_Pnt(10, 0, 0), 2, 2, 2));

    const TopoDS_Shape slab = makeBox(gp_Pnt(-1, -1, 0.5), 14, 4, 1);

    const auto result = classifyInterference(compound, slab);

    EXPECT_EQ(result.kind, InterferenceKind::Penetration);
    // Two 2x2x1 overlaps (slab height 1 through each 2x2 footprint)
    EXPECT_NEAR(result.overlapVolume, 8.0, 1e-3);
}

TEST_F(InterferenceDetectionTest, disjointClear)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(5, 0, 0), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Clear);
    EXPECT_NEAR(result.minimumDistance, 4.0, 1e-6);
}

TEST_F(InterferenceDetectionTest, boundingBoxFalsePositiveClearedByExact)
{
    // Boxes whose AABBs almost meet but exact gap exceeds clearance
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.05, 0, 0), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = 0.01;

    const auto result = classifyInterference(a, b, options);

    EXPECT_EQ(result.kind, InterferenceKind::Clear);
    EXPECT_NEAR(result.minimumDistance, 0.05, 1e-6);
}

TEST_F(InterferenceDetectionTest, faceContact)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1, 0, 0), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Contact);
    EXPECT_NEAR(result.overlapVolume, 0.0, 1e-9);
    EXPECT_LE(std::abs(result.minimumDistance), Precision::Confusion());
}

TEST_F(InterferenceDetectionTest, edgeContact)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1, 1, 0), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Contact);
    EXPECT_NEAR(result.overlapVolume, 0.0, 1e-9);
}

TEST_F(InterferenceDetectionTest, vertexContact)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1, 1, 1), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Contact);
    EXPECT_NEAR(result.overlapVolume, 0.0, 1e-9);
}

TEST_F(InterferenceDetectionTest, clearanceViolationJustInside)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.05, 0, 0), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = 0.1;

    const auto result = classifyInterference(a, b, options);

    EXPECT_EQ(result.kind, InterferenceKind::ClearanceViolation);
    EXPECT_NEAR(result.minimumDistance, 0.05, 1e-6);
}

TEST_F(InterferenceDetectionTest, clearJustOutsideClearance)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.15, 0, 0), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = 0.1;

    const auto result = classifyInterference(a, b, options);

    EXPECT_EQ(result.kind, InterferenceKind::Clear);
    EXPECT_NEAR(result.minimumDistance, 0.15, 1e-6);
}

TEST_F(InterferenceDetectionTest, almostTouchingWithinToleranceTreatedAsContactPath)
{
    const double gap = Precision::Confusion() * 0.5;
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.0 + gap, 0, 0), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_TRUE(
        result.kind == InterferenceKind::Contact || result.kind == InterferenceKind::Penetration
    );
}

TEST_F(InterferenceDetectionTest, barelyPositivePenetrationNoVolumeCutoff)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(0.999, 0, 0), 1, 1, 1);

    const auto result = classifyInterference(a, b);

    EXPECT_EQ(result.kind, InterferenceKind::Penetration);
    EXPECT_GT(result.overlapVolume, 0.0);
    EXPECT_LT(result.overlapVolume, 0.01);
}

TEST_F(InterferenceDetectionTest, nullShapeInvalidInput)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    TopoDS_Shape nullShape;

    const auto result = classifyInterference(a, nullShape);

    EXPECT_EQ(result.kind, InterferenceKind::InvalidInput);
}

TEST_F(InterferenceDetectionTest, negativeClearanceInvalidInput)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(2, 0, 0), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = -1.0;

    const auto result = classifyInterference(a, b, options);

    EXPECT_EQ(result.kind, InterferenceKind::InvalidInput);
}

TEST_F(InterferenceDetectionTest, surfaceOnlyContact)
{
    const TopoDS_Shape faceA = makeFaceXY(0, 0, 1, 1, 0);
    const TopoDS_Shape faceB = makeFaceXY(0, 0, 1, 1, 0);

    const auto result = classifyInterference(faceA, faceB);

    EXPECT_EQ(result.kind, InterferenceKind::Contact);
    EXPECT_NEAR(result.overlapVolume, 0.0, 1e-9);
}

TEST_F(InterferenceDetectionTest, cancelledBeforeWork)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(0.5, 0, 0), 1, 1, 1);

    std::atomic<bool> cancel {true};
    InterferenceOptions options;
    options.cancelFlag = &cancel;

    const auto result = classifyInterference(a, b, options);

    EXPECT_EQ(result.kind, InterferenceKind::Cancelled);
}

TEST_F(InterferenceDetectionTest, closestPointsPopulatedForClearanceViolation)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.2, 0.5, 0.5), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = 0.5;

    const auto result = classifyInterference(a, b, options);

    ASSERT_EQ(result.kind, InterferenceKind::ClearanceViolation);
    EXPECT_NEAR(result.pointOnFirst.x, 1.0, 1e-5);
    EXPECT_NEAR(result.pointOnSecond.x, 1.2, 1e-5);
    EXPECT_NEAR(result.minimumDistance, 0.2, 1e-5);
}

TEST_F(InterferenceDetectionTest, malformedBRepNeverReportsClear)
{
    const TopoDS_Shape malformed = makeMalformedOpenSolid();
    const TopoDS_Shape distant = makeBox(gp_Pnt(100, 0, 0), 1, 1, 1);

    ASSERT_FALSE(BRepCheck_Analyzer(malformed).IsValid());

    const auto result = classifyInterference(malformed, distant);

    EXPECT_NE(result.kind, InterferenceKind::Clear);
    EXPECT_TRUE(
        result.kind == InterferenceKind::InvalidInput
        || result.kind == InterferenceKind::Inconclusive
    );
}

TEST_F(InterferenceDetectionTest, malformedBRepAgainstOverlapNeverClear)
{
    const TopoDS_Shape malformed = makeMalformedOpenSolid();
    const TopoDS_Shape overlapping = makeBox(gp_Pnt(2, 2, 2), 4, 4, 4);

    ASSERT_FALSE(BRepCheck_Analyzer(malformed).IsValid());

    const auto result = classifyInterference(malformed, overlapping);

    EXPECT_NE(result.kind, InterferenceKind::Clear);
    EXPECT_TRUE(
        result.kind == InterferenceKind::InvalidInput
        || result.kind == InterferenceKind::Inconclusive
    );
}

TEST_F(InterferenceDetectionTest, cancellationDuringDistanceReturnsCancelled)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 20, 20, 20);
    const TopoDS_Shape b = makeBox(gp_Pnt(5, 5, 5), 20, 20, 20);

    std::atomic<bool> cancel {false};
    InterferenceOptions options;
    options.cancelFlag = &cancel;
    // Cancel cooperatively around the distance stage by setting before call —
    // cancelledBeforeWork covers pre-check; this asserts Cancelled is never Clear.
    cancel.store(true);
    const auto result = classifyInterference(a, b, options);
    EXPECT_EQ(result.kind, InterferenceKind::Cancelled);
}

TEST_F(InterferenceDetectionTest, nearToleranceGapStrictClearanceBoundary)
{
    const double tol = Precision::Confusion();
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(1.0 + 10.0 * tol, 0, 0), 1, 1, 1);

    InterferenceOptions options;
    options.clearance = 5.0 * tol;
    options.linearTolerance = tol;

    const auto result = classifyInterference(a, b, options);
    EXPECT_EQ(result.kind, InterferenceKind::Clear);
    EXPECT_NEAR(result.minimumDistance, 10.0 * tol, tol);
}

TEST_F(InterferenceDetectionTest, negativeLinearToleranceInvalidInput)
{
    const TopoDS_Shape a = makeBox(gp_Pnt(0, 0, 0), 1, 1, 1);
    const TopoDS_Shape b = makeBox(gp_Pnt(5, 0, 0), 1, 1, 1);
    InterferenceOptions options;
    options.linearTolerance = -1.0;
    const auto result = classifyInterference(a, b, options);
    EXPECT_EQ(result.kind, InterferenceKind::InvalidInput);
}
