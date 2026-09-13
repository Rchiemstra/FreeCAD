// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/GeometryArchive.h>
#include <App/GeometryJobManager.h>

#include <chrono>
#include <cmath>
#include <concepts>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace App;
using namespace std::chrono_literals;

namespace App::Internal
{

class GeometryJobManagerTestAccess
{
public:
    static std::optional<GeometryJobDispatch> takeNext(GeometryJobManager& manager)
    {
        return manager.takeNext();
    }

    static std::optional<GeometryJobDispatch> takeNext(GeometryJobManager& manager,
                                                       GeometryArchive& archive)
    {
        return manager.takeNext(archive);
    }

    static bool reportProgress(GeometryJobManager& manager,
                               GeometryJobId id,
                               GeometryJobProgress progress)
    {
        return manager.reportProgress(id, std::move(progress));
    }

    static bool finish(GeometryJobManager& manager, GeometryJobResult result)
    {
        return manager.finish(std::move(result));
    }
};

}  // namespace App::Internal

namespace
{

template<typename ApplicationType>
concept HasPublicGeometryManager = requires(ApplicationType& application) {
    { application.geometryJobManager() } -> std::same_as<GeometryJobManager&>;
};

static_assert(HasPublicGeometryManager<Application>);
static_assert(!std::is_constructible_v<GeometryJobRequest, Document*>);
static_assert(!std::is_constructible_v<GeometryJobRequest, DocumentObject*>);
static_assert(!std::is_constructible_v<GeometryJobStatus, Document*>);
static_assert(!std::is_constructible_v<GeometryJobResult, DocumentObject*>);
static_assert(std::is_copy_constructible_v<GeometryJobRequest>);
static_assert(std::is_copy_constructible_v<GeometryJobStatus>);
static_assert(std::is_copy_constructible_v<GeometryJobResult>);

GeometryJobRequest request(std::string key,
                           GeometryJobCoalescing coalescing = GeometryJobCoalescing::None,
                           std::string digest = "digest")
{
    GeometryJobRequest value;
    value.operationType = "test.geometry";
    value.coalescingKey = std::move(key);
    value.inputDigest = std::move(digest);
    value.coalescing = coalescing;
    value.deadline = std::chrono::steady_clock::now() + 2s;
    return value;
}

std::optional<GeometryJobStatus> waitForTerminal(GeometryJobManager& manager,
                                                 GeometryJobId id,
                                                 std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = manager.status(id);
        if (snapshot
            && (snapshot->state == GeometryJobState::Completed
                || snapshot->state == GeometryJobState::Cancelled
                || snapshot->state == GeometryJobState::DeadlineExceeded
                || snapshot->state == GeometryJobState::WorkerCrashed
                || snapshot->state == GeometryJobState::WorkerOutOfMemory
                || snapshot->state == GeometryJobState::Failed)) {
            return snapshot;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

GeometryJobResult completed(GeometryJobId id)
{
    return GeometryJobResult {
        id, GeometryJobState::Completed, "result.fcg", "result-digest", {}};
}

}  // namespace

TEST(GeometryJobManagerTest, validatesBoundedIsolatedRequests)
{
    EXPECT_THROW(GeometryJobManager(0, 1), std::invalid_argument);
    EXPECT_THROW(GeometryJobManager(1, 0), std::invalid_argument);

    GeometryJobManager manager(1, 2);
    EXPECT_EQ(manager.activeCapacity(), 1U);
    EXPECT_EQ(manager.queueCapacity(), 2U);

    auto missingOperation = request("missing");
    missingOperation.operationType.clear();
    EXPECT_THROW(static_cast<void>(manager.submit(std::move(missingOperation))),
                 std::invalid_argument);

    auto inlineRequest = request("inline");
    inlineRequest.policy = PreparationPolicy::Inline;
    EXPECT_THROW(static_cast<void>(manager.submit(std::move(inlineRequest))),
                 std::invalid_argument);

    auto detachedRequest = request("detached");
    detachedRequest.policy = PreparationPolicy::DetachedInProcess;
    EXPECT_THROW(static_cast<void>(manager.submit(std::move(detachedRequest))),
                 std::invalid_argument);

    auto missingKey = request("", GeometryJobCoalescing::LatestWins);
    EXPECT_THROW(static_cast<void>(manager.submit(std::move(missingKey))),
                 std::invalid_argument);

    auto oversizedDigest = request("oversized");
    oversizedDigest.inputDigest.assign(257, 'd');
    EXPECT_THROW(static_cast<void>(manager.submit(std::move(oversizedDigest))),
                 std::invalid_argument);
}

TEST(GeometryJobManagerTest, processDispatchAtomicallyCarriesItsPointerFreeArchive)
{
    GeometryJobManager manager(1, 2);
    const auto manualId = manager.submit(request("manual"));
    GeometryArchive input;
    input.sections = {{"payload", {1, 2, 3}}};
    const auto processId = manager.submit(request("process"), input);

    GeometryArchive dispatchedArchive;
    const auto processDispatch =
        Internal::GeometryJobManagerTestAccess::takeNext(manager, dispatchedArchive);
    ASSERT_TRUE(processDispatch.has_value());
    EXPECT_EQ(processDispatch->id, processId);
    EXPECT_EQ(dispatchedArchive.sections, input.sections);
    EXPECT_EQ(manager.queuedCount(), 1U);

    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(
        manager, completed(processId)));
    const auto manualDispatch = Internal::GeometryJobManagerTestAccess::takeNext(manager);
    ASSERT_TRUE(manualDispatch.has_value());
    EXPECT_EQ(manualDispatch->id, manualId);
    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(
        manager, completed(manualId)));
}

