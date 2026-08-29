// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryJobManager.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace App;

namespace
{

constexpr auto DefaultDeadline = std::chrono::minutes(5);
constexpr std::size_t MaxOperationTypeLength = 128;
constexpr std::size_t MaxCoalescingKeyLength = 256;
constexpr std::size_t MaxDigestLength = 256;

bool isTerminal(const GeometryJobState state) noexcept
{
    return state == GeometryJobState::Completed
        || state == GeometryJobState::Cancelled
        || state == GeometryJobState::DeadlineExceeded
        || state == GeometryJobState::WorkerCrashed
        || state == GeometryJobState::WorkerOutOfMemory
        || state == GeometryJobState::Failed;
}

bool isWorkerTerminal(const GeometryJobState state) noexcept
{
    return isTerminal(state) && state != GeometryJobState::Queued
        && state != GeometryJobState::Running && state != GeometryJobState::Cancelling;
}

std::string coalescingIdentity(const GeometryJobRequest& request)
{
    return request.operationType + "\n" + request.coalescingKey;
}

bool requestsAreIdentical(const GeometryJobRequest& left,
                          const GeometryJobRequest& right) noexcept
{
    return left.operationType == right.operationType
        && left.policy == right.policy
        && left.coalescingKey == right.coalescingKey
        && left.inputDigest == right.inputDigest;
}

void validateRequest(const GeometryJobRequest& request)
{
    if (request.operationType.empty()
        || request.operationType.size() > MaxOperationTypeLength) {
        throw std::invalid_argument("geometry job operation type is missing or oversized");
    }
    if (request.policy != PreparationPolicy::IsolatedProcess) {
        throw std::invalid_argument(
            "geometry jobs require IsolatedProcess preparation policy");
    }
    if (request.coalescing != GeometryJobCoalescing::None
        && request.coalescingKey.empty()) {
        throw std::invalid_argument("coalesced geometry job requires a key");
    }
    if (request.coalescingKey.size() > MaxCoalescingKeyLength
        || request.inputDigest.size() > MaxDigestLength) {
        throw std::invalid_argument("geometry job identity field is oversized");
    }
}

}  // namespace

class GeometryJobManager::Impl
{
public:
    struct Job
    {
        GeometryJobId id {0};
        GeometryJobRequest request;
        GeometryJobState state {GeometryJobState::Queued};
        GeometryJobProgress progress;
        bool cancellationRequested {false};
        std::string diagnostic;
        std::optional<GeometryJobResult> result;
    };

