// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentExecutionStall.h>
#include <App/DocumentHandle.h>
#include <App/DocumentPythonCommand.h>
#include <App/DocumentWouldBlock.h>
#include <App/DocumentExecutionTelemetry.h>
#include <App/FeatureTest.h>
#include <App/MainThreadSignal.h>
#include <App/PropertyStandard.h>
#include <App/RecomputeHandle.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <Base/Interpreter.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <deque>
#include <future>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;

namespace App::Internal
{

class DocumentExecutionLaneTestAccess
{
public:
    static bool isCollaborationOwnerThread(const Document& document) noexcept
    {
        return document.isCollaborationOwnerThread();
    }
};

}  // namespace App::Internal

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

void waitForApplicationClosePermitted(App::DocumentExecutionLane* lane,
                                      const std::chrono::milliseconds timeout = 5s)
{
    if (!lane) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!lane->permitsApplicationClose()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
}

void closeDocumentAllowingLaneWait(const char* docName)
{
    auto* document = App::GetApplication().getDocument(docName);
    if (!document) {
        return;
    }
    if (auto* lane = document->executionLane()) {
        waitForApplicationClosePermitted(lane);
        ASSERT_TRUE(lane->permitsApplicationClose())
            << "application close still blocked by lane command or admission";
    }

    const auto close = [&] {
        return App::GetApplication().closeDocument(docName);
    };
    if (App::DocumentWouldBlock::isGuiThread()) {
        std::jthread worker([&] {
            ASSERT_TRUE(close()) << "closeDocument failed after lane became idle";
        });
        worker.join();
    }
    else {
        ASSERT_TRUE(close()) << "closeDocument failed after lane became idle";
    }
}

class GuiCloseDispatcher
{
public:
    GuiCloseDispatcher()
    {
        Active = this;
        App::MainThreadSignalConfig::setHooks(&isMain, &invoke);
    }

    ~GuiCloseDispatcher()
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        Active = nullptr;
    }

    bool runOne(const std::chrono::milliseconds timeout)
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
        {
            App::MainThreadSignalConfig::BlockingInvokeScope scope;
            task->callback();
        }
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

    static bool isMain()
    {
        return Active && std::this_thread::get_id() == Active->_main;
    }

    static void invoke(std::function<void()>&& callback, const bool blocking)
    {
        if (!Active || isMain()) {
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
        if (!blocking) {
            return;
        }
        std::unique_lock lock(task->mutex);
        task->changed.wait(lock, [&] { return task->done; });
    }

    static inline GuiCloseDispatcher* Active {nullptr};
    const std::thread::id _main {std::this_thread::get_id()};
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<Task>> _tasks;
};

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
        closeDocumentAllowingLaneWait(_docName.c_str());
        _doc = nullptr;
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

    App::DocumentCommand makeLaneBlockingRecomputeCommand(int options = 0) const
    {
        return makeRecomputeCommand(std::string("lane-stall:") + _blockingToken, options);
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

    std::string _docName;
    std::string _blockingToken;
    App::Document* _doc {};
    std::shared_ptr<LaneBlockingRecomputeState> _blocking;
};

TEST_F(DocumentExecutionLaneTest, OwnerThreadIsLaneThread)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_FALSE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
        *doc()));
    lane->dispatchToOwner([&] {
        EXPECT_TRUE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
            *doc()));
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
    // An empty document can finish the first recompute before the second
    // trySubmit. Hold that command until this admission has been checked.
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto second = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_EQ(second.commandId, first.commandId);
    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, RecomputeWithSameKeyAndOptionsCoalesces)
{
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(
        makeLaneBlockingRecomputeCommand(App::Document::DepNoCycle));
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto second = handle.trySubmit(
        makeLaneBlockingRecomputeCommand(App::Document::DepNoCycle));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_EQ(second.commandId, first.commandId);
    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, RecomputeWithSameKeyButDifferentOptionsReturnsBusy)
{
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto second = handle.trySubmit(
        makeLaneBlockingRecomputeCommand(App::Document::DepNoCycle));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Busy);
    EXPECT_NE(second.commandId, first.commandId);
    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, EditReturnsBusyWhileRecomputeActive)
{
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    App::DocumentCommand edit;
    edit.kind = App::DocumentCommandKind::Edit;
    edit.document = handle.identity();
    const auto busy = handle.trySubmit(std::move(edit));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
    _blocking->release();
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
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("test cancellation"));
    // The stall ignores the operation stop token; release it so cancellation
    // can reach a terminal state.
    _blocking->release();
    ASSERT_TRUE(waitForTerminal(commandHandle));
    const auto snapshot = commandHandle.status();
    EXPECT_TRUE(snapshot.state == App::DocumentCommandState::Cancelling
                || snapshot.state == App::DocumentCommandState::Cancelled
                || snapshot.state == App::DocumentCommandState::Failed);

    waitForApplicationClosePermitted(doc()->executionLane());
    ASSERT_TRUE(doc()->executionLane()->permitsApplicationClose());
    ASSERT_TRUE(App::GetApplication().closeDocument(_docName.c_str()));
    _doc = nullptr;
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
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto busy = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Undo));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
    _blocking->release();
}

TEST_F(DocumentExecutionLaneTest, SaveReturnsBusyWhileRecomputeActive)
{
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto busy = handle.trySubmit(makeKindCommand(App::DocumentCommandKind::Save));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
    _blocking->release();
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
    const auto snapshot = commandHandle.status();
    ASSERT_TRUE(snapshot.recompute);
    ASSERT_NE(snapshot.recompute->id, 0U);
    const App::DocumentRecomputeId recomputeId =
        static_cast<App::DocumentRecomputeId>(snapshot.recompute->id);

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
    const auto idSnapshot = commandHandle.status();
    ASSERT_TRUE(idSnapshot.recompute);
    ASSERT_NE(idSnapshot.recompute->id, 0U);
    const App::DocumentRecomputeId recomputeId =
        static_cast<App::DocumentRecomputeId>(idSnapshot.recompute->id);

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
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Failed);
}

TEST_F(DocumentExecutionLaneTest, CloseDocumentReturnsFalseWhileBusy)
{
    _blocking->reset();
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());

    EXPECT_FALSE(App::GetApplication().closeDocument(_docName.c_str()));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("allow teardown"));
    _blocking->release();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!doc()->executionLane()->isIdle()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(doc()->executionLane()->isIdle());
}

// ---------------------------------------------------------------------------
// New tests for PR-62 fixes
// ---------------------------------------------------------------------------

