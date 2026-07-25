// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "GeometryJob.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace App
{

/** Request handed to a registered FreeCADCmd process backend on launch. */
struct AppExport GeometryProcessLaunchRequest
{
    GeometryJobId id {0};
    GeometryJobSpec spec;
    std::string workspaceDir;
    std::shared_ptr<std::atomic<bool>> cancelRequested;
};

/**
 * Process backend hooks (installed by Gui). App owns dispatch/workspace lifecycle;
 * Gui owns QProcess / GeometryWorkerProcess. Launch returns false on start failure.
 */
using GeometryProcessLaunchFn = std::function<bool(const GeometryProcessLaunchRequest&)>;
using GeometryProcessCancelFn = std::function<void(GeometryJobId, CancelReason)>;

class AppExport GeometryJobManager
{
public:
    using JobCallback = std::function<void(GeometryJobId, GeometryJobState, const DetachedGeometryResult&)>;

    GeometryJobManager();
    ~GeometryJobManager();

    GeometryJobHandle submit(GeometryJobSpec spec);
    void cancel(GeometryJobId id, CancelReason reason);
    void invalidateDocument(const DocumentRevisionToken& docToken, CancelReason reason);
    void invalidateObject(const ObjectRevisionToken& objToken, CancelReason reason);

    GeometryJobState getJobState(GeometryJobId id) const;
    DetachedGeometryResult getJobResult(GeometryJobId id) const;
    void registerCallback(GeometryJobId id, JobCallback callback);

    void setJobState(GeometryJobId id, GeometryJobState state, const DetachedGeometryResult& result = {});
    void updateProgress(GeometryJobId id, double fraction, const std::string& phase);

    void setAllowInProcess(bool allow);
    bool isAllowInProcess() const;

    /// Install / clear FreeCADCmd process backend (typically Gui GeometryWorkerProcess bridge).
    void setProcessBackend(GeometryProcessLaunchFn launch, GeometryProcessCancelFn cancel = {});
    void clearProcessBackend();
    bool hasProcessBackend() const;

    /// Optional global listeners (Gui progress controller). Invoked on the calling thread.
    using ProgressListener = std::function<void(GeometryJobId, double, const std::string&)>;
    using StateListener = std::function<void(GeometryJobId, GeometryJobState)>;
    void setProgressListener(ProgressListener listener);
    void setStateListener(StateListener listener);
    void clearProgressListeners();

    /**
     * Release a job's retained worker workspace (result archives, logs).
     * Must be called after decode/commit (or after deciding the result is unused).
     * Safe to call multiple times; no-op if the workspace was already released.
     */
    void releaseJobArtifacts(GeometryJobId id);

    /// Join and erase finished in-process worker threads (never joins under the manager mutex).
    void reclaimFinishedWorkers();

    /// Threads still retained (running or not yet reclaimed).
    size_t retainedWorkerThreadCount() const;

    /// Peak unfinished (still-running) in-process workers since resetPeakWorkerThreads().
    size_t peakUnfinishedWorkerThreadCount() const;

    void resetPeakWorkerThreads();

    static constexpr size_t MaxConcurrentInProcessWorkers = 2;

    static GeometryJobManager& instance();

private:
    struct JobRecord
    {
        GeometryJobSpec spec;
        GeometryJobState state {GeometryJobState::Queued};
        DetachedGeometryResult result;
        std::vector<JobCallback> callbacks;
        bool callbackInvoked {false};
        double progress {0.0};
        std::string progressPhase;
        std::shared_ptr<std::atomic<bool>> cancelRequested {std::make_shared<std::atomic<bool>>(false)};
        std::string workspaceDir;
        bool workspaceOwned {false};
    };

    struct WorkerSlot
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished {std::make_shared<std::atomic<bool>>(false)};
    };

    struct DeferredStartFailure
    {
        GeometryJobId id {0};
        std::string workspaceDir;
        bool workspaceOwned {false};
    };

    void invokeCallbackOnce(JobRecord& rec, GeometryJobId id);
    void pruneTerminalJobsLocked();
    void launchInProcessUnlocked(GeometryJobId id, JobRecord& rec);
    void tryDispatchInProcessUnlocked();
    void tryDispatchProcessUnlocked(std::vector<GeometryProcessLaunchRequest>& outLaunches,
                                    std::vector<DeferredStartFailure>& outFailures);
    void launchProcessPrepareUnlocked(GeometryJobId id,
                                      JobRecord& rec,
                                      GeometryProcessLaunchRequest& outReq);
    void failProcessStart(GeometryJobId id, const std::string& workspaceDir, bool workspaceOwned);
    void executeProcessDispatches(std::vector<GeometryProcessLaunchRequest> launches,
                                  std::vector<DeferredStartFailure> failures);
    size_t unfinishedWorkerCountLocked() const;
    void noteUnfinishedWorkerCountLocked();
    void deliverTerminal(GeometryJobId id, GeometryJobState state, const DetachedGeometryResult& result);
    void cleanupWorkspace(const std::string& dir);
    static void requestCancelFlag(JobRecord& rec);
    static bool isInProcessEligible(const JobRecord& rec, bool allowInProcess);
    static bool isProcessEligible(const JobRecord& rec, bool hasBackend);

    mutable std::mutex _mutex;
    std::atomic<uint64_t> _nextId {1};
    std::unordered_map<GeometryJobId, JobRecord> _jobs;
    std::map<GeometryJobKey, GeometryJobId> _activeByKey;
    bool _allowInProcess {false};
    GeometryProcessLaunchFn _processLaunch;
    GeometryProcessCancelFn _processCancel;
    ProgressListener _progressListener;
    StateListener _stateListener;
    std::vector<WorkerSlot> _workerThreads;
    std::atomic<size_t> _peakUnfinishedWorkers {0};
    static constexpr size_t MaxRetainedTerminalJobs = 256;
};

} // namespace App
