// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryJobManager.h"
#include "MainThreadSignal.h"
#include <Base/Console.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <utility>

namespace App
{

namespace
{

struct DeferredCallback
{
    std::vector<GeometryJobManager::JobCallback> callbacks;
    GeometryJobId id {0};
    GeometryJobState state {GeometryJobState::Queued};
    DetachedGeometryResult result;
};

void invokeDeferred(DeferredCallback& deferred)
{
    for (auto& callback : deferred.callbacks) {
        if (callback) {
            callback(deferred.id, deferred.state, deferred.result);
        }
    }
}

bool isTerminalState(GeometryJobState state)
{
    switch (state) {
        case GeometryJobState::Completed:
        case GeometryJobState::Cancelled:
        case GeometryJobState::Failed:
        case GeometryJobState::Stale:
        case GeometryJobState::DocumentClosed:
        case GeometryJobState::TimedOut:
        case GeometryJobState::Crashed:
            return true;
        default:
            return false;
    }
}

DeferredCallback drainAllCallbacks(std::vector<GeometryJobManager::JobCallback>& callbacks,
                                   GeometryJobId id,
                                   GeometryJobState state,
                                   const DetachedGeometryResult& result,
                                   bool& callbackInvoked)
{
    DeferredCallback deferred;
    if (callbackInvoked || callbacks.empty()) {
        return deferred;
    }
    callbackInvoked = true;
    deferred.callbacks = callbacks;
    deferred.id = id;
    deferred.state = state;
    deferred.result = result;
    return deferred;
}

bool tasksEquivalent(const std::shared_ptr<const DetachedGeometryTask>& a,
                     const std::shared_ptr<const DetachedGeometryTask>& b)
{
    if (!a && !b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return a->operationType() == b->operationType()
        && a->codecVersion() == b->codecVersion()
        && a->parameterDigest() == b->parameterDigest();
}

} // namespace

GeometryJobManager& GeometryJobManager::instance()
{
    static GeometryJobManager mgr;
    return mgr;
}

GeometryJobManager::GeometryJobManager() = default;

void GeometryJobManager::requestCancelFlag(JobRecord& rec)
{
    if (rec.cancelRequested) {
        rec.cancelRequested->store(true);
    }
}

GeometryJobManager::~GeometryJobManager()
{
    std::vector<DeferredCallback> deferred;
    std::vector<std::string> ownedWorkspaces;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& [id, rec] : _jobs) {
            requestCancelFlag(rec);
            if (!rec.callbackInvoked && !rec.callbacks.empty()) {
                rec.state = GeometryJobState::DocumentClosed;
                deferred.push_back(
                    drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked));
            }
            if (rec.workspaceOwned && !rec.workspaceDir.empty()) {
                ownedWorkspaces.push_back(std::move(rec.workspaceDir));
                rec.workspaceOwned = false;
            }
        }
        _jobs.clear();
        _activeByKey.clear();
    }
    for (auto& d : deferred) {
        invokeDeferred(d);
    }
    // Never force-terminate in-process workers; join cooperatively outside the mutex.
    reclaimFinishedWorkers();
    std::vector<std::thread> remaining;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        remaining.reserve(_workerThreads.size());
        for (auto& slot : _workerThreads) {
            if (slot.thread.joinable()) {
                remaining.push_back(std::move(slot.thread));
            }
        }
        _workerThreads.clear();
    }
    for (auto& t : remaining) {
        if (t.joinable()) {
            t.join();
        }
    }
    for (const auto& dir : ownedWorkspaces) {
        cleanupWorkspace(dir);
    }
}