// H1: Stalled lane rejects admission immediately (no wait).
TEST_F(DocumentExecutionLaneTest, AdmissionRejectedImmediatelyWhenStalled)
{
    _blocking->reset();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted(5s));

    // Wait until watchdog declares Stalled.
    const auto stallDelay = std::chrono::milliseconds(
        App::DocumentExecutionWatchdogSnapshot::StallThresholdMilliseconds + 1000);
    std::this_thread::sleep_for(stallDelay);

    // Off-GUI-thread: trySubmit should return Busy, not block.
    const auto busyOutcome = handle.trySubmit(makeRecomputeCommand("stall-admission"));
    EXPECT_EQ(busyOutcome.result, App::DocumentCommandSubmitResult::Busy);

    _blocking->release();
}

// H1: Admission succeeds without timeout when the lane becomes free quickly.
TEST_F(DocumentExecutionLaneTest, AdmissionSucceedsBeforeTimeout)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "AdmissionObj"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    // Submit a fast recompute that completes quickly.
    const auto outcome = handle.trySubmit(makeRecomputeCommand("admission-fast"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
    EXPECT_TRUE(commandHandle.status().terminal());
}

// H5/M3: First status() of an admitted Recompute is not terminal Cancelled.
TEST_F(DocumentExecutionLaneTest, AdmittedRecomputeStatusIsNotTerminalOnFirstCheck)
{
    _blocking->reset();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    // The command snapshot must have a recompute id (set at trySubmit).
    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto snap = commandHandle.status();
    ASSERT_TRUE(snap.recompute) << "Recompute snapshot must be set at admission";
    EXPECT_NE(snap.recompute->id, 0U) << "Admission id must be non-zero";
    // State must NOT be terminal (Cancelled/Completed/Failed).
    EXPECT_FALSE(snap.terminal()) << "Admitted command must not be terminal immediately";
    EXPECT_EQ(snap.state, App::DocumentCommandState::Running);

    _blocking->release();
}

// M9: After a successful Close, the handle still reports Completed/Close.
TEST_F(DocumentExecutionLaneTest, CloseHandleStaysCompletedAfterLaneGone)
{
    // Create a separate document so TearDown doesn't double-close.
    const std::string docName =
        App::GetApplication().getUniqueDocumentName("lane-close-m9");
    auto* closeDoc = App::GetApplication().newDocument(docName.c_str(), "testUser");
    ASSERT_NE(closeDoc, nullptr);

    auto handle = closeDoc->executionHandle();
    App::DocumentCommand closeCmd;
    closeCmd.kind = App::DocumentCommandKind::Close;
    closeCmd.document = handle.identity();
    const auto outcome = handle.trySubmit(std::move(closeCmd));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));

    // After lane teardown the handle must still return Completed/Close.
    const auto terminal = commandHandle.status();
    EXPECT_EQ(terminal.state, App::DocumentCommandState::Completed)
        << "Close handle must stay Completed after lane gone, got: "
        << terminal.diagnostic;
    EXPECT_EQ(terminal.kind, App::DocumentCommandKind::Close);
}

// M5: Shutdown drains the dispatch queue and does not hang.
TEST_F(DocumentExecutionLaneTest, ShutdownDoesNotHang)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);

    // Post some dispatch work that will be in the queue.
    std::atomic<int> counter {0};
    for (int i = 0; i < 4; ++i) {
        lane->dispatchToOwner([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    // Request shutdown; lane must drain and exit quickly.
    lane->requestShutdown("test shutdown");

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!lane->shutdownRequested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_TRUE(lane->shutdownRequested());
}

std::string dumpIntegerProperty(const long value)
{
    App::PropertyInteger property;
    property.setValue(value);
    std::ostringstream stream(std::ios::binary);
    property.dumpToStream(stream, 1);
    return stream.str();
}

App::DocumentCommandPropertyValue integerEdit(App::Document& document,
                                              App::DocumentObject& object,
                                              const long value)
{
    App::DocumentCommandPropertyValue propertyValue;
    propertyValue.stableObjectIdentity = document.collaborationObjectIdentity(object);
    propertyValue.propertyName = "Integer";
    propertyValue.copiedValue = dumpIntegerProperty(value);
    return propertyValue;
}

// M2: a real property change followed by an unknown identity rolls back, and a
// successful edit is named "Edit" in the undo list.
TEST_F(DocumentExecutionLaneTest, EditCommandRollsBackOnUnknownObject)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "EditRollback"));
    ASSERT_NE(feature, nullptr);
    feature->Integer.setValue(1);

    auto handle = doc()->executionHandle();
    App::DocumentCommand editCmd;
    editCmd.kind = App::DocumentCommandKind::Edit;
    editCmd.document = handle.identity();
    editCmd.edit = App::DocumentCommandEditPayload {};
    editCmd.edit->operationId = "test-edit-rollback";
    editCmd.edit->propertyValues.push_back(integerEdit(*doc(), *feature, 7));
    App::DocumentCommandPropertyValue unknown;
    unknown.stableObjectIdentity = "nonexistent-stable-id-1234";
    unknown.propertyName = "Integer";
    unknown.copiedValue = dumpIntegerProperty(9);
    editCmd.edit->propertyValues.push_back(std::move(unknown));

    const auto outcome = handle.trySubmit(std::move(editCmd));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Failed);
    EXPECT_EQ(feature->Integer.getValue(), 1);

    App::DocumentCommand goodEdit;
    goodEdit.kind = App::DocumentCommandKind::Edit;
    goodEdit.document = handle.identity();
    goodEdit.edit = App::DocumentCommandEditPayload {};
    goodEdit.edit->operationId = "gui.property-editor";
    goodEdit.edit->propertyValues.push_back(integerEdit(*doc(), *feature, 4));
    const auto goodOutcome = handle.trySubmit(std::move(goodEdit));
    ASSERT_EQ(goodOutcome.result, App::DocumentCommandSubmitResult::Accepted);
    App::DocumentCommandHandle goodHandle(goodOutcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(goodHandle, 5s));
    EXPECT_EQ(goodHandle.status().state, App::DocumentCommandState::Completed);
    EXPECT_EQ(feature->Integer.getValue(), 4);
    const auto undoNames = doc()->getAvailableUndoNames();
    EXPECT_NE(std::find(undoNames.begin(), undoNames.end(), "Edit"), undoNames.end());
    ASSERT_TRUE(doc()->undo());
    EXPECT_EQ(feature->Integer.getValue(), 1);
}

// M10: Canonical saveAsync with no FileName fails with the expected diagnostic.
TEST_F(DocumentExecutionLaneTest, SaveAsyncWithNoFileNameFailsWithDiagnostic)
{
    // doc() should have an empty FileName (not saved yet).
    const char* fn = doc()->FileName.getValue();
    ASSERT_TRUE(!fn || *fn == '\0') << "Test expects an unsaved document";

    auto handle = doc()->executionHandle();
    App::DocumentCommand saveCmd;
    saveCmd.kind = App::DocumentCommandKind::Save;
    saveCmd.document = handle.identity();
    // No save payload = canonical save.

    const auto outcome = handle.trySubmit(std::move(saveCmd));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Failed);
    EXPECT_NE(commandHandle.status().diagnostic.find("no file name"), std::string::npos)
        << "Diagnostic must mention 'no file name', got: "
        << commandHandle.status().diagnostic;
}

