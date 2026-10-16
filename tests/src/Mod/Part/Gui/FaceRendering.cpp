// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <BRep_Builder.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS_Face.hxx>
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