GeometryJobHandle GeometryJobManager::submit(GeometryJobSpec spec)
{
    DeferredCallback superseded;
    GeometryJobHandle handle;

    {
        std::lock_guard<std::mutex> lock(_mutex);

        GeometryJobKey key = spec.key;
        // Align purpose with the coalescing key before deadline selection.
        spec.purpose = key.purpose;

        auto now = std::chrono::steady_clock::now();
        if (spec.deadline == std::chrono::steady_clock::time_point{}) {
            switch (spec.purpose) {
                case GeometryJobPurpose::Preview:
                    spec.deadline = now + std::chrono::seconds(10);
                    break;
                case GeometryJobPurpose::Tessellation:
                    spec.deadline = now + std::chrono::seconds(15);
                    break;
                case GeometryJobPurpose::ModelRecompute:
                    spec.deadline = now + std::chrono::seconds(120);
                    break;
                case GeometryJobPurpose::LegacyIsolatedRecompute:
                    spec.deadline = now + std::chrono::seconds(600);
                    break;
            }
        }

        // Deny-by-default in-process: require global allowlist and task opt-in.
        if (spec.backend == GeometryBackend::VerifiedInProcess) {
            const bool taskAllows = spec.task
                && (spec.task->traits().allowInProcess || spec.task->traits().supportsInProcess);
            if (!_allowInProcess || !taskAllows) {
                spec.backend = GeometryBackend::FreeCADCmd;
            }
        }

        auto it = _activeByKey.find(key);
        if (it != _activeByKey.end()) {
            GeometryJobId activeId = it->second;
            auto jobIt = _jobs.find(activeId);
            if (jobIt != _jobs.end()) {
                JobRecord& activeRec = jobIt->second;
                const bool activeAlive = !isTerminalState(activeRec.state);
                const bool sameGeneration =
                    activeRec.spec.document.modelGeneration == spec.document.modelGeneration;
                const bool olderGeneration =
                    spec.document.modelGeneration < activeRec.spec.document.modelGeneration;
                const bool identicalRequest =
                    sameGeneration && tasksEquivalent(activeRec.spec.task, spec.task);

                auto cancelActive = [&](const char* code, const char* message) {
                    requestCancelFlag(activeRec);
                    activeRec.state = GeometryJobState::Cancelled;
                    activeRec.result.success = false;
                    activeRec.result.errorCode = code;
                    activeRec.result.errorMessage = message;
                    superseded = drainAllCallbacks(activeRec.callbacks,
                                                   activeId,
                                                   activeRec.state,
                                                   activeRec.result,
                                                   activeRec.callbackInvoked);
                    _activeByKey.erase(it);
                };

                if (activeAlive) {
                    switch (spec.coalescing) {
                        case CoalesceMode::None:
                            // Independent job: do not join or cancel the active peer.
                            _activeByKey.erase(it);
                            break;

                        case CoalesceMode::LatestWins:
                            cancelActive("Superseded", "Replaced by a newer coalesced request");
                            break;

                        case CoalesceMode::SingleInstance:
                        case CoalesceMode::Union:
                            if (identicalRequest) {
                                return GeometryJobHandle(activeId, key);
                            }
                            if (olderGeneration) {
                                // A newer generation already owns this key; do not displace it.
                                return GeometryJobHandle(activeId, key);
                            }
                            // Different parameters (same gen) or a newer generation replace.
                            cancelActive("Superseded",
                                         "Replaced by a non-identical or newer request");
                            break;
                    }
                }
                else {
                    _activeByKey.erase(it);
                }
            }
        }

        GeometryJobId newId = _nextId++;
        spec.id = newId;

        JobRecord rec;
        rec.spec = std::move(spec);
        rec.state = GeometryJobState::Queued;

        handle = GeometryJobHandle(newId, key);
        _jobs[newId] = std::move(rec);
        _activeByKey[key] = newId;

        tryDispatchInProcessUnlocked();
    }

    invokeDeferred(superseded);
    reclaimFinishedWorkers();

    std::vector<GeometryProcessLaunchRequest> processLaunches;
    std::vector<DeferredStartFailure> processFailures;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        tryDispatchInProcessUnlocked();
        tryDispatchProcessUnlocked(processLaunches, processFailures);
    }
    executeProcessDispatches(std::move(processLaunches), std::move(processFailures));
    return handle;
}

