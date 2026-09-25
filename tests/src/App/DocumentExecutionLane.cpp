// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentExecutionStall.h>
#include <App/DocumentHandle.h>
#include <App/DocumentExecutionTelemetry.h>
#include <App/FeatureTest.h>
#include <App/RecomputeHandle.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace
{

constexpr std::string_view LaneBlockingRecomputeOperationType =
    "FreeCAD.Tests.DocumentExecutionLaneBlockingRecompute";

class LaneBlockingRecomputeState
{
public:
    bool block(const std::stop_token stopToken)
    {
        static_cast<void>(stopToken);
        {
            std::lock_guard lock(_mutex);
            _started = true;
        }
        _changed.notify_all();
        const auto result = App::DocumentExecutionStall::run(_stopSource.get_token(), 30s);
        return !result.completed;
    }

    bool waitUntilStarted(const std::chrono::milliseconds timeout = 5s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return _started; });
    }

    void release()
    {
        _stopSource.request_stop();
    }

    void reset()
    {
        _stopSource = std::stop_source {};
        std::lock_guard lock(_mutex);
        _started = false;
    }

private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::stop_source _stopSource;
    bool _started {false};
};

class LaneBlockingRecomputeStore
{
public:
    static void add(const std::string& token,
                    const std::shared_ptr<LaneBlockingRecomputeState>& state)
    {
        std::lock_guard lock(Mutex);
        States[token] = state;
    }

    static void remove(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        States.erase(token);
    }

    static std::shared_ptr<LaneBlockingRecomputeState> get(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        const auto found = States.find(token);
        return found == States.end() ? nullptr : found->second.lock();
    }

private:
    static inline std::mutex Mutex;
    static inline std::map<std::string, std::weak_ptr<LaneBlockingRecomputeState>> States;
};

class LaneBlockingRecomputeOperation final: public App::CollaborativeOperation
{
public:
    std::string_view typeId() const noexcept override
    {
        return LaneBlockingRecomputeOperationType;
    }

    void apply(App::Document&) const override
    {}

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document&) const override
    {
        return {true, {}};
    }
};

void ensureLaneBlockingRecomputeAdapterRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(LaneBlockingRecomputeOperationType),
            [](const App::Document&,
               const App::CollaborativeOperationIntent& intent) {
                if (intent.arguments.size() != 1 || !intent.arguments.contains("token")) {
                    throw std::invalid_argument("invalid lane blocking recompute intent");
                }
                auto state = LaneBlockingRecomputeStore::get(intent.arguments.at("token"));
                if (!state) {
                    throw std::invalid_argument("unknown lane blocking recompute state");
                }
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [state = std::move(state)](const std::stop_token stopToken) {
                        if (!state->block(stopToken)) {
                            throw std::runtime_error("lane blocking recompute was cancelled");
                        }
                        return std::make_unique<const LaneBlockingRecomputeOperation>();
                    };
                return App::CollaborativeOperationPreparation {
                    {},
                    {},
                    {},
                    std::move(task),
                    App::PreparationPolicy::DetachedInProcess};
            }));
    });
}

class DocumentExecutionLaneTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureLaneBlockingRecomputeAdapterRegistered();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("lane-test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _blockingToken = _docName + "-lane-stall";
        _blocking = std::make_shared<LaneBlockingRecomputeState>();
        LaneBlockingRecomputeStore::add(_blockingToken, _blocking);
    }

    void TearDown() override
    {
        if (_blocking) {
            _blocking->release();
        }
        LaneBlockingRecomputeStore::remove(_blockingToken);
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc() const noexcept
    {
        return _doc;
    }

    App::DocumentCommand makeRecomputeCommand(const std::string& coalescingKey,
                                              int options = 0) const
    {
        App::DocumentCommand command;
        command.kind = App::DocumentCommandKind::Recompute;
        command.document = doc()->executionHandle().identity();
        command.recompute = App::DocumentCommandRecomputePayload {};
        command.recompute->coalescingKey = coalescingKey;
        command.recompute->options = options;
        return command;
    }

    App::DocumentCommand makeLaneBlockingRecomputeCommand() const
    {
        return makeRecomputeCommand(std::string("lane-stall:") + _blockingToken);
    }

    App::DocumentCommand makeKindCommand(App::DocumentCommandKind kind) const
    {
        App::DocumentCommand command;
        command.kind = kind;
        command.document = doc()->executionHandle().identity();
        return command;
    }

    static bool waitForTerminal(App::DocumentCommandHandle& commandHandle,
                                const std::chrono::milliseconds timeout = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        App::DocumentCommandSnapshot snapshot;
        do {
            snapshot = commandHandle.status();
            if (snapshot.terminal()) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        return snapshot.terminal();
    }

private:
    std::string _docName;
    std::string _blockingToken;
    App::Document* _doc {};
    std::shared_ptr<LaneBlockingRecomputeState> _blocking;
};

TEST_F(DocumentExecutionLaneTest, OwnerThreadIsLaneThread)
{
    const auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_FALSE(doc()->isCollaborationOwnerThread());
    lane->dispatchToOwner([&] {
        EXPECT_TRUE(doc()->isCollaborationOwnerThread());
        EXPECT_EQ(std::this_thread::get_id(), lane->ownerThreadId());
    });
}

TEST_F(DocumentExecutionLaneTest, TrySubmitAcceptsRecompute)
{
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("lane-test-recompute"));
    EXPECT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_NE(outcome.commandId, 0U);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    App::DocumentCommandSnapshot snapshot;
    do {
        snapshot = commandHandle.status();
        if (snapshot.terminal()) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);

    EXPECT_TRUE(snapshot.terminal());
}

TEST_F(DocumentExecutionLaneTest, IdenticalRecomputeSharesHandle)
{
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(makeRecomputeCommand("shared-recompute"));
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);

    const auto second = handle.trySubmit(makeRecomputeCommand("shared-recompute"));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_EQ(second.commandId, first.commandId);
}

TEST_F(DocumentExecutionLaneTest, RecomputeWithSameKeyAndOptionsCoalesces)
{
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(
        makeRecomputeCommand("shared-options-recompute", App::Document::DepNoCycle));
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);

    const auto second = handle.trySubmit(
        makeRecomputeCommand("shared-options-recompute", App::Document::DepNoCycle));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_EQ(second.commandId, first.commandId);
}

TEST_F(DocumentExecutionLaneTest, RecomputeWithSameKeyButDifferentOptionsReturnsBusy)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "OptionsCoalesceBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(makeRecomputeCommand("shared-key-options"));
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);

    const auto second = handle.trySubmit(
        makeRecomputeCommand("shared-key-options", App::Document::DepNoCycle));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Busy);
    EXPECT_NE(second.commandId, first.commandId);
}

TEST_F(DocumentExecutionLaneTest, EditReturnsBusyWhileRecomputeActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeRecomputeCommand("busy-edit-test"));
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommand edit;
    edit.kind = App::DocumentCommandKind::Edit;
    edit.document = handle.identity();
    const auto busy = handle.trySubmit(std::move(edit));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
}

TEST_F(DocumentExecutionLaneTest, StatusObservationReturnsStableIdentity)
{
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("status-observation"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto first = commandHandle.status();
    const auto second = commandHandle.status();
    EXPECT_EQ(first.id, second.id);
    EXPECT_EQ(first.document, second.document);
    EXPECT_NE(first.state, App::DocumentCommandState::Failed);
}

TEST_F(DocumentExecutionLaneTest, CancelRemainsCallableWhileActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneCancel"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("cancel-test"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("test cancellation"));
    const auto snapshot = commandHandle.status();
    EXPECT_TRUE(snapshot.state == App::DocumentCommandState::Cancelling
                || snapshot.state == App::DocumentCommandState::Cancelled
                || snapshot.state == App::DocumentCommandState::Failed);
}

TEST_F(DocumentExecutionLaneTest, TrySubmitAcceptsUndoWhenIdle)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneUndo"));
    ASSERT_NE(feature, nullptr);

    doc()->openTransaction("lane-undo-first");
    feature->Integer.setValue(10);
    doc()->commitTransaction();
    doc()->openTransaction("lane-undo-second");
    feature->Integer.setValue(20);
    doc()->commitTransaction();
    ASSERT_EQ(feature->Integer.getValue(), 20);

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Undo));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle));
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Completed);
    EXPECT_EQ(feature->Integer.getValue(), 10);
}

