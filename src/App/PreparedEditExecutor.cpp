// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreparedEditExecutor.h"

#include <Base/Exception.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace App;

namespace
{

constexpr std::size_t MinDefaultWorkerCount = 2;
constexpr std::size_t MaxDefaultWorkerCount = 4;

std::size_t defaultWorkerCount() noexcept
{
    const auto available = static_cast<std::size_t>(std::thread::hardware_concurrency());
    return std::clamp(available, MinDefaultWorkerCount, MaxDefaultWorkerCount);
}

bool isTerminal(PreparedEditExecutionStatus status) noexcept
{
    return status == PreparedEditExecutionStatus::Completed
        || status == PreparedEditExecutionStatus::Cancelled
        || status == PreparedEditExecutionStatus::Failed;
}

}  // namespace

class PreparedEditExecutor::Impl
{
public:
    struct Job
    {
        explicit Job(CollaborativeOperationPreparation::DetachedTask submittedTask)
            : task(std::move(submittedTask))
        {}

        PreparedEditExecutionId id {0};
        PreparedEditExecutionStatus status {PreparedEditExecutionStatus::Queued};
        CollaborativeOperationPreparation::DetachedTask task;
        std::stop_source stopSource;
        bool cancellationRequested {false};
        bool abandonOnCompletion {false};
        std::unique_ptr<const CollaborativeOperation> operation;
        std::string diagnostic;
    };

    Impl(std::size_t requestedWorkerCount, std::size_t requestedQueueCapacity)
        : workerCountValue(requestedWorkerCount == 0 ? defaultWorkerCount()
                                                     : requestedWorkerCount)
        , queueCapacityValue(requestedQueueCapacity)
    {
        if (queueCapacityValue == 0) {
            throw std::invalid_argument("prepared-edit executor queue capacity must be positive");
        }

        workers.reserve(workerCountValue);
        try {
            for (std::size_t index = 0; index < workerCountValue; ++index) {
                workers.emplace_back([this](std::stop_token stopToken) {
                    workerLoop(stopToken);
                });
            }
        }
        catch (...) {
            {
                std::lock_guard lock(mutex);
                shuttingDown = true;
            }
            for (auto& worker : workers) {
                worker.request_stop();
            }
            condition.notify_all();
            throw;
        }
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            shuttingDown = true;
            for (auto& [id, job] : jobs) {
                (void)id;
                if (isTerminal(job->status)) {
                    continue;
                }
                job->cancellationRequested = true;
                if (job->status == PreparedEditExecutionStatus::Queued) {
                    job->status = PreparedEditExecutionStatus::Cancelled;
                }
            }
            queue.clear();
        }