void GeometryJobManager::cancel(GeometryJobId id, CancelReason reason)
{
    DeferredCallback deferred;
    GeometryProcessCancelFn cancelFn;
    bool invokeProcessCancel = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _jobs.find(id);
        if (it == _jobs.end()) {
            return;
        }
        JobRecord& rec = it->second;
        if (isTerminalState(rec.state)) {
            return;
        }
        if (rec.cancelRequested) {
            rec.cancelRequested->store(true);
        }

        // Queued / not-yet-running jobs cancel immediately.
        if (rec.state == GeometryJobState::Queued
            || rec.state == GeometryJobState::Snapshotting) {
            rec.state = GeometryJobState::Cancelled;
            rec.result.success = false;
            rec.result.errorCode = "Cancelled";
            rec.result.errorMessage = "Job cancelled";
            deferred = drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
            _activeByKey.erase(rec.spec.key);
        }
        else if (rec.spec.backend == GeometryBackend::VerifiedInProcess && rec.spec.task) {
            rec.state = GeometryJobState::Cancelling;
        }
        else if (rec.spec.backend == GeometryBackend::FreeCADCmd) {
            cancelFn = _processCancel;
            if (cancelFn) {
                rec.state = GeometryJobState::Cancelling;
                invokeProcessCancel = true;
            }
            else {
                rec.state = GeometryJobState::Cancelled;
                rec.result.success = false;
                rec.result.errorCode = "Cancelled";
                rec.result.errorMessage = "Job cancelled";
                deferred = drainAllCallbacks(
                    rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
                _activeByKey.erase(rec.spec.key);
            }
        }
        else {
            rec.state = GeometryJobState::Cancelled;
            rec.result.success = false;
            rec.result.errorCode = "Cancelled";
            rec.result.errorMessage = "Job cancelled";
            deferred = drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
            _activeByKey.erase(rec.spec.key);
        }
    }
    if (invokeProcessCancel) {
        cancelFn(id, reason);
    }
    invokeDeferred(deferred);
}

void GeometryJobManager::invalidateDocument(const DocumentRevisionToken& docToken, CancelReason reason)
{
    std::vector<DeferredCallback> deferred;
    std::vector<GeometryJobId> processCancelIds;
    GeometryProcessCancelFn cancelFn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        cancelFn = _processCancel;
        for (auto& [id, rec] : _jobs) {
            if (rec.spec.document.runtimeIncarnation != docToken.runtimeIncarnation) {
                continue;
            }
            if (!(rec.spec.document.modelGeneration < docToken.modelGeneration
                  || reason == CancelReason::DocumentClosed)) {
                continue;
            }
            if (isTerminalState(rec.state)) {
                continue;
            }
            requestCancelFlag(rec);
            // Ask the FreeCADCmd controller to stop the QProcess without waiting here.
            if (cancelFn
                && rec.spec.backend == GeometryBackend::FreeCADCmd
                && (rec.state == GeometryJobState::Running
                    || rec.state == GeometryJobState::Cancelling
                    || rec.state == GeometryJobState::Snapshotting)) {
                processCancelIds.push_back(id);
            }
            rec.state = (reason == CancelReason::DocumentClosed)
                ? GeometryJobState::DocumentClosed
                : GeometryJobState::Stale;
            deferred.push_back(
                drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked));
            _activeByKey.erase(rec.spec.key);
        }
    }
    for (GeometryJobId id : processCancelIds) {
        cancelFn(id, reason);
    }
    for (auto& d : deferred) {
        invokeDeferred(d);
    }
}

void GeometryJobManager::invalidateObject(const ObjectRevisionToken& objToken, CancelReason reason)
{
    std::vector<DeferredCallback> deferred;
    std::vector<GeometryJobId> processCancelIds;
    GeometryProcessCancelFn cancelFn;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        cancelFn = _processCancel;
        for (auto& [id, rec] : _jobs) {
            if (rec.spec.target.objectId != objToken.objectId) {
                continue;
            }
            if (objToken.documentIncarnation != 0
                && rec.spec.document.runtimeIncarnation != objToken.documentIncarnation) {
                continue;
            }
            if (!objToken.internalName.empty()
                && rec.spec.target.internalName != objToken.internalName) {
                continue;
            }
            if (isTerminalState(rec.state)) {
                continue;
            }
            requestCancelFlag(rec);
            if (cancelFn
                && rec.spec.backend == GeometryBackend::FreeCADCmd
                && (rec.state == GeometryJobState::Running
                    || rec.state == GeometryJobState::Cancelling
                    || rec.state == GeometryJobState::Snapshotting)) {
                processCancelIds.push_back(id);
            }
            rec.state = GeometryJobState::Stale;
            deferred.push_back(
                drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked));
            _activeByKey.erase(rec.spec.key);
        }
    }
    for (GeometryJobId id : processCancelIds) {
        cancelFn(id, reason);
    }
    for (auto& d : deferred) {
        invokeDeferred(d);
    }
}