TEST_F(DocumentExecutionLaneTest, TrySubmitAcceptsSaveWhenIdle)
{
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Save));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle));
    const auto snapshot = commandHandle.status();
    EXPECT_TRUE(snapshot.terminal());
    EXPECT_NE(snapshot.state, App::DocumentCommandState::Running);
}

TEST_F(DocumentExecutionLaneTest, UndoReturnsBusyWhileRecomputeActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneUndoBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeRecomputeCommand("busy-undo-test"));
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);

    const auto busy = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Undo));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
}

TEST_F(DocumentExecutionLaneTest, SaveReturnsBusyWhileRecomputeActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneSaveBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeRecomputeCommand("busy-save-test"));
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);

    const auto busy = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Save));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleStatusDoesNotBlockNonOwner)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneStatus"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("status-non-owner"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    App::DocumentRecomputeId recomputeId {0};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (recomputeId == 0 && std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = commandHandle.status();
        if (snapshot.recompute) {
            recomputeId = snapshot.recompute->id;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_NE(recomputeId, 0U);

    std::atomic<bool> statusReturned {false};
    std::jthread observer([&] {
        App::RecomputeHandle recomputeHandle(*doc(), recomputeId);
        static_cast<void>(recomputeHandle.status());
        statusReturned.store(true, std::memory_order_release);
    });

    const auto statusDeadline = std::chrono::steady_clock::now() + 200ms;
    while (!statusReturned.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < statusDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(statusReturned.load(std::memory_order_acquire));
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleWaitDoesNotDeadlockNonOwner)
{
    _blocking->reset();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted(5s));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    App::DocumentRecomputeId recomputeId {0};
    const auto idDeadline = std::chrono::steady_clock::now() + 2s;
    while (recomputeId == 0 && std::chrono::steady_clock::now() < idDeadline) {
        const auto snapshot = commandHandle.status();
        if (snapshot.recompute) {
            recomputeId = snapshot.recompute->id;
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_NE(recomputeId, 0U);

    std::atomic<bool> waitReturned {false};
    std::jthread waiter([&] {
        App::RecomputeHandle recomputeHandle(*doc(), recomputeId);
        static_cast<void>(recomputeHandle.wait(500ms));
        waitReturned.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!waitReturned.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(waitReturned.load(std::memory_order_acquire));

    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, StalledStatePersistsWithoutProgress)
{
    _blocking->reset();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted(5s));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto stallDelay = std::chrono::milliseconds(
        App::DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds + 500);
    std::this_thread::sleep_for(stallDelay);

    const auto stalledSnapshot = commandHandle.status();
    EXPECT_EQ(stalledSnapshot.state, App::DocumentCommandState::Stalled);

    const auto observeDeadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < observeDeadline) {
        EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Stalled);
        std::this_thread::sleep_for(100ms);
    }

    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, RecomputePassesOptionsToAsync)
{
    auto* objectA = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "OptionsCycleA"));
    auto* objectB = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "OptionsCycleB"));
    ASSERT_NE(objectA, nullptr);
    ASSERT_NE(objectB, nullptr);
    objectA->Link.setValue(objectB);
    objectB->Link.setValue(objectA);
    objectA->touch();
    objectB->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(
        makeRecomputeCommand("force;options=4;", App::Document::DepNoCycle));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle));
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Completed);
}

TEST_F(DocumentExecutionLaneTest, CloseDocumentReturnsFalseWhileBusy)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneCloseBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("close-busy"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    EXPECT_FALSE(App::GetApplication().closeDocument(_docName.c_str()));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("allow teardown"));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!doc()->executionLane()->isIdle()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(doc()->executionLane()->isIdle());
}

}  // namespace