TEST(GeometryJobManagerTest, boundsQueueAndActiveDispatch)
{
    GeometryJobManager manager(1, 1);
    const auto first = manager.submit(request("first"));
    auto firstDispatch = Internal::GeometryJobManagerTestAccess::takeNext(manager);
    ASSERT_TRUE(firstDispatch.has_value());
    EXPECT_EQ(firstDispatch->id, first);
    EXPECT_EQ(manager.activeCount(), 1U);
    EXPECT_EQ(manager.queuedCount(), 0U);

    const auto second = manager.submit(request("second"));
    EXPECT_EQ(manager.queuedCount(), 1U);
    EXPECT_THROW(static_cast<void>(manager.submit(request("third"))), GeometryJobQueueFull);
    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());

    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::finish(manager, completed(first)));
    auto secondDispatch = Internal::GeometryJobManagerTestAccess::takeNext(manager);
    ASSERT_TRUE(secondDispatch.has_value());
    EXPECT_EQ(secondDispatch->id, second);
    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(manager, completed(second)));
    EXPECT_EQ(manager.activeCount(), 0U);
}

TEST(GeometryJobManagerTest, reportsMonotonicProgressAndConsumesResultExactlyOnce)
{
    GeometryJobManager manager(1, 2);
    const auto id = manager.submit(request("progress"));
    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());

    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::reportProgress(
        manager, id, GeometryJobProgress {-0.1, "invalid"}));
    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::reportProgress(
        manager, id, GeometryJobProgress {std::numeric_limits<double>::quiet_NaN(), "invalid"}));
    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::reportProgress(
        manager, id, GeometryJobProgress {0.5, "occ.boolean"}));
    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::reportProgress(
        manager, id, GeometryJobProgress {0.4, "regression"}));

    auto running = manager.status(id);
    ASSERT_TRUE(running.has_value());
    EXPECT_EQ(running->state, GeometryJobState::Running);
    EXPECT_DOUBLE_EQ(running->progress.fraction, 0.5);
    EXPECT_EQ(running->progress.phase, "occ.boolean");

    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(manager, completed(id)));
    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::finish(manager, completed(id)));
    auto terminal = manager.status(id);
    ASSERT_TRUE(terminal.has_value());
    EXPECT_EQ(terminal->state, GeometryJobState::Completed);
    EXPECT_DOUBLE_EQ(terminal->progress.fraction, 1.0);

    auto result = manager.takeResult(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, GeometryJobState::Completed);
    EXPECT_EQ(result->resultArtifact, "result.fcg");
    EXPECT_EQ(result->resultDigest, "result-digest");
    EXPECT_FALSE(manager.takeResult(id).has_value());
    EXPECT_FALSE(manager.status(id).has_value());
}

TEST(GeometryJobManagerTest, cancellationHandlesQueuedAndRunningJobs)
{
    GeometryJobManager manager(1, 2);
    const auto queuedId = manager.submit(request("queued"));
    EXPECT_TRUE(manager.cancel(queuedId));
    EXPECT_FALSE(manager.cancel(queuedId));
    auto queuedResult = manager.takeResult(queuedId);
    ASSERT_TRUE(queuedResult.has_value());
    EXPECT_EQ(queuedResult->state, GeometryJobState::Cancelled);

    const auto runningId = manager.submit(request("running"));
    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());
    EXPECT_TRUE(manager.cancel(runningId));
    auto cancelling = manager.status(runningId);
    ASSERT_TRUE(cancelling.has_value());
    EXPECT_EQ(cancelling->state, GeometryJobState::Cancelling);
    EXPECT_TRUE(cancelling->cancellationRequested);

    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(
        manager, completed(runningId)));
    auto runningResult = manager.takeResult(runningId);
    ASSERT_TRUE(runningResult.has_value());
    EXPECT_EQ(runningResult->state, GeometryJobState::Cancelled);
    EXPECT_TRUE(runningResult->resultArtifact.empty());
    EXPECT_TRUE(runningResult->resultDigest.empty());
}

