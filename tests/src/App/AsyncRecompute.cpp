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
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <boost/scope_exit.hpp>
#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborativeSetPropertyOperation.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/FeatureTest.h"
#include <src/App/InitApplication.h>

using namespace std::chrono_literals;

namespace
{

class BlockingRecomputeDispatcher
{
public:
    BlockingRecomputeDispatcher()
        : _owner(std::this_thread::get_id())
    {
        Active = this;
        App::MainThreadSignalConfig::setHooks(&isOwnerThread, &invoke);
    }

    ~BlockingRecomputeDispatcher()
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        Active = nullptr;
    }

    bool waitUntilQueued(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return !_tasks.empty(); });
    }

    bool runOneFor(std::chrono::milliseconds timeout)
    {
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(_mutex);
            if (!_changed.wait_for(lock, timeout, [&] { return !_tasks.empty(); })) {
                return false;
            }
            task = std::move(_tasks.front());
            _tasks.pop_front();
        }
        task->callback();
        {
            std::lock_guard lock(task->mutex);
            task->done = true;
        }
        task->changed.notify_all();
        return true;
    }

private:
    struct Task
    {
        std::function<void()> callback;
        std::mutex mutex;
        std::condition_variable changed;
        bool done {false};
    };

    static bool isOwnerThread()
    {
        return Active && std::this_thread::get_id() == Active->_owner;
    }

    static void invoke(std::function<void()>&& callback, bool blocking)
    {
        if (!Active || isOwnerThread()) {
            callback();
            return;
        }
        auto task = std::make_shared<Task>();
        task->callback = std::move(callback);
        {
            std::lock_guard lock(Active->_mutex);
            Active->_tasks.push_back(task);
        }
        Active->_changed.notify_one();
        if (blocking) {
            std::unique_lock lock(task->mutex);
            task->changed.wait(lock, [&] { return task->done; });
        }
    }

    static inline BlockingRecomputeDispatcher* Active {nullptr};
    const std::thread::id _owner;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<Task>> _tasks;
};

class RecomputeCloseHookBarrier
{
public:
    RecomputeCloseHookBarrier()
    {
        Active = this;
    }

    ~RecomputeCloseHookBarrier()
    {
        release();
        Active = nullptr;
    }

    static void invoke()
    {
        std::unique_lock lock(Active->_mutex);
        Active->_entered = true;
        Active->_changed.notify_all();
        Active->_changed.wait(lock, [] { return Active->_released; });
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return _entered; });
    }

    void release()
    {
        {
            std::lock_guard lock(_mutex);
            _released = true;
        }
        _changed.notify_all();
    }

private:
    static inline RecomputeCloseHookBarrier* Active {nullptr};
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _entered {false};
    bool _released {false};
};

}  // namespace

namespace App::Internal
{

class AsyncRecomputeTestAccess
{
public:
    static void setPostClosingAdmissionHook(void (*hook)())
    {
        Application::_postRecomputeClosingAdmissionTestHook.store(
            hook, std::memory_order_release);
    }

    static bool waitUntilDocumentInProgress(Application& application,
                                            const std::string& documentName,
                                            std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(application._recomputeMutex);
        return application._recomputeStateChanged.wait_for(
            lock,
            timeout,
            [&application, &documentName] {
                return application._recomputeDocumentActivityCounts.contains(documentName);
            });
    }

    static bool documentIsActive(Application& application,
                                 const std::string& documentName)
    {
        std::lock_guard lock(application._recomputeMutex);
        return application._recomputeDocumentActivityCounts.contains(documentName);
    }
};

}  // namespace App::Internal

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

