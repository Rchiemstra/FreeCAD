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
#include <Mod/Sketcher/App/Constraint.h>
#include <Mod/Sketcher/App/SketchObject.h>
#include <src/App/InitApplication.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

}  // namespace