    Impl(const std::size_t requestedActiveCapacity,
         const std::size_t requestedQueueCapacity)
        : activeCapacityValue(requestedActiveCapacity)
        , queueCapacityValue(requestedQueueCapacity)
    {
        if (activeCapacityValue == 0 || queueCapacityValue == 0) {
            throw std::invalid_argument("geometry job capacities must be positive");
        }
        deadlineThread = std::thread([this] {
            monitorDeadlines();
        });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            shuttingDown = true;
            for (auto& [id, job] : jobs) {
                if (!isTerminal(job.state)) {
                    makeTerminal(job,
                                 GeometryJobState::Cancelled,
                                 "geometry job manager is shutting down");
                }
            }
            queue.clear();
            activeByKey.clear();
            activeJobs = 0;
        }
        condition.notify_all();
        if (deadlineThread.joinable()) {
            deadlineThread.join();
        }
    }

    GeometryJobId submit(GeometryJobRequest request)
    {
        validateRequest(request);
        const auto now = std::chrono::steady_clock::now();
        if (request.deadline == std::chrono::steady_clock::time_point {}) {
            request.deadline = now + DefaultDeadline;
        }

        std::lock_guard lock(mutex);
        if (shuttingDown) {
            throw std::runtime_error("geometry job manager is shutting down");
        }

        if (request.deadline <= now) {
            const GeometryJobId id = allocateId();
            Job job;
            job.id = id;
            job.request = std::move(request);
            makeTerminal(job,
                         GeometryJobState::DeadlineExceeded,
                         "geometry job deadline elapsed before admission");
            jobs.emplace(id, std::move(job));
            return id;
        }

        const std::string identity = coalescingIdentity(request);
        if (request.coalescing != GeometryJobCoalescing::None) {
            const auto active = activeByKey.find(identity);
            if (active != activeByKey.end()) {
                const auto found = jobs.find(active->second);
                if (found != jobs.end() && !isTerminal(found->second.state)) {
                    if (request.coalescing == GeometryJobCoalescing::JoinIdentical) {
                        if (!requestsAreIdentical(found->second.request, request)) {
                            throw std::invalid_argument(
                                "JoinIdentical geometry request does not match active job");
                        }
                        return found->second.id;
                    }
                    if (found->second.state != GeometryJobState::Queued
                        && queue.size() >= queueCapacityValue) {
                        throw std::runtime_error("geometry job queue is full");
                    }
                    supersede(found->second);
                }
            }
        }

        if (queue.size() >= queueCapacityValue) {
            throw std::runtime_error("geometry job queue is full");
        }
        const GeometryJobId id = allocateId();
        Job job;
        job.id = id;
        job.request = std::move(request);
        const auto [position, inserted] = jobs.emplace(id, std::move(job));
        if (!inserted) {
            throw std::runtime_error("duplicate geometry job identity");
        }
        try {
            queue.push_back(id);
        }
        catch (...) {
            jobs.erase(position);
            throw;
        }
        if (position->second.request.coalescing != GeometryJobCoalescing::None) {
            activeByKey[identity] = id;
        }
        condition.notify_all();
        return id;
    }

    bool cancel(const GeometryJobId id)
    {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end() || isTerminal(found->second.state)
            || found->second.cancellationRequested) {
            return false;
        }
        auto& job = found->second;
        job.cancellationRequested = true;
        if (job.state == GeometryJobState::Queued) {
            eraseQueued(id);
            makeTerminal(job, GeometryJobState::Cancelled, "geometry job cancelled before start");
            eraseActiveIdentity(job);
        }
        else {
            job.state = GeometryJobState::Cancelling;
            job.diagnostic = "geometry job cancellation requested";
        }
        condition.notify_all();
        return true;
    }

    std::optional<GeometryJobStatus> status(const GeometryJobId id) const
    {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end()) {
            return std::nullopt;
        }
        const auto& job = found->second;
        return GeometryJobStatus {job.id,
                                  job.state,
                                  job.progress,
                                  job.request.deadline,
                                  job.cancellationRequested,
                                  job.diagnostic};
    }

    std::optional<GeometryJobResult> takeResult(const GeometryJobId id)
    {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end() || !found->second.result) {
            return std::nullopt;
        }
        auto result = std::move(found->second.result);
        jobs.erase(found);
        return result;
    }

    std::optional<GeometryJobDispatch> takeNext()
    {
        std::lock_guard lock(mutex);
        if (activeJobs >= activeCapacityValue) {
            return std::nullopt;
        }
        expireDeadlines(std::chrono::steady_clock::now());
        while (!queue.empty()) {
            const GeometryJobId id = queue.front();
            queue.pop_front();
            const auto found = jobs.find(id);
            if (found == jobs.end() || found->second.state != GeometryJobState::Queued) {
                continue;
            }
            found->second.state = GeometryJobState::Running;
            ++activeJobs;
            return GeometryJobDispatch {id, found->second.request};
        }
        return std::nullopt;
    }

    bool reportProgress(const GeometryJobId id, GeometryJobProgress progress)
    {
        if (!std::isfinite(progress.fraction) || progress.fraction < 0.0
            || progress.fraction > 1.0 || progress.phase.size() > 256) {
            return false;
        }
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end() || found->second.state != GeometryJobState::Running
            || progress.fraction < found->second.progress.fraction) {
            return false;
        }
        found->second.progress = std::move(progress);
        return true;
    }

    bool finish(GeometryJobResult result)
    {
        if (!isWorkerTerminal(result.state)) {
            return false;
        }
        std::lock_guard lock(mutex);
        const auto found = jobs.find(result.id);
        if (found == jobs.end() || isTerminal(found->second.state)
            || found->second.state == GeometryJobState::Queued) {
            return false;
        }
        auto& job = found->second;
        if (job.cancellationRequested) {
            result.state = GeometryJobState::Cancelled;
            result.resultArtifact.clear();
            result.resultDigest.clear();
            if (result.diagnostic.empty()) {
                result.diagnostic = "geometry job cancelled";
            }
        }
        if (activeJobs > 0) {
            --activeJobs;
        }
        job.state = result.state;
        job.diagnostic = result.diagnostic;
        if (result.state == GeometryJobState::Completed) {
            job.progress.fraction = 1.0;
        }
        job.result = std::move(result);
        eraseActiveIdentity(job);
        condition.notify_all();
        return true;
    }

    std::size_t activeCount() const
    {
        std::lock_guard lock(mutex);
        return activeJobs;
    }

    std::size_t queuedCount() const
    {
        std::lock_guard lock(mutex);
        return queue.size();
    }

    const std::size_t activeCapacityValue;
    const std::size_t queueCapacityValue;

