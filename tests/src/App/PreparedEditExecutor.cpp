// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/PreparedEditExecutor.h"

#include <Base/Exception.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace App;
using namespace std::chrono_literals;

namespace App
{
class DocumentObject;

namespace Internal
{

class PreparedEditExecutorTestAccess
{
public:
    static PreparedEditExecutionId
    submit(PreparedEditExecutor& executor,
           CollaborativeOperationPreparation::DetachedTask task,
           PreparationPolicy policy = PreparationPolicy::DetachedInProcess)
    {
        return executor.submit(std::move(task), policy);
    }

    static bool abandon(PreparedEditExecutor& executor, PreparedEditExecutionId id)
    {
        return executor.abandon(id);
    }
};

}  // namespace Internal
}

namespace
{

using DetachedTask = CollaborativeOperationPreparation::DetachedTask;

template<typename Executor>
concept PubliclySubmittable = requires(Executor& executor, DetachedTask task) {
    executor.submit(std::move(task));
};

template<typename Executor>
concept PubliclyAbandonable = requires(Executor& executor, PreparedEditExecutionId id) {
    executor.abandon(id);
};

template<typename ApplicationType>
concept ApplicationAccessorIsPublic = requires(ApplicationType& application) {
    application.preparedEditExecutor();
};

template<typename ApplicationType>
concept ApplicationAccessorAllowsSubmission =
    requires(ApplicationType& application, DetachedTask task) {
        application.preparedEditExecutor().submit(std::move(task));
    };

static_assert(!PubliclySubmittable<PreparedEditExecutor>);
static_assert(!PubliclyAbandonable<PreparedEditExecutor>);
static_assert(!ApplicationAccessorIsPublic<Application>);
static_assert(!ApplicationAccessorAllowsSubmission<Application>);
static_assert(!std::is_constructible_v<PreparedEditExecutor, Document*>);
static_assert(!std::is_constructible_v<PreparedEditExecutor, DocumentObject*>);
static_assert(std::is_copy_constructible_v<PreparedEditExecutionSnapshot>);
static_assert(std::is_copy_assignable_v<PreparedEditExecutionSnapshot>);
static_assert(!std::is_copy_constructible_v<PreparedEditExecutionResult>);
static_assert(!std::is_copy_assignable_v<PreparedEditExecutionResult>);
static_assert(std::is_nothrow_move_constructible_v<PreparedEditExecutionResult>);
static_assert(std::is_nothrow_move_assignable_v<PreparedEditExecutionResult>);

class ReentryProbe
{
public:
    void markReady()
    {
        {
            std::lock_guard lock(_mutex);
            _ready = true;
        }
        _condition.notify_all();
    }

    [[nodiscard]] bool waitUntilReady(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _condition.wait_for(lock, timeout, [this] {
            return _ready;
        });
    }

    void recordStatusReentry(bool statusWasVisible)
    {
        {
            std::lock_guard lock(_mutex);
            _called = true;
            _statusWasVisible = statusWasVisible;
        }
        _condition.notify_all();
    }

    [[nodiscard]] bool waitUntilCalled(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _condition.wait_for(lock, timeout, [this] {
            return _called;
        });
    }

    [[nodiscard]] bool statusWasVisible() const
    {
        std::lock_guard lock(_mutex);
        return _statusWasVisible;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    bool _ready {false};
    bool _called {false};
    bool _statusWasVisible {false};
};

class TestOperation final: public CollaborativeOperation
{
public:
    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return "test.prepared-executor";
    }

    void apply(Document&) const override
    {}

    [[nodiscard]] CollaborativePostconditionResult
    checkPostcondition(const Document&) const override
    {
        return {true, {}};
    }
};

class StatusReenteringDestructorOperation final: public CollaborativeOperation
{
public:
    StatusReenteringDestructorOperation(
        PreparedEditExecutor& executor,
        std::shared_ptr<std::atomic<PreparedEditExecutionId>> id,
        std::shared_ptr<ReentryProbe> probe)
        : _executor(executor)
        , _id(std::move(id))
        , _probe(std::move(probe))
    {}

