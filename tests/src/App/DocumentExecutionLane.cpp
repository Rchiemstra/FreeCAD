// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentExecutionStall.h>
#include <App/DocumentHandle.h>
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
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <functional>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace App::Internal
{

class DocumentExecutionLaneTestAccess
{
public:
    using Hook = void (*)();

    static auto exchangeAdmissionHook(void (*hook)())
    {
        return DocumentCollaborationService::_postLifecycleAdmissionTestHook.exchange(hook);
    }

    static bool isCollaborationOwnerThread(const Document& document) noexcept
    {
        return document.isCollaborationOwnerThread();
    }

    [[nodiscard]] static bool tryLockMutex(const DocumentExecutionLane& lane)
    {
        std::unique_lock<std::mutex> probe(lane._mutex, std::try_to_lock);
        return probe.owns_lock();
    }

    static void setPrePumpSnapshotCaptureHook(Hook hook) noexcept
    {
        DocumentExecutionLane::_prePumpSnapshotCaptureTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPumpSnapshotCaptureHook(Hook hook) noexcept
    {
        DocumentExecutionLane::_pumpSnapshotCaptureTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPrePublishActiveSnapshotHook(Hook hook) noexcept
    {
        DocumentExecutionLane::_prePublishActiveSnapshotTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPreCancelSnapshotMutationHook(Hook hook) noexcept
    {
        DocumentExecutionLane::_preCancelSnapshotMutationTestHook.store(
            hook, std::memory_order_release);
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

// Reuse the existing lifecycle-admission hook to release preparation only
// after RecomputeHandle::wait has entered. Other owner work cannot trigger it:
// the callback is armed on that thread immediately around the wait call.
struct WaitAdmissionRelease
{
    explicit WaitAdmissionRelease(std::function<void()> release)
    {
        Release = std::move(release);
    }
    ~WaitAdmissionRelease()
    {
        Release = {};
    }
    static void invoke()
    {
        auto release = std::exchange(Release, {});
        if (release) {
            release();
        }
    }
    static inline thread_local std::function<void()> Release;
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

// A stranded owner cannot safely be detached or allowed to outlive test state.
// Terminate this test process after the existing cleanup bound instead.
[[noreturn]] void failStrandedOwner()
{
    std::fputs("Owner work did not quiesce during test cleanup; terminating process\n", stderr);
    std::fflush(stderr);
    std::abort();
}

template<typename T>
class OwnerWorkCleanup
{
public:
    OwnerWorkCleanup(std::shared_future<T>& result,
                     App::DocumentExecutionLane* lane,
                     LaneBlockingRecomputeState* blocking) noexcept
        : _result(result), _lane(lane), _blocking(blocking)
    {}

    ~OwnerWorkCleanup()
    {
        finish();
    }

    void finish() noexcept
    {
        _blocking->release();
        if (_result.wait_for(5s) != std::future_status::ready) {
            failStrandedOwner();
        }
        // A callback may have admitted a command and returned before it ran.
        waitForApplicationClosePermitted(_lane);
        if (!_lane->permitsApplicationClose()) {
            failStrandedOwner();
        }
    }

private:
    std::shared_future<T>& _result;
    App::DocumentExecutionLane* _lane;
    LaneBlockingRecomputeState* _blocking;
};

class WaiterCleanup
{
public:
    WaiterCleanup(std::jthread& thread,
                  std::future<void>& exited,
                  LaneBlockingRecomputeState* blocking) noexcept
        : _thread(thread), _exited(exited), _blocking(blocking)
    {}

    ~WaiterCleanup()
    {
        finish();
    }

    void finish() noexcept
    {
        _blocking->release();
        if (_thread.joinable()) {
            if (_exited.wait_for(5s) != std::future_status::ready) {
                failStrandedOwner();
            }
            _thread.join();
        }
    }

private:
    std::jthread& _thread;
    std::future<void>& _exited;
    LaneBlockingRecomputeState* _blocking;
};

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
        auto* liveDocument = App::GetApplication().getDocument(_docName.c_str());
        if (liveDocument && liveDocument == _doc && liveDocument->executionLane()) {
            auto* lane = liveDocument->executionLane();
            waitForApplicationClosePermitted(lane);
            if (!lane->permitsApplicationClose()) {
                failStrandedOwner();
            }
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

TEST_F(DocumentExecutionLaneTest, SameKeyUnequalRecomputeInputsAreBusyBeforeEffects)
{
    auto* blocker = dynamic_cast<App::FeatureTestAsyncBlocker*>(
        doc()->addObject("App::FeatureTestAsyncBlocker", "CoalescingBlocker"));
    auto* companion = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "CoalescingCompanion"));
    auto* target = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "CoalescingTarget"));
    ASSERT_NE(blocker, nullptr);
    ASSERT_NE(companion, nullptr);
    ASSERT_NE(target, nullptr);
    App::FeatureTestAsyncBlocker::resetBlocker();
    struct ReleaseBlocker {
        ~ReleaseBlocker() { App::FeatureTestAsyncBlocker::releaseBlocker(); }
    } releaseBlocker;
    blocker->touch();
    const auto initialExecutions = target->ExecCount.getValue();
    auto handle = doc()->executionHandle();
    auto firstCommand = makeRecomputeCommand("explicit-recompute-key");
    firstCommand.recompute->featureIds = {"CoalescingBlocker", "CoalescingCompanion"};
    const auto structureKey = App::DocumentRevisionKey::documentStructure();
    const auto objectKey = App::DocumentRevisionKey::objectProperty("CoalescingTarget", "ExecCount");
    firstCommand.expectedRevisions.emplace_back(
        structureKey, doc()->collaborationRevisions().current(structureKey));
    firstCommand.expectedRevisions.emplace_back(
        objectKey, doc()->collaborationRevisions().current(objectKey));

    const auto first = handle.trySubmit(firstCommand);
    ASSERT_TRUE(first.accepted());
    ASSERT_NE(first.commandId, 0U);
    ASSERT_TRUE(App::FeatureTestAsyncBlocker::waitUntilStarted(5s));
    const auto identical = handle.trySubmit(firstCommand);
    EXPECT_TRUE(identical.accepted());
    EXPECT_EQ(identical.commandId, first.commandId);

    auto differentFeatures = firstCommand;
    differentFeatures.recompute->featureIds = {"CoalescingBlocker", "CoalescingTarget"};
    auto differentOrder = firstCommand;
    std::reverse(differentOrder.recompute->featureIds.begin(),
                 differentOrder.recompute->featureIds.end());
    auto differentForce = firstCommand;
    differentForce.recompute->force = true;
    auto differentOptions = firstCommand;
    differentOptions.recompute->options = App::Document::DepNoCycle;
    auto differentFence = firstCommand;
    differentFence.recompute->refreshRevisionFenceAfterEachCommit = true;
    auto differentSnapshots = firstCommand;
    differentSnapshots.recompute->declaresCrossDocumentSnapshots = true;
    auto differentRevision = firstCommand;
    ++differentRevision.expectedRevisions[0].revision;
    auto differentRevisionKey = firstCommand;
    differentRevisionKey.expectedRevisions[0].key = App::DocumentRevisionKey::unknownModelMutation();
    auto differentRevisionSubject = firstCommand;
    differentRevisionSubject.expectedRevisions[1].key.subject = "CoalescingCompanion";
    auto differentRevisionProperty = firstCommand;
    differentRevisionProperty.expectedRevisions[1].key.propertyName = "ExecResult";
    auto differentObservationOrder = firstCommand;
    std::reverse(differentObservationOrder.expectedRevisions.begin(),
                 differentObservationOrder.expectedRevisions.end());
    auto missingObservation = firstCommand;
    missingObservation.expectedRevisions.pop_back();
    const std::vector<std::pair<const char*, App::DocumentCommand>> variants {
        {"feature IDs", differentFeatures}, {"feature order", differentOrder},
        {"force", differentForce}, {"options", differentOptions},
        {"fence policy", differentFence}, {"cross-document snapshots", differentSnapshots},
        {"expected revision", differentRevision}, {"revision kind", differentRevisionKey},
        {"revision subject", differentRevisionSubject}, {"revision property", differentRevisionProperty},
        {"observation order", differentObservationOrder}, {"missing observation", missingObservation}
    };
    std::atomic<int> beforeRecompute {0};
    fastsignals::scoped_connection beforeConnection = doc()->signalBeforeRecompute.connect(
        [&](const App::Document&) { beforeRecompute.fetch_add(1, std::memory_order_relaxed); });
    for (const auto& [label, command] : variants) {
        SCOPED_TRACE(label);
        const auto rejected = handle.trySubmit(command);
        EXPECT_EQ(rejected.result, App::DocumentCommandSubmitResult::Busy);
        EXPECT_EQ(rejected.commandId, 0U);
    }
    EXPECT_EQ(beforeRecompute.load(std::memory_order_relaxed), 0);
    EXPECT_EQ(target->ExecCount.getValue(), initialExecutions);

    App::FeatureTestAsyncBlocker::releaseBlocker();
    App::DocumentCommandHandle firstReceipt(first.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(firstReceipt, 30s));
    ASSERT_EQ(firstReceipt.status().state, App::DocumentCommandState::Completed);
    EXPECT_EQ(target->ExecCount.getValue(), initialExecutions);
    auto retryCommand = differentFeatures;
    retryCommand.recompute->force = true;
    // Fence enforcement is separate; the retry does not depend on stale observations.
    retryCommand.expectedRevisions.clear();
    const auto retry = handle.trySubmit(retryCommand);
    ASSERT_TRUE(retry.accepted());
    EXPECT_NE(retry.commandId, first.commandId);
    App::DocumentCommandHandle retryReceipt(retry.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(retryReceipt, 30s));
    EXPECT_EQ(retryReceipt.status().state, App::DocumentCommandState::Completed);
    EXPECT_EQ(target->ExecCount.getValue(), initialExecutions + 1);
}

TEST_F(DocumentExecutionLaneTest, EmptyRecomputeKeyDoesNotJoinActiveRequest)
{
    auto* blocker = dynamic_cast<App::FeatureTestAsyncBlocker*>(
        doc()->addObject("App::FeatureTestAsyncBlocker", "UnkeyedCoalescingBlocker"));
    ASSERT_NE(blocker, nullptr);
    App::FeatureTestAsyncBlocker::resetBlocker();
    struct ReleaseBlocker {
        ~ReleaseBlocker() { App::FeatureTestAsyncBlocker::releaseBlocker(); }
    } releaseBlocker;
    blocker->touch();
    auto handle = doc()->executionHandle();
    auto command = makeRecomputeCommand("");
    command.recompute->featureIds = {"UnkeyedCoalescingBlocker"};
    const auto first = handle.trySubmit(command);
    ASSERT_TRUE(first.accepted());
    ASSERT_TRUE(App::FeatureTestAsyncBlocker::waitUntilStarted(5s));
    const auto second = handle.trySubmit(command);
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Busy);
    EXPECT_EQ(second.commandId, 0U);
    App::FeatureTestAsyncBlocker::releaseBlocker();
    App::DocumentCommandHandle receipt(first.commandId, handle.identity());
    ASSERT_TRUE(waitForTerminal(receipt, 30s));
    EXPECT_EQ(receipt.status().state, App::DocumentCommandState::Completed);
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
    // Establish binding before starting the non-owner wait; preparation remains
    // blocked, so the observation cannot already be terminal.
    const auto idSnapshot = doc()->executionLane()->dispatchToOwner(
        [commandHandle] { return commandHandle.status(); }, true, true);
    ASSERT_TRUE(idSnapshot.recompute);
    ASSERT_NE(idSnapshot.recompute->id, 0U);
    const App::DocumentRecomputeId recomputeId =
        static_cast<App::DocumentRecomputeId>(idSnapshot.recompute->id);

    App::DocumentRecomputeSnapshot waited;
    std::atomic<bool> waitReturned {false};
    std::promise<void> exited;
    auto exitFuture = exited.get_future();
    std::jthread waiter([&] {
        App::RecomputeHandle recomputeHandle(*doc(), recomputeId);
        waited = recomputeHandle.wait(500ms);
        waitReturned.store(true, std::memory_order_release);
        exited.set_value_at_thread_exit();
    });
    WaiterCleanup cleanup(waiter, exitFuture, _blocking.get());

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!waitReturned.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    const bool returned = waitReturned.load(std::memory_order_acquire);
    EXPECT_TRUE(returned);
    if (returned) {
        EXPECT_FALSE(waited.terminal());
        EXPECT_EQ(waited.state, App::DocumentRecomputeState::Running) << waited.diagnostic;
    }

    cleanup.finish();
    App::RecomputeHandle recomputeHandle(*doc(), recomputeId);
    const auto completed = recomputeHandle.wait(5s);
    EXPECT_EQ(completed.state, App::DocumentRecomputeState::Completed) << completed.diagnostic;
    EXPECT_EQ(commandHandle.status().state, App::DocumentCommandState::Completed);
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleWaitCompletesActiveCommand)
{
    _blocking->reset();
    auto execution = doc()->executionHandle();
    const auto outcome = execution.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted());
    App::DocumentCommandHandle command(outcome.commandId, execution.identity());

    auto entered = std::make_shared<std::promise<void>>();
    auto waitEntered = entered->get_future();
    struct RestoreHook
    {
        void (*previous)();
        ~RestoreHook()
        {
            App::Internal::DocumentExecutionLaneTestAccess::exchangeAdmissionHook(previous);
        }
    } restore {App::Internal::DocumentExecutionLaneTestAccess::exchangeAdmissionHook(
        &WaitAdmissionRelease::invoke)};
    auto* owner = doc();
    auto blocking = _blocking;
    // The preparation is still blocked when this callable enters the dispatch
    // drain. Only admission from inside wait() releases it.
    auto result = owner->executionLane()->postToOwner([owner, command, entered, blocking] {
        const auto observation = command.status();
        if (!observation.recompute) {
            throw std::runtime_error("active recompute has no observation");
        }
        const auto id = static_cast<App::DocumentRecomputeId>(observation.recompute->id);
        if (!owner->recomputeCoordinator().status(id)) {
            throw std::runtime_error("controlled preparation is not bound");
        }
        App::RecomputeHandle handle(*owner, id);
        WaitAdmissionRelease release([blocking, entered] {
            blocking->release();
            entered->set_value();
        });
        return handle.wait(5s);
    }).share();
    OwnerWorkCleanup cleanup(result, owner->executionLane(), _blocking.get());
    const auto entry = waitEntered.wait_for(5s);
    EXPECT_EQ(entry, std::future_status::ready);
    const auto releasedAt = std::chrono::steady_clock::now();
    // Also release on the failure-to-enter path, before any fatal assertion.
    _blocking->release();
    const auto promptly = result.wait_for(2s);
    EXPECT_EQ(promptly, std::future_status::ready);
    ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
    const auto snapshot = result.get();
    EXPECT_LT(std::chrono::steady_clock::now() - releasedAt, 2s);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed) << snapshot.diagnostic;
    EXPECT_TRUE(snapshot.terminal());
    EXPECT_EQ(snapshot.completedFeatures, 1U);
    EXPECT_EQ(snapshot.failedFeatures, 0U);
    const auto completed = command.status();
    EXPECT_EQ(completed.state, App::DocumentCommandState::Completed);
    ASSERT_TRUE(completed.recompute);
    EXPECT_EQ(snapshot.id, completed.recompute->id);
    // A second wait observes the retained terminal result without advancing work.
    App::RecomputeHandle again(*owner, snapshot.id);
    EXPECT_EQ(again.wait(0ms).state, App::DocumentRecomputeState::Completed);
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleWaitBindsAdmittedCommand)
{
    auto* feature = doc()->addObject<App::FeatureTest>("PrebindingWait");
    ASSERT_NE(feature, nullptr);
    feature->touch();
    const auto count = feature->ExecCount.getValue();
    auto* owner = doc();
    auto request = makeRecomputeCommand("prebinding-handle-wait");
    // Submit and wait in a single owner dispatch. The enclosing threadMain
    // cannot bind the admitted id before wait starts.
    auto result = owner->executionLane()->postToOwner(
        [owner, request = std::move(request)]() mutable {
            auto* lane = owner->executionLane();
            const auto admitted = lane->trySubmit(std::move(request));
            if (admitted.result != App::DocumentCommandSubmitResult::Accepted) {
                throw std::runtime_error("prebinding command admission failed");
            }
            const auto before = lane->commandStatus(admitted.commandId);
            if (!before.recompute) {
                throw std::runtime_error("admission has no recompute observation");
            }
            const auto id = static_cast<App::DocumentRecomputeId>(before.recompute->id);
            EXPECT_FALSE(owner->recomputeCoordinator().status(id));
            EXPECT_EQ(before.recompute->state, App::DocumentCommandRecomputeState::Running);
            App::RecomputeHandle handle(*owner, id);
            const auto snapshot = handle.wait(5s);
            EXPECT_EQ(lane->commandStatus(admitted.commandId).state,
                      App::DocumentCommandState::Completed);
            return std::make_pair(
                snapshot, App::DocumentCommandHandle(admitted.commandId, lane->identity()));
        }).share();
    OwnerWorkCleanup cleanup(result, owner->executionLane(), _blocking.get());
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    auto [snapshot, command] = result.get();
    // On an unfixed build wait returns before execution; synchronize before
    // reading live feature properties even on that failure path.
    ASSERT_TRUE(waitForTerminal(command));
    EXPECT_TRUE(snapshot.terminal());
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.completedFeatures, 1U);
    EXPECT_EQ(feature->ExecCount.getValue(), count + 1);
}