GeometryJobState GeometryJobManager::getJobState(GeometryJobId id) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        return it->second.state;
    }
    return GeometryJobState::Failed;
}

DetachedGeometryResult GeometryJobManager::getJobResult(GeometryJobId id) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        return it->second.result;
    }
    return {};
}

void GeometryJobManager::registerCallback(GeometryJobId id, JobCallback callback)
{
    DeferredCallback deferred;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _jobs.find(id);
        if (it == _jobs.end() || !callback) {
            return;
        }
        JobRecord& rec = it->second;
        rec.callbacks.push_back(callback);
        if (isTerminalState(rec.state)) {
            if (!rec.callbackInvoked) {
                deferred = drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
            }
            else {
                // Late joined observer after terminal delivery already happened.
                deferred.callbacks.push_back(std::move(callback));
                deferred.id = id;
                deferred.state = rec.state;
                deferred.result = rec.result;
            }
        }
    }
    invokeDeferred(deferred);
}

void GeometryJobManager::setJobState(GeometryJobId id,
                                     GeometryJobState state,
                                     const DetachedGeometryResult& result)
{
    DeferredCallback deferred;
    StateListener stateListener;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _jobs.find(id);
        if (it == _jobs.end()) {
            return;
        }
        JobRecord& rec = it->second;
        // Terminal transitions are monotonic: never overwrite a finished job.
        if (isTerminalState(rec.state)) {
            return;
        }
        rec.state = state;
        if (result.success || !result.errorCode.empty()) {
            rec.result = result;
        }
        if (isTerminalState(state)) {
            deferred = drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
            _activeByKey.erase(rec.spec.key);
            pruneTerminalJobsLocked();
        }
        stateListener = _stateListener;
    }
    if (stateListener) {
        stateListener(id, state);
    }
    invokeDeferred(deferred);
    if (isTerminalState(state)) {
        reclaimFinishedWorkers();
        std::vector<GeometryProcessLaunchRequest> processLaunches;
        std::vector<DeferredStartFailure> processFailures;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            tryDispatchInProcessUnlocked();
            tryDispatchProcessUnlocked(processLaunches, processFailures);
        }
        executeProcessDispatches(std::move(processLaunches), std::move(processFailures));
    }
}

void GeometryJobManager::updateProgress(GeometryJobId id, double fraction, const std::string& phase)
{
    ProgressListener listener;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _jobs.find(id);
        if (it != _jobs.end()) {
            it->second.progress = fraction;
            it->second.progressPhase = phase;
        }
        listener = _progressListener;
    }
    if (listener) {
        listener(id, fraction, phase);
    }
}

void GeometryJobManager::invokeCallbackOnce(JobRecord& rec, GeometryJobId id)
{
    // Kept for compatibility; prefer unlocking before invoke at call sites.
    if (!rec.callbackInvoked && !rec.callbacks.empty()) {
        auto deferred = drainAllCallbacks(rec.callbacks, id, rec.state, rec.result, rec.callbackInvoked);
        invokeDeferred(deferred);
    }
}

void GeometryJobManager::pruneTerminalJobsLocked()
{
    size_t terminalCount = 0;
    for (const auto& [id, rec] : _jobs) {
        if (isTerminalState(rec.state) && rec.callbackInvoked) {
            ++terminalCount;
        }
    }
    if (terminalCount <= MaxRetainedTerminalJobs) {
        return;
    }

    std::vector<GeometryJobId> removable;
    for (const auto& [id, rec] : _jobs) {
        if (isTerminalState(rec.state) && rec.callbackInvoked) {
            removable.push_back(id);
        }
    }
    std::sort(removable.begin(), removable.end());
    const size_t toRemove = terminalCount - MaxRetainedTerminalJobs;
    std::vector<std::string> dirsToClean;
    for (size_t i = 0; i < toRemove && i < removable.size(); ++i) {
        auto it = _jobs.find(removable[i]);
        if (it == _jobs.end()) {
            continue;
        }
        if (it->second.workspaceOwned && !it->second.workspaceDir.empty()) {
            dirsToClean.push_back(std::move(it->second.workspaceDir));
            it->second.workspaceOwned = false;
        }
        _jobs.erase(it);
    }
    // Unlock is not available here; cleanup after loop without holding filesystem
    // work under the mutex by deferring to temporary list only — still under lock.
    // Prefer short cleanup: remove_all is best-effort and rare (prune path).
    for (const auto& dir : dirsToClean) {
        cleanupWorkspace(dir);
    }
}

