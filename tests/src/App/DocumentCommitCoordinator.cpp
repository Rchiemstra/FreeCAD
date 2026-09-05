// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

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
