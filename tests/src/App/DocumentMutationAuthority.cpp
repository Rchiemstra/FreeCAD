// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentMutationAuthority.h"
#include "App/DocumentObject.h"
#include "App/MutationKind.h"
#include "Base/Exception.h"
#include <src/App/InitApplication.h>

using namespace App;

class DocumentMutationAuthorityTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("mutationAuth");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "mutationAuthUser");
        DocumentMutationAuthority::instance().clearOwner(*_doc);
    }

    void TearDown() override
    {
        if (_doc) {
            DocumentMutationAuthority::instance().clearOwner(*_doc);
        }
        App::GetApplication().closeDocument(_docName.c_str());
        _doc = nullptr;
    }

    Document* doc()
    {
        return _doc;
    }

private:
    std::string _docName;
    Document* _doc {};
};

TEST_F(DocumentMutationAuthorityTest, unrestrictedAllowsMutationWithoutCapability)
{
    EXPECT_EQ(DocumentMutationAuthority::instance().owner(*doc()), MutationOwner::Unrestricted);
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "Obj1"));
}

TEST_F(DocumentMutationAuthorityTest, mcpOwnedDeniesWithoutCapability)
{
    DocumentMutationAuthority::instance().setOwner(*doc(), MutationOwner::McpOwned, 7, "test");
    EXPECT_TRUE(DocumentMutationAuthority::instance().isRestricted(*doc()));
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "Blocked"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, validCapabilityAllowsMutation)
{
    auto& authority = DocumentMutationAuthority::instance();
    authority.setOwner(*doc(), MutationOwner::McpOwned, 3, "test");
    {
        auto scope = authority.openCapability(
            *doc(),
            mutationKindBit(MutationKind::AddObject) | mutationKindBit(MutationKind::PropertyWrite)
                | mutationKindBit(MutationKind::TransactionOpen)
                | mutationKindBit(MutationKind::TransactionCommit)
                | mutationKindBit(MutationKind::TransactionAbort),
            3);
        ASSERT_TRUE(scope.valid());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "Allowed"));
    }
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "AfterRelease"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, staleGenerationDenied)
{
    auto& authority = DocumentMutationAuthority::instance();
    authority.setOwner(*doc(), MutationOwner::McpOwned, 5, "test");
    auto scope = authority.openCapability(*doc(), MutationKindAll, 4);
    EXPECT_FALSE(scope.valid());
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "Stale"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, wrongKindDenied)
{
    auto& authority = DocumentMutationAuthority::instance();
    authority.setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    auto scope = authority.openCapability(*doc(), mutationKindBit(MutationKind::Save), 1);
    ASSERT_TRUE(scope.valid());
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "WrongKind"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, internalScopeAllowsRecomputePath)
{
    auto& authority = DocumentMutationAuthority::instance();
    authority.setOwner(*doc(), MutationOwner::McpOwned, 2, "test");
    {
        MutationInternalScope internal(doc());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "InternalOk"));
    }
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "AfterInternal"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, takeoverInvalidatesOldCapability)
{
    auto& authority = DocumentMutationAuthority::instance();
    authority.setOwner(*doc(), MutationOwner::McpOwned, 9, "test");
    auto scope = authority.openCapability(*doc(), MutationKindAll, 9);
    ASSERT_TRUE(scope.valid());

    const auto newGen = authority.takeover(*doc());
    EXPECT_EQ(newGen, 10u);
    EXPECT_EQ(authority.owner(*doc()), MutationOwner::UserOwned);
    // UserOwned allows local mutation without capability.
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "UserAfterTakeover"));

    // Old MCP generation cannot open a new capability until re-owned.
    auto stale = authority.openCapability(*doc(), MutationKindAll, 9);
    EXPECT_FALSE(stale.valid());
}

TEST_F(DocumentMutationAuthorityTest, multiDocumentNoCrossLeak)
{
    auto& authority = DocumentMutationAuthority::instance();
    const std::string otherName = App::GetApplication().getUniqueDocumentName("mutationAuthOther");
    Document* other = App::GetApplication().newDocument(otherName.c_str(), "otherUser");

    authority.setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    authority.setOwner(*other, MutationOwner::McpOwned, 1, "test");

    {
        auto scope = authority.openCapability(*doc(), MutationKindAll, 1);
        ASSERT_TRUE(scope.valid());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "DocA"));
        EXPECT_THROW(other->addObject("App::FeatureTest", "DocB"), Base::MutationDeniedException);
    }

    authority.clearOwner(*other);
    App::GetApplication().closeDocument(otherName.c_str());
}
