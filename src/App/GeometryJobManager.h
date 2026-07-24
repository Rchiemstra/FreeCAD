// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "GeometryJob.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <atomic>
#include <functional>

namespace App
{

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

    static GeometryJobManager& instance();

private:
    struct JobRecord
    {
        GeometryJobSpec spec;
        GeometryJobState state {GeometryJobState::Queued};
        DetachedGeometryResult result;
        JobCallback callback;
        bool callbackInvoked {false};
        double progress {0.0};
        std::string progressPhase;
    };

    void invokeCallbackOnce(JobRecord& rec, GeometryJobId id);

    mutable std::mutex _mutex;
    std::atomic<uint64_t> _nextId {1};
    std::unordered_map<GeometryJobId, JobRecord> _jobs;
    std::map<GeometryJobKey, GeometryJobId> _activeByKey;
    bool _allowInProcess {false};
};

} // namespace App