// L5: Force is a boolean field on the payload, not encoded in coalescingKey.
TEST_F(DocumentExecutionLaneTest, ForceFieldOnRecomputePayload)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "ForceFieldObj"));
    ASSERT_NE(feature, nullptr);

    auto handle = doc()->executionHandle();
    App::DocumentCommand cmd;
    cmd.kind = App::DocumentCommandKind::Recompute;
    cmd.document = handle.identity();
    cmd.recompute = App::DocumentCommandRecomputePayload {};
    cmd.recompute->coalescingKey = "force-field-test";
    cmd.recompute->force = true;  // L5: use the field, not the key string
    EXPECT_TRUE(cmd.recompute->force);
    EXPECT_EQ(cmd.recompute->coalescingKey.find("force;"), std::string::npos)
        << "L5: force must not be encoded as 'force;' in coalescingKey";

    const auto outcome = handle.trySubmit(std::move(cmd));
    EXPECT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
}

// L6: cancelCommand stores reason in the diagnostic.
TEST_F(DocumentExecutionLaneTest, CancelCommandStoresReason)
{
    _blocking->reset();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted(5s));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const std::string cancelReason = "test cancellation reason L6";
    EXPECT_TRUE(commandHandle.cancel(cancelReason));

    // After cancel the diagnostic should contain the reason.
    const auto snap = commandHandle.status();
    EXPECT_NE(snap.diagnostic.find("test cancellation reason L6"), std::string::npos)
        << "Cancel reason must appear in diagnostic, got: " << snap.diagnostic;

    _blocking->release();
}

// N1: a close marshalled onto a thread that is not the lane owner must succeed
// and must not leave the document behind with a dead lane.
TEST_F(DocumentExecutionLaneTest, GuiMarshalledCloseRemovesDocument)
{
    const std::string name = doc()->getName();
    auto handle = doc()->executionHandle();
    GuiCloseDispatcher dispatcher;
    App::DocumentCommand closeCmd;
    closeCmd.kind = App::DocumentCommandKind::Close;
    closeCmd.document = handle.identity();
    const auto outcome = handle.trySubmit(std::move(closeCmd));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(dispatcher.runOne(5s));
    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Completed);
    EXPECT_EQ(commandHandle.status().kind, App::DocumentCommandKind::Close);
    EXPECT_EQ(App::GetApplication().getDocument(name.c_str()), nullptr);
    _doc = nullptr;
}

// Opening or restoring a document from an RPC worker activates it there.
// signalActiveDocument then ran GUI observers (the tree widget) off the GUI
// thread: "QBasicTimer::start: Timers cannot be started from another thread".
TEST_F(DocumentExecutionLaneTest, ActiveDocumentSignalRunsOnTheMainThread)
{
    GuiCloseDispatcher dispatcher;
    std::thread::id observed;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalActiveDocument.connect(
            [&observed](const App::Document&) { observed = std::this_thread::get_id(); });

    std::jthread worker([this] { App::GetApplication().setActiveDocument(doc()); });

    ASSERT_TRUE(dispatcher.runOne(5s)) << "signalActiveDocument was not marshalled";
    worker.join();
    EXPECT_EQ(observed, std::this_thread::get_id());
}

// N3: wait() round-trips a C++ commit result. A Py::Dict stored in the promise
// aborts on MSVC when the owner assigns it without the GIL.
TEST_F(DocumentExecutionLaneTest, AsyncMutationWaitReturnsCommitDict)
{
    const std::string script = std::string(R"PY(
import FreeCAD as App
doc = App.getDocument(")PY")
        + doc()->getName() + R"PY(")
def cb():
    doc.addObject("App::FeatureTest", "AsyncMade")
handle = doc.commitCompatibilityMutationAsync(cb, structural=True, recompute=False)
result = handle["wait"](10.0)
if not isinstance(result, dict) or not result.get("committed"):
    raise RuntimeError("async wait did not return a committed dict: %r" % (result,))
if doc.getObject("AsyncMade") is None:
    raise RuntimeError("async mutation did not create the object")
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(script.c_str()));
}

// N3: a timed wait() that expires raises TimeoutError. Python 3.12 rejects %g in
// PyErr_Format and raised SystemError, so pollers treated a timeout as failure.
TEST_F(DocumentExecutionLaneTest, AsyncMutationWaitTimeoutRaisesTimeoutError)
{
    const std::string script = std::string(R"PY(
import threading
import FreeCAD as App
doc = App.getDocument(")PY")
        + doc()->getName() + R"PY(")
gate = threading.Event()
def cb():
    gate.wait(10.0)
    doc.addObject("App::FeatureTest", "AsyncAfterTimeout")
handle = doc.commitCompatibilityMutationAsync(cb, structural=True, recompute=False)
try:
    handle["wait"](0.05)
except TimeoutError as exc:
    if "0.05 seconds" not in str(exc):
        raise RuntimeError("unexpected timeout message: %s" % exc)
else:
    raise RuntimeError("timed wait() returned before the mutation finished")
finally:
    gate.set()
result = handle["wait"](10.0)
if not isinstance(result, dict) or not result.get("committed"):
    raise RuntimeError("wait() after a timeout did not commit: %r" % (result,))
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(script.c_str()));
}

// N3: wait() on the thread the hooks call the GUI thread must refuse.
TEST_F(DocumentExecutionLaneTest, AsyncMutationWaitRefusesGuiThread)
{
    struct Hooks
    {
        Hooks()
        {
            App::MainThreadSignalConfig::setHooks(
                [] { return true; },
                [](std::function<void()>&& fn, bool) { fn(); });
        }
        ~Hooks()
        {
            App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        }
    };
    const std::string script = std::string(R"PY(
import FreeCAD as App
doc = App.getDocument(")PY")
        + doc()->getName() + R"PY(")
def cb():
    doc.addObject("App::FeatureTest", "GuiWaitRefused")
handle = doc.commitCompatibilityMutationAsync(cb, structural=True, recompute=False)
refused = False
try:
    handle["wait"]()
except Exception as exc:
    refused = "would block" in str(exc) or type(exc).__name__ == "DocumentWouldBlock"
if not refused:
    raise RuntimeError("GUI-thread wait() was not refused")
)PY";
    {
        Hooks hooks;
        EXPECT_NO_THROW(Base::Interpreter().runString(script.c_str()));
    }
    // The mutation was posted before the refusal. Finish it off the GUI thread
    // so TearDown is not left with a busy lane. The handle lives in Python.
    const std::string finish = R"PY(
