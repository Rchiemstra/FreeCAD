// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentMutationAuthority.h"
#include "App/DocumentObject.h"
#include "App/MutationCapability.h"
#include "App/MutationKind.h"
#include "App/PropertyStandard.h"
#include "Base/Exception.h"
#include "Base/FileInfo.h"
#include <src/App/InitApplication.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace App;

namespace
{

MutationKindMask liveKinds()
{
    return MutationKindAll;
}

}  // namespace

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
            const std::string name = _docName;
            _doc = nullptr;
            App::GetApplication().closeDocument(name.c_str());
        }
    }

    Document* doc()
    {
        return _doc;
    }

    DocumentMutationAuthority& auth()
    {
        return DocumentMutationAuthority::instance();
    }

    void abandonDocumentWithoutClosing()
    {
        _doc = nullptr;
    }

    void replaceDocument(Document* document, const std::string& name)
    {
        _doc = document;
        _docName = name;
    }

    std::string documentName() const
    {
        return _docName;
    }

private:
    std::string _docName;
    Document* _doc {};
};

TEST_F(DocumentMutationAuthorityTest, unrestrictedAllowsMutationWithoutCapability)
{
    EXPECT_EQ(auth().owner(*doc()), MutationOwner::Unrestricted);
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "Obj1"));
}

TEST_F(DocumentMutationAuthorityTest, mcpOwnedDeniesAddObjectWithoutCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 7, "test");
    EXPECT_TRUE(auth().isRestricted(*doc()));
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "Blocked"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, validCapabilityAllowsAndReleaseDenies)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 3, "test");
    {
        auto scope = auth().openCapability(*doc(), liveKinds(), 3);
        ASSERT_TRUE(scope.valid());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "Allowed"));
    }
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "AfterRelease"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, propertyWriteRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    DocumentObject* obj = nullptr;
    {
        auto scope = auth().openCapability(*doc(), liveKinds(), 1);
        obj = doc()->addObject("App::FeatureTest", "PropObj");
    }
    ASSERT_NE(obj, nullptr);
    EXPECT_THROW(obj->Label.setValue("Denied"), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::PropertyWrite), 1);
        ASSERT_TRUE(scope.valid());
        EXPECT_NO_THROW(obj->Label.setValue("Allowed"));
    }
}

TEST_F(DocumentMutationAuthorityTest, structuralPropertyRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    EXPECT_THROW(doc()->addDynamicProperty("App::PropertyFloat", "DynDenied"),
                 Base::MutationDeniedException);
    {
        auto scope =
            auth().openCapability(*doc(), mutationKindBit(MutationKind::StructuralProperty), 1);
        ASSERT_TRUE(scope.valid());
        Property* prop = doc()->addDynamicProperty("App::PropertyFloat", "DynOk");
        ASSERT_NE(prop, nullptr);
    }
    EXPECT_THROW(doc()->removeDynamicProperty("DynOk"), Base::MutationDeniedException);
    {
        auto scope =
            auth().openCapability(*doc(), mutationKindBit(MutationKind::StructuralProperty), 1);
        EXPECT_TRUE(doc()->removeDynamicProperty("DynOk"));
    }
}

TEST_F(DocumentMutationAuthorityTest, removeObjectRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    DocumentObject* obj = nullptr;
    {
        auto scope = auth().openCapability(*doc(), liveKinds(), 1);
        obj = doc()->addObject("App::FeatureTest", "ToRemove");
    }
    ASSERT_NE(obj, nullptr);
    EXPECT_THROW(doc()->removeObject("ToRemove"), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::RemoveObject), 1);
        EXPECT_NO_THROW(doc()->removeObject("ToRemove"));
    }
    EXPECT_EQ(doc()->getObject("ToRemove"), nullptr);
}