TEST_F(AsyncRecomputeTest, CloseDocumentWaitsForInFlightAsyncRecompute)
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

    EXPECT_EQ(closeFuture.wait_for(50ms), std::future_status::timeout);

    App::FeatureTestAsyncBlocker::releaseBlocker();

    ASSERT_EQ(closeFuture.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(closeFuture.get());

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

TEST_F(AsyncRecomputeTest, InlineRecomputeObserverCloseRejectsWithoutSelfWait)
{
    auto* object = dynamic_cast<App::FeatureTestAttribute*>(
        _doc->addObject("App::FeatureTestAttribute", "InlineObserverFeature")
    );
    ASSERT_NE(object, nullptr);
    object->touch();

    bool observerRan = false;
    bool closeResult = true;
    auto connection = _doc->signalBeforeRecompute.connect(
        [&](const App::Document& recomputing) {
            if (&recomputing == _doc) {
                observerRan = true;
                closeResult =
                    App::GetApplication().closeDocument(_docName.c_str());
            }
        });

    App::GetApplication().queueRecomputeRequest(
        App::RecomputeRequest::fromDocument(*_doc));

    EXPECT_TRUE(observerRan);
    EXPECT_FALSE(closeResult);
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}

TEST_F(AsyncRecomputeTest, OwnerCloseRejectsQueuedOwnerThreadRecompute)
{
    BlockingRecomputeDispatcher dispatcher;
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "OwnerDispatchFeature")
    );
    ASSERT_NE(object, nullptr);
    object->touch();
    auto completionPromise = std::make_shared<std::promise<void>>();
    auto recompute = completionPromise->get_future();
    auto request = App::RecomputeRequest::fromDocument(*_doc);
    request.callback = [completionPromise](App::RecomputeRequest&,
                                             App::RecomputeResult&) {
        completionPromise->set_value();
    };
    App::GetApplication().queueRecomputeRequest(std::move(request));
    ASSERT_TRUE(dispatcher.waitUntilQueued());

    EXPECT_FALSE(App::GetApplication().closeDocument(_docName.c_str()));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (recompute.wait_for(0ms) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(dispatcher.runOneFor(100ms));
    }
    ASSERT_EQ(recompute.wait_for(0ms), std::future_status::ready);
    recompute.get();
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}

TEST_F(AsyncRecomputeTest, WorkerCompletionCallbackRetainsActivityAndCannotCloseItsDocument)
{
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "CallbackCloseFeature")
    );
    ASSERT_NE(object, nullptr);

    struct CallbackResult
    {
        bool markerRetained {false};
        bool closed {false};
    };
    auto closeResult = std::make_shared<std::promise<CallbackResult>>();
    auto closeFuture = closeResult->get_future();
    auto request = App::RecomputeRequest::fromDocumentObject(*object);
    request.callback = [closeResult](App::RecomputeRequest& completed,
                                    App::RecomputeResult&) {
        CallbackResult result;
        result.markerRetained =
            App::Internal::AsyncRecomputeTestAccess::documentIsActive(
                App::GetApplication(), completed.documentName);
        if (result.markerRetained) {
            result.closed =
                App::GetApplication().closeDocument(completed.documentName.c_str());
        }
        closeResult->set_value(result);
    };

    App::GetApplication().queueRecomputeRequest(std::move(request));

    ASSERT_EQ(closeFuture.wait_for(2s), std::future_status::ready);
    const auto result = closeFuture.get();
    EXPECT_TRUE(result.markerRetained);
    EXPECT_FALSE(result.closed);
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}

TEST_F(AsyncRecomputeTest, InlineCompletionCallbackRetainsActivityAndCannotCloseItsDocument)
{
    auto* object = dynamic_cast<App::FeatureTestAttribute*>(
        _doc->addObject("App::FeatureTestAttribute", "InlineCallbackFeature")
    );
    ASSERT_NE(object, nullptr);
    object->touch();

    bool callbackRan = false;
    bool markerRetained = false;
    bool closeResult = true;
    auto request = App::RecomputeRequest::fromDocument(*_doc);
    request.callback = [&](App::RecomputeRequest& completed, App::RecomputeResult&) {
        callbackRan = true;
        markerRetained = App::Internal::AsyncRecomputeTestAccess::documentIsActive(
            App::GetApplication(), completed.documentName);
        closeResult = App::GetApplication().closeDocument(completed.documentName.c_str());
    };

    App::GetApplication().queueRecomputeRequest(std::move(request));

    EXPECT_TRUE(callbackRan);
    EXPECT_TRUE(markerRetained);
    EXPECT_FALSE(closeResult);
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}

