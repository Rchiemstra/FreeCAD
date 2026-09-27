// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCrossDocumentSnapshot.h>
#include <src/App/InitApplication.h>

namespace
{

class DocumentCrossDocumentSnapshotTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tests::initApplication();
        _docA = App::GetApplication().newDocument("CrossDocA");
        _docB = App::GetApplication().newDocument("CrossDocB");
        ASSERT_NE(_docA, nullptr);
        ASSERT_NE(_docB, nullptr);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument("CrossDocA");
        App::GetApplication().closeDocument("CrossDocB");
    }

    App::Document* _docA {nullptr};
    App::Document* _docB {nullptr};
};

}  // namespace

TEST_F(DocumentCrossDocumentSnapshotTest, CaptureBindsRevisionObservations)
{
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::documentStructure()};
    const auto snapshot = App::captureCrossDocumentSnapshot(*_docA, keys);
    ASSERT_TRUE(snapshot.valid());
    EXPECT_TRUE(snapshot.revisionsAtCapture.size() >= 1);
    EXPECT_TRUE(snapshot.validateAgainstCurrent(*_docA).empty());
}

TEST_F(DocumentCrossDocumentSnapshotTest, UndeclaredLiveCrossDocumentDependencyIsUnsupported)
{
    const auto identityA = *_docA->collaborationRevisions().documentIdentity();
    const auto identityB = *_docB->collaborationRevisions().documentIdentity();
    const auto outcome = App::tryReserveDocumentsForCrossDocumentCommand(
        {identityA, identityB},
        App::DocumentCrossDocumentDependencyKind::UndeclaredLiveReference);
    EXPECT_EQ(outcome.result, App::DocumentCrossDocumentReservationResult::Unsupported);
}

TEST_F(DocumentCrossDocumentSnapshotTest, ReservationsUseStableDocumentIdOrder)
{
    const auto identityA = *_docA->collaborationRevisions().documentIdentity();
    const auto identityB = *_docB->collaborationRevisions().documentIdentity();
    std::vector<App::DocumentRevisionIdentityBinding> identities {identityB, identityA};
    const auto outcome = App::tryReserveDocumentsForCrossDocumentCommand(
        identities,
        App::DocumentCrossDocumentDependencyKind::DeclaredSnapshot);
    ASSERT_EQ(outcome.result, App::DocumentCrossDocumentReservationResult::Reserved);
    ASSERT_EQ(outcome.reservedInOrder.size(), 2U);
    EXPECT_LE(outcome.reservedInOrder.front().documentInstanceId,
              outcome.reservedInOrder.back().documentInstanceId);
    App::releaseCrossDocumentReservations(outcome.reservedInOrder);
}

TEST_F(DocumentCrossDocumentSnapshotTest, SecondReservationReturnsBusyWithoutWaiting)
{
    const auto identityA = *_docA->collaborationRevisions().documentIdentity();
    const auto first = App::tryReserveDocumentsForCrossDocumentCommand(
        {identityA},
        App::DocumentCrossDocumentDependencyKind::DeclaredSnapshot);
    ASSERT_EQ(first.result, App::DocumentCrossDocumentReservationResult::Reserved);

    const auto second = App::tryReserveDocumentsForCrossDocumentCommand(
        {identityA},
        App::DocumentCrossDocumentDependencyKind::DeclaredSnapshot);
    EXPECT_EQ(second.result, App::DocumentCrossDocumentReservationResult::Busy);

    App::releaseCrossDocumentReservations(first.reservedInOrder);
}