TEST(GeometryJobManagerTest, coalescesIdenticalAndLatestWinsWithoutPartialReplacement)
{
    GeometryJobManager manager(1, 2);
    const auto joined = manager.submit(
        request("join", GeometryJobCoalescing::JoinIdentical, "same"));
    EXPECT_EQ(manager.submit(
                  request("join", GeometryJobCoalescing::JoinIdentical, "same")),
              joined);
    EXPECT_THROW(static_cast<void>(manager.submit(
                     request("join", GeometryJobCoalescing::JoinIdentical, "different"))),
                 std::invalid_argument);
    EXPECT_TRUE(manager.cancel(joined));
    ASSERT_TRUE(manager.takeResult(joined).has_value());

    const auto oldId = manager.submit(
        request("latest", GeometryJobCoalescing::LatestWins, "old"));
    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());
    const auto newId = manager.submit(
        request("latest", GeometryJobCoalescing::LatestWins, "new"));
    EXPECT_NE(newId, oldId);
    auto oldStatus = manager.status(oldId);
    ASSERT_TRUE(oldStatus.has_value());
    EXPECT_EQ(oldStatus->state, GeometryJobState::Cancelling);
    EXPECT_TRUE(oldStatus->cancellationRequested);
    EXPECT_EQ(manager.queuedCount(), 1U);

    EXPECT_TRUE(Internal::GeometryJobManagerTestAccess::finish(manager, completed(oldId)));
    auto oldResult = manager.takeResult(oldId);
    ASSERT_TRUE(oldResult.has_value());
    EXPECT_EQ(oldResult->state, GeometryJobState::Cancelled);
    auto newDispatch = Internal::GeometryJobManagerTestAccess::takeNext(manager);
    ASSERT_TRUE(newDispatch.has_value());
    EXPECT_EQ(newDispatch->id, newId);
}

TEST(GeometryJobManagerTest, rejectedReplacementNeverCancelsTheActiveJob)
{
    GeometryJobManager manager(1, 1);
    const auto activeId = manager.submit(
        request("latest", GeometryJobCoalescing::LatestWins, "active"));
    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());
    const auto queuedId = manager.submit(request("queue-filler"));

    EXPECT_THROW(static_cast<void>(manager.submit(
                     request("latest", GeometryJobCoalescing::LatestWins, "replacement"))),
                 std::runtime_error);
    auto active = manager.status(activeId);
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->state, GeometryJobState::Running);
    EXPECT_FALSE(active->cancellationRequested);
    EXPECT_TRUE(manager.status(queuedId).has_value());
    EXPECT_EQ(manager.queuedCount(), 1U);

    auto expiredReplacement = request(
        "latest", GeometryJobCoalescing::LatestWins, "expired-replacement");
    expiredReplacement.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto expiredId = manager.submit(std::move(expiredReplacement));
    auto expired = manager.takeResult(expiredId);
    ASSERT_TRUE(expired.has_value());
    EXPECT_EQ(expired->state, GeometryJobState::DeadlineExceeded);
    active = manager.status(activeId);
    ASSERT_TRUE(active.has_value());
    EXPECT_EQ(active->state, GeometryJobState::Running);
    EXPECT_FALSE(active->cancellationRequested);
}

TEST(GeometryJobManagerTest, expiresQueuedAndRunningDeadlinesWithoutPollingDependency)
{
    GeometryJobManager manager(1, 2);
    auto alreadyExpired = request("expired");
    alreadyExpired.deadline = std::chrono::steady_clock::now() - 1ms;
    const auto expiredId = manager.submit(std::move(alreadyExpired));
    auto expired = manager.takeResult(expiredId);
    ASSERT_TRUE(expired.has_value());
    EXPECT_EQ(expired->state, GeometryJobState::DeadlineExceeded);

    auto queuedRequest = request("queued-deadline");
    queuedRequest.deadline = std::chrono::steady_clock::now() + 30ms;
    const auto queuedId = manager.submit(std::move(queuedRequest));
    auto queuedTerminal = waitForTerminal(manager, queuedId);
    ASSERT_TRUE(queuedTerminal.has_value());
    EXPECT_EQ(queuedTerminal->state, GeometryJobState::DeadlineExceeded);
    EXPECT_EQ(manager.queuedCount(), 0U);

    auto runningRequest = request("running-deadline");
    runningRequest.deadline = std::chrono::steady_clock::now() + 30ms;
    const auto runningId = manager.submit(std::move(runningRequest));
    ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());
    auto runningTerminal = waitForTerminal(manager, runningId);
    ASSERT_TRUE(runningTerminal.has_value());
    EXPECT_EQ(runningTerminal->state, GeometryJobState::DeadlineExceeded);
    EXPECT_TRUE(runningTerminal->cancellationRequested);
    EXPECT_EQ(manager.activeCount(), 0U);
    EXPECT_FALSE(Internal::GeometryJobManagerTestAccess::finish(
        manager, completed(runningId)));
}

TEST(GeometryJobManagerTest, preservesStructuredWorkerFailureStates)
{
    for (const auto state : {GeometryJobState::WorkerCrashed,
                             GeometryJobState::WorkerOutOfMemory,
                             GeometryJobState::Failed}) {
        GeometryJobManager manager(1, 1);
        const auto id = manager.submit(request("failure"));
        ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::takeNext(manager).has_value());
        GeometryJobResult failure;
        failure.id = id;
        failure.state = state;
        failure.diagnostic = "worker terminal classification";
        ASSERT_TRUE(Internal::GeometryJobManagerTestAccess::finish(
            manager, std::move(failure)));
        auto result = manager.takeResult(id);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->state, state);
        EXPECT_EQ(result->diagnostic, "worker terminal classification");
    }
}