TEST_F(DocumentExecutionLaneTest, RecomputeHandleWaitRefusesCallbackReentry)
{
    struct Reentry
    {
        App::DocumentRecomputeId id {0};
        std::atomic<int> before {0};
        std::atomic<int> after {0};
        void wait(App::Document& document, std::atomic<int>& rejected)
        {
            App::RecomputeHandle handle(document, id);
            try {
                static_cast<void>(handle.wait(0ms));
                ADD_FAILURE() << "recursive wait must yield to the enclosing recompute";
            }
            catch (const App::DocumentWouldBlock&) {
                ++rejected;
            }
        }
    };
    auto reentry = std::make_shared<Reentry>();
    auto* owner = doc();
    ::fastsignals::scoped_connection before = owner->signalBeforeRecompute.connect(
        [owner, reentry](const App::Document&) { reentry->wait(*owner, reentry->before); });
    ::fastsignals::scoped_connection after = owner->signalRecomputed.connect(
        [owner, reentry](const App::Document&, const std::vector<App::DocumentObject*>&) {
            reentry->wait(*owner, reentry->after);
        });
    auto request = makeRecomputeCommand("callback-reentry-wait");
    auto result = owner->executionLane()->postToOwner(
        [owner, reentry, request = std::move(request)]() mutable {
            auto* lane = owner->executionLane();
            const auto admitted = lane->trySubmit(std::move(request));
            if (admitted.result != App::DocumentCommandSubmitResult::Accepted) {
                throw std::runtime_error("reentry command admission failed");
            }
            const auto observation = lane->commandStatus(admitted.commandId);
            if (!observation.recompute) {
                throw std::runtime_error("reentry command has no recompute observation");
            }
            reentry->id = observation.recompute->id;
            App::RecomputeHandle handle(*owner, reentry->id);
            return handle.wait(5s);
        }).share();
    OwnerWorkCleanup cleanup(result, owner->executionLane(), _blocking.get());
    ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(result.get().state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(reentry->before.load(), 1);
    EXPECT_EQ(reentry->after.load(), 1);
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

TEST_F(DocumentExecutionLaneTest, PumpSnapshotCaptureAndCancelOverlapPreservesLongReason)
{
    _blocking->reset();

    struct OverlapState
    {
        std::thread::id ownerThreadId {};

        // Step 1: cross-thread mutex exclusion probe at the capture critical section.
        // Touched only by the owner thread (iteration 1) and the test thread.
        std::mutex captureMutex;
        std::condition_variable captureCv;
        std::size_t captureProbeCalls {0};
        bool ownerInCaptureSection {false};
        bool releaseCaptureProbe {false};
        bool captureProbeTimedOut {false};

        // Owner-thread-only pump iteration counter (accessed only on ownerThreadId).
        std::size_t preCaptureCalls {0};

        // Step 2 pre-overlap rendezvous: touched by owner (iteration 2, before snapshot
        // read R), test thread (before spawning canceller), and canceller (only BEFORE
        // snapshot write W). Disjoint from postMutex so W -> postMutex cannot create a
        // happens-before edge to R in the unfixed build.
        std::mutex preMutex;
        std::condition_variable preCv;
        std::thread::id cancelThreadId {};
        bool ownerReadyForOverlap {false};
        bool cancelAtMutationHook {false};
        bool cancelReadyForOverlap {false};
        bool preCaptureTimedOut {false};

        // Step 2 post-write gate and Step 3 next-pump pause: touched by canceller ONLY
        // AFTER W, by owner ONLY AFTER R (pre-publish on iteration 2 and pre-capture
        // on iteration 3), and by test thread after spawning canceller.
        std::mutex postMutex;
        std::condition_variable postCv;
        bool ownerAtPublishHook {false};
        bool cancelMutationCompleted {false};
        bool cancelResult {false};
        bool ownerPausedOnNextPump {false};
        bool allowSubsequentPumps {false};
        bool postPublishTimedOut {false};
    };

    static std::mutex activeStateMutex;
    static std::shared_ptr<OverlapState> activeState;

    auto state = std::make_shared<OverlapState>();
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    state->ownerThreadId = lane->ownerThreadId();
    ASSERT_NE(std::this_thread::get_id(), state->ownerThreadId);
    {
        std::lock_guard lock(activeStateMutex);
        activeState = state;
    }

    struct CleanupGuard
    {
        std::shared_ptr<OverlapState> state;
        std::shared_ptr<LaneBlockingRecomputeState> blocking;
        std::thread canceller;

        CleanupGuard(std::shared_ptr<OverlapState> overlapState,
                     std::shared_ptr<LaneBlockingRecomputeState> blockingState)
            : state(std::move(overlapState))
            , blocking(std::move(blockingState))
        {}

        CleanupGuard(const CleanupGuard&) = delete;
        CleanupGuard& operator=(const CleanupGuard&) = delete;

        void unblockAll() const
        {
            App::Internal::DocumentExecutionLaneTestAccess::setPrePumpSnapshotCaptureHook(nullptr);
            App::Internal::DocumentExecutionLaneTestAccess::setPumpSnapshotCaptureHook(nullptr);
            App::Internal::DocumentExecutionLaneTestAccess::setPrePublishActiveSnapshotHook(
                nullptr);
            App::Internal::DocumentExecutionLaneTestAccess::setPreCancelSnapshotMutationHook(
                nullptr);
            {
                std::lock_guard lock(activeStateMutex);
                if (activeState == state) {
                    activeState.reset();
                }
            }
            if (state) {
                {
                    std::lock_guard lock(state->captureMutex);
                    state->releaseCaptureProbe = true;
                }
                state->captureCv.notify_all();
                {
                    std::lock_guard lock(state->preMutex);
                    state->cancelReadyForOverlap = true;
                }
                state->preCv.notify_all();
                {
                    std::lock_guard lock(state->postMutex);
                    state->cancelMutationCompleted = true;
                    state->allowSubsequentPumps = true;
                }
                state->postCv.notify_all();
            }
            if (blocking) {
                blocking->release();
            }
        }

        ~CleanupGuard()
        {
            // Release all blocked callbacks and the detached recompute task BEFORE
            // joining canceller so an early ASSERT failure or timeout cannot deadlock.
            unblockAll();
            if (canceller.joinable()) {
                canceller.join();
            }
        }
    } cleanup(state, _blocking);

    App::Internal::DocumentExecutionLaneTestAccess::setPumpSnapshotCaptureHook(+[]() {
        std::shared_ptr<OverlapState> current;
        {
            std::lock_guard lock(activeStateMutex);
            current = activeState;
        }
        if (!current || std::this_thread::get_id() != current->ownerThreadId) {
            return;
        }
        std::unique_lock lock(current->captureMutex);
        ++current->captureProbeCalls;
        if (current->captureProbeCalls == 1) {
            current->ownerInCaptureSection = true;
            current->captureCv.notify_all();
            if (!current->captureCv.wait_for(lock, 5s, [&] {
                    return current->releaseCaptureProbe;
                })) {
                current->captureProbeTimedOut = true;
            }
        }
    });

    App::Internal::DocumentExecutionLaneTestAccess::setPrePumpSnapshotCaptureHook(+[]() {
        std::shared_ptr<OverlapState> current;
        {
            std::lock_guard lock(activeStateMutex);
            current = activeState;
        }
        if (!current || std::this_thread::get_id() != current->ownerThreadId) {
            return;
        }
        const auto call = ++current->preCaptureCalls;
        if (call == 2) {
            std::unique_lock lock(current->preMutex);
            current->ownerReadyForOverlap = true;
            current->preCv.notify_all();
            if (!current->preCv.wait_for(lock, 5s, [&] {
                    return current->cancelReadyForOverlap;
                })) {
                current->preCaptureTimedOut = true;
            }
        }
        else if (call == 3) {
            std::unique_lock lock(current->postMutex);
            current->ownerPausedOnNextPump = true;
            current->postCv.notify_all();
            if (!current->postCv.wait_for(lock, 5s, [&] {
                    return current->allowSubsequentPumps;
                })) {
                current->postPublishTimedOut = true;
            }
        }
    });

    App::Internal::DocumentExecutionLaneTestAccess::setPreCancelSnapshotMutationHook(+[]() {
        std::shared_ptr<OverlapState> current;
        {
            std::lock_guard lock(activeStateMutex);
            current = activeState;
        }
        if (!current) {
            return;
        }
        {
            std::lock_guard lock(current->preMutex);
            if (std::this_thread::get_id() != current->cancelThreadId) {
                return;
            }
            current->cancelAtMutationHook = true;
            current->cancelReadyForOverlap = true;
        }
        current->preCv.notify_all();
    });

    App::Internal::DocumentExecutionLaneTestAccess::setPrePublishActiveSnapshotHook(+[]() {
        std::shared_ptr<OverlapState> current;
        {
            std::lock_guard lock(activeStateMutex);
            current = activeState;
        }
        if (!current || std::this_thread::get_id() != current->ownerThreadId) {
            return;
        }
        if (current->preCaptureCalls == 2) {
            std::unique_lock lock(current->postMutex);
            current->ownerAtPublishHook = true;
            current->postCv.notify_all();
            if (!current->postCv.wait_for(lock, 5s, [&] {
                    return current->cancelMutationCompleted;
                })) {
                current->postPublishTimedOut = true;
            }
        }
    });

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeLaneBlockingRecomputeCommand());
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    ASSERT_TRUE(_blocking->waitUntilStarted(5s));

    // Step 1: Witness from a non-owner thread that DocumentExecutionLane::_mutex
    // is held across pumpActiveRecompute()'s snapshot capture critical section.
    {
        std::unique_lock lock(state->captureMutex);
        ASSERT_TRUE(
            state->captureCv.wait_for(lock, 5s, [&] { return state->ownerInCaptureSection; }));
    }
    bool mutexAcquiredDuringCapture = false;
    for (int attempt = 0; attempt < 64 && !mutexAcquiredDuringCapture; ++attempt) {
        mutexAcquiredDuringCapture =
            App::Internal::DocumentExecutionLaneTestAccess::tryLockMutex(*lane);
    }
    EXPECT_FALSE(mutexAcquiredDuringCapture)
        << "pumpActiveRecompute must hold DocumentExecutionLane::_mutex while capturing "
           "command.snapshot";

    // Disarm the inside-capture hook before releasing iteration 1 so iteration 2's
    // capture/cancel overlap runs without any hook inside the capture section.
    App::Internal::DocumentExecutionLaneTestAccess::setPumpSnapshotCaptureHook(nullptr);
    {
        std::lock_guard lock(state->captureMutex);
        state->releaseCaptureProbe = true;
    }
    state->captureCv.notify_all();

    // Step 2: Pause iteration 2 immediately before snapshot capture (outside _mutex),
    // then run cancelCommand() with a long non-SSO diagnostic on a separate thread.
    // Neither the test thread nor canceller touches preMutex after canceller's
    // pre-mutation release, so in an unfixed build there is no happens-before edge
    // between the owner's snapshot copy and canceller's snapshot mutation under TSAN.
    {
        std::unique_lock lock(state->preMutex);
        ASSERT_TRUE(state->preCv.wait_for(lock, 5s, [&] { return state->ownerReadyForOverlap; }));
    }

    const std::string longCancelReason =
        "cooperative-cancel-during-pump-snapshot-capture:"
        + std::string(192, 'R')
        + ":must-survive-cancellation-merge-and-terminal-completion";
    ASSERT_GT(longCancelReason.size(), 128U);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    cleanup.canceller = std::thread([state, commandHandle, longCancelReason]() mutable {
        {
            std::lock_guard lock(state->preMutex);
            state->cancelThreadId = std::this_thread::get_id();
        }
        const bool cancelled = commandHandle.cancel(longCancelReason);
        {
            std::lock_guard lock(state->postMutex);
            state->cancelResult = cancelled;
            state->cancelMutationCompleted = true;
        }
        state->postCv.notify_all();
    });

    // Step 3: Wait until canceller has completed its snapshot write and iteration 2
    // has finished publishActiveSnapshot() so iteration 3 is paused after its
    // coordinator poll/status and before it captures or publishes _active->snapshot.
    bool cancelResult = false;
    {
        std::unique_lock lock(state->postMutex);
        ASSERT_TRUE(state->postCv.wait_for(lock, 5s, [&] {
            return state->cancelMutationCompleted && state->ownerPausedOnNextPump;
        }));
        cancelResult = state->cancelResult;
        EXPECT_TRUE(state->ownerAtPublishHook);
        EXPECT_FALSE(state->postPublishTimedOut);
    }
    cleanup.canceller.join();
    {
        std::lock_guard lock(state->captureMutex);
        EXPECT_FALSE(state->captureProbeTimedOut);
    }
    {
        std::lock_guard lock(state->preMutex);
        EXPECT_TRUE(state->cancelAtMutationHook);
        EXPECT_FALSE(state->preCaptureTimedOut);
    }
    EXPECT_TRUE(cancelResult);

    const auto mergedActiveSnapshot = commandHandle.status();
    EXPECT_EQ(mergedActiveSnapshot.state, App::DocumentCommandState::Cancelling);
    EXPECT_EQ(mergedActiveSnapshot.diagnostic, longCancelReason);

    // Step 4: Resume subsequent pumps and release the detached recompute task;
    // verify terminal Cancelled state and long diagnostic survive completion.
    {
        std::lock_guard lock(state->postMutex);
        state->allowSubsequentPumps = true;
    }
    state->postCv.notify_all();
    _blocking->release();

    ASSERT_TRUE(waitForTerminal(commandHandle, 5s));
    const auto terminalSnapshot = commandHandle.status();
    EXPECT_EQ(terminalSnapshot.state, App::DocumentCommandState::Cancelled);
    EXPECT_EQ(terminalSnapshot.diagnostic, longCancelReason);
    ASSERT_TRUE(terminalSnapshot.recompute.has_value());
    EXPECT_EQ(terminalSnapshot.recompute->state, App::DocumentCommandRecomputeState::Cancelled);
    EXPECT_EQ(terminalSnapshot.recompute->diagnostic, longCancelReason);
}

}  // namespace