void GeometryJobManager::cleanupWorkspace(const std::string& dir)
{
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

void GeometryJobManager::releaseJobArtifacts(GeometryJobId id)
{
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _jobs.find(id);
        if (it == _jobs.end()) {
            return;
        }
        if (!it->second.workspaceOwned || it->second.workspaceDir.empty()) {
            return;
        }
        dir = std::move(it->second.workspaceDir);
        it->second.workspaceOwned = false;
        it->second.workspaceDir.clear();
    }
    cleanupWorkspace(dir);
}

namespace
{

class InProcessWorkerContext : public GeometryWorkerContext
{
public:
    InProcessWorkerContext(std::string tempDir,
                           std::chrono::steady_clock::time_point deadline,
                           std::shared_ptr<std::atomic<bool>> cancelled,
                           GeometryJobId id,
                           GeometryJobManager* manager)
        : _tempDir(std::move(tempDir))
        , _deadline(deadline)
        , _cancelled(std::move(cancelled))
        , _id(id)
        , _manager(manager)
    {
    }

    void reportProgress(double fraction, const std::string& phase = "") override
    {
        if (_manager) {
            _manager->updateProgress(_id, fraction, phase);
        }
    }

    bool isCancelled() const override
    {
        return _cancelled && _cancelled->load();
    }

    std::chrono::steady_clock::time_point deadline() const override
    {
        return _deadline;
    }

    std::string tempDir() const override
    {
        return _tempDir;
    }

private:
    std::string _tempDir;
    std::chrono::steady_clock::time_point _deadline;
    std::shared_ptr<std::atomic<bool>> _cancelled;
    GeometryJobId _id {0};
    GeometryJobManager* _manager {nullptr};
};

} // namespace

void GeometryJobManager::launchInProcessUnlocked(GeometryJobId id, JobRecord& rec)
{
    rec.state = GeometryJobState::Running;
    auto task = rec.spec.task;
    auto cancelFlag = rec.cancelRequested;
    auto deadline = rec.spec.deadline;
    // Unique per launch: job id alone can collide across process restarts.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tempDir =
        (std::filesystem::temp_directory_path()
         / ("fc_geom_job_" + std::to_string(id) + "_" + std::to_string(stamp)))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    rec.workspaceDir = tempDir;
    rec.workspaceOwned = true;

    WorkerSlot slot;
    auto finished = slot.finished;
    slot.thread = std::thread([this, id, task, cancelFlag, deadline, tempDir, finished]() {
        InProcessWorkerContext ctx(tempDir, deadline, cancelFlag, id, this);
        DetachedGeometryResult result;
        GeometryJobState state = GeometryJobState::Failed;
        try {
            if (ctx.isCancelled() || std::chrono::steady_clock::now() > deadline) {
                result.success = false;
                result.errorCode = "Cancelled";
                result.errorMessage = "Cancelled before start";
                state = GeometryJobState::Cancelled;
            }
            else {
                result = task->run(ctx);
                if (ctx.isCancelled()) {
                    result.success = false;
                    result.errorCode = "Cancelled";
                    result.errorMessage = "Cancelled during execution";
                    state = GeometryJobState::Cancelled;
                }
                else if (std::chrono::steady_clock::now() > deadline) {
                    result.success = false;
                    result.errorCode = "TimedOut";
                    result.errorMessage = "In-process job exceeded deadline";
                    state = GeometryJobState::TimedOut;
                }
                else {
                    state = result.success ? GeometryJobState::Completed : GeometryJobState::Failed;
                }
            }
        }
        catch (const std::exception& ex) {
            result.success = false;
            result.errorCode = "Exception";
            result.errorMessage = ex.what();
            state = GeometryJobState::Failed;
        }
        catch (...) {
            result.success = false;
            result.errorCode = "Exception";
            result.errorMessage = "Unknown exception in geometry worker";
            state = GeometryJobState::Failed;
        }

        // Mark finished before terminal delivery so dispatch can reuse this slot without
        // waiting for this thread to exit. reclaimFinishedWorkers() must not join itself.
        finished->store(true);
        // Retain workspace until releaseJobArtifacts() / prune / destructor.
        deliverTerminal(id, state, result);
    });
    _workerThreads.push_back(std::move(slot));
    noteUnfinishedWorkerCountLocked();
}

