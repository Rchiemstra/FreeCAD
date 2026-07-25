// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#include <chrono>
#include <future>
#include <thread>

#include <boost/scope_exit.hpp>
#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentRecomputeCoordinator.h"
#include "App/FeatureTest.h"
#include "App/GeometryJob.h"
#include "App/GeometryJobManager.h"
#include "App/StringHasher.h"
#include <src/App/InitApplication.h>

using namespace std::chrono_literals;

class AsyncRecomputeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("async_recompute");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
    }

    void TearDown() override
    {
        if (!_docName.empty() && App::GetApplication().getDocument(_docName.c_str())) {
            App::GetApplication().closeDocument(_docName.c_str());
        }
    }

    std::string _docName;
    App::Document* _doc {};
};

TEST_F(AsyncRecomputeTest, CloseDocumentDoesNotWaitForInFlightAsyncRecompute)
{
    auto* object = dynamic_cast<App::FeatureTestAsyncBlocker*>(
        _doc->addObject("App::FeatureTestAsyncBlocker", "BlockingFeature")
    );
    ASSERT_NE(object, nullptr);

    App::FeatureTestAsyncBlocker::resetBlocker();
    BOOST_SCOPE_EXIT_ALL(&)
    {
        App::FeatureTestAsyncBlocker::releaseBlocker();
    };

    object->touch();

    App::GetApplication().queueRecomputeRequest(App::RecomputeRequest::fromDocumentObject(*object));

    ASSERT_TRUE(App::FeatureTestAsyncBlocker::waitUntilStarted(2s));

    auto closeFuture = std::async(std::launch::async, [this] {
        return App::GetApplication().closeDocument(_docName.c_str());
    });

    // Close must return without waiting for the in-flight legacy worker.
    ASSERT_EQ(closeFuture.wait_for(200ms), std::future_status::ready);
    EXPECT_TRUE(closeFuture.get());
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), nullptr);

    // Unblock the worker so deferred Document destruction can complete.
    App::FeatureTestAsyncBlocker::releaseBlocker();
    // Give the worker a moment to finish destroying the deferred document.
    std::this_thread::sleep_for(50ms);

    _doc = nullptr;
}

TEST_F(AsyncRecomputeTest, WorkerSafetyIsCheckedFromRequest)
{
    auto* safeObject = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "SafeFeature")
    );
    auto* unsafeObject = dynamic_cast<App::FeatureTestAttribute*>(
        _doc->addObject("App::FeatureTestAttribute", "UnsafeFeature")
    );

    ASSERT_NE(safeObject, nullptr);
    ASSERT_NE(unsafeObject, nullptr);

    EXPECT_TRUE(
        App::GetApplication().canRecomputeRequestOnWorker(
            App::RecomputeRequest::fromDocumentObject(*safeObject)
        )
    );
    EXPECT_FALSE(
        App::GetApplication().canRecomputeRequestOnWorker(
            App::RecomputeRequest::fromDocumentObject(*unsafeObject)
        )
    );
    EXPECT_FALSE(
        App::GetApplication().canRecomputeRequestOnWorker(App::RecomputeRequest::fromDocument(*_doc))
    );
}

TEST_F(AsyncRecomputeTest, EmptyRecomputeAsyncExpandsTouchedTargets)
{
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "TouchedFeature")
    );
    ASSERT_NE(object, nullptr);
    object->touch();

    App::RecomputeTargets targets;
    targets.forceAll = false;
    // Empty objectIds: coordinator must expand to touched objects.
    auto handle = _doc->getRecomputeCoordinator().request(targets, {});
    EXPECT_TRUE(handle.isValid());
    // Without detached adapters the session completes immediately after skipping.
    EXPECT_FALSE(_doc->getRecomputeCoordinator().isRecomputing());
    ASSERT_FALSE(_doc->getRecomputeCoordinator().unsupportedObjects().empty());
    EXPECT_EQ(_doc->getRecomputeCoordinator().unsupportedObjects().front(), "TouchedFeature");
}