result = handle["wait"](10.0)
if not isinstance(result, dict):
    raise RuntimeError("wait() did not return a dict after the GUI refusal")
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(finish.c_str()));
}

// N7: cancel while a Python feature holds the GIL must return. The old
// coordinator took the operation lock from this thread and deadlocked.
TEST_F(DocumentExecutionLaneTest, CancelDuringPythonFeatureReturns)
{
    const std::string script = std::string(R"PY(
import FreeCAD as App
doc = App.getDocument(")PY")
        + doc()->getName() + R"PY(")
class Sleeper:
    def execute(self, obj):
        import time
        time.sleep(0.2)
obj = doc.addObject("App::FeaturePython", "CancelSleeper")
obj.Proxy = Sleeper()
obj.touch()
)PY";
    ASSERT_NO_THROW(Base::Interpreter().runString(script.c_str()));

    std::atomic<bool> holdingGil {false};
    std::atomic<bool> cancelNow {false};
    std::atomic<bool> cancelDone {false};
    std::optional<App::DocumentCommandHandle> commandHandle;
    std::thread canceller([&] {
        Base::PyGILStateLocker gil;
        holdingGil.store(true);
        while (!cancelNow.load()) {
            std::this_thread::sleep_for(5ms);
        }
        if (commandHandle) {
            static_cast<void>(commandHandle->cancel("n7-cancel"));
        }
        cancelDone.store(true);
    });

    const auto holdDeadline = std::chrono::steady_clock::now() + 2s;
    while (!holdingGil.load() && std::chrono::steady_clock::now() < holdDeadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(holdingGil.load());

    auto handle = doc()->executionHandle();
    App::DocumentCommand cmd;
    cmd.kind = App::DocumentCommandKind::Recompute;
    cmd.document = handle.identity();
    cmd.recompute = App::DocumentCommandRecomputePayload {};
    cmd.recompute->coalescingKey = "n7-cancel-sleeper";
    cmd.recompute->force = true;
    const auto outcome = handle.trySubmit(std::move(cmd));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    commandHandle.emplace(outcome.commandId, handle.identity());
    std::this_thread::sleep_for(200ms);
    cancelNow.store(true);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!cancelDone.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (cancelDone.load()) {
        canceller.join();
    }
    else {
        canceller.detach();
    }
    EXPECT_TRUE(cancelDone.load()) << "cancel() did not return; coordinator lock deadlock";
    if (cancelDone.load()) {
        ASSERT_TRUE(waitForTerminal(*commandHandle, 5s));
    }
}

TEST_F(DocumentExecutionLaneTest, NonGuiDispatchRechecksRejectionBeforeQueuePush)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    ASSERT_FALSE(App::DocumentWouldBlock::isGuiThread());

    struct ShutdownBetweenCheckAndPushCallable
    {
        App::DocumentExecutionLane* lane {nullptr};
        std::shared_ptr<std::atomic<bool>> shutdownTriggered {
            std::make_shared<std::atomic<bool>>(false)
        };
        std::shared_ptr<std::atomic<bool>> invoked {std::make_shared<std::atomic<bool>>(false)};

        explicit ShutdownBetweenCheckAndPushCallable(App::DocumentExecutionLane* laneIn)
            : lane(laneIn)
        {}

        ShutdownBetweenCheckAndPushCallable(const ShutdownBetweenCheckAndPushCallable& other)
            : lane(other.lane)
            , shutdownTriggered(other.shutdownTriggered)
            , invoked(other.invoked)
        {
            shutdownAfterInitialCheck();
        }

        ShutdownBetweenCheckAndPushCallable(ShutdownBetweenCheckAndPushCallable&& other) noexcept
            : lane(other.lane)
            , shutdownTriggered(std::move(other.shutdownTriggered))
            , invoked(std::move(other.invoked))
        {
            shutdownAfterInitialCheck();
        }

        void shutdownAfterInitialCheck() const
        {
            if (lane && !shutdownTriggered->exchange(true, std::memory_order_acq_rel)) {
                lane->requestShutdown("lane shut down between initial check and queue push");
                lane->joinThread();
            }
        }

        void operator()() const
        {
            invoked->store(true, std::memory_order_release);
        }
    };

    ShutdownBetweenCheckAndPushCallable callable(lane);
    EXPECT_THROW(lane->dispatchToOwner(callable), App::DocumentWouldBlock);
    EXPECT_TRUE(callable.shutdownTriggered->load(std::memory_order_acquire));
    EXPECT_FALSE(callable.invoked->load(std::memory_order_acquire));
}

TEST_F(DocumentExecutionLaneTest, DetachedLaneSkipsActiveCommandAfterOwnerDocumentDestroyed)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    auto handle = doc()->executionHandle();

    struct GateState
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool ownerInDispatch {false};
        bool releaseOwner {false};
    };
    auto gate = std::make_shared<GateState>();

    auto holdFuture = lane->postToOwner([gate] {
        {
            std::lock_guard lock(gate->mutex);
            gate->ownerInDispatch = true;
        }
        gate->cv.notify_all();
        std::unique_lock lock(gate->mutex);
        gate->cv.wait(lock, [&] { return gate->releaseOwner; });
    });
    {
        std::unique_lock lock(gate->mutex);
        gate->cv.wait(lock, [&] { return gate->ownerInDispatch; });
    }

    const auto outcome = handle.trySubmit(makeRecomputeCommand("active-after-owner-destroyed"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto admittedSnapshot = commandHandle.status();
    ASSERT_TRUE(admittedSnapshot.recompute.has_value());
    const auto admissionId = static_cast<App::DocumentRecomputeId>(admittedSnapshot.recompute->id);
    ASSERT_NE(admissionId, 0U);
    ASSERT_FALSE(doc()->recomputeCoordinator().status(admissionId).has_value());

    lane->markOwnerDocumentDestroyed();
    lane->requestShutdown("document closing");

    {
        std::lock_guard lock(gate->mutex);
        gate->releaseOwner = true;
    }
    gate->cv.notify_all();
    holdFuture.get();
    lane->joinThread();

    EXPECT_FALSE(doc()->recomputeCoordinator().status(admissionId).has_value())
        << "detached lane must not submit active recompute to a destroyed Document";
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Cancelled);
}

