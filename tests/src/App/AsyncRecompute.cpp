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
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/scope_exit.hpp>
#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborativeSetPropertyOperation.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/FeatureTest.h"
#include "Base/Parameter.h"
#include <src/App/InitApplication.h>

using namespace std::chrono_literals;

namespace
{

class ScopedBoolPreference
{
public:
    ScopedBoolPreference(ParameterGrp::handle group,
                         std::string name,
                         const bool value)
        : _group(std::move(group))
        , _name(std::move(name))
    {
        for (const auto& [key, existing] : _group->GetBoolMap()) {
            if (key == _name) {
                _wasPresent = true;
                _previous = existing;
                break;
            }
        }
        _group->SetBool(_name.c_str(), value);
    }

    ~ScopedBoolPreference()
    {
        if (_wasPresent) {
            _group->SetBool(_name.c_str(), _previous);
        }
        else {
            _group->RemoveBool(_name.c_str());
        }
    }

private:
    ParameterGrp::handle _group;
    std::string _name;
    bool _wasPresent {false};
    bool _previous {false};
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

TEST_F(AsyncRecomputeTest, ProcessLocalBlockerIsRejectedAndDocumentClosesPromptly)
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

    bool callbackRan = false;
    bool callbackSucceeded = true;
    App::RecomputeFailure failure = App::RecomputeFailure::None;
    auto request = App::RecomputeRequest::fromDocumentObject(*object);
    request.callback = [&](App::RecomputeRequest&, App::RecomputeResult& result) {
        callbackRan = true;
        callbackSucceeded = result.success;
        failure = result.failure;
    };

    const auto started = std::chrono::steady_clock::now();
    App::GetApplication().queueRecomputeRequest(std::move(request));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
    EXPECT_TRUE(callbackRan);
    EXPECT_FALSE(callbackSucceeded);
    EXPECT_EQ(failure, App::RecomputeFailure::Exception);
    EXPECT_FALSE(App::FeatureTestAsyncBlocker::waitUntilStarted(50ms));

