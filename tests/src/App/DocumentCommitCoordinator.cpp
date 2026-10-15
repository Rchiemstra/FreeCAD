// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCommitCoordinator.h"
#include "App/NativeRecomputeErrors.h"
#include <src/App/InitApplication.h>

#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace App;

namespace App
{
class DocumentObject;
}

static_assert(!std::is_default_constructible_v<DocumentCommitCoordinator>);
static_assert(!std::is_constructible_v<DocumentCommitCoordinator, Document&>);
static_assert(!std::is_copy_constructible_v<DocumentCommitCoordinator>);
static_assert(!std::is_copy_assignable_v<DocumentCommitCoordinator>);
static_assert(!std::is_constructible_v<DocumentCommitResult, Document*>);
static_assert(!std::is_constructible_v<DocumentCommitResult, DocumentObject*>);

using OpenTransactionByName = int (Document::*)(TransactionName, int);
using OpenTransactionByString = int (Document::*)(std::string, int);
using SetActiveTransaction = int (Document::*)(TransactionName, int);
using CommitTransaction = void (Document::*)();
using AbortTransaction = void (Document::*)() const;
using HistoryTransaction = bool (Document::*)(int);
using ClearTransactionHistory = void (Document::*)();

static_assert(std::is_same_v<
              decltype(static_cast<OpenTransactionByName>(&Document::openTransaction)),
              OpenTransactionByName>);
static_assert(std::is_same_v<
              decltype(static_cast<OpenTransactionByString>(&Document::openTransaction)),
              OpenTransactionByString>);
static_assert(std::is_same_v<decltype(&Document::commitTransaction), CommitTransaction>);
static_assert(std::is_same_v<decltype(&Document::abortTransaction), AbortTransaction>);
static_assert(std::is_same_v<decltype(&Document::setActiveTransaction), SetActiveTransaction>);
static_assert(std::is_same_v<decltype(&Document::undo), HistoryTransaction>);
static_assert(std::is_same_v<decltype(&Document::redo), HistoryTransaction>);
static_assert(std::is_same_v<decltype(&Document::clearUndos), ClearTransactionHistory>);

TEST(DocumentCommitResultTest, exposesStableStatusNames)
{
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::Committed), "Committed");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::StaleDocument), "StaleDocument");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::InvalidPreparedEdit),
                 "InvalidPreparedEdit");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::Conflict), "Conflict");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::Cancelled), "Cancelled");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::Unsupported), "Unsupported");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::Busy), "Busy");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::ApplyFailed), "ApplyFailed");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::RecomputeFailed),
                 "RecomputeFailed");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::PostconditionFailed),
                 "PostconditionFailed");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::PublicationFailed),
                 "PublicationFailed");
    EXPECT_STREQ(documentCommitStatusName(DocumentCommitStatus::RollbackFailed),
                 "RollbackFailed");
}

TEST(DocumentCommitResultTest, committedPredicateReflectsOnlyCommittedStatus)
{
    DocumentCommitResult committed;
    committed.status = DocumentCommitStatus::Committed;
    DocumentCommitResult conflict;
    conflict.status = DocumentCommitStatus::Conflict;

    EXPECT_TRUE(committed.committed());
    EXPECT_FALSE(conflict.committed());
}

TEST(DocumentCommitCoordinatorContractTest, constructionIsRestrictedToCollaborationService)
{
    EXPECT_FALSE((std::is_constructible_v<DocumentCommitCoordinator, Document&>));
    EXPECT_FALSE(std::is_copy_constructible_v<DocumentCommitCoordinator>);
}

class NativeRecomputeRequestCaptureTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        auto& application = GetApplication();
        documentName = application.getUniqueDocumentName("RecomputeCapture");
        document = application.newDocument(documentName.c_str());
    }

    void TearDown() override
    {
        if (!foreignDocumentName.empty()) {
            GetApplication().closeDocument(foreignDocumentName.c_str());
        }
        GetApplication().closeDocument(documentName.c_str());
    }

    std::string documentName;
    std::string foreignDocumentName;
    Document* document {};
};

TEST_F(NativeRecomputeRequestCaptureTest, recordsRepeatedTouchesOfAlreadyDirtyObjects)
{
    auto* object = document->addObject("App::DocumentObject", "Target");
    object->touch();
    Internal::NativeRecomputeRequests requests;
    Internal::NativeRecomputeRequestCapture capture(*document, requests);

    object->touch();
    EXPECT_EQ(requests, (Internal::NativeRecomputeRequests {object}));
    requests.clear();
    object->touch();
    EXPECT_EQ(requests, (Internal::NativeRecomputeRequests {object}));
}

TEST_F(NativeRecomputeRequestCaptureTest, nestedCapturesRestoreThePreviousScope)
{
    auto* first = document->addObject("App::DocumentObject", "First");
    auto* second = document->addObject("App::DocumentObject", "Second");
    auto* third = document->addObject("App::DocumentObject", "Third");
    auto* outside = document->addObject("App::DocumentObject", "Outside");
    Internal::NativeRecomputeRequests outerRequests;
    Internal::NativeRecomputeRequests innerRequests;
    {
        Internal::NativeRecomputeRequestCapture outer(*document, outerRequests);
        first->touch();
        {
            Internal::NativeRecomputeRequestCapture inner(*document, innerRequests);
            second->touch();
        }
        third->touch();
    }
    outside->touch();

    EXPECT_EQ(outerRequests, (Internal::NativeRecomputeRequests {first, second, third}));
    EXPECT_EQ(innerRequests, (Internal::NativeRecomputeRequests {second}));
}

TEST_F(NativeRecomputeRequestCaptureTest, ignoresObjectsFromOtherDocuments)
{
    auto* object = document->addObject("App::DocumentObject", "Target");
    foreignDocumentName = GetApplication().getUniqueDocumentName("ForeignRecomputeCapture");
    auto* foreignDocument = GetApplication().newDocument(foreignDocumentName.c_str());
    auto* foreignObject = foreignDocument->addObject("App::DocumentObject", "Foreign");
    DocumentObject detached;
    Internal::NativeRecomputeRequests requests;
    Internal::NativeRecomputeRequestCapture capture(*document, requests);

    Internal::NativeRecomputeRequestCapture::record(detached);
    Internal::NativeRecomputeRequestCapture::record(*foreignObject);
    Internal::NativeRecomputeRequestCapture::record(*object);

    EXPECT_EQ(requests, (Internal::NativeRecomputeRequests {object}));
}

TEST_F(NativeRecomputeRequestCaptureTest, isolatesCapturesBetweenThreads)
{
    auto* first = document->addObject("App::DocumentObject", "OwnerThread");
    auto* second = document->addObject("App::DocumentObject", "WorkerThread");
    Internal::NativeRecomputeRequests ownerRequests;
    Internal::NativeRecomputeRequests workerRequests;
    Internal::NativeRecomputeRequestCapture ownerCapture(*document, ownerRequests);

    std::thread worker([&] {
        Internal::NativeRecomputeRequestCapture::record(*first);
        Internal::NativeRecomputeRequestCapture workerCapture(*document, workerRequests);
        Internal::NativeRecomputeRequestCapture::record(*second);
    });
    worker.join();

    EXPECT_TRUE(ownerRequests.empty());
    EXPECT_EQ(workerRequests, (Internal::NativeRecomputeRequests {second}));
    Internal::NativeRecomputeRequestCapture::record(*first);
    EXPECT_EQ(ownerRequests, (Internal::NativeRecomputeRequests {first}));
}