TEST_F(DocumentMutationAuthorityTest, recomputeRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    {
        auto scope = auth().openCapability(*doc(), liveKinds(), 1);
        doc()->addObject("App::FeatureTest", "RecomputeObj");
    }
    EXPECT_THROW(doc()->recompute(), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::Recompute), 1);
        EXPECT_NO_THROW(doc()->recompute());
    }
}

TEST_F(DocumentMutationAuthorityTest, transactionsRequireCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    EXPECT_THROW(doc()->openTransaction("denied"), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(
            *doc(),
            mutationKindBit(MutationKind::TransactionOpen)
                | mutationKindBit(MutationKind::TransactionCommit)
                | mutationKindBit(MutationKind::TransactionAbort)
                | mutationKindBit(MutationKind::PropertyWrite)
                | mutationKindBit(MutationKind::AddObject),
            1);
        ASSERT_TRUE(scope.valid());
        EXPECT_NE(doc()->openTransaction("allowed"), 0);
        doc()->addObject("App::FeatureTest", "TxnObj");
        EXPECT_NO_THROW(doc()->commitTransaction());
    }
}

TEST_F(DocumentMutationAuthorityTest, undoRedoRequireCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    {
        auto scope = auth().openCapability(*doc(), liveKinds(), 1);
        doc()->openTransaction("edit");
        doc()->addObject("App::FeatureTest", "UndoObj");
        doc()->commitTransaction();
    }
    EXPECT_THROW(doc()->undo(), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::Undo), 1);
        EXPECT_TRUE(doc()->undo());
    }
    EXPECT_THROW(doc()->redo(), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::Redo), 1);
        EXPECT_TRUE(doc()->redo());
    }
}

TEST_F(DocumentMutationAuthorityTest, saveRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    const auto tempDir = std::filesystem::temp_directory_path()
        / ("fc_mut_auth_" + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tempDir);
    const auto path = (tempDir / "doc.FCStd").string();

    EXPECT_THROW(doc()->saveAs(path.c_str()), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(
            *doc(),
            mutationKindBit(MutationKind::SaveAs) | mutationKindBit(MutationKind::PropertyWrite),
            1);
        EXPECT_TRUE(doc()->saveAs(path.c_str()));
    }
    EXPECT_THROW(doc()->save(), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(
            *doc(),
            mutationKindBit(MutationKind::Save) | mutationKindBit(MutationKind::PropertyWrite),
            1);
        EXPECT_TRUE(doc()->save());
    }
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

TEST_F(DocumentMutationAuthorityTest, closeRequiresCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    const std::string name = doc()->getName();
    EXPECT_THROW(App::GetApplication().closeDocument(name.c_str()), Base::MutationDeniedException);
    {
        auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::Close), 1);
        abandonDocumentWithoutClosing();
        EXPECT_TRUE(App::GetApplication().closeDocument(name.c_str()));
    }
}

TEST_F(DocumentMutationAuthorityTest, wrongKindDenied)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    auto scope = auth().openCapability(*doc(), mutationKindBit(MutationKind::Save), 1);
    ASSERT_TRUE(scope.valid());
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "WrongKind"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, staleGenerationDenied)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 5, "test");
    auto scope = auth().openCapability(*doc(), MutationKindAll, 4);
    EXPECT_FALSE(scope.valid());
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "Stale"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, internalScopeAllows)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 2, "test");
    {
        MutationInternalScope internal(doc());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "InternalOk"));
    }
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "AfterInternal"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, clearReownSameGenerationRejectsOldCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 9, "test");
    auto oldCap = auth().openCapability(*doc(), MutationKindAll, 9);
    ASSERT_TRUE(oldCap.valid());
    auth().clearOwner(*doc());
    auth().setOwner(*doc(), MutationOwner::McpOwned, 9, "test");
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "OldCap"), Base::MutationDeniedException);
    auto fresh = auth().openCapability(*doc(), MutationKindAll, 9);
    ASSERT_TRUE(fresh.valid());
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "FreshCap"));
}

