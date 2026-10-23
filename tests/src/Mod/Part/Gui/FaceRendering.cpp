// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <Inventor/SoDB.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoNormal.h>

#include <Base/Console.h>
#include <Mod/Part/Gui/SoBrepEdgeSet.h>
#include <Mod/Part/Gui/SoBrepFaceSet.h>
#include <Mod/Part/Gui/SoBrepPointSet.h>
#include <Mod/Part/Gui/SoFCShapeObject.h>
#include <Mod/Part/Gui/ViewProviderExt.h>

#include <string>
#include <thread>

namespace
{

class RenderErrorCapture: public Base::ILogger
{
public:
    void sendLog(const std::string&, const std::string& message, Base::LogStyle level,
                 Base::IntendedRecipient, Base::ContentType) override
    {
        if (level == Base::LogStyle::Error || level == Base::LogStyle::Critical) {
            ++count;
            errors += message;
        }
    }

    int count {0};
    std::string errors;
};

class PartFaceRenderingTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        SoDB::init();
        if (PartGui::SoBrepFaceSet::getClassTypeId().isBad()) {
            PartGui::SoBrepFaceSet::initClass();
        }
        if (PartGui::SoBrepEdgeSet::getClassTypeId().isBad()) {
            PartGui::SoBrepEdgeSet::initClass();
        }
        if (PartGui::SoBrepPointSet::getClassTypeId().isBad()) {
            PartGui::SoBrepPointSet::initClass();
        }
        if (PartGui::SoFCShape::getClassTypeId().isBad()) {
            PartGui::SoFCShape::initClass();
        }
    }

    void SetUp() override
    {
        node = new PartGui::SoFCShape;
        node->ref();
        node->addChild(node->coords);
        node->addChild(node->norm);
        node->addChild(node->faceset);
        node->addChild(node->lineset);
        node->addChild(node->nodeset);
        Base::Console().attachObserver(&errors);
    }

    void TearDown() override
    {
        Base::Console().detachObserver(&errors);
        node->unref();
    }

    void render(const TopoDS_Shape& shape)
    {
        PartGui::ViewProviderPartExt::setupCoinGeometry(shape, node, 0.2, 28.65);
    }

    TopoDS_Face validFace()
    {
        return BRepBuilderAPI_MakeFace(gp_Pln(), -1, 1, -1, 1).Face();
    }

    void expectEmpty()
    {
        EXPECT_EQ(node->coords->point.getNum(), 0);
        EXPECT_EQ(node->norm->vector.getNum(), 0);
        EXPECT_EQ(node->faceset->coordIndex.getNum(), 0);
        EXPECT_EQ(node->faceset->partIndex.getNum(), 0);
        EXPECT_EQ(node->lineset->coordIndex.getNum(), 0);
        EXPECT_EQ(node->lineset->edgeIndexFromLine(0), 0);
        EXPECT_EQ(node->nodeset->startIndex.getValue(), 0);
    }

    PartGui::SoFCShape* node {};
    RenderErrorCapture errors;
};

TEST_F(PartFaceRenderingTest, ValidFaceStillProducesGeometry)
{
    EXPECT_NO_THROW(render(validFace()));
    EXPECT_GT(node->coords->point.getNum(), 0);
    EXPECT_GT(node->faceset->coordIndex.getNum(), 0);
    EXPECT_EQ(errors.count, 0) << errors.errors;
}

TEST_F(PartFaceRenderingTest, InfiniteFaceKeepsSurfaceLocationAndReversedNormals)
{
    gp_Trsf transform;
    transform.SetRotation(gp_Ax1(gp::Origin(), gp::DY()), M_PI / 6);
    transform.SetTranslationPart(gp_Vec(11, 22, 33));
    TopoDS_Face face;
    BRep_Builder().MakeFace(face, new Geom_Plane(gp::XOY()), TopLoc_Location(transform),
                           Precision::Confusion());
    face.Reverse();
    for (bool fromUV : {false, true}) {
        PartGui::ViewProviderPartExt::setupCoinGeometry(
            face, node->coords, node->faceset, node->norm, node->lineset, node->nodeset,
            0.2, 28.65, fromUV);
        ASSERT_EQ(node->faceset->coordIndex.getNum(), 8);
        ASSERT_EQ(node->coords->point.getNum(), 4);
        for (int i = 0; i < 4; ++i) {
            const auto& point = node->coords->point[i];
            const auto p = gp_Pnt(point[0], point[1], point[2]).Transformed(transform.Inverted());
            EXPECT_NEAR(std::abs(p.X()), 50, 1e-4);
            EXPECT_NEAR(std::abs(p.Y()), 50, 1e-4);
            EXPECT_NEAR(p.Z(), 0, 1e-4);
            const auto& normal = node->norm->vector[i];
            const auto n = gp_Vec(normal[0], normal[1], normal[2]).Transformed(transform.Inverted());
            EXPECT_NEAR(n.Z(), -1, 1e-5);
        }
        EXPECT_EQ(errors.count, 0) << errors.errors;
    }
}

