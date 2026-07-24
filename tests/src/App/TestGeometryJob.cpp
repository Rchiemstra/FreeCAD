// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <App/GeometryJob.h>
#include <App/GeometryJobManager.h>
#include <App/DocumentRecomputeCoordinator.h>

TEST(GeometryJobTest, StateMachineAndExactOnceCallback)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 100;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 42;
    spec.target.internalName = "Box001";
    spec.key.documentIncarnation = 100;
    spec.key.targetObjectId = 42;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    App::GeometryJobHandle handle = App::GeometryJobManager::instance().submit(spec);
    EXPECT_TRUE(handle.isValid());

    int callbackCount = 0;
    App::GeometryJobState lastState = App::GeometryJobState::Queued;

    App::GeometryJobManager::instance().registerCallback(handle.id(),
        [&callbackCount, &lastState](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            callbackCount++;
            lastState = state;
        });

    App::DetachedGeometryResult dummyResult;
    dummyResult.success = true;
    dummyResult.resultArchivePath = "/tmp/test.fcg";

    App::GeometryJobManager::instance().setJobState(handle.id(), App::GeometryJobState::Completed, dummyResult);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastState, App::GeometryJobState::Completed);

    // Verify exact-once guarantee: setting state again will not re-invoke callback
    App::GeometryJobManager::instance().setJobState(handle.id(), App::GeometryJobState::Completed, dummyResult);
    EXPECT_EQ(callbackCount, 1);
}

TEST(GeometryJobTest, CoalescingSameGeneration)
{
    App::GeometryJobSpec spec1;
    spec1.document.runtimeIncarnation = 200;
    spec1.document.modelGeneration = 5;
    spec1.target.objectId = 99;
    spec1.key.documentIncarnation = 200;
    spec1.key.targetObjectId = 99;
    spec1.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    App::GeometryJobHandle handle1 = App::GeometryJobManager::instance().submit(spec1);

    App::GeometryJobSpec spec2 = spec1;
    App::GeometryJobHandle handle2 = App::GeometryJobManager::instance().submit(spec2);

    // Identical generation submissions must coalesce into the same job handle
    EXPECT_EQ(handle1.id(), handle2.id());
}

TEST(GeometryJobTest, InvalidationOnDocumentClose)
{
    App::DocumentRevisionToken docToken;
    docToken.runtimeIncarnation = 300;
    docToken.modelGeneration = 1;

    App::GeometryJobSpec spec;
    spec.document = docToken;
    spec.target.objectId = 10;
    spec.key.documentIncarnation = 300;
    spec.key.targetObjectId = 10;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    App::GeometryJobHandle handle = App::GeometryJobManager::instance().submit(spec);

    bool documentClosedCalled = false;
    App::GeometryJobManager::instance().registerCallback(handle.id(),
        [&documentClosedCalled](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::DocumentClosed) {
                documentClosedCalled = true;
            }
        });

    App::GeometryJobManager::instance().invalidateDocument(docToken, App::CancelReason::DocumentClosed);
    EXPECT_TRUE(documentClosedCalled);
}
