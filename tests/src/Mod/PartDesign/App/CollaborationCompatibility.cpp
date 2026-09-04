// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/Sketcher/App/SketchObject.h>
#include <src/App/InitApplication.h>

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
    bodyThenSketchStructuralMutationsReachCleanStableBoundaries
)
{
    PartDesign::Body* body = nullptr;
    const auto bodyResult = commitStructural([&] {
        body = _document->addObject<PartDesign::Body>("Body");
    });
    ASSERT_EQ(bodyResult.status, App::DocumentCommitStatus::Committed) << bodyResult.message;
    ASSERT_NE(body, nullptr);
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_TRUE(body->isValid());

    Sketcher::SketchObject* sketch = nullptr;
    const auto sketchResult = commitStructural([&] {
        sketch = _document->addObject<Sketcher::SketchObject>("Sketch");
        body->addObject(sketch);
    });
    ASSERT_EQ(sketchResult.status, App::DocumentCommitStatus::Committed) << sketchResult.message;
    ASSERT_NE(sketch, nullptr);
    EXPECT_EQ(_document->getObject("Sketch"), sketch);
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_TRUE(body->isValid());
    EXPECT_TRUE(sketch->isValid());
}

}  // namespace
