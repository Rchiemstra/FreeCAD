// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/Expression.h>
#include <App/ObjectIdentifier.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <App/VarSet.h>
#include <Base/Vector3D.h>
#include <Mod/Part/App/Geometry.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/PartDesign/App/FeaturePad.h>
#include <Mod/PartDesign/App/FeaturePrimitive.h>
#include <Mod/Sketcher/App/Constraint.h>
#include <Mod/Sketcher/App/SketchObject.h>
#include <src/App/InitApplication.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{

class PartDesignCollaborationCompatibilityTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("partDesignCollaboration");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(),
            "PartDesign collaboration test"
        );
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        if (_document) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::DocumentCommitResult commitStructural(App::CollaborationCompatibilityCallback callback)
    {
        App::CollaborationCompatibilityMutation mutation;
        mutation.scope = App::CollaborationCompatibilityScope::Structural;
        return _document->collaborationService().commitCompatibilityMutation(
            std::move(mutation),
            std::move(callback)
        );
    }

    std::string _documentName;
    App::Document* _document {nullptr};
};

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    bodyThenExpressionConstrainedSketchAndPadReachValidSolidBoundary
)
{
    PartDesign::Body* body = nullptr;
    App::VarSet* dimensions = nullptr;
    const auto bodyResult = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
        dimensions = _document->addObject<App::VarSet>("Dims");
        auto* radius = freecad_cast<App::PropertyLength*>(dimensions->addDynamicProperty(
            "App::PropertyLength", "Radius", "Parameters"));
        if (!radius) {
            throw std::runtime_error("failed to create sketch radius parameter");
        }
        radius->setValue(10.0);
    });
    ASSERT_EQ(bodyResult.status, App::DocumentCommitStatus::Committed) << bodyResult.message;
    ASSERT_NE(body, nullptr);
    ASSERT_NE(dimensions, nullptr);
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_TRUE(body->isValid());

    Sketcher::SketchObject* sketch = nullptr;
    PartDesign::Pad* pad = nullptr;
    const auto sketchResult = commitStructural([&] {
        sketch = _document->addObject<Sketcher::SketchObject>("Sketch");
        body->addObject(sketch);

        Part::GeomCircle circle;
        circle.setRadius(5.0);
        const int geometryId = sketch->addGeometry(&circle, false);
        auto radiusConstraint = std::make_unique<Sketcher::Constraint>();
        radiusConstraint->Type = Sketcher::ConstraintType::Radius;
        radiusConstraint->First = geometryId;
        radiusConstraint->setValue(5.0);
        radiusConstraint->Name = "Radius";
        const int constraintId = sketch->addConstraint(std::move(radiusConstraint));
        sketch->setExpression(
            sketch->Constraints.createPath(constraintId),
            std::shared_ptr<App::Expression>(App::Expression::parse(sketch, "Dims.Radius")));

        pad = _document->addObject<PartDesign::Pad>("Pad");
        body->addObject(pad);
        pad->Profile.setValue(sketch, {""});
        pad->Length.setValue(12.0);
    });
    ASSERT_EQ(sketchResult.status, App::DocumentCommitStatus::Committed) << sketchResult.message;
    ASSERT_NE(sketch, nullptr);
    ASSERT_NE(pad, nullptr);
    EXPECT_EQ(_document->getObject("Sketch"), sketch);
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_TRUE(body->isValid());
    EXPECT_TRUE(sketch->isValid());
    EXPECT_TRUE(pad->isValid());
    EXPECT_FALSE(pad->Shape.getShape().isNull());
    EXPECT_TRUE(pad->Shape.getShape().isValid());
    EXPECT_EQ(pad->Shape.getShape().countSubShapes("Solid"), 1U);
    EXPECT_DOUBLE_EQ(sketch->Constraints.getValues().front()->getValue(), 10.0);

    const auto customZeroResult = commitStructural([&] {
        pad->UseCustomVector.setValue(true);
        pad->Direction.setValue(Base::Vector3d());
    });
    ASSERT_EQ(customZeroResult.status, App::DocumentCommitStatus::Committed)
        << customZeroResult.message;
    EXPECT_TRUE(pad->UseCustomVector.getValue());
    EXPECT_GT(pad->Direction.getValue().Length(), 0.0);
    EXPECT_TRUE(pad->isValid());
    EXPECT_TRUE(pad->Shape.getShape().isValid());
    EXPECT_EQ(pad->Shape.getShape().countSubShapes("Solid"), 1U);
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    expressionDrivenUseCustomVectorFlipKeepsStableDirectionOutputSchema
)
{
    PartDesign::Body* body = nullptr;
    App::VarSet* dimensions = nullptr;
    App::PropertyBool* customDirection = nullptr;
    const auto setupResult = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
        dimensions = _document->addObject<App::VarSet>("Dims");
        customDirection = freecad_cast<App::PropertyBool*>(dimensions->addDynamicProperty(
            "App::PropertyBool", "CustomDirection", "Parameters"));
        if (!customDirection) {
            throw std::runtime_error("failed to create custom-direction parameter");
        }
        customDirection->setValue(false);
    });
    ASSERT_EQ(setupResult.status, App::DocumentCommitStatus::Committed)
        << setupResult.message;
    ASSERT_NE(body, nullptr);
    ASSERT_NE(dimensions, nullptr);
    ASSERT_NE(customDirection, nullptr);

    Sketcher::SketchObject* sketch = nullptr;
    PartDesign::Pad* pad = nullptr;
    const auto padResult = commitStructural([&] {
        sketch = _document->addObject<Sketcher::SketchObject>("Sketch");
        body->addObject(sketch);
        Part::GeomCircle circle;
        circle.setRadius(8.0);
        sketch->addGeometry(&circle, false);

        pad = _document->addObject<PartDesign::Pad>("Pad");
        body->addObject(pad);
        pad->Profile.setValue(sketch, {""});
        pad->Length.setValue(9.0);
        pad->setExpression(
            App::ObjectIdentifier(pad->UseCustomVector),
            std::shared_ptr<App::Expression>(
                App::Expression::parse(pad, "Dims.CustomDirection")));
    });
    ASSERT_EQ(padResult.status, App::DocumentCommitStatus::Committed) << padResult.message;
    ASSERT_NE(sketch, nullptr);
    ASSERT_NE(pad, nullptr);
    EXPECT_FALSE(pad->UseCustomVector.getValue());
    EXPECT_TRUE(pad->isValid());
    EXPECT_EQ(pad->Shape.getShape().countSubShapes("Solid"), 1U);

    const auto flipResult = commitStructural([&] {
        pad->Direction.setValue(Base::Vector3d());
        customDirection->setValue(true);
    });
    ASSERT_EQ(flipResult.status, App::DocumentCommitStatus::Committed) << flipResult.message;
    EXPECT_TRUE(pad->UseCustomVector.getValue());
    EXPECT_GT(pad->Direction.getValue().Length(), 0.0);
    EXPECT_TRUE(pad->isValid());
    EXPECT_TRUE(pad->Shape.getShape().isValid());
    EXPECT_EQ(pad->Shape.getShape().countSubShapes("Solid"), 1U);
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    deactivatedSketchPlacementSurvivesAuthoritativeRecompute
)
{
    PartDesign::Body* body = nullptr;
    Sketcher::SketchObject* sketch = nullptr;
    PartDesign::Pad* pad = nullptr;
    const Base::Placement placement(
        Base::Vector3d(44.0, 0.0, 0.0),
        Base::Rotation(Base::Vector3d(1.0, 1.0, 1.0), Base::toRadians(120.0)));

    const auto result = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
        sketch = _document->addObject<Sketcher::SketchObject>("Sketch");
        body->addObject(sketch);
        Part::GeomCircle circle;
        circle.setRadius(5.0);
        sketch->addGeometry(&circle, false);

        pad = _document->addObject<PartDesign::Pad>("Pad");
        body->addObject(pad);
        pad->Profile.setValue(sketch, {""});
        pad->Length.setValue(12.0);
        sketch->MapMode.setValue(Attacher::mmDeactivated);
        sketch->Placement.setValue(placement);
    });

    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    ASSERT_NE(sketch, nullptr);
    ASSERT_NE(pad, nullptr);
    Base::Vector3d actualAxis;
    double actualAngle = 0.0;
    sketch->Placement.getValue().getRotation().getValue(actualAxis, actualAngle);
    EXPECT_DOUBLE_EQ(sketch->Placement.getValue().getPosition().x, 44.0);
    EXPECT_NEAR(actualAngle, Base::toRadians(120.0), 1e-12);
    EXPECT_TRUE(sketch->Placement.getValue().isSame(placement, 1e-12));
    EXPECT_TRUE(
        sketch->globalPlacement().isSame(body->Placement.getValue() * placement, 1e-12));
    EXPECT_TRUE(pad->isValid());
    EXPECT_FALSE(pad->Shape.getShape().isNull());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    deactivatedAdditiveCylinderPublishesItsRotatedShape
)
{
    PartDesign::Body* body = nullptr;
    PartDesign::AdditiveCylinder* cylinder = nullptr;
    const Base::Placement placement(
        Base::Vector3d(),
        Base::Rotation(Base::Vector3d(0.0, 1.0, 0.0), Base::toRadians(90.0)));

    const auto result = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
        cylinder = _document->addObject<PartDesign::AdditiveCylinder>("Cylinder");
        body->addObject(cylinder);
        cylinder->Radius.setValue(2.0);
        cylinder->Height.setValue(1.0);
        cylinder->Angle.setValue(360.0);
        auto* mapMode = freecad_cast<App::PropertyEnumeration*>(
            cylinder->getPropertyByName("MapMode"));
        if (!mapMode) {
            throw std::runtime_error("additive cylinder has no MapMode property");
        }
        mapMode->setValue("Deactivated");
        cylinder->Placement.setValue(placement);
    });

    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    ASSERT_NE(cylinder, nullptr);
    ASSERT_FALSE(cylinder->Shape.getShape().isNull());
    EXPECT_TRUE(cylinder->Placement.getValue().isSame(placement, 1e-12));
    const auto bounds = cylinder->Shape.getBoundingBox();
    EXPECT_NEAR(bounds.MinX, 0.0, 1e-3);
    EXPECT_NEAR(bounds.MaxX, 1.0, 1e-3);
    EXPECT_NEAR(bounds.MinY, -2.0, 1e-3);
    EXPECT_NEAR(bounds.MaxY, 2.0, 1e-3);
    EXPECT_NEAR(bounds.MinZ, -2.0, 1e-3);
    EXPECT_NEAR(bounds.MaxZ, 2.0, 1e-3);
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    liveSketchAttachmentOwnsOnlyItsFrameworkPropertyStatusChanges
)
{
    PartDesign::Body* body = nullptr;
    Sketcher::SketchObject* sketch = nullptr;
    App::PropertyString* arbitrary = nullptr;
    const auto setupResult = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
        sketch = _document->addObject<Sketcher::SketchObject>("Sketch");
        body->addObject(sketch);
        Part::GeomCircle circle;
        circle.setRadius(5.0);
        sketch->addGeometry(&circle, false);
        arbitrary = freecad_cast<App::PropertyString*>(sketch->addDynamicProperty(
            "App::PropertyString", "ArbitraryAttachmentStatus", "Test"));
        if (!arbitrary) {
            throw std::runtime_error("failed to create attachment status probe");
        }
    });
    ASSERT_EQ(setupResult.status, App::DocumentCommitStatus::Committed)
        << setupResult.message;
    ASSERT_NE(body, nullptr);
    ASSERT_NE(sketch, nullptr);
    ASSERT_NE(arbitrary, nullptr);

    auto* xyPlane = _document->getObject("XY_Plane");
    ASSERT_NE(xyPlane, nullptr);
    const auto attachResult = commitStructural([&] {
        sketch->AttachmentSupport.setValue(xyPlane, "");
        sketch->MapMode.setValue(Attacher::mmFlatFace);
    });
    ASSERT_EQ(attachResult.status, App::DocumentCommitStatus::Committed)
        << attachResult.message;
    EXPECT_FALSE(sketch->MapReversed.testStatus(App::Property::Hidden));
    EXPECT_FALSE(sketch->AttachmentOffset.testStatus(App::Property::Hidden));
    EXPECT_TRUE(sketch->Placement.testStatus(App::Property::ReadOnly));
    EXPECT_FALSE(arbitrary->testStatus(App::Property::Hidden));
    EXPECT_FALSE(_document->mustExecute());

    const auto rejectedResult = commitStructural([&] {
        arbitrary->setStatus(App::Property::Hidden, true);
    });
    EXPECT_NE(rejectedResult.status, App::DocumentCommitStatus::Committed);
    EXPECT_FALSE(arbitrary->testStatus(App::Property::Hidden));
}