TEST_F(DocumentMutationAuthorityTest, takeoverReownSameGenerationRejectsOldCapability)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 9, "test");
    auto oldCap = auth().openCapability(*doc(), MutationKindAll, 9);
    ASSERT_TRUE(oldCap.valid());
    const auto newGen = auth().takeover(*doc());
    EXPECT_EQ(auth().owner(*doc()), MutationOwner::UserOwned);
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "UserAfterTakeover"));

    auth().setOwner(*doc(), MutationOwner::McpOwned, 9, "test");
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "StaleAfterTakeover"),
                 Base::MutationDeniedException);
    EXPECT_GT(newGen, 9u);
}

TEST_F(DocumentMutationAuthorityTest, documentRecreateSameAddressDoesNotCrossGrant)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    const Document* oldPtr = doc();
    const auto oldEpoch = auth().authorityEpoch(*doc());
    {
        auto oldCap = auth().openCapability(*doc(), MutationKindAll, 1);
        ASSERT_TRUE(oldCap.valid());
    }

    auth().clearOwner(*doc());
    const std::string oldName = doc()->getName();
    App::GetApplication().closeDocument(oldName.c_str());
    abandonDocumentWithoutClosing();

    const std::string newName = App::GetApplication().getUniqueDocumentName("mutationAuth");
    Document* created = App::GetApplication().newDocument(newName.c_str(), "mutationAuthUser");
    replaceDocument(created, newName);
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    EXPECT_NE(auth().authorityEpoch(*doc()), 0u);
    if (doc() == oldPtr) {
        EXPECT_GT(auth().authorityEpoch(*doc()), oldEpoch);
    }
    EXPECT_THROW(doc()->addObject("App::FeatureTest", "CrossDoc"), Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, multiDocumentNoCrossLeak)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    const std::string otherName = App::GetApplication().getUniqueDocumentName("mutationAuthOther");
    Document* other = App::GetApplication().newDocument(otherName.c_str(), "otherUser");
    auth().setOwner(*other, MutationOwner::McpOwned, 1, "test");

    {
        auto scope = auth().openCapability(*doc(), MutationKindAll, 1);
        ASSERT_TRUE(scope.valid());
        EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "DocA"));
        EXPECT_THROW(other->addObject("App::FeatureTest", "DocB"), Base::MutationDeniedException);
        EXPECT_FALSE(auth().hasActiveCapabilityForDocument(*other));
        EXPECT_TRUE(auth().hasActiveCapabilityForDocument(*doc()));
    }

    auth().clearOwner(*other);
    App::GetApplication().closeDocument(otherName.c_str());
}

TEST_F(DocumentMutationAuthorityTest, crossThreadCapabilityDestructionRevokes)
{
    auth().setOwner(*doc(), MutationOwner::McpOwned, 1, "test");
    auto* heapScope =
        new MutationCapabilityScope(auth().openCapability(*doc(), MutationKindAll, 1));
    ASSERT_TRUE(heapScope->valid());
    EXPECT_NO_THROW(doc()->addObject("App::FeatureTest", "BeforeCrossThread"));

    std::thread worker([heapScope]() {
        delete heapScope;
    });
    worker.join();

    EXPECT_THROW(doc()->addObject("App::FeatureTest", "AfterCrossThread"),
                 Base::MutationDeniedException);
}

TEST_F(DocumentMutationAuthorityTest, generationAbove32BitsPreserved)
{
    constexpr std::uint64_t largeGen = (1ull << 33) + 42ull;
    auth().setOwner(*doc(), MutationOwner::McpOwned, largeGen, "test");
    EXPECT_EQ(auth().fencingGeneration(*doc()), largeGen);
    auto scope = auth().openCapability(*doc(), MutationKindAll, largeGen);
    ASSERT_TRUE(scope.valid());
    EXPECT_EQ(scope.capability().fencingGeneration, largeGen);
}