    const auto closeStarted = std::chrono::steady_clock::now();
    const bool closed = App::GetApplication().closeDocument(_docName.c_str());
    EXPECT_LT(std::chrono::steady_clock::now() - closeStarted, 2s);
    EXPECT_TRUE(closed);
    if (closed) {
        _doc = nullptr;
    }
}

TEST_F(AsyncRecomputeTest, WorkerSafetyIsCheckedFromRequest)
{
    auto* safeObject = dynamic_cast<App::FeatureTest*>(
        _doc->addObject("App::FeatureTest", "SafeFeature")
    );
    auto* unsafeObject = dynamic_cast<App::FeatureTestAttribute*>(
        _doc->addObject("App::FeatureTestAttribute", "UnsafeFeature")
    );
    auto* processLocalBlocker = dynamic_cast<App::FeatureTestAsyncBlocker*>(
        _doc->addObject("App::FeatureTestAsyncBlocker", "ProcessLocalBlocker")
    );

    ASSERT_NE(safeObject, nullptr);
    ASSERT_NE(unsafeObject, nullptr);
    ASSERT_NE(processLocalBlocker, nullptr);

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
        App::GetApplication().canRecomputeRequestOnWorker(
            App::RecomputeRequest::fromDocumentObject(*processLocalBlocker)
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

TEST_F(AsyncRecomputeTest, DisabledAsyncSwitchRunsCoordinatorFacadeSynchronously)
{
    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference disableAsync(preferences, "EnableAsyncRecompute", false);

    auto* object = dynamic_cast<App::FeatureTestColumn*>(
        _doc->addObject("App::FeatureTestColumn", "SynchronousCoordinatorFeature")
    );
    ASSERT_NE(object, nullptr);
    object->Column.setValue("E");
    object->touch();
    ASSERT_TRUE(App::GetApplication().canRecomputeRequestOnWorker(
        App::RecomputeRequest::fromDocumentObject(*object)));

    bool callbackRan = false;
    bool callbackSucceeded = false;
    std::thread::id callbackThread;
    const auto ownerThread = std::this_thread::get_id();
    auto request = App::RecomputeRequest::fromDocumentObject(*object);
    request.callback = [&](App::RecomputeRequest&, App::RecomputeResult& result) {
        callbackRan = true;
        callbackSucceeded = result.success;
        callbackThread = std::this_thread::get_id();
    };
    App::GetApplication().queueRecomputeRequest(std::move(request));

    EXPECT_TRUE(callbackRan);
    EXPECT_TRUE(callbackSucceeded);
    EXPECT_EQ(callbackThread, ownerThread);
    EXPECT_EQ(object->Value.getValue(), 4);
    EXPECT_FALSE(object->mustRecompute());
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}

TEST_F(AsyncRecomputeTest, CoordinatorCallbackRetainsActivityAndCannotCloseItsDocument)
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

TEST_F(AsyncRecomputeTest, CommitObserverQueueReturnsStructuredReplayRejectionWithoutDeadlock)
{
    auto* object = dynamic_cast<App::FeatureTestColumn*>(
        _doc->addObject("App::FeatureTestColumn", "ObserverColumnFeature")
    );
    ASSERT_NE(object, nullptr);
    object->Column.setValue("A");
    _doc->recompute();
    ASSERT_EQ(object->Column.getStrValue(), "A");
    ASSERT_EQ(object->Value.getValue(), 0);

    const auto session = _doc->collaborationService().beginEditSession("observer-close");
    App::CollaborativeOperationIntent intent;
    intent.operationType = std::string(App::CollaborativeSetPropertyOperationType);
    intent.arguments = {{"object", object->getNameInDocument()},
                        {"property", "Column"},
                        {"value_type", "string"},
                        {"value", "E"}};
    auto prepared = _doc->collaborationService().prepareEdit(session.sessionId(),
                                                              "observer-close",
                                                              intent,
                                                              "async-recompute-test");
    const auto undosBefore = _doc->getAvailableUndos();

    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ScopedBoolPreference disableAsync(preferences, "EnableAsyncRecompute", false);

    bool observerRan = false;
    bool callbackRan = false;
    bool callbackSucceeded = true;
    App::RecomputeFailure callbackFailure = App::RecomputeFailure::None;
    bool callbackExceptionPresent = false;
    std::string callbackDiagnostic;
    bool callbackRetainedActivity = false;
    bool activityClearedBeforeReturn = false;
    std::thread::id callbackThread;
    std::chrono::steady_clock::duration queueDuration {};
    const auto ownerThread = std::this_thread::get_id();
    auto connection = _doc->signalCommitTransaction.connect([&](const App::Document&) {
        observerRan = true;
        auto request = App::RecomputeRequest::fromDocumentObject(*object);
        request.callback = [&](App::RecomputeRequest& completed,
                               App::RecomputeResult& result) {
            callbackRan = true;
            callbackSucceeded = result.success;
            callbackFailure = result.failure;
            callbackExceptionPresent = result.exception != nullptr;
            if (result.exception) {
                callbackDiagnostic = result.exception->what();
            }
            callbackThread = std::this_thread::get_id();
            callbackRetainedActivity =
                App::Internal::AsyncRecomputeTestAccess::documentIsActive(
                    App::GetApplication(), completed.documentName);
        };
        const auto queueStarted = std::chrono::steady_clock::now();
        App::GetApplication().queueRecomputeRequest(std::move(request));
        queueDuration = std::chrono::steady_clock::now() - queueStarted;
        activityClearedBeforeReturn =
            !App::Internal::AsyncRecomputeTestAccess::documentIsActive(
                App::GetApplication(), _docName);
    });

    const auto commitStarted = std::chrono::steady_clock::now();
    const auto commitResult =
        _doc->collaborationService().commitEdit(session.sessionId(), prepared);
    const auto commitDuration = std::chrono::steady_clock::now() - commitStarted;
    connection.disconnect();

    EXPECT_TRUE(commitResult.committed())
        << App::documentCommitStatusName(commitResult.status) << ": "
        << commitResult.message;
    EXPECT_TRUE(observerRan);
    EXPECT_TRUE(callbackRan);
    EXPECT_FALSE(callbackSucceeded);
    EXPECT_EQ(callbackFailure, App::RecomputeFailure::Exception);
    EXPECT_TRUE(callbackExceptionPresent);
    EXPECT_NE(callbackDiagnostic.find("document object recompute did not complete successfully"),
              std::string::npos)
        << callbackDiagnostic;
    EXPECT_TRUE(callbackRetainedActivity);
    EXPECT_TRUE(activityClearedBeforeReturn);
    EXPECT_EQ(callbackThread, ownerThread);
    EXPECT_LT(queueDuration, 2s);
    EXPECT_LT(commitDuration, 2s);
    EXPECT_EQ(object->Column.getStrValue(), "E");
    EXPECT_EQ(object->Value.getValue(), 4);
    EXPECT_EQ(_doc->getAvailableUndos(), undosBefore + 1)
        << "the set-property intent and its derived recompute share one user undo entry";
    EXPECT_EQ(App::GetApplication().getDocument(_docName.c_str()), _doc);
}
