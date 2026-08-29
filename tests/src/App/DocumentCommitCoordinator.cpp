// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Document.h"
#include "App/DocumentCommitCoordinator.h"

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