bool GeometryJobManager::isInProcessEligible(const JobRecord& rec, bool allowInProcess)
{
    return rec.spec.backend == GeometryBackend::VerifiedInProcess
        && rec.spec.task
        && allowInProcess
        && (rec.spec.task->traits().allowInProcess || rec.spec.task->traits().supportsInProcess);
}

size_t GeometryJobManager::unfinishedWorkerCountLocked() const
{
    size_t count = 0;
    for (const auto& slot : _workerThreads) {
        if (!slot.finished->load()) {
            ++count;
        }
    }
    return count;
}

void GeometryJobManager::noteUnfinishedWorkerCountLocked()
{
    const size_t current = unfinishedWorkerCountLocked();
    size_t peak = _peakUnfinishedWorkers.load();
    while (current > peak
           && !_peakUnfinishedWorkers.compare_exchange_weak(peak, current)) {
        // retry with updated peak
    }
}

void GeometryJobManager::tryDispatchInProcessUnlocked()
{
    while (unfinishedWorkerCountLocked() < MaxConcurrentInProcessWorkers) {
        GeometryJobId nextId = 0;
        for (auto& [id, rec] : _jobs) {
            if (rec.state == GeometryJobState::Queued && isInProcessEligible(rec, _allowInProcess)) {
                nextId = id;
                break;
            }
        }
        if (nextId == 0) {
            break;
        }
        launchInProcessUnlocked(nextId, _jobs[nextId]);
    }
}

bool GeometryJobManager::isProcessEligible(const JobRecord& rec, bool hasBackend)
{
    return hasBackend
        && rec.state == GeometryJobState::Queued
        && rec.spec.backend == GeometryBackend::FreeCADCmd;
}

void GeometryJobManager::launchProcessPrepareUnlocked(GeometryJobId id,
                                                      JobRecord& rec,
                                                      GeometryProcessLaunchRequest& outReq)
{
    rec.state = GeometryJobState::Running;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string tempDir =
        (std::filesystem::temp_directory_path()
         / ("fc_geom_cmd_" + std::to_string(id) + "_" + std::to_string(stamp)))
            .string();
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    rec.workspaceDir = tempDir;
    rec.workspaceOwned = true;

    outReq.id = id;
    outReq.spec = rec.spec;
    outReq.workspaceDir = tempDir;
    outReq.cancelRequested = rec.cancelRequested;
}

void GeometryJobManager::tryDispatchProcessUnlocked(
    std::vector<GeometryProcessLaunchRequest>& outLaunches,
    std::vector<DeferredStartFailure>& outFailures)
{
    const bool hasBackend = static_cast<bool>(_processLaunch);
    if (!hasBackend) {
        return;
    }

    std::vector<GeometryJobId> candidates;
    for (const auto& [id, rec] : _jobs) {
        if (isProcessEligible(rec, hasBackend)) {
            candidates.push_back(id);
        }
    }
    for (GeometryJobId id : candidates) {
        auto it = _jobs.find(id);
        if (it == _jobs.end() || !isProcessEligible(it->second, hasBackend)) {
            continue;
        }
        GeometryProcessLaunchRequest req;
        launchProcessPrepareUnlocked(id, it->second, req);
        if (req.workspaceDir.empty()
            || !std::filesystem::exists(req.workspaceDir)) {
            // Workspace creation failed before the backend was invoked.
            it->second.workspaceOwned = false;
            DeferredStartFailure failure;
            failure.id = id;
            failure.workspaceDir = req.workspaceDir;
            failure.workspaceOwned = false;
            outFailures.push_back(std::move(failure));
            continue;
        }
        outLaunches.push_back(std::move(req));
    }
}