private:
    GeometryJobId allocateId()
    {
        if (nextId == 0) {
            throw std::overflow_error("geometry job identity exhausted");
        }
        const GeometryJobId id = nextId;
        nextId = nextId == std::numeric_limits<GeometryJobId>::max() ? 0 : nextId + 1;
        return id;
    }

    void supersede(Job& job)
    {
        job.cancellationRequested = true;
        if (job.state == GeometryJobState::Queued) {
            eraseQueued(job.id);
            makeTerminal(job,
                         GeometryJobState::Cancelled,
                         "geometry job superseded by a newer request");
            eraseActiveIdentity(job);
        }
        else {
            job.state = GeometryJobState::Cancelling;
            job.diagnostic = "geometry job superseded by a newer request";
        }
    }

    void eraseQueued(const GeometryJobId id)
    {
        const auto queued = std::find(queue.begin(), queue.end(), id);
        if (queued != queue.end()) {
            queue.erase(queued);
        }
    }

    void eraseActiveIdentity(const Job& job)
    {
        if (job.request.coalescing == GeometryJobCoalescing::None) {
            return;
        }
        const auto identity = coalescingIdentity(job.request);
        const auto found = activeByKey.find(identity);
        if (found != activeByKey.end() && found->second == job.id) {
            activeByKey.erase(found);
        }
    }

    static void makeTerminal(Job& job,
                             const GeometryJobState state,
                             std::string diagnostic)
    {
        job.state = state;
        job.diagnostic = std::move(diagnostic);
        GeometryJobResult result;
        result.id = job.id;
        result.state = state;
        result.diagnostic = job.diagnostic;
        job.result = std::move(result);
    }

    void expireDeadlines(const std::chrono::steady_clock::time_point now)
    {
        for (auto& [id, job] : jobs) {
            if (isTerminal(job.state) || job.request.deadline > now) {
                continue;
            }
            job.cancellationRequested = true;
            if (job.state == GeometryJobState::Queued) {
                eraseQueued(id);
            }
            else if (activeJobs > 0) {
                --activeJobs;
            }
            makeTerminal(job,
                         GeometryJobState::DeadlineExceeded,
                         "geometry job deadline exceeded");
            eraseActiveIdentity(job);
        }
    }

    void monitorDeadlines()
    {
        std::unique_lock lock(mutex);
        while (!shuttingDown) {
            auto earliest = std::chrono::steady_clock::time_point::max();
            for (const auto& [id, job] : jobs) {
                (void)id;
                if (!isTerminal(job.state)) {
                    earliest = std::min(earliest, job.request.deadline);
                }
            }
            if (earliest == std::chrono::steady_clock::time_point::max()) {
                condition.wait(lock, [this] {
                    return shuttingDown || std::any_of(jobs.begin(), jobs.end(), [](const auto& item) {
                        return !isTerminal(item.second.state);
                    });
                });
                continue;
            }
            condition.wait_until(lock, earliest);
            if (!shuttingDown) {
                expireDeadlines(std::chrono::steady_clock::now());
            }
        }
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<GeometryJobId> queue;
    std::unordered_map<GeometryJobId, Job> jobs;
    std::unordered_map<std::string, GeometryJobId> activeByKey;
    GeometryJobId nextId {1};
    std::size_t activeJobs {0};
    bool shuttingDown {false};
    std::thread deadlineThread;
};

GeometryJobManager::GeometryJobManager(const std::size_t activeCapacity,
                                       const std::size_t queueCapacity)
    : _impl(std::make_unique<Impl>(activeCapacity, queueCapacity))
{}

GeometryJobManager::~GeometryJobManager() = default;

GeometryJobId GeometryJobManager::submit(GeometryJobRequest request)
{
    return _impl->submit(std::move(request));
}

bool GeometryJobManager::cancel(const GeometryJobId id)
{
    return _impl->cancel(id);
}

std::optional<GeometryJobStatus> GeometryJobManager::status(const GeometryJobId id) const
{
    return _impl->status(id);
}

std::optional<GeometryJobResult> GeometryJobManager::takeResult(const GeometryJobId id)
{
    return _impl->takeResult(id);
}

std::size_t GeometryJobManager::activeCapacity() const noexcept
{
    return _impl->activeCapacityValue;
}

std::size_t GeometryJobManager::queueCapacity() const noexcept
{
    return _impl->queueCapacityValue;
}

std::size_t GeometryJobManager::activeCount() const
{
    return _impl->activeCount();
}

std::size_t GeometryJobManager::queuedCount() const
{
    return _impl->queuedCount();
}

std::optional<GeometryJobDispatch> GeometryJobManager::takeNext()
{
    return _impl->takeNext();
}

bool GeometryJobManager::reportProgress(const GeometryJobId id,
                                        GeometryJobProgress progress)
{
    return _impl->reportProgress(id, std::move(progress));
}

bool GeometryJobManager::finish(GeometryJobResult result)
{
    return _impl->finish(std::move(result));
}
