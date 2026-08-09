// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/MutationClassification.h>
#include <Base/Exception.h>

#include <string>

namespace App
{
namespace
{

class CollaborationAuthorityRemovalTest: public ::testing::Test
{
protected:
    void SetUp() override
    {
        static unsigned int sequence = 0;
        _name = "CollaborationAuthorityRemoval" + std::to_string(++sequence);
        _document = GetApplication().newDocument(_name.c_str());
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        if (GetApplication().getDocument(_name.c_str())) {
            GetApplication().closeDocument(_name.c_str());
        }
    }

    Document& document() const
    {
        return *_document;
    }

private:
    std::string _name;
    Document* _document {nullptr};
};

TEST_F(CollaborationAuthorityRemovalTest, ordinaryDocumentOperationsNeedNoExternalCapability)
{
    auto* object = document().addObject("App::FeatureTest", "Feature");
    ASSERT_NE(object, nullptr);

    EXPECT_NO_THROW(object->Label.setValue("Changed without an external capability"));
    EXPECT_NO_THROW(document().openTransaction("ordinary edit"));
    EXPECT_NO_THROW(object->Label.setValue("Transactional edit"));
    EXPECT_NO_THROW(document().commitTransaction());
    EXPECT_NO_THROW(document().recompute());
    EXPECT_NO_THROW(document().removeObject(object->getNameInDocument()));
}

TEST_F(CollaborationAuthorityRemovalTest, atomicPresentationStillRejectsCrossDocumentMutation)
{
    const std::string otherName = "CollaborationAuthorityRemovalOther";
    if (GetApplication().getDocument(otherName.c_str())) {
        GetApplication().closeDocument(otherName.c_str());
    }
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);

    beginAtomicPresentationMutationTarget(document());
    EXPECT_NO_THROW(document().addObject("App::FeatureTest", "SameDocument"));
    EXPECT_THROW(other->addObject("App::FeatureTest", "CrossDocument"), Base::RuntimeError);
    endAtomicPresentationMutationTarget(document());

    EXPECT_NO_THROW(other->addObject("App::FeatureTest", "AfterAudit"));
    GetApplication().closeDocument(otherName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest, atomicPresentationGuardAcceptsNullMutationFunnels)
{
    beginAtomicPresentationMutationTarget(document());
    EXPECT_NO_THROW(enforceAtomicPresentationMutationTarget(nullptr));
    endAtomicPresentationMutationTarget(document());
}

}  // namespace
}  // namespace App