TEST_F(DocumentExecutionLaneTest, GuiCloseDocumentSealsLaneAgainstTrySubmitDuringClose)
{
    const std::string closeDocName = App::GetApplication().getUniqueDocumentName("lane-gui-close-seal");
    auto* closeDoc = App::GetApplication().newDocument(closeDocName.c_str(), "testUser");
    ASSERT_NE(closeDoc, nullptr);
    auto* lane = closeDoc->executionLane();
    ASSERT_NE(lane, nullptr);
    auto handle = closeDoc->executionHandle();
    const auto weakLane = std::weak_ptr<App::DocumentExecutionLane>(
        App::DocumentExecutionLane::find(handle.instanceId())
    );
    ASSERT_FALSE(weakLane.expired());

    bool deleteSignalObserved = false;
    App::DocumentCommandSubmitOutcome submitOutcome;
    fastsignals::scoped_connection connection = App::GetApplication().signalDeleteDocument.connect(
        [&](const App::Document& deletedDoc) {
            if (closeDocName != deletedDoc.getName()) {
                return;
            }
            deleteSignalObserved = true;
            App::DocumentCommand cmd;
            cmd.kind = App::DocumentCommandKind::Recompute;
            cmd.document = handle.identity();
            cmd.recompute = App::DocumentCommandRecomputePayload {};
            cmd.recompute->coalescingKey = "submit-during-gui-close";
            cmd.recompute->force = true;
            submitOutcome = handle.trySubmit(std::move(cmd));
        }
    );

    {
        GuiCloseDispatcher dispatcher;
        ASSERT_TRUE(App::DocumentWouldBlock::isGuiThread());
        ASSERT_TRUE(App::GetApplication().closeDocument(closeDocName.c_str()));
    }
    connection.disconnect();

    EXPECT_TRUE(deleteSignalObserved);
    EXPECT_EQ(submitOutcome.result, App::DocumentCommandSubmitResult::Closed)
        << "trySubmit during Application::closeDocument must be rejected as Closed";
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleStatusDoesNotTouchCoordinatorAfterOwnerDestroyed)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);

    App::DocumentRecomputeRequest request;
    request.coalescingKey = "direct-coordinator-recompute-after-owner-destroyed";
    const auto recomputeId = doc()->recomputeCoordinator().submit(std::move(request));
    ASSERT_NE(recomputeId, 0U);

    App::RecomputeHandle handle(*doc(), recomputeId);
    ASSERT_EQ(handle.status().state, App::DocumentRecomputeState::Completed);

    lane->markOwnerDocumentDestroyed();

    ASSERT_FALSE(lane->recomputeStatus(recomputeId).has_value());
    const auto afterDestroyed = handle.status();
    EXPECT_EQ(afterDestroyed.state, App::DocumentRecomputeState::Cancelled)
        << "RecomputeHandle::status must not fall back to owner->recomputeCoordinator() "
           "after lane->recomputeStatus() refused because the owner document is destroyed";
}

TEST_F(DocumentExecutionLaneTest, GuiCloseDocumentRefusesWhilePostToOwnerTaskIsExecuting)
{
    const std::string closeDocName = App::GetApplication().getUniqueDocumentName(
        "lane-gui-close-during-post"
    );
    auto* closeDoc = App::GetApplication().newDocument(closeDocName.c_str(), "testUser");
    ASSERT_NE(closeDoc, nullptr);
    auto* lane = closeDoc->executionLane();
    ASSERT_NE(lane, nullptr);

    struct ActivePostGate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool postExecuting {false};
        bool releasePost {false};
    };
    auto postGate = std::make_shared<ActivePostGate>();

    auto holdFuture = lane->postToOwner([postGate] {
        {
            std::lock_guard lock(postGate->mutex);
            postGate->postExecuting = true;
        }
        postGate->cv.notify_all();
        std::unique_lock lock(postGate->mutex);
        postGate->cv.wait(lock, [&] { return postGate->releasePost; });
    });
    {
        std::unique_lock lock(postGate->mutex);
        postGate->cv.wait(lock, [&] { return postGate->postExecuting; });
    }

    bool closedWhilePostExecuting = false;
    {
        GuiCloseDispatcher dispatcher;
        ASSERT_TRUE(App::DocumentWouldBlock::isGuiThread());
        closedWhilePostExecuting = App::GetApplication().closeDocument(closeDocName.c_str());
    }
    const bool docStillOpenWhileBusy = App::GetApplication().getDocument(closeDocName.c_str())
        == closeDoc;

    {
        std::lock_guard lock(postGate->mutex);
        postGate->releasePost = true;
    }
    postGate->cv.notify_all();
    holdFuture.get();

    EXPECT_FALSE(closedWhilePostExecuting)
        << "GUI closeDocument must not destroy the document and detach the lane while a "
           "postToOwner task is actively executing";
    EXPECT_TRUE(docStillOpenWhileBusy);

    if (App::GetApplication().getDocument(closeDocName.c_str()) != nullptr) {
        GuiCloseDispatcher dispatcher;
        ASSERT_TRUE(App::DocumentWouldBlock::isGuiThread());
        EXPECT_TRUE(App::GetApplication().closeDocument(closeDocName.c_str()));
        EXPECT_EQ(App::GetApplication().getDocument(closeDocName.c_str()), nullptr);
    }
}