TEST_F(PartFaceRenderingTest, HalfInfiniteFaceKeepsFiniteBounds)
{
    const auto face = BRepBuilderAPI_MakeFace(
        gp_Pln(), 150, Precision::Infinite(), -20, 30).Face();
    render(face);
    ASSERT_EQ(node->faceset->coordIndex.getNum(), 8);
    for (const auto& point : {node->coords->point[0], node->coords->point[1],
                              node->coords->point[2], node->coords->point[3]}) {
        EXPECT_TRUE(point[0] == 150 || point[0] == 250);
        EXPECT_TRUE(point[1] == -20 || point[1] == 30);
        EXPECT_EQ(point[2], 0);
    }
    EXPECT_EQ(errors.count, 0) << errors.errors;
}

TEST_F(PartFaceRenderingTest, LineFallbackKeepsFiniteAndHalfInfiniteRanges)
{
    const gp_Lin line(gp::Origin(), gp::DX());
    for (const auto& range : {std::pair {150.0, 160.0},
                              std::pair {150.0, Precision::Infinite()},
                              std::pair {-Precision::Infinite(), -150.0},
                              std::pair {-Precision::Infinite(), Precision::Infinite()}}) {
        SCOPED_TRACE(range.first);
        SCOPED_TRACE(range.second);
        auto edge = BRepBuilderAPI_MakeEdge(line, range.first, range.second).Edge();
        gp_Trsf transform;
        transform.SetRotation(gp_Ax1(gp::Origin(), gp::DY()), M_PI / 6);
        transform.SetTranslationPart(gp_Vec(11, 22, 33));
        edge.Location(TopLoc_Location(transform));
        TopoDS_Compound compound;
        BRep_Builder builder;
        builder.MakeCompound(compound);
        builder.Add(compound, edge);
        render(compound);
        ASSERT_GE(node->coords->point.getNum(), 2);
        ASSERT_EQ(node->lineset->coordIndex.getNum(), 3);
        const double first = Precision::IsInfinite(range.first)
            ? (Precision::IsInfinite(range.second) ? -50 : range.second - 100) : range.first;
        const double last = Precision::IsInfinite(range.second)
            ? (Precision::IsInfinite(range.first) ? 50 : range.first + 100) : range.second;
        for (int i = 0; i < 2; ++i) {
            const auto& point = node->coords->point[i];
            const auto p = gp_Pnt(point[0], point[1], point[2]).Transformed(transform.Inverted());
            EXPECT_NEAR(p.X(), i == 0 ? first : last, 1e-4);
            EXPECT_NEAR(p.Y(), 0, 1e-4);
            EXPECT_NEAR(p.Z(), 0, 1e-4);
        }
        EXPECT_EQ(node->lineset->edgeIndexFromLine(0), 1);
        EXPECT_EQ(errors.count, 0) << errors.errors;
    }
}

TEST_F(PartFaceRenderingTest, NullFaceClearsPreviousGeometrySilently)
{
    render(validFace());
    EXPECT_NO_THROW(render(TopoDS_Face()));
    expectEmpty();
    EXPECT_EQ(errors.count, 0) << errors.errors;
}

TEST_F(PartFaceRenderingTest, OccMeshingFailureClearsGeometryAndRecovers)
{
    render(validFace());
    EXPECT_NO_THROW(PartGui::ViewProviderPartExt::setupCoinGeometry(
        validFace(), node, 0.2, 0.0));  // OCCT rejects a zero angular deflection.
    expectEmpty();
    EXPECT_EQ(errors.count, 1) << errors.errors;
    render(validFace());
    EXPECT_GT(node->faceset->coordIndex.getNum(), 0);
    EXPECT_EQ(errors.count, 1) << errors.errors;
}

TEST_F(PartFaceRenderingTest, InvalidFaceOnOwnerThreadLogsOnceAndClearsGeometry)
{
    render(validFace());
    TopoDS_Face invalid;
    BRep_Builder().MakeFace(invalid);  // A face with no supporting surface.
    std::string escapedException;
    std::thread owner([&] {
        try {
            render(invalid);
        }
        catch (...) {
            escapedException = "Tessellation exception escaped on the owner thread";
        }
    });
    owner.join();
    EXPECT_TRUE(escapedException.empty()) << escapedException;
    expectEmpty();
    EXPECT_EQ(errors.count, 1) << errors.errors;
    EXPECT_NE(errors.errors.find("Cannot compute Inventor representation"), std::string::npos);
    render(validFace());
    EXPECT_GT(node->faceset->coordIndex.getNum(), 0);
    EXPECT_EQ(errors.count, 1) << errors.errors;
}

}  // namespace