TEST_F(AsyncRecomputeTest, CommitFenceRejectsMismatchedObjectIdentity)
{
    auto* object = dynamic_cast<App::FeatureTestDetached*>(
        _doc->addObject("App::FeatureTestDetached", "DetachedFeature")
    );
    ASSERT_NE(object, nullptr);

    App::CommitFence fence;
    fence.jobId = 42;
    fence.objectId = object->getID();
    fence.objectName = "DetachedFeature";
    fence.objectType = object->getTypeId();
    fence.runtimeIncarnation = _doc->getRuntimeIncarnation();
    fence.modelGeneration = _doc->getModelGeneration();
    fence.inputFingerprint = "test.detached.echo|1|value=1";

    EXPECT_TRUE(App::commitFenceMatches(fence, *_doc, *object, 42));

    fence.objectName = "RenamedAway";
    EXPECT_FALSE(App::commitFenceMatches(fence, *_doc, *object, 42));

    fence.objectName = "DetachedFeature";
    fence.modelGeneration = _doc->getModelGeneration() + 1;
    EXPECT_FALSE(App::commitFenceMatches(fence, *_doc, *object, 42));

    fence.modelGeneration = _doc->getModelGeneration();
    fence.jobId = 99;
    EXPECT_FALSE(App::commitFenceMatches(fence, *_doc, *object, 42));
}

TEST_F(AsyncRecomputeTest, FailedDetachedCommitDoesNotAdvanceGeneration)
{
    App::FeatureTestDetached::resetTestHooks();
    App::GeometryJobManager::instance().setAllowInProcess(true);

    auto* object = dynamic_cast<App::FeatureTestDetached*>(
        _doc->addObject("App::FeatureTestDetached", "DetachedFail")
    );
    ASSERT_NE(object, nullptr);
    object->Value.setValue(7);
    object->touch();

    const uint64_t genBefore = _doc->getModelGeneration();
    const uint64_t hasherRevBefore = _doc->getStringHasher()
        ? _doc->getStringHasher()->getRevision()
        : 0;
    App::FeatureTestDetached::setFailNextCommit(true);

    App::RecomputeTargets targets;
    targets.objectIds.push_back(object->getID());
    auto handle = _doc->getRecomputeCoordinator().request(targets, {});
    ASSERT_TRUE(handle.isValid());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
           && _doc->getRecomputeCoordinator().isRecomputing()) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_FALSE(_doc->getRecomputeCoordinator().isRecomputing());
    EXPECT_EQ(App::FeatureTestDetached::commitCount(), 0);
    EXPECT_EQ(_doc->getModelGeneration(), genBefore)
        << "failed commit must not advance model generation";
    ASSERT_TRUE(_doc->getStringHasher());
    EXPECT_EQ(_doc->getStringHasher()->getRevision(), hasherRevBefore)
        << "failed commit must not advance hasher revision";

    App::GeometryJobManager::instance().setAllowInProcess(false);
    App::FeatureTestDetached::resetTestHooks();
}

TEST_F(AsyncRecomputeTest, SuccessfulDetachedCommitAdvancesGenerationOnce)
{
    App::FeatureTestDetached::resetTestHooks();
    App::GeometryJobManager::instance().setAllowInProcess(true);

    auto* object = dynamic_cast<App::FeatureTestDetached*>(
        _doc->addObject("App::FeatureTestDetached", "DetachedOk")
    );
    ASSERT_NE(object, nullptr);
    object->Value.setValue(11);
    object->touch();

    const uint64_t genBefore = _doc->getModelGeneration();
    const uint64_t hasherRevBefore = _doc->getStringHasher()
        ? _doc->getStringHasher()->getRevision()
        : 0;

    App::RecomputeTargets targets;
    targets.objectIds.push_back(object->getID());
    auto handle = _doc->getRecomputeCoordinator().request(targets, {});
    ASSERT_TRUE(handle.isValid());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
           && _doc->getRecomputeCoordinator().isRecomputing()) {
        std::this_thread::sleep_for(5ms);
    }

    EXPECT_FALSE(_doc->getRecomputeCoordinator().isRecomputing());
    EXPECT_EQ(App::FeatureTestDetached::commitCount(), 1);
    EXPECT_EQ(object->CommittedValue.getValue(), 11);
    EXPECT_EQ(_doc->getModelGeneration(), genBefore + 1);
    ASSERT_TRUE(_doc->getStringHasher());
    EXPECT_EQ(_doc->getStringHasher()->getRevision(), hasherRevBefore + 1)
        << "successful detached commit must advance document hasher revision";

    App::GeometryJobManager::instance().setAllowInProcess(false);
    App::FeatureTestDetached::resetTestHooks();
}