TEST_F(DocumentExecutionLaneTest, DrainDispatchQueueSkipsSubsequentTasksAfterReentrantClose)
{
    const std::string closeDocName = App::GetApplication().getUniqueDocumentName(
        "lane-reentrant-close-batch"
    );
    auto* closeDoc = App::GetApplication().newDocument(closeDocName.c_str(), "testUser");
    ASSERT_NE(closeDoc, nullptr);
    auto* lane = closeDoc->executionLane();
    ASSERT_NE(lane, nullptr);
    const auto lanePin = lane->shared_from_this();

    struct BatchBlockerGate
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool blockerEntered {false};
        bool releaseBlocker {false};
    };
    auto batchGate = std::make_shared<BatchBlockerGate>();

    auto blockerFuture = lanePin->postToOwner([batchGate] {
        {
            std::lock_guard lock(batchGate->mutex);
            batchGate->blockerEntered = true;
        }
        batchGate->cv.notify_all();
        std::unique_lock lock(batchGate->mutex);
        batchGate->cv.wait(lock, [&] { return batchGate->releaseBlocker; });
    });
    {
        std::unique_lock lock(batchGate->mutex);
        batchGate->cv.wait(lock, [&] { return batchGate->blockerEntered; });
    }

    auto task1Future = lanePin->postToOwner([closeDocName] {
        return App::GetApplication().closeDocument(closeDocName.c_str());
    });
    std::atomic<bool> task2Ran {false};
    auto task2Future = lanePin->postToOwner([&task2Ran] {
        task2Ran.store(true, std::memory_order_release);
    });

    {
        std::lock_guard lock(batchGate->mutex);
        batchGate->releaseBlocker = true;
    }
    batchGate->cv.notify_all();

    blockerFuture.get();
    EXPECT_TRUE(task1Future.get());
    bool task2Dropped = false;
    try {
        task2Future.get();
    }
    catch (const std::future_error&) {
        task2Dropped = true;
    }
    lanePin->joinThread();

    EXPECT_FALSE(task2Ran.load(std::memory_order_acquire))
        << "drainDispatchQueue must not execute subsequent queued tasks after a task "
           "closes the owning document";
    EXPECT_TRUE(task2Dropped);
    EXPECT_EQ(App::GetApplication().getDocument(closeDocName.c_str()), nullptr);
}

}  // namespace

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST_F(DocumentExecutionLaneTest, RecomputeAdmissionReturnsUsableLegacyHandles)
{
    struct Reset {
        ~Reset() { App::Internal::setCompatibilityReceiptFaultForTesting(
            App::Internal::CompatibilityReceiptFault::None); }
    } reset;
    ASSERT_NE(doc()->addObject("App::FeatureTest", "Receipt3Feature"), nullptr);
    const std::string setup = std::string("import FreeCAD as App\ndoc = App.getDocument(\"")
        + _docName + "\")\nfeature = doc.getObject('Receipt3Feature')\n"
        "assert not doc.RecomputesFrozen\nbefore_count = feature.ExecCount\nfeature.touch()\n";
    ASSERT_NO_THROW(Base::Interpreter().runString(setup.c_str()));
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    for (const auto fault : {App::Internal::CompatibilityReceiptFault::RecomputeHandleAllocation,
                             App::Internal::CompatibilityReceiptFault::RecomputePythonWrapperAllocation,
                             App::Internal::CompatibilityReceiptFault::RecomputeCommandHandleAllocation}) {
        App::Internal::setCompatibilityReceiptFaultForTesting(fault);
        ASSERT_NO_THROW(Base::Interpreter().runString(
            "try:\n    doc.recomputeAsync()\nexcept Exception:\n    pass\n"
            "else:\n    raise AssertionError('allocation fault was not raised')\n"));
        EXPECT_FALSE(App::Internal::consumeCompatibilityReceiptFaultForTesting(fault));
        EXPECT_TRUE(lane->isIdle());
        ASSERT_NO_THROW(Base::Interpreter().runString("assert 'Touched' in feature.State\n"));
    }

    ASSERT_NO_THROW(Base::Interpreter().runString(
        "h = doc.recomputeAsync()\nassert isinstance(h, App.RecomputeHandle)\n"
        "assert h.id() != 0\n"));
    const auto idleDeadline = std::chrono::steady_clock::now() + 30s;
    while (!lane->isIdle() && std::chrono::steady_clock::now() < idleDeadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(lane->isIdle());
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "result = h.wait(30.0)\nassert result['terminal'] and result['state'] == 'completed'\n"
        "assert result['id'] == h.id()\nassert 'Touched' not in feature.State\n"
        "assert feature.ExecCount == before_count + 1\n"));

    // Force the same fallback that an unavailable status produces, then verify
    // that the returned public command handle observes the completed work.
    ASSERT_NO_THROW(Base::Interpreter().runString("feature.touch()\n"));
    App::Internal::setCompatibilityReceiptFaultForTesting(
        App::Internal::CompatibilityReceiptFault::RecomputeReturnFallback);
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "fallback = doc.recomputeAsync()\n"
        "assert type(fallback).__name__ == 'DocumentCommandHandle'\n"
        "fallback_id = fallback.id()\nassert fallback_id != 0\n"));
    unsigned long long id = 0;
    {
        Base::PyGILStateLocker gil;
        auto* module = PyImport_AddModule("__main__");
        ASSERT_NE(module, nullptr);
        auto* value = PyDict_GetItemString(PyModule_GetDict(module), "fallback_id");
        ASSERT_NE(value, nullptr);
        id = PyLong_AsUnsignedLongLong(value);
        ASSERT_FALSE(PyErr_Occurred());
    }
    App::DocumentCommandHandle command(id, doc()->executionHandle().identity());
    ASSERT_TRUE(waitForTerminal(command, 30s));
    ASSERT_EQ(command.status().state, App::DocumentCommandState::Completed);
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "assert fallback.done() and fallback.status()['state'] == 'Completed'\n"
        "assert 'Touched' not in feature.State\n"
        "assert feature.ExecCount == before_count + 2\n"));
}

TEST_F(DocumentExecutionLaneTest, RecomputeBusyDoesNotStartPublicRequest)
{
    auto* blocker = doc()->addObject<App::FeatureTestAsyncBlocker>("Receipt3Blocker");
    ASSERT_NE(blocker, nullptr);
    ASSERT_NE(doc()->addObject("App::FeatureTest", "Receipt3Busy"), nullptr);
    const std::string setup = std::string("import FreeCAD as App\ndoc = App.getDocument(\"")
        + _docName + "\")\nfeature = doc.getObject('Receipt3Busy')\n"
        "assert not doc.RecomputesFrozen\nbefore_count = feature.ExecCount\n"
        "assert not feature.OutList\n"
        "feature.touch()\nassert 'Touched' in feature.State\n";
    ASSERT_NO_THROW(Base::Interpreter().runString(setup.c_str()));

    App::FeatureTestAsyncBlocker::resetBlocker();
    struct ReleaseBlocker {
        ~ReleaseBlocker() { App::FeatureTestAsyncBlocker::releaseBlocker(); }
    } release;
    blocker->touch();
    auto command = makeRecomputeCommand("receipt3-real-blocker");
    command.recompute->featureIds.emplace_back("Receipt3Blocker");
    const auto first = doc()->executionHandle().trySubmit(std::move(command));
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(App::FeatureTestAsyncBlocker::waitUntilStarted(5s));

    auto attempt = std::async(std::launch::async, [] {
        return Base::Interpreter().runString(
            "try:\n    doc.recomputeAsync()\nexcept Exception as e:\n"
            "    assert 'busy' in str(e).lower()\n"
            "else:\n    raise AssertionError('expected Busy')\n");
    });
    const auto readyWhileBlocked = attempt.wait_for(5s) == std::future_status::ready;
    App::FeatureTestAsyncBlocker::releaseBlocker();
    EXPECT_TRUE(readyWhileBlocked);
    EXPECT_NO_THROW(attempt.get());
    App::DocumentCommandHandle handle(first.commandId, doc()->executionHandle().identity());
    ASSERT_TRUE(waitForTerminal(handle, 30s));
    ASSERT_EQ(handle.status().state, App::DocumentCommandState::Completed);
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "assert 'Touched' in feature.State\nassert feature.ExecCount == before_count\n"));
}