TEST_F(
    PartDesignCollaborationCompatibilityTest,
    nineteenPartPlacementBatchRecomputesWithActionableCommitResult
)
{
    constexpr int partCount = 19;
    std::vector<Sketcher::SketchObject*> sketches;
    std::vector<PartDesign::Pad*> pads;
    sketches.reserve(partCount);
    pads.reserve(partCount);

    const auto result = commitStructural([&] {
        for (int index = 0; index < partCount; ++index) {
            const std::string suffix = std::to_string(index);
            const std::string bodyName = "Body" + suffix;
            const std::string sketchName = "Sketch" + suffix;
            const std::string padName = "Pad" + suffix;
            auto* body = _document->addObject<PartDesign::Body>(bodyName.c_str());
            auto* sketch =
                _document->addObject<Sketcher::SketchObject>(sketchName.c_str());
            body->addObject(sketch);
            Part::GeomCircle circle;
            circle.setRadius(2.0 + index * 0.1);
            sketch->addGeometry(&circle, false);
            sketch->MapMode.setValue(Attacher::mmDeactivated);
            sketch->Placement.setValue(Base::Placement(
                Base::Vector3d(index * 15.0, 0.0, 0.0),
                Base::Rotation(Base::Vector3d(1.0, 1.0, 1.0),
                               Base::toRadians(120.0))));

            auto* pad = _document->addObject<PartDesign::Pad>(padName.c_str());
            body->addObject(pad);
            pad->Profile.setValue(sketch, {""});
            pad->Length.setValue(5.0 + index);
            sketches.push_back(sketch);
            pads.push_back(pad);
        }
    });

    ASSERT_EQ(result.status, App::DocumentCommitStatus::Committed) << result.message;
    ASSERT_EQ(sketches.size(), partCount);
    ASSERT_EQ(pads.size(), partCount);
    for (int index = 0; index < partCount; ++index) {
        EXPECT_DOUBLE_EQ(sketches[index]->Placement.getValue().getPosition().x,
                         index * 15.0);
        EXPECT_TRUE(sketches[index]->isValid());
        EXPECT_TRUE(pads[index]->isValid());
        EXPECT_FALSE(pads[index]->Shape.getShape().isNull());
    }
    EXPECT_FALSE(_document->mustExecute());
}

}  // namespace