        // The job map is structurally stable during shutdown. Requesting stop
        // outside the mutex is essential because stop callbacks run inline.
        for (auto& [id, job] : jobs) {
            (void)id;
            job->stopSource.request_stop();
        }
        for (auto& worker : workers) {
            worker.request_stop();
        }
        condition.notify_all();
        workers.clear();
    }

    [[nodiscard]] PreparedEditExecutionId
    submit(CollaborativeOperationPreparation::DetachedTask task)
    {
        if (!task) {
            throw std::invalid_argument("prepared-edit executor task is required");
        }

        auto job = std::make_shared<Job>(std::move(task));
        PreparedEditExecutionId id = 0;
        {
            std::lock_guard lock(mutex);
            if (shuttingDown) {
                throw std::runtime_error("prepared-edit executor is shutting down");
            }
            if (queue.size() >= queueCapacityValue) {
                throw std::runtime_error("prepared-edit executor queue is full");
            }
            if (nextId == 0) {
                throw std::overflow_error("prepared-edit execution identity exhausted");
            }

            id = nextId;
            job->id = id;
            const auto inserted = jobs.emplace(id, job);
            try {
                queue.push_back(job);
            }
            catch (...) {
                jobs.erase(inserted.first);
                throw;
            }
            ++nextId;
        }
        condition.notify_one();
        return id;
    }

    [[nodiscard]] bool cancel(PreparedEditExecutionId id)
    {
        std::stop_source stopSource(std::nostopstate);
        CollaborativeOperationPreparation::DetachedTask abandonedTask;
        bool requestStop = false;
        {
            std::lock_guard lock(mutex);
            const auto found = jobs.find(id);
            if (found == jobs.end() || isTerminal(found->second->status)
                || found->second->cancellationRequested) {
                return false;
            }

            const auto& job = found->second;
            job->cancellationRequested = true;
            stopSource = job->stopSource;
            requestStop = true;
            if (job->status == PreparedEditExecutionStatus::Queued) {
                const auto queued = std::find(queue.begin(), queue.end(), job);
                if (queued != queue.end()) {
                    queue.erase(queued);
                }
                abandonedTask = std::move(job->task);
                job->status = PreparedEditExecutionStatus::Cancelled;
            }
        }

        if (requestStop) {
            stopSource.request_stop();
        }
        return true;
    }

    [[nodiscard]] bool abandon(PreparedEditExecutionId id)
    {
        std::shared_ptr<Job> abandonedJob;
        std::stop_source stopSource(std::nostopstate);
        bool requestStop = false;
        {
            std::lock_guard lock(mutex);
            const auto found = jobs.find(id);
            if (found == jobs.end() || found->second->abandonOnCompletion) {
                return false;
            }

            const auto& job = found->second;
            if (job->status == PreparedEditExecutionStatus::Running) {
                job->cancellationRequested = true;
                job->abandonOnCompletion = true;
                stopSource = job->stopSource;
                requestStop = true;
            }
            else {
                if (job->status == PreparedEditExecutionStatus::Queued) {
                    const auto queued = std::find(queue.begin(), queue.end(), job);
                    if (queued != queue.end()) {
                        queue.erase(queued);
                    }
                    job->cancellationRequested = true;
                    stopSource = job->stopSource;
                    requestStop = true;
                }
                abandonedJob = std::move(found->second);
                jobs.erase(found);
            }
        }

        if (requestStop) {
            stopSource.request_stop();
        }
        abandonedJob.reset();
        return true;
    }

    [[nodiscard]] std::optional<PreparedEditExecutionSnapshot>
    status(PreparedEditExecutionId id) const
    {
        std::lock_guard lock(mutex);
        const auto found = jobs.find(id);
        if (found == jobs.end()) {
            return std::nullopt;
        }
        return PreparedEditExecutionSnapshot {
            found->second->id, found->second->status, found->second->diagnostic};
    }

    [[nodiscard]] std::optional<PreparedEditExecutionResult>
    takeResult(PreparedEditExecutionId id)
    {
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(mutex);
            const auto found = jobs.find(id);
            if (found == jobs.end() || !isTerminal(found->second->status)) {
                return std::nullopt;
            }
            job = std::move(found->second);
            jobs.erase(found);
        }

        PreparedEditExecutionResult result;
        result.id = job->id;
        result.status = job->status;
        result.operation = std::move(job->operation);
        result.diagnostic = std::move(job->diagnostic);
        return result;
    }

    void workerLoop(std::stop_token workerStopToken)
    {
        while (true) {
            std::shared_ptr<Job> job;
            CollaborativeOperationPreparation::DetachedTask task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, workerStopToken, [this] {
                    return shuttingDown || !queue.empty();
                });
                if (queue.empty()) {
                    if (shuttingDown || workerStopToken.stop_requested()) {
                        return;
                    }
                    continue;
                }

                job = std::move(queue.front());
                queue.pop_front();
                if (job->status != PreparedEditExecutionStatus::Queued) {
                    continue;
                }
                job->status = PreparedEditExecutionStatus::Running;
                task = std::move(job->task);
            }

            std::unique_ptr<const CollaborativeOperation> operation;
            std::string diagnostic;
            bool failed = false;
            try {
                operation = task(job->stopSource.get_token());
            }
            catch (const Base::Exception& exception) {
                failed = true;
                diagnostic = exception.what();
            }
            catch (const std::exception& exception) {
                failed = true;
                diagnostic = exception.what();
            }
            catch (...) {
                failed = true;
                diagnostic = "unknown exception during prepared-edit execution";
            }

            {
                std::lock_guard lock(mutex);
                if (job->cancellationRequested || job->stopSource.stop_requested()
                    || shuttingDown) {
                    job->status = PreparedEditExecutionStatus::Cancelled;
                    job->diagnostic.clear();
                }
                else if (failed) {
                    job->status = PreparedEditExecutionStatus::Failed;
                    job->diagnostic = std::move(diagnostic);
                }
                else {
                    job->status = PreparedEditExecutionStatus::Completed;
                    job->operation = std::move(operation);
                }
                if (job->abandonOnCompletion && !shuttingDown) {
                    jobs.erase(job->id);
                }
            }
        }
    }

    const std::size_t workerCountValue;
    const std::size_t queueCapacityValue;
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<std::shared_ptr<Job>> queue;
    std::unordered_map<PreparedEditExecutionId, std::shared_ptr<Job>> jobs;
    std::vector<std::jthread> workers;
    PreparedEditExecutionId nextId {1};
    bool shuttingDown {false};
};

PreparedEditExecutor::PreparedEditExecutor(std::size_t workerCount,
                                           std::size_t queueCapacity)
    : _impl(std::make_unique<Impl>(workerCount, queueCapacity))
{}

PreparedEditExecutor::~PreparedEditExecutor() = default;

PreparedEditExecutionId PreparedEditExecutor::submit(
    CollaborativeOperationPreparation::DetachedTask task)
{
    return _impl->submit(std::move(task));
}

bool PreparedEditExecutor::cancel(PreparedEditExecutionId id)
{
    return _impl->cancel(id);
}

bool PreparedEditExecutor::abandon(PreparedEditExecutionId id)
{
    return _impl->abandon(id);
}

std::optional<PreparedEditExecutionSnapshot>
PreparedEditExecutor::status(PreparedEditExecutionId id) const
{
    return _impl->status(id);
}

std::optional<PreparedEditExecutionResult>
PreparedEditExecutor::takeResult(PreparedEditExecutionId id)
{
    return _impl->takeResult(id);
}

std::size_t PreparedEditExecutor::workerCount() const noexcept
{
    return _impl->workerCountValue;
}

std::size_t PreparedEditExecutor::queueCapacity() const noexcept
{
    return _impl->queueCapacityValue;
}
