// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include <string>

#include <Mod/Inspection/App/PhotoInspectionEngine.h>
#include <Mod/Inspection/App/PhotoInspectionProjection.h>
#include <Mod/Inspection/App/PhotoInspectionSheet.h>

namespace
{

using Inspection::Photo::AnalysisInput;
using Inspection::Photo::ConformanceDecision;
using Inspection::Photo::DiagnosticCode;
using Inspection::Photo::OperationStatus;
using Inspection::Photo::ProjectionInput;
using Inspection::Photo::SheetIdentity;
using Inspection::Photo::SourceIdentitySnapshot;
using Inspection::Photo::SourceSnapshotState;
using Inspection::Photo::analyzePhotoInspection;
using Inspection::Photo::buildPhotoInspectionSheet;
using Inspection::Photo::compareSourceSnapshot;
using Inspection::Photo::projectPlanarFace;

TopoDS_Face rectangleFace(const double width, const double height)
{
    BRepBuilderAPI_MakePolygon polygon;
    polygon.Add(gp_Pnt(0.0, 0.0, 0.0));
    polygon.Add(gp_Pnt(width, 0.0, 0.0));
    polygon.Add(gp_Pnt(width, height, 0.0));
    polygon.Add(gp_Pnt(0.0, height, 0.0));
    polygon.Close();
    return BRepBuilderAPI_MakeFace(polygon.Wire()).Face();
}

TEST(PhotoInspectionFutureGateTest, phase7WholeBodyInputRejectsNonPlanarSurface)
{
    const TopoDS_Shape cylinder = BRepPrimAPI_MakeCylinder(10.0, 20.0).Shape();
    TopExp_Explorer faces(cylinder, TopAbs_FACE);
    ASSERT_TRUE(faces.More());

    ProjectionInput input;
    input.resolvedFace = TopoDS::Face(faces.Current());

    const auto result = projectPlanarFace(input);
    EXPECT_EQ(result.status, OperationStatus::InvalidInput);
    EXPECT_EQ(result.diagnostic.code, DiagnosticCode::UnsupportedGeometry);
}

TEST(PhotoInspectionFutureGateTest, phase7TilingDoesNotSilentlyScaleOversizeGeometry)
{
    ProjectionInput input;
    input.resolvedFace = rectangleFace(500.0, 500.0);
    const auto projection = projectPlanarFace(input);
    ASSERT_EQ(projection.status, OperationStatus::Complete);

    const auto sheet = buildPhotoInspectionSheet(
        projection.projection,
        SheetIdentity {"series", "revision", "source", 1},
        {}
    );
    EXPECT_EQ(sheet.status, OperationStatus::InvalidInput);
    EXPECT_EQ(sheet.diagnostic.code, DiagnosticCode::InvalidGeometry);
    EXPECT_NE(sheet.diagnostic.message.find("does not fit"), std::string::npos);
}

TEST(PhotoInspectionFutureGateTest, phase7OccurrenceIdentityCannotCollapseRepeatedInstances)
{
    SourceIdentitySnapshot first;
    first.documentUuid = "document";
    first.objectUuid = "definition";
    first.subelementPath = "Assembly/Occurrence001/Body/Face1";
    first.projectionSha256 = std::string(64, 'a');

    SourceIdentitySnapshot second = first;
    second.subelementPath = "Assembly/Occurrence002/Body/Face1";

    EXPECT_EQ(
        compareSourceSnapshot(first, second, 1.0e-9),
        SourceSnapshotState::IdentityChanged
    );
}

TEST(PhotoInspectionFutureGateTest, phase8CannotEmitPassWithoutPhysicalProfileEvidence)
{
    ProjectionInput projectionInput;
    projectionInput.resolvedFace = rectangleFace(100.0, 50.0);
    const auto projection = projectPlanarFace(projectionInput);
    ASSERT_EQ(projection.status, OperationStatus::Complete);

    AnalysisInput analysis;
    analysis.generation = 8;
    analysis.nominalProjection = projection.projection;
    analysis.measuredCycles = projection.projection.snapshot.cycles;
    analysis.options.cameraProfileDecisionCapable = false;
    analysis.options.printerProfileDecisionCapable = false;

    const auto result = analyzePhotoInspection(analysis);
    EXPECT_EQ(result.status, OperationStatus::Complete);
    EXPECT_EQ(result.decision, ConformanceDecision::Inconclusive);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::MissingCalibration);
}

}  // namespace
