// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/MutationClassification.h>
#include <Base/Exception.h>

#include <atomic>
#include <string>
#include <optional>
#include <thread>

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
    EXPECT_NO_THROW(enforceAtomicPresentationMutationTarget(document()));
    EXPECT_THROW(enforceAtomicPresentationMutationTarget(*other), Base::RuntimeError);
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

TEST_F(CollaborationAuthorityRemovalTest,
       concurrentSecondDocumentCannotAcquirePreparedMutationAdmission)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("ConcurrentAdmissionOther");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);

    bool rejected = false;
    beginAtomicPresentationMutationTarget(document());
    std::thread worker([&] {
        try {
            beginAtomicPresentationMutationTarget(*other);
            endAtomicPresentationMutationTarget(*other);
        }
        catch (const Base::Exception&) {
            rejected = true;
        }
    });
    worker.join();
    endAtomicPresentationMutationTarget(document());

    EXPECT_TRUE(rejected);
    EXPECT_NO_THROW(beginAtomicPresentationMutationTarget(*other));
    endAtomicPresentationMutationTarget(*other);
    GetApplication().closeDocument(otherName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest,
       deniedRevisionReservationCommitAndCancelReleaseForeignIndexImmediately)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("RevisionReservationOther");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);
    const auto key = DocumentRevisionKey::unknownModelMutation();
    const auto revisionBefore = other->collaborationRevisions().current(key);

    auto deniedCommit = other->collaborationRevisions().reservePublication(
        {}, {{key, std::nullopt}});
    ASSERT_TRUE(deniedCommit.ready());
    beginAtomicPresentationMutationTarget(document());
    const auto observations = deniedCommit.commit();
    EXPECT_TRUE(observations.empty());
    EXPECT_FALSE(deniedCommit.ready());
    EXPECT_EQ(other->collaborationRevisions().current(key), revisionBefore);
    endAtomicPresentationMutationTarget(document());

    auto deniedCancel = other->collaborationRevisions().reservePublication(
        {}, {{key, std::nullopt}});
    ASSERT_TRUE(deniedCancel.ready());
    beginAtomicPresentationMutationTarget(document());
    deniedCancel.cancel();
    EXPECT_FALSE(deniedCancel.ready());
    EXPECT_EQ(other->collaborationRevisions().current(key), revisionBefore);
    endAtomicPresentationMutationTarget(document());

    GetApplication().closeDocument(otherName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest,
       revisionAdmissionIsAtomicWithTheIndexedMutation)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("AtomicRevisionAdmissionOther");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);
    auto& index = other->collaborationRevisions();
    const auto revisionBefore =
        index.current(DocumentRevisionKey::unknownModelMutation());
    const auto identityBefore = index.documentIdentity();
    ASSERT_TRUE(identityBefore.has_value());

    auto bindBlocker = index.reservePublication({}, {});
    ASSERT_TRUE(bindBlocker.ready());
    std::atomic_bool bindStarted {false};
    std::atomic_bool bindRejected {false};
    std::thread bindWorker([&] {
        bindStarted.store(true, std::memory_order_release);
        try {
            index.bindDocumentIdentity(identityBefore->documentInstanceId,
                                       identityBefore->lifecycleEpoch + 1);
        }
        catch (const Base::Exception&) {
            bindRejected.store(true, std::memory_order_release);
        }
    });
    while (!bindStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    beginAtomicPresentationMutationTarget(document());
    bindBlocker.cancel();
    bindWorker.join();
    EXPECT_TRUE(bindRejected.load(std::memory_order_acquire));
    const auto identityAfterDeniedBind = index.documentIdentity();
    ASSERT_TRUE(identityAfterDeniedBind.has_value());
    EXPECT_EQ(identityAfterDeniedBind->lifecycleEpoch,
              identityBefore->lifecycleEpoch);
    endAtomicPresentationMutationTarget(document());

    auto reserveBlocker = index.reservePublication({}, {});
    ASSERT_TRUE(reserveBlocker.ready());
    std::atomic_bool reserveStarted {false};
    std::atomic_bool reserveRejected {false};
    std::thread reserveWorker([&] {
        reserveStarted.store(true, std::memory_order_release);
        try {
            auto denied = index.reservePublication(
                {}, {{DocumentRevisionKey::unknownModelMutation(), std::nullopt}});
            denied.cancel();
        }
        catch (const Base::Exception&) {
            reserveRejected.store(true, std::memory_order_release);
        }
    });
    while (!reserveStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    beginAtomicPresentationMutationTarget(document());
    reserveBlocker.cancel();
    reserveWorker.join();
    EXPECT_TRUE(reserveRejected.load(std::memory_order_acquire));
    EXPECT_EQ(index.current(DocumentRevisionKey::unknownModelMutation()), revisionBefore);
    endAtomicPresentationMutationTarget(document());

    auto afterBoundary = index.reservePublication({}, {});
    EXPECT_TRUE(afterBoundary.ready());
    afterBoundary.cancel();
    GetApplication().closeDocument(otherName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest, recomputeBecomesStableAfterPendingRemoval)
{
    auto* object = document().addObject("App::FeatureTest", "PendingRemoval");
    ASSERT_NE(object, nullptr);
    object->setStatus(ObjectStatus::PendingRecompute, true);
    document().removeObject(object);
    object->setStatus(ObjectStatus::PendingRecompute, false);

    int stableCalls = 0;
    auto stableConnection = document().signalBecameStable.connect([&](const Document&) {
        ++stableCalls;
        EXPECT_EQ(document().getObject("PendingRemoval"), nullptr);
        EXPECT_TRUE(document().getMutationReadiness().ready);
    });

    document().recompute();
    stableConnection.disconnect();

    EXPECT_EQ(stableCalls, 1);
    EXPECT_EQ(document().getObject("PendingRemoval"), nullptr);
}

TEST_F(CollaborationAuthorityRemovalTest,
       preparedMutationTargetRequeuesRejectedForeignPendingRemoval)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("PendingRemovalOther");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);
    auto* object = other->addObject("App::FeatureTest", "PendingRemoval");
    ASSERT_NE(object, nullptr);
    object->setStatus(ObjectStatus::PendingRecompute, true);
    other->removeObject(object);
    object->setStatus(ObjectStatus::PendingRecompute, false);

    int otherStableCalls = 0;
    auto stableConnection = other->signalBecameStable.connect([&](const Document&) {
        ++otherStableCalls;
        EXPECT_TRUE(other->getMutationReadiness().ready);
    });
    EXPECT_TRUE(other->getMutationReadiness().pendingRemoval);
    EXPECT_FALSE(other->getMutationReadiness().ready);
    EXPECT_FALSE(other->canWriteRecoverySnapshot());

    beginAtomicPresentationMutationTarget(document());
    EXPECT_NO_THROW(document().recompute());
    endAtomicPresentationMutationTarget(document());
    EXPECT_EQ(other->getObject("PendingRemoval"), object);
    EXPECT_TRUE(other->getMutationReadiness().pendingRemoval);
    EXPECT_FALSE(other->getMutationReadiness().ready);
    EXPECT_EQ(otherStableCalls, 0);

    document().recompute();

    EXPECT_EQ(other->getObject("PendingRemoval"), nullptr);
    EXPECT_FALSE(other->getMutationReadiness().pendingRemoval);
    EXPECT_TRUE(other->getMutationReadiness().ready);
    EXPECT_EQ(otherStableCalls, 1);
    stableConnection.disconnect();
    GetApplication().closeDocument(otherName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest,
       recomputeDrainsForeignPendingRemovalAndWakesThatDocument)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("PendingRemovalDrainOther");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);
    const auto laterName =
        GetApplication().getUniqueDocumentName("ZZPendingRemovalLaterDocument");
    auto* later = GetApplication().newDocument(laterName.c_str());
    ASSERT_NE(later, nullptr);
    auto* object = other->addObject("App::FeatureTest", "PendingRemoval");
    ASSERT_NE(object, nullptr);
    object->setStatus(ObjectStatus::PendingRecompute, true);
    other->removeObject(object);
    object->setStatus(ObjectStatus::PendingRecompute, false);

    int otherStableCalls = 0;
    bool deletionObserved = false;
    bool deletionObservedBlocked = false;
    bool deletionRecoveryBlocked = false;
    bool nestedCommitBusy = false;
    bool activeCommitBusy = false;
    bool laterCommitBusy = false;
    bool deletionCloseRejected = false;
    bool stableCloseRejected = false;
    bool deletionActiveCloseRejected = false;
    bool deletionLaterCloseRejected = false;
    bool stableActiveCloseRejected = false;
    bool stableLaterCloseRejected = false;
    bool concurrentDeletionCloseRejected = false;
    bool deletionSnapshotReadinessBlocked = false;
    int nestedRecomputeResult = -1;
    int activeStableCalls = 0;
    auto activeStableConnection = document().signalBecameStable.connect(
        [&](const Document&) {
            ++activeStableCalls;
            EXPECT_TRUE(document().getMutationReadiness().ready);
        });
    auto deletedConnection = other->signalDeletedObject.connect(
        [&](const DocumentObject&) {
            deletionObserved = true;
            const auto readiness = other->getMutationReadiness();
            deletionObservedBlocked = readiness.pendingRemoval && !readiness.ready;
            deletionRecoveryBlocked = !other->canWriteRecoverySnapshot();
            const auto activeReadiness = document().getMutationReadiness();
            const auto laterReadiness = later->getMutationReadiness();
            deletionSnapshotReadinessBlocked = activeReadiness.recomputing
                && !activeReadiness.ready && laterReadiness.recomputing
                && !laterReadiness.ready;
            nestedRecomputeResult = document().recompute();
            const auto attemptCommit = [](Document& target) {
                CollaborationCompatibilityMutation mutation;
                mutation.scope = CollaborationCompatibilityScope::UnknownModel;
                return target.collaborationService().commitCompatibilityMutation(
                    std::move(mutation), [] {});
            };
            nestedCommitBusy =
                attemptCommit(*other).status == DocumentCommitStatus::Busy;
            activeCommitBusy =
                attemptCommit(document()).status == DocumentCommitStatus::Busy;
            laterCommitBusy =
                attemptCommit(*later).status == DocumentCommitStatus::Busy;
            deletionCloseRejected =
                !GetApplication().closeDocument(otherName.c_str());
            deletionActiveCloseRejected =
                !GetApplication().closeDocument(document().getName());
            deletionLaterCloseRejected =
                !GetApplication().closeDocument(laterName.c_str());
            std::thread concurrentClose([&] {
                concurrentDeletionCloseRejected =
                    !GetApplication().closeDocument(otherName.c_str());
            });
            concurrentClose.join();
        });
    auto stableConnection = other->signalBecameStable.connect([&](const Document&) {
        ++otherStableCalls;
        EXPECT_EQ(other->getObject("PendingRemoval"), nullptr);
        EXPECT_TRUE(other->getMutationReadiness().ready);
        EXPECT_TRUE(document().getMutationReadiness().ready);
        EXPECT_TRUE(later->getMutationReadiness().ready);
        stableCloseRejected =
            !GetApplication().closeDocument(otherName.c_str());
        stableActiveCloseRejected =
            !GetApplication().closeDocument(document().getName());
        stableLaterCloseRejected =
            !GetApplication().closeDocument(laterName.c_str());
    });

    EXPECT_TRUE(other->getMutationReadiness().pendingRemoval);
    EXPECT_NO_THROW(document().recompute());

    stableConnection.disconnect();
    activeStableConnection.disconnect();
    deletedConnection.disconnect();
    EXPECT_EQ(otherStableCalls, 1);
    EXPECT_TRUE(deletionObserved);
    EXPECT_TRUE(deletionObservedBlocked);
    EXPECT_TRUE(deletionRecoveryBlocked);
    EXPECT_TRUE(nestedCommitBusy);
    EXPECT_TRUE(activeCommitBusy);
    EXPECT_TRUE(laterCommitBusy);
    EXPECT_TRUE(deletionCloseRejected);
    EXPECT_TRUE(stableCloseRejected);
    EXPECT_TRUE(deletionActiveCloseRejected);
    EXPECT_TRUE(deletionLaterCloseRejected);
    EXPECT_TRUE(stableActiveCloseRejected);
    EXPECT_TRUE(stableLaterCloseRejected);
    EXPECT_TRUE(concurrentDeletionCloseRejected);
    EXPECT_TRUE(deletionSnapshotReadinessBlocked);
    EXPECT_EQ(nestedRecomputeResult, 0);
    EXPECT_EQ(activeStableCalls, 1);
    EXPECT_EQ(other->getObject("PendingRemoval"), nullptr);
    EXPECT_TRUE(other->getMutationReadiness().ready);
    GetApplication().closeDocument(otherName.c_str());
    GetApplication().closeDocument(laterName.c_str());
}

TEST_F(CollaborationAuthorityRemovalTest,
       queuedPendingRemovalDoesNotPreventOrdinaryDocumentClose)
{
    const auto otherName =
        GetApplication().getUniqueDocumentName("QueuedRemovalClose");
    auto* other = GetApplication().newDocument(otherName.c_str());
    ASSERT_NE(other, nullptr);
    auto* object = other->addObject("App::FeatureTest", "PendingRemoval");
    ASSERT_NE(object, nullptr);
    object->setStatus(ObjectStatus::PendingRecompute, true);
    other->removeObject(object);
    object->setStatus(ObjectStatus::PendingRecompute, false);
    ASSERT_TRUE(other->getMutationReadiness().pendingRemoval);

    EXPECT_TRUE(GetApplication().closeDocument(otherName.c_str()));
    EXPECT_EQ(GetApplication().getDocument(otherName.c_str()), nullptr);
}

}  // namespace
}  // namespace App