TEST_F(AsyncRecomputeTest, CloseDrainsRejectedRecomputeCallbackBeforeDeletion)
{
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "RejectedCallbackFeature")
    );
    ASSERT_NE(object, nullptr);

    RecomputeCloseHookBarrier closeAdmission;
    App::Internal::AsyncRecomputeTestAccess::setPostClosingAdmissionHook(
        &RecomputeCloseHookBarrier::invoke);
    auto close = std::async(std::launch::async, [&] {
        return App::GetApplication().closeDocument(_docName.c_str());
    });
    const bool closeAdmitted = closeAdmission.waitUntilEntered();

    std::promise<void> callbackEnteredPromise;
    auto callbackEntered = callbackEnteredPromise.get_future();
    std::promise<void> releaseCallbackPromise;
    auto releaseCallback = releaseCallbackPromise.get_future().share();
    bool callbackRetainedActivity = false;
    bool callbackCloseResult = true;
    auto rejection = std::async(std::launch::async, [&] {
        auto request = App::RecomputeRequest::fromDocumentObject(*object);
        request.callback = [&](App::RecomputeRequest& completed,
                               App::RecomputeResult& result) {
            callbackRetainedActivity =
                App::Internal::AsyncRecomputeTestAccess::documentIsActive(
                    App::GetApplication(), completed.documentName);
            callbackCloseResult =
                App::GetApplication().closeDocument(completed.documentName.c_str());
            EXPECT_FALSE(result.success);
            callbackEnteredPromise.set_value();
            releaseCallback.wait();
        };
        App::GetApplication().queueRecomputeRequest(std::move(request));
    });
    const bool callbackEnteredInTime =
        callbackEntered.wait_for(2s) == std::future_status::ready;

    closeAdmission.release();
    App::Internal::AsyncRecomputeTestAccess::setPostClosingAdmissionHook(nullptr);
    EXPECT_EQ(close.wait_for(50ms), std::future_status::timeout);
    releaseCallbackPromise.set_value();

    EXPECT_TRUE(closeAdmitted);
    EXPECT_TRUE(callbackEnteredInTime);
    ASSERT_EQ(rejection.wait_for(2s), std::future_status::ready);
    rejection.get();
    EXPECT_TRUE(callbackRetainedActivity);
    EXPECT_FALSE(callbackCloseResult);
    ASSERT_EQ(close.wait_for(2s), std::future_status::ready);
    const bool closed = close.get();
    EXPECT_TRUE(closed);
    if (closed) {
        _doc = nullptr;
    }
}

TEST_F(AsyncRecomputeTest, RecomputeAdmissionIsSealedWhileDocumentCloseIsSignalling)
{
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "CloseAdmissionFeature")
    );
    ASSERT_NE(object, nullptr);

    bool closeSignalRan = false;
    bool callbackRan = false;
    bool callbackSucceeded = true;
    auto connection = App::GetApplication().signalDeleteDocument.connect(
        [&](const App::Document& closing) {
            if (&closing != _doc) {
                return;
            }
            closeSignalRan = true;
            auto request = App::RecomputeRequest::fromDocumentObject(*object);
            request.callback = [&](App::RecomputeRequest&, App::RecomputeResult& result) {
                callbackRan = true;
                callbackSucceeded = result.success;
            };
            App::GetApplication().queueRecomputeRequest(std::move(request));
        });

    const bool closed = App::GetApplication().closeDocument(_docName.c_str());
    connection.disconnect();

    EXPECT_TRUE(closed);
    EXPECT_TRUE(closeSignalRan);
    EXPECT_FALSE(callbackRan);
    EXPECT_TRUE(callbackSucceeded);
    if (closed) {
        _doc = nullptr;
    }
}