    ~StatusReenteringDestructorOperation() override
    {
        try {
            _probe->recordStatusReentry(
                _executor.status(_id->load(std::memory_order_acquire)).has_value());
        }
        catch (...) {
            try {
                _probe->recordStatusReentry(false);
            }
            catch (...) {
            }
        }
    }

    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return "test.prepared-executor-reentrant-destructor";
    }

    void apply(Document&) const override
    {}

    [[nodiscard]] CollaborativePostconditionResult
    checkPostcondition(const Document&) const override
    {
        return {true, {}};
    }

private:
    PreparedEditExecutor& _executor;
    std::shared_ptr<std::atomic<PreparedEditExecutionId>> _id;
    std::shared_ptr<ReentryProbe> _probe;
};

std::unique_ptr<const CollaborativeOperation> makeOperation()
{
    return std::make_unique<TestOperation>();
}

bool terminal(PreparedEditExecutionStatus status)
{
    return status == PreparedEditExecutionStatus::Completed
        || status == PreparedEditExecutionStatus::Cancelled
        || status == PreparedEditExecutionStatus::Failed;
}

template<typename Predicate>
std::optional<PreparedEditExecutionSnapshot>
waitForStatus(PreparedEditExecutor& executor,
              PreparedEditExecutionId id,
              Predicate predicate,
              std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = executor.status(id);
        if (snapshot && predicate(snapshot->status)) {
            return snapshot;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

bool waitUntilMissing(PreparedEditExecutor& executor,
                      PreparedEditExecutionId id,
                      std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!executor.status(id)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

class WorkControl
{
public:
    void enterAndWait(std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        ++_entered;
        ++_active;
        _maxActive = std::max(_maxActive, _active);
        _condition.notify_all();
        _condition.wait(lock, stopToken, [this] {
            return _released;
        });
        if (stopToken.stop_requested()) {
            _sawStop = true;
        }
        --_active;
        _condition.notify_all();
    }

    void enterAndWaitForRelease(std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        ++_entered;
        ++_active;
        _maxActive = std::max(_maxActive, _active);
        _condition.notify_all();
        _condition.wait(lock, [this] {
            return _released;
        });
        if (stopToken.stop_requested()) {
            _sawStop = true;
        }
        --_active;
        _condition.notify_all();
    }

    [[nodiscard]] bool waitUntilEntered(std::size_t count,
                                        std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _condition.wait_for(lock, timeout, [this, count] {
            return _entered >= count;
        });
    }

    void release()
    {
        {
            std::lock_guard lock(_mutex);
            _released = true;
        }
        _condition.notify_all();
    }

    [[nodiscard]] std::size_t maxActive() const
    {
        std::lock_guard lock(_mutex);
        return _maxActive;
    }

    [[nodiscard]] bool sawStop() const
    {
        std::lock_guard lock(_mutex);
        return _sawStop;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable_any _condition;
    std::size_t _entered {0};
    std::size_t _active {0};
    std::size_t _maxActive {0};
    bool _released {false};
    bool _sawStop {false};
};

DetachedTask controlledTask(const std::shared_ptr<WorkControl>& control)
{
    return [control](std::stop_token stopToken) {
        control->enterAndWait(stopToken);
        return makeOperation();
    };
}

DetachedTask releaseControlledTask(const std::shared_ptr<WorkControl>& control)
{
    return [control](std::stop_token stopToken) {
        control->enterAndWaitForRelease(stopToken);
        return makeOperation();
    };
}

PreparedEditExecutionId submit(PreparedEditExecutor& executor, DetachedTask task)
{
    return Internal::PreparedEditExecutorTestAccess::submit(
        executor, std::move(task), PreparationPolicy::DetachedInProcess);
}

bool abandon(PreparedEditExecutor& executor, PreparedEditExecutionId id)
{
    return Internal::PreparedEditExecutorTestAccess::abandon(executor, id);
}

}  // namespace

TEST(PreparedEditExecutorTest, exposesBoundedPoolConfiguration)
{
    PreparedEditExecutor defaultExecutor;
    EXPECT_GE(defaultExecutor.workerCount(), 2U);
    EXPECT_LE(defaultExecutor.workerCount(), 4U);
    EXPECT_EQ(defaultExecutor.queueCapacity(), 64U);

    PreparedEditExecutor configuredExecutor(1, 7);
    EXPECT_EQ(configuredExecutor.workerCount(), 1U);
    EXPECT_EQ(configuredExecutor.queueCapacity(), 7U);
    EXPECT_THROW(PreparedEditExecutor(1, 0), std::invalid_argument);
}

TEST(PreparedEditExecutorTest, rejectsInlineAndIsolatedProcessPolicies)
{
    PreparedEditExecutor executor(1, 1);
    EXPECT_THROW(
        Internal::PreparedEditExecutorTestAccess::submit(
            executor,
            [](std::stop_token) {
                return makeOperation();
            },
            PreparationPolicy::Inline),
        std::invalid_argument);
    EXPECT_THROW(
        Internal::PreparedEditExecutorTestAccess::submit(
            executor,
            [](std::stop_token) {
                return makeOperation();
            },
            PreparationPolicy::IsolatedProcess),
        std::invalid_argument);
}

TEST(PreparedEditExecutorTest, rejectsEmptyTasksAndBoundsWaitingQueue)
{
    PreparedEditExecutor executor(1, 1);
    EXPECT_THROW(submit(executor, {}), std::invalid_argument);

    auto runningControl = std::make_shared<WorkControl>();
    const auto runningId = submit(executor, controlledTask(runningControl));
    ASSERT_TRUE(runningControl->waitUntilEntered(1));

    const auto queuedId = submit(executor, [](std::stop_token) {
        return makeOperation();
    });
    const auto queued = executor.status(queuedId);
    ASSERT_TRUE(queued.has_value());
    EXPECT_EQ(queued->status, PreparedEditExecutionStatus::Queued);
    EXPECT_THROW(submit(executor, [](std::stop_token) {
                     return makeOperation();
                 }),
                 std::runtime_error);

    EXPECT_TRUE(executor.cancel(queuedId));
    runningControl->release();
    ASSERT_TRUE(waitForStatus(executor, runningId, terminal).has_value());
}

TEST(PreparedEditExecutorTest, RetainsTerminalResultUntilItIsConsumed)
{
    PreparedEditExecutor executor(1, 2);
    auto control = std::make_shared<WorkControl>();
    const auto id = submit(executor, controlledTask(control));

    ASSERT_TRUE(control->waitUntilEntered(1));
    const auto running = executor.status(id);
    ASSERT_TRUE(running.has_value());
    EXPECT_EQ(running->id, id);
    EXPECT_EQ(running->status, PreparedEditExecutionStatus::Running);
    EXPECT_TRUE(running->diagnostic.empty());
    EXPECT_FALSE(executor.takeResult(id).has_value());

    control->release();
    const auto completed = waitForStatus(executor, id, [](auto status) {
        return status == PreparedEditExecutionStatus::Completed;
    });
    ASSERT_TRUE(completed.has_value());
    EXPECT_TRUE(executor.status(id).has_value());

    auto result = executor.takeResult(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id, id);
    EXPECT_EQ(result->status, PreparedEditExecutionStatus::Completed);
    EXPECT_NE(result->operation, nullptr);
    EXPECT_TRUE(result->diagnostic.empty());
    EXPECT_FALSE(executor.status(id).has_value());
    EXPECT_FALSE(executor.takeResult(id).has_value());
    EXPECT_FALSE(executor.cancel(id));
}

TEST(PreparedEditExecutorTest, CancelsQueuedTaskWithoutExecutingIt)
{
    PreparedEditExecutor executor(1, 2);
    auto runningControl = std::make_shared<WorkControl>();
    const auto runningId = submit(executor, controlledTask(runningControl));
    ASSERT_TRUE(runningControl->waitUntilEntered(1));

    std::atomic_bool queuedTaskRan {false};
    const auto queuedId = submit(executor, [&queuedTaskRan](std::stop_token) {
        queuedTaskRan = true;
        return makeOperation();
    });
    EXPECT_TRUE(executor.cancel(queuedId));
    EXPECT_FALSE(executor.cancel(queuedId));

    auto cancelled = executor.takeResult(queuedId);
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status, PreparedEditExecutionStatus::Cancelled);
    EXPECT_EQ(cancelled->operation, nullptr);
    EXPECT_TRUE(cancelled->diagnostic.empty());

    runningControl->release();
    ASSERT_TRUE(waitForStatus(executor, runningId, terminal).has_value());
    EXPECT_FALSE(queuedTaskRan);
}

TEST(PreparedEditExecutorTest, RunningCancellationIsCooperativeAndDiscardsResult)
{
    PreparedEditExecutor executor(1, 1);
    auto control = std::make_shared<WorkControl>();
    const auto id = submit(executor, controlledTask(control));
    ASSERT_TRUE(control->waitUntilEntered(1));

    EXPECT_TRUE(executor.cancel(id));
    const auto snapshot = waitForStatus(executor, id, [](auto status) {
        return status == PreparedEditExecutionStatus::Cancelled;
    });
    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(control->sawStop());

    auto result = executor.takeResult(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, PreparedEditExecutionStatus::Cancelled);
    EXPECT_EQ(result->operation, nullptr);
    EXPECT_TRUE(result->diagnostic.empty());
}

TEST(PreparedEditExecutorTest, StopCallbackCanReenterStatusWithoutExecutorLock)
{
    PreparedEditExecutor executor(1, 1);
    auto executionId = std::make_shared<std::atomic<PreparedEditExecutionId>>(0);
    auto probe = std::make_shared<ReentryProbe>();
    const auto id = submit(
        executor,
        [executorPointer = &executor, executionId, probe](std::stop_token stopToken) {
            std::stop_callback callback(stopToken, [executorPointer, executionId, probe] {
                bool statusWasVisible = false;
                try {
                    statusWasVisible = executorPointer
                                           ->status(executionId->load(
                                               std::memory_order_acquire))
                                           .has_value();
                }
                catch (...) {
                }
                probe->recordStatusReentry(statusWasVisible);
            });
            probe->markReady();
            while (!stopToken.stop_requested()) {
                std::this_thread::yield();
            }
            return makeOperation();
        });
    executionId->store(id, std::memory_order_release);
    ASSERT_TRUE(probe->waitUntilReady());

    EXPECT_TRUE(executor.cancel(id));
    EXPECT_TRUE(probe->waitUntilCalled());
    EXPECT_TRUE(probe->statusWasVisible());
    ASSERT_TRUE(waitForStatus(executor, id, [](auto status) {
        return status == PreparedEditExecutionStatus::Cancelled;
    }).has_value());
    EXPECT_TRUE(executor.takeResult(id).has_value());
}

TEST(PreparedEditExecutorTest, DiscardedOperationDestructorCanReenterStatus)
{
    PreparedEditExecutor executor(1, 1);
    auto control = std::make_shared<WorkControl>();
    auto executionId = std::make_shared<std::atomic<PreparedEditExecutionId>>(0);
    auto probe = std::make_shared<ReentryProbe>();
    const auto id = submit(
        executor,
        [executorPointer = &executor, executionId, probe, control](
            std::stop_token stopToken) -> std::unique_ptr<const CollaborativeOperation> {
            control->enterAndWaitForRelease(stopToken);
            return std::make_unique<StatusReenteringDestructorOperation>(
                *executorPointer, executionId, probe);
        });
    executionId->store(id, std::memory_order_release);
    ASSERT_TRUE(control->waitUntilEntered(1));

    EXPECT_TRUE(executor.cancel(id));
    control->release();
    EXPECT_TRUE(probe->waitUntilCalled());
    EXPECT_TRUE(probe->statusWasVisible());
    ASSERT_TRUE(waitForStatus(executor, id, [](auto status) {
        return status == PreparedEditExecutionStatus::Cancelled;
    }).has_value());
    auto result = executor.takeResult(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->operation, nullptr);
}

TEST(PreparedEditExecutorTest, AbandonRemovesQueuedAndTerminalJobs)
{
    PreparedEditExecutor executor(1, 2);
    auto runningControl = std::make_shared<WorkControl>();
    const auto runningId = submit(executor, controlledTask(runningControl));
    ASSERT_TRUE(runningControl->waitUntilEntered(1));

    std::atomic_bool queuedTaskRan {false};
    const auto queuedId = submit(executor, [&queuedTaskRan](std::stop_token) {
        queuedTaskRan = true;
        return makeOperation();
    });
    EXPECT_TRUE(abandon(executor, queuedId));
    EXPECT_FALSE(abandon(executor, queuedId));
    EXPECT_FALSE(executor.status(queuedId).has_value());
    EXPECT_FALSE(executor.takeResult(queuedId).has_value());

    const auto replacementId = submit(executor, [](std::stop_token) {
        return makeOperation();
    });
    runningControl->release();
    ASSERT_TRUE(waitForStatus(executor, runningId, terminal).has_value());
    ASSERT_TRUE(waitForStatus(executor, replacementId, [](auto status) {
        return status == PreparedEditExecutionStatus::Completed;
    }).has_value());
    EXPECT_TRUE(abandon(executor, replacementId));
    EXPECT_FALSE(executor.status(replacementId).has_value());
    EXPECT_FALSE(executor.takeResult(replacementId).has_value());
    EXPECT_FALSE(queuedTaskRan);
}

TEST(PreparedEditExecutorTest, AbandonOfRunningJobDoesNotJoinAndAutoReaps)
{
    PreparedEditExecutor executor(1, 1);
    auto control = std::make_shared<WorkControl>();
    const auto id = submit(executor, releaseControlledTask(control));
    ASSERT_TRUE(control->waitUntilEntered(1));

    EXPECT_TRUE(abandon(executor, id));
    EXPECT_FALSE(abandon(executor, id));
    const auto stillRunning = executor.status(id);
    const bool resultWasAvailable = executor.takeResult(id).has_value();
    control->release();

    ASSERT_TRUE(stillRunning.has_value());
    EXPECT_EQ(stillRunning->status, PreparedEditExecutionStatus::Running);
    EXPECT_FALSE(resultWasAvailable);
    EXPECT_TRUE(waitUntilMissing(executor, id));
    EXPECT_TRUE(control->sawStop());
    EXPECT_FALSE(executor.takeResult(id).has_value());
}

TEST(PreparedEditExecutorTest, SimultaneousTerminalTakeHasExactlyOneWinner)
{
    PreparedEditExecutor executor(1, 1);
    const auto id = submit(executor, [](std::stop_token) {
        return makeOperation();
    });
    ASSERT_TRUE(waitForStatus(executor, id, [](auto status) {
        return status == PreparedEditExecutionStatus::Completed;
    }).has_value());

    std::array<std::optional<PreparedEditExecutionResult>, 2> results;
    std::barrier<> start(3);
    std::array<std::thread, 2> consumers {
        std::thread([&] {
            start.arrive_and_wait();
            results[0] = executor.takeResult(id);
        }),
        std::thread([&] {
            start.arrive_and_wait();
            results[1] = executor.takeResult(id);
        })};

    start.arrive_and_wait();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    const auto winners = static_cast<unsigned int>(results[0].has_value())
        + static_cast<unsigned int>(results[1].has_value());
    EXPECT_EQ(winners, 1U);
    const auto& winner = results[0] ? results[0] : results[1];
    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(winner->status, PreparedEditExecutionStatus::Completed);
    EXPECT_NE(winner->operation, nullptr);
    EXPECT_FALSE(executor.status(id).has_value());
}

TEST(PreparedEditExecutorTest, AbandonRacingTaskCompletionAlwaysReaps)
{
    std::barrier<> start(3);
    PreparedEditExecutor executor(1, 1);
    const auto id = submit(executor, [&start](std::stop_token) {
        start.arrive_and_wait();
        return makeOperation();
    });

    bool abandonAccepted = false;
    std::thread abandoning([&] {
        start.arrive_and_wait();
        abandonAccepted = abandon(executor, id);
    });
    start.arrive_and_wait();
    abandoning.join();

    EXPECT_TRUE(abandonAccepted);
    EXPECT_TRUE(waitUntilMissing(executor, id));
    EXPECT_FALSE(executor.takeResult(id).has_value());
}

TEST(PreparedEditExecutorTest, CompletionCancelAndTakeRaceHasOneTerminalResult)
{
    std::barrier<> start(4);
    PreparedEditExecutor executor(1, 1);
    const auto id = submit(executor, [&start](std::stop_token) {
        start.arrive_and_wait();
        return makeOperation();
    });

    bool cancellationAccepted = false;
    std::optional<PreparedEditExecutionResult> racedResult;
    std::thread cancelling([&] {
        start.arrive_and_wait();
        cancellationAccepted = executor.cancel(id);
    });
    std::thread taking([&] {
        start.arrive_and_wait();
        racedResult = executor.takeResult(id);
    });
    start.arrive_and_wait();
    cancelling.join();
    taking.join();

    if (!racedResult) {
        ASSERT_TRUE(waitForStatus(executor, id, terminal).has_value());
        racedResult = executor.takeResult(id);
    }
    ASSERT_TRUE(racedResult.has_value());
    EXPECT_EQ(racedResult->status,
              cancellationAccepted ? PreparedEditExecutionStatus::Cancelled
                                   : PreparedEditExecutionStatus::Completed);
    EXPECT_EQ(racedResult->operation != nullptr,
              !cancellationAccepted);
    EXPECT_FALSE(executor.takeResult(id).has_value());
    EXPECT_FALSE(executor.status(id).has_value());
}

TEST(PreparedEditExecutorTest, ConvertsTaskExceptionsToFailedResults)
{
    PreparedEditExecutor executor(2, 3);
    const auto baseId = submit(executor, [](std::stop_token)
                                            -> std::unique_ptr<const CollaborativeOperation> {
        throw Base::RuntimeError("base preparation failure");
    });
    const auto standardId = submit(executor, [](std::stop_token)
                                                -> std::unique_ptr<const CollaborativeOperation> {
        throw std::runtime_error("standard preparation failure");
    });
    const auto unknownId = submit(executor, [](std::stop_token)
                                               -> std::unique_ptr<const CollaborativeOperation> {
        throw 7;
    });

    for (const auto id : {baseId, standardId, unknownId}) {
        ASSERT_TRUE(waitForStatus(executor, id, [](auto status) {
            return status == PreparedEditExecutionStatus::Failed;
        }).has_value());
    }

    auto baseResult = executor.takeResult(baseId);
    auto standardResult = executor.takeResult(standardId);
    auto unknownResult = executor.takeResult(unknownId);
    ASSERT_TRUE(baseResult.has_value());
    ASSERT_TRUE(standardResult.has_value());
    ASSERT_TRUE(unknownResult.has_value());
    EXPECT_NE(baseResult->diagnostic.find("base preparation failure"), std::string::npos);
    EXPECT_NE(standardResult->diagnostic.find("standard preparation failure"),
              std::string::npos);
    EXPECT_NE(unknownResult->diagnostic.find("unknown exception"), std::string::npos);
    EXPECT_EQ(baseResult->operation, nullptr);
    EXPECT_EQ(standardResult->operation, nullptr);
    EXPECT_EQ(unknownResult->operation, nullptr);
}

TEST(PreparedEditExecutorTest, IndependentWorkersActuallyOverlap)
{
    PreparedEditExecutor executor(2, 2);
    auto control = std::make_shared<WorkControl>();
    const auto firstId = submit(executor, controlledTask(control));
    const auto secondId = submit(executor, controlledTask(control));

    const bool overlapped = control->waitUntilEntered(2);
    control->release();
    EXPECT_TRUE(overlapped);
    EXPECT_GE(control->maxActive(), 2U);
    ASSERT_TRUE(waitForStatus(executor, firstId, terminal).has_value());
    ASSERT_TRUE(waitForStatus(executor, secondId, terminal).has_value());
}

TEST(PreparedEditExecutorTest, ShutdownStopsRunningWorkAndSkipsQueuedWork)
{
    auto runningControl = std::make_shared<WorkControl>();
    std::atomic_bool queuedTaskRan {false};
    {
        PreparedEditExecutor executor(1, 1);
        (void)submit(executor, controlledTask(runningControl));
        ASSERT_TRUE(runningControl->waitUntilEntered(1));
        (void)submit(executor, [&queuedTaskRan](std::stop_token) {
            queuedTaskRan = true;
            return makeOperation();
        });
    }

    EXPECT_TRUE(runningControl->sawStop());
    EXPECT_FALSE(queuedTaskRan);
}
