// SPDX-License-Identifier: LGPL-2.1-or-later

#include <BRepAdaptor_Surface.hxx>
#include <TopoDS.hxx>

#include <gtest/gtest.h>
#include "src/App/InitApplication.h"

#include <App/Application.h>
#include <App/Document.h>
#include <App/Origin.h>
#include <App/Part.h>
#include <Base/Tools.h>
#include "Mod/Part/App/Attacher.h"
#include "Mod/PartDesign/App/Body.h"
#include "Mod/PartDesign/App/DatumPlane.h"
#include "Mod/PartDesign/App/FeaturePrimitive.h"

class DatumPlaneTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _body = _doc->addObject<PartDesign::Body>();
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* getDocument() const
    {
        return _doc;
    }

    PartDesign::Body* getBody() const
    {
        return _body;
    }

private:
    std::string _docName;
    App::Document* _doc = nullptr;
    PartDesign::Body* _body = nullptr;
};

TEST_F(DatumPlaneTest, attachDatumPlane)
{
    auto datumPlane = getDocument()->addObject<PartDesign::Plane>("Plane");
    ASSERT_TRUE(datumPlane);
    getBody()->addObject(datumPlane);
    auto origin = getBody()->getOrigin();

    App::PropertyLinkSubList support;
    std::vector<App::DocumentObject*> objs;
    std::vector<std::string> subs;
    objs.push_back(origin->getXY());
    subs.emplace_back();
    support.setValues(objs, subs);

    auto attach = datumPlane->getExtensionByType<Part::AttachExtension>();
    attach->attacher().setReferences(support);
    Attacher::SuggestResult sugr;
    attach->attacher().suggestMapModes(sugr);
    EXPECT_EQ(sugr.message, Attacher::SuggestResult::srOK);
}

TEST_F(DatumPlaneTest, attachDatumPlaneToPlacedCrossBodyFace)
{
    constexpr double tolerance = 1.0e-6;
    const std::string faceName = "Face1";

    auto sourcePart =
        freecad_cast<App::Part*>(getDocument()->addObject("App::Part", "SourcePart"));
    ASSERT_NE(sourcePart, nullptr);
    auto sourceBody = getDocument()->addObject<PartDesign::Body>("SourceBody");
    ASSERT_NE(sourceBody, nullptr);
    sourcePart->addObject(sourceBody);

    auto sourceBox = freecad_cast<PartDesign::AdditiveBox*>(
        getDocument()->addObject("PartDesign::AdditiveBox", "SourceBox")
    );
    ASSERT_NE(sourceBox, nullptr);
    sourceBox->Length.setValue(4.0);
    sourceBox->Width.setValue(6.0);
    sourceBox->Height.setValue(2.0);
    sourceBody->addObject(sourceBox);

    sourcePart->Placement.setValue(Base::Placement(
        Base::Vector3d(10.0, -3.0, 5.0),
        Base::Rotation(Base::Vector3d(0.0, 0.0, 1.0), Base::toRadians(90.0))
    ));
    sourceBody->Placement.setValue(Base::Placement(
        Base::Vector3d(0.0, 0.0, 7.0),
        Base::Rotation(Base::Vector3d(0.0, 1.0, 0.0), Base::toRadians(90.0))
    ));
    sourceBox->Placement.setValue(Base::Placement(
        Base::Vector3d(2.0, 3.0, -1.0),
        Base::Rotation(Base::Vector3d(1.0, 0.0, 0.0), Base::toRadians(90.0))
    ));
    getDocument()->recompute();

    auto datumPlane = getDocument()->addObject<PartDesign::Plane>("CrossBodyDatumPlane");
    ASSERT_NE(datumPlane, nullptr);
    getBody()->addObject(datumPlane);

    auto attach = datumPlane->getExtensionByType<Part::AttachExtension>();
    std::vector<App::DocumentObject*> supportObjects {sourceBox};
    std::vector<std::string> supportSubs {faceName};
    attach->AttachmentSupport.setValues(supportObjects, supportSubs);
    attach->MapMode.setValue("FlatFace");
    attach->positionBySupport();

    auto localFaceShape = sourceBox->Shape.getShape().getSubTopoShape(faceName.c_str(), true);
    ASSERT_FALSE(localFaceShape.isNull());
    auto localFace = TopoDS::Face(localFaceShape.getShape());
    ASSERT_FALSE(localFace.IsNull());

    BRepAdaptor_Surface surface(localFace);
    ASSERT_EQ(surface.GetType(), GeomAbs_Plane);
    gp_Pln localPlane = surface.Plane();
    gp_Dir localPlaneDirection = localPlane.Axis().Direction();
    Base::Vector3d localNormal(
        localPlaneDirection.X(),
        localPlaneDirection.Y(),
        localPlaneDirection.Z()
    );
    if (localFace.Orientation() == TopAbs_REVERSED) {
        localNormal = -localNormal;
    }

    Base::Placement sourceGlobalPlacement =
        sourcePart->Placement.getValue() * sourceBody->Placement.getValue()
        * sourceBox->Placement.getValue();
    Base::Vector3d expectedNormal;
    sourceGlobalPlacement.getRotation().multVec(localNormal, expectedNormal);
    expectedNormal.Normalize();

    gp_Pnt localPlaneLocation = localPlane.Location();
    Base::Vector3d globalPlanePoint;
    sourceGlobalPlacement.multVec(
        Base::Vector3d(localPlaneLocation.X(), localPlaneLocation.Y(), localPlaneLocation.Z()),
        globalPlanePoint
    );

    Base::Vector3d globalSupportOrigin;
    sourceGlobalPlacement.multVec(Base::Vector3d(), globalSupportOrigin);
    Base::Vector3d expectedOrigin =
        globalSupportOrigin
        - expectedNormal * ((globalSupportOrigin - globalPlanePoint) * expectedNormal);

    Base::Placement actualPlacement = datumPlane->Placement.getValue();
    Base::Vector3d actualNormal;
    actualPlacement.getRotation().multVec(Base::Vector3d(0.0, 0.0, 1.0), actualNormal);
    actualNormal.Normalize();

    EXPECT_TRUE(actualPlacement.getPosition().IsEqual(expectedOrigin, tolerance))
        << "Expected datum origin (" << expectedOrigin.x << ", " << expectedOrigin.y << ", "
        << expectedOrigin.z << ") but got (" << actualPlacement.getPosition().x << ", "
        << actualPlacement.getPosition().y << ", " << actualPlacement.getPosition().z << ")";
    EXPECT_TRUE(actualNormal.IsEqual(expectedNormal, tolerance))
        << "Expected datum normal (" << expectedNormal.x << ", " << expectedNormal.y << ", "
        << expectedNormal.z << ") but got (" << actualNormal.x << ", " << actualNormal.y << ", "
        << actualNormal.z << ")";
}