TEST_F(AsyncRecomputeTest, DelayedRequestCannotTargetReplacementDocumentWithSameName)
{
    auto* oldObject = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "ReusedObjectName")
    );
    ASSERT_NE(oldObject, nullptr);
    auto staleRequest = App::RecomputeRequest::fromDocumentObject(*oldObject);
    const auto oldInstance = staleRequest.documentInstanceId;

    ASSERT_TRUE(App::GetApplication().closeDocument(_docName.c_str()));
    _doc = App::GetApplication().newDocument(_docName.c_str(), "replacement");
    auto* replacementObject = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "ReusedObjectName")
    );
    ASSERT_NE(replacementObject, nullptr);
    EXPECT_NE(_doc->collaborationIdentity().instanceId, oldInstance);

    auto completion = std::make_shared<std::promise<bool>>();
    auto completed = completion->get_future();
    staleRequest.callback = [completion](App::RecomputeRequest&,
                                         App::RecomputeResult& result) {
        completion->set_value(!result.success
                              && result.failure == App::RecomputeFailure::Exception);
    };
    App::GetApplication().queueRecomputeRequest(std::move(staleRequest));

    ASSERT_EQ(completed.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(completed.get());
}

TEST_F(AsyncRecomputeTest, CommitObserverCloseDoesNotWaitForQueuedRecompute)
{
    auto* object = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "ObserverCloseFeature")
    );
    ASSERT_NE(object, nullptr);
    _doc->recompute();

    const auto session = _doc->collaborationService().beginEditSession("observer-close");
    App::CollaborativeOperationIntent intent;
    intent.operationType = std::string(App::CollaborativeSetPropertyOperationType);
    intent.arguments = {{"object", object->getNameInDocument()},
                        {"property", "Integer"},
                        {"value_type", "integer"},
                        {"value", "42"}};
    auto prepared = _doc->collaborationService().prepareEdit(session.sessionId(),
                                                              "observer-close",
                                                              intent,
                                                              "async-recompute-test");

    std::promise<bool> closeResultPromise;
    auto closeResultFuture = closeResultPromise.get_future();
    std::optional<std::thread> closeThread;
    bool observerRan = false;
    bool recomputeWasInProgress = false;
    bool closeCompletedInsideObserver = false;
    std::optional<bool> closeResult;
    auto connection = _doc->signalCommitTransaction.connect([&](const App::Document&) {
        observerRan = true;
        App::GetApplication().queueRecomputeRequest(
            App::RecomputeRequest::fromDocumentObject(*object));
        recomputeWasInProgress =
            App::Internal::AsyncRecomputeTestAccess::waitUntilDocumentInProgress(
                App::GetApplication(), _docName, 2s);
        if (!recomputeWasInProgress) {
            return;
        }

        // Run close on a separate thread so a regression reports a bounded
        // observer failure and can unwind the commit instead of hanging the
        // entire test process. It still begins at the observer boundary while
        // the recompute is waiting on this document's serialization mutex.
        closeThread.emplace([&] {
            closeResultPromise.set_value(
                App::GetApplication().closeDocument(_docName.c_str()));
        });
        closeCompletedInsideObserver =
            closeResultFuture.wait_for(1s) == std::future_status::ready;
        if (closeCompletedInsideObserver) {
            closeResult = closeResultFuture.get();
        }
    });

    const auto commitResult =
        _doc->collaborationService().commitEdit(session.sessionId(), prepared);
    connection.disconnect();

    EXPECT_TRUE(commitResult.committed());
    EXPECT_TRUE(observerRan);
    EXPECT_TRUE(recomputeWasInProgress);
    EXPECT_TRUE(closeCompletedInsideObserver);
    ASSERT_TRUE(closeThread.has_value());
    if (!closeResult) {
        ASSERT_EQ(closeResultFuture.wait_for(2s), std::future_status::ready);
        closeResult = closeResultFuture.get();
    }
    closeThread->join();
    EXPECT_FALSE(*closeResult);
    if (*closeResult) {
        _doc = nullptr;
    }
}