void GeometryJobManager::failProcessStart(GeometryJobId id,
                                          const std::string& workspaceDir,
                                          bool workspaceOwned)
{
    DetachedGeometryResult result;
    result.success = false;
    result.errorCode = "ProcessStartFailed";
    result.errorMessage = "FreeCADCmd process backend failed to start";
    setJobState(id, GeometryJobState::Failed, result);
    if (workspaceOwned && !workspaceDir.empty()) {
        // Drop ownership so prune/destructor does not double-delete after cleanup.
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _jobs.find(id);
            if (it != _jobs.end() && it->second.workspaceDir == workspaceDir) {
                it->second.workspaceOwned = false;
                it->second.workspaceDir.clear();
            }
        }
        cleanupWorkspace(workspaceDir);
    }
}

void GeometryJobManager::executeProcessDispatches(
    std::vector<GeometryProcessLaunchRequest> launches,
    std::vector<DeferredStartFailure> failures)
{
    for (const auto& failure : failures) {
        failProcessStart(failure.id, failure.workspaceDir, failure.workspaceOwned);
    }
    for (const auto& req : launches) {
        GeometryProcessLaunchFn launchFn;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            launchFn = _processLaunch;
        }
        bool started = false;
        if (launchFn) {
            try {
                started = launchFn(req);
            }
            catch (...) {
                started = false;
            }
        }
        if (!started) {
            failProcessStart(req.id, req.workspaceDir, /*workspaceOwned=*/true);
        }
    }
}

void GeometryJobManager::setProcessBackend(GeometryProcessLaunchFn launch,
                                           GeometryProcessCancelFn cancel)
{
    std::vector<GeometryProcessLaunchRequest> processLaunches;
    std::vector<DeferredStartFailure> processFailures;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _processLaunch = std::move(launch);
        _processCancel = std::move(cancel);
        tryDispatchProcessUnlocked(processLaunches, processFailures);
    }
    executeProcessDispatches(std::move(processLaunches), std::move(processFailures));
}

void GeometryJobManager::clearProcessBackend()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _processLaunch = nullptr;
    _processCancel = nullptr;
}

void GeometryJobManager::setProgressListener(ProgressListener listener)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _progressListener = std::move(listener);
}

void GeometryJobManager::setStateListener(StateListener listener)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _stateListener = std::move(listener);
}

void GeometryJobManager::clearProgressListeners()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _progressListener = nullptr;
    _stateListener = nullptr;
}

bool GeometryJobManager::hasProcessBackend() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return static_cast<bool>(_processLaunch);
}

void GeometryJobManager::reclaimFinishedWorkers()
{
    std::vector<std::thread> done;
    const auto self = std::this_thread::get_id();
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<WorkerSlot> remaining;
        remaining.reserve(_workerThreads.size());
        for (auto& slot : _workerThreads) {
            if (!slot.finished->load()) {
                remaining.push_back(std::move(slot));
                continue;
            }
            if (!slot.thread.joinable()) {
                continue;
            }
            if (slot.thread.get_id() == self) {
                // Cannot join the calling worker; detach so the slot is not retained.
                slot.thread.detach();
                continue;
            }
            done.push_back(std::move(slot.thread));
        }
        _workerThreads.swap(remaining);
        noteUnfinishedWorkerCountLocked();
    }
    for (auto& t : done) {
        if (t.joinable()) {
            t.join();
        }
    }
}

size_t GeometryJobManager::retainedWorkerThreadCount() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _workerThreads.size();
}

size_t GeometryJobManager::peakUnfinishedWorkerThreadCount() const
{
    return _peakUnfinishedWorkers.load();
}

void GeometryJobManager::resetPeakWorkerThreads()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _peakUnfinishedWorkers.store(unfinishedWorkerCountLocked());
}

void GeometryJobManager::deliverTerminal(GeometryJobId id,
                                         GeometryJobState state,
                                         const DetachedGeometryResult& result)
{
    auto apply = [this, id, state, result]() {
        setJobState(id, state, result);
    };

    if (MainThreadSignalConfig::hasHooks() && !MainThreadSignalConfig::isMainThread()) {
        MainThreadSignalConfig::invoke(apply, /*blocking=*/false);
        // Worker finished delivering (queued); reclaim may join this thread once finished is set.
    }
    else {
        apply();
    }
}

void GeometryJobManager::setAllowInProcess(bool allow)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _allowInProcess = allow;
    if (allow) {
        tryDispatchInProcessUnlocked();
    }
}

bool GeometryJobManager::isAllowInProcess() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _allowInProcess;
}

} // namespace App