#endif

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST_F(DocumentExecutionLaneTest, CommandWrapperFaultPreservesUndoRedoAndSaveEffects)
{
    struct Reset {
        ~Reset() {
            App::Internal::setCompatibilityReceiptFaultForTesting(App::Internal::CompatibilityReceiptFault::None);
            try { Base::Interpreter().runString("receipt2_temp.cleanup()\n"); } catch (...) {}
        }
    } reset;
    ASSERT_NE(doc()->addObject("App::FeatureTest", "Receipt2History"), nullptr);
    const std::string setup = std::string(R"PY(
import FreeCAD as App
import pathlib, tempfile
receipt2_temp = tempfile.TemporaryDirectory(prefix="freecad-receipt2-")
missing = pathlib.Path(receipt2_temp.name) / "missing.FCStd"
existing = pathlib.Path(receipt2_temp.name) / "existing.FCStd"
original_bytes = b"unchanged receipt test\n"
existing.write_bytes(original_bytes)
doc = App.getDocument(")PY") + _docName + R"PY(")
feature = doc.getObject("Receipt2History")
original_label = feature.Label
doc.openTransaction("receipt2 history")
feature.Label = "receipt2 changed"
doc.commitTransaction()
assert doc.UndoCount > 0
)PY";
    ASSERT_NO_THROW(Base::Interpreter().runString(setup.c_str()));
    const auto fault = [&](const char* call) {
        App::Internal::setCompatibilityReceiptFaultForTesting(App::Internal::CompatibilityReceiptFault::CommandHandleAllocation);
        const std::string script = std::string("try:\n    ") + call
            + "\nexcept Exception:\n    pass\nelse:\n    raise AssertionError('wrapper allocation fault was not raised')\n";
        ASSERT_NO_THROW(Base::Interpreter().runString(script.c_str()));
        EXPECT_FALSE(App::Internal::consumeCompatibilityReceiptFaultForTesting(
            App::Internal::CompatibilityReceiptFault::CommandHandleAllocation));
        auto* lane = doc()->executionLane();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!lane->isIdle() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_TRUE(lane->isIdle());
        auto fence = lane->postToOwner([] { return 1; });
        EXPECT_EQ(fence.get(), 1);
    };
    ASSERT_NO_THROW(Base::Interpreter().runString("undo_before, redo_before = doc.UndoCount, doc.RedoCount\n"));
    ASSERT_NO_FATAL_FAILURE(fault("doc.undoAsync()"));
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "assert feature.Label == 'receipt2 changed'\n"
        "assert (doc.UndoCount, doc.RedoCount) == (undo_before, redo_before)\n"));

    ASSERT_NO_THROW(Base::Interpreter().runString("undo_handle = doc.undoAsync()\nundo_id = undo_handle.id()\n"));
    unsigned long long id = 0;
    {
        Base::PyGILStateLocker gil;
        PyObject* module = PyImport_AddModule("__main__");
        ASSERT_NE(module, nullptr);
        PyObject* value = PyDict_GetItemString(PyModule_GetDict(module), "undo_id");
        ASSERT_NE(value, nullptr);
        id = PyLong_AsUnsignedLongLong(value);
        ASSERT_FALSE(PyErr_Occurred());
    }
    App::DocumentCommandHandle undo(id, doc()->executionHandle().identity());
    ASSERT_TRUE(waitForTerminal(undo, 30s));
    ASSERT_EQ(undo.status().state, App::DocumentCommandState::Completed);
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "assert feature.Label == original_label and doc.RedoCount > 0\n"
        "undo_before, redo_before = doc.UndoCount, doc.RedoCount\n"));
    ASSERT_NO_FATAL_FAILURE(fault("doc.redoAsync()"));
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "assert feature.Label == original_label\n"
        "assert (doc.UndoCount, doc.RedoCount) == (undo_before, redo_before)\n"));
    ASSERT_NO_FATAL_FAILURE(fault("doc.saveAsync(str(missing))"));
    ASSERT_NO_THROW(Base::Interpreter().runString("assert not missing.exists()\n"));
    ASSERT_NO_FATAL_FAILURE(fault("doc.saveAsync(str(existing), True)"));
    ASSERT_NO_THROW(Base::Interpreter().runString("assert existing.read_bytes() == original_bytes\n"));

    // Exercise the same public APIs without a fault and inspect their real effects.
    auto waitPythonCommand = [&](const char* expression) {
        const std::string start = std::string("receipt2_handle = ") + expression
            + "\nreceipt2_id = receipt2_handle.id()\n";
        EXPECT_NO_THROW(Base::Interpreter().runString(start.c_str()));
        unsigned long long commandId = 0;
        {
            Base::PyGILStateLocker gil;
            PyObject* module = PyImport_AddModule("__main__");
            if (!module) { ADD_FAILURE() << "missing Python main module"; return; }
            PyObject* value = PyDict_GetItemString(PyModule_GetDict(module), "receipt2_id");
            if (!value) { ADD_FAILURE() << "missing command id"; return; }
            commandId = PyLong_AsUnsignedLongLong(value);
            if (PyErr_Occurred()) { PyErr_Clear(); ADD_FAILURE() << "invalid command id"; return; }
        }
        App::DocumentCommandHandle handle(commandId, doc()->executionHandle().identity());
        ASSERT_TRUE(waitForTerminal(handle, 30s));
        EXPECT_EQ(handle.status().state, App::DocumentCommandState::Completed);
    };
    ASSERT_NO_FATAL_FAILURE(waitPythonCommand("doc.redoAsync()"));
    ASSERT_NO_THROW(Base::Interpreter().runString("assert feature.Label == 'receipt2 changed'\n"));
    ASSERT_NO_FATAL_FAILURE(waitPythonCommand("doc.saveAsync(str(missing))"));
    ASSERT_NO_THROW(Base::Interpreter().runString(
        "import zipfile\n"
        "assert missing.is_file() and zipfile.is_zipfile(missing)\n"
        "with zipfile.ZipFile(missing) as saved:\n"
        "    assert b'receipt2 changed' in saved.read('Document.xml')\n"));
}

TEST_F(DocumentExecutionLaneTest, CompatibilityWaitGraphAllocationFailuresArePreEffect)
{
    struct ResetFault {
        ~ResetFault() { App::Internal::setCompatibilityReceiptFaultForTesting(App::Internal::CompatibilityReceiptFault::None); }
    } reset;
    const auto faults = {App::Internal::CompatibilityReceiptFault::CompatibilityFutureAllocation,
                         App::Internal::CompatibilityReceiptFault::CompatibilityStateAllocation,
                         App::Internal::CompatibilityReceiptFault::CompatibilityCapsuleAllocation,
                         App::Internal::CompatibilityReceiptFault::CompatibilityCallableAllocation,
                         App::Internal::CompatibilityReceiptFault::CompatibilityDictionaryAllocation};
    for (const auto fault : faults) {
        App::Internal::setCompatibilityReceiptFaultForTesting(fault);
        const std::string script = std::string(R"PY(
import FreeCAD as App
doc = App.getDocument(")PY") + _docName + R"PY(")
def callback():
    doc.addObject("App::FeatureTest", "GraphFailureEffect")
try:
    doc.commitCompatibilityMutationAsync(callback, structural=True, recompute=False)
except Exception:
    pass
else:
    raise AssertionError("wait graph allocation fault did not fail")
)PY";
        EXPECT_NO_THROW(Base::Interpreter().runString(script.c_str()));
        EXPECT_FALSE(App::Internal::consumeCompatibilityReceiptFaultForTesting(fault));
        auto fence = doc()->executionLane()->postToOwner([] { return 1; });
        EXPECT_EQ(fence.get(), 1);
        EXPECT_EQ(doc()->getObject("GraphFailureEffect"), nullptr);
    }
}


namespace {
std::atomic<std::promise<void>*> compatibilityEnteredPromise {nullptr};
void notifyCompatibilityWaitEntered() noexcept
{
    if (auto* promise = compatibilityEnteredPromise.exchange(nullptr)) {
        try { promise->set_value(); } catch (...) {}
    }
}
}

TEST_F(DocumentExecutionLaneTest, ConcurrentCompatibilityWaitIsRefusedWithoutBlockingOwner)
{
    std::promise<void> enteredPromise;
    auto entered = enteredPromise.get_future();
    compatibilityEnteredPromise.store(&enteredPromise);
    App::Internal::setCompatibilityWaitHookForTesting(&notifyCompatibilityWaitEntered);
    struct Cleanup {
        ~Cleanup() {
            App::Internal::setCompatibilityWaitHookForTesting(nullptr);
            compatibilityEnteredPromise.store(nullptr);
            try { Base::Interpreter().runString(
                "gate.set()\nwaiter.join(10.0)\n"); } catch (...) {}
        }
    } cleanup;
    const std::string start = std::string(R"PY(
import FreeCAD as App
import threading
doc = App.getDocument(")PY") + _docName + R"PY(")
gate = threading.Event()
def callback():
    gate.wait(10.0)
    doc.addObject("App::FeatureTest", "ConcurrentWaitEffect")
handle = doc.commitCompatibilityMutationAsync(callback, structural=True, recompute=False)
results = []
def first_wait():
    try:
        results.append(handle["wait"](10.0))
    except Exception as exc:
        results.append(exc)
waiter = threading.Thread(target=first_wait)
waiter.start()
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(start.c_str()));
    ASSERT_EQ(entered.wait_for(5s), std::future_status::ready);
    const char* finish = R"PY(
try:
    try:
        handle["wait"](1.0)
    except RuntimeError as exc:
        assert "in progress" in str(exc)
    else:
        raise AssertionError("concurrent wait was accepted")
finally:
    gate.set()
    waiter.join(10.0)
assert not waiter.is_alive()
assert len(results) == 1 and isinstance(results[0], dict) and results[0]["committed"]
assert doc.getObject("ConcurrentWaitEffect") is not None
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(finish));
}

TEST_F(DocumentExecutionLaneTest, CompatibilityResultConversionFailureRetainsCompletedResult)
{
    struct ResetFault {
        ~ResetFault() { App::Internal::setCompatibilityReceiptFaultForTesting(App::Internal::CompatibilityReceiptFault::None); }
    } reset;
    App::Internal::setCompatibilityReceiptFaultForTesting(App::Internal::CompatibilityReceiptFault::CompatibilityResultConversion);
    const std::string script = std::string(R"PY(
import FreeCAD as App
doc = App.getDocument(")PY") + _docName + R"PY(")
calls = []
def callback():
    calls.append(1)
    doc.addObject("App::FeatureTest", "RetainedResultEffect")
handle = doc.commitCompatibilityMutationAsync(callback, structural=True, recompute=False)
try:
    handle["wait"](10.0)
except Exception:
    pass
else:
    raise AssertionError("result conversion fault did not fail")
result = handle["wait"](10.0)
assert result["status"] == "Committed" and result["committed"]
assert result["conflicts"] == [] and isinstance(result["published_revisions"], list)
assert calls == [1] and doc.getObject("RetainedResultEffect") is not None
try:
    handle["wait"](10.0)
except RuntimeError as exc:
    assert "consumed" in str(exc)
else:
    raise AssertionError("result delivered twice")
)PY";
    EXPECT_NO_THROW(Base::Interpreter().runString(script.c_str()));
    EXPECT_FALSE(App::Internal::consumeCompatibilityReceiptFaultForTesting(
        App::Internal::CompatibilityReceiptFault::CompatibilityResultConversion));
}
#endif

TEST_F(DocumentExecutionLaneTest, SaveAsyncWritesEncodedFilename)
{
    struct Cleanup {
        ~Cleanup() {
            try { Base::Interpreter().runString("receipt_temp_dir.cleanup()\n"); } catch (...) {}
        }
    } cleanup;
    const std::string start = std::string(R"PY(
import FreeCAD as App
import os
import pathlib
import tempfile
import zipfile
receipt_temp_dir = tempfile.TemporaryDirectory(prefix="freecad-receipt-")
receipt_path = pathlib.Path(receipt_temp_dir.name) / "caf\u00e9.FCStd"
# saveAsync consumes UTF-8 independently of Python's locale filesystem codec.
# Use that exact byte name for filesystem checks even when the codec is ASCII.
receipt_bytes = os.path.join(os.fsencode(receipt_temp_dir.name), "caf\u00e9.FCStd".encode("utf-8"))
assert not os.path.exists(receipt_bytes)
receipt_doc = App.getDocument(")PY") + _docName + R"PY(")
receipt_doc.addObject("App::FeatureTest", "EncodedSaveMarker")
receipt_handle = receipt_doc.saveAsync(str(receipt_path))
receipt_id = receipt_handle.id()
)PY";
    ASSERT_NO_THROW(Base::Interpreter().runString(start.c_str()));
    unsigned long long id = 0;
    {
        Base::PyGILStateLocker gil;
        PyObject* mainModule = PyImport_AddModule("__main__");
        ASSERT_NE(mainModule, nullptr);
        PyObject* idObject = PyDict_GetItemString(PyModule_GetDict(mainModule), "receipt_id");
        ASSERT_NE(idObject, nullptr);
        id = PyLong_AsUnsignedLongLong(idObject);
        ASSERT_FALSE(PyErr_Occurred());
    }
    App::DocumentCommandHandle handle(id, doc()->executionHandle().identity());
    ASSERT_TRUE(waitForTerminal(handle, 30s));
    EXPECT_EQ(handle.status().state, App::DocumentCommandState::Completed);
    EXPECT_NO_THROW(Base::Interpreter().runString(R"PY(
assert receipt_handle.id() == receipt_id
assert receipt_handle.done() and receipt_handle.status()['state'] == 'Completed'
assert receipt_doc.FileName == str(receipt_path)
assert os.path.isfile(receipt_bytes) and os.stat(receipt_bytes).st_size > 0
with open(receipt_bytes, 'rb') as receipt_file:
    with zipfile.ZipFile(receipt_file) as archive:
        assert archive.testzip() is None
        assert b'EncodedSaveMarker' in archive.read('Document.xml')
)PY"));
}
