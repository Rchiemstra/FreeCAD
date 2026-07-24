// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryJobManager.h"
#include <Base/Console.h>
#include <chrono>

namespace App
{

GeometryJobManager& GeometryJobManager::instance()
{
    static GeometryJobManager mgr;
    return mgr;
}

GeometryJobManager::GeometryJobManager() = default;

GeometryJobManager::~GeometryJobManager()
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [id, rec] : _jobs) {
        if (!rec.callbackInvoked && rec.callback) {
            rec.state = GeometryJobState::DocumentClosed;
            rec.callback(id, rec.state, rec.result);
            rec.callbackInvoked = true;
        }
    }
}

GeometryJobHandle GeometryJobManager::submit(GeometryJobSpec spec)
{
    std::lock_guard<std::mutex> lock(_mutex);

    // Compute default deadline if not set
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

    // Coalescing check
    GeometryJobKey key = spec.key;
    auto it = _activeByKey.find(key);
    if (it != _activeByKey.end()) {
        GeometryJobId activeId = it->second;
        auto jobIt = _jobs.find(activeId);
        if (jobIt != _jobs.end()) {
            JobRecord& activeRec = jobIt->second;
            // Identical generation: join existing job
            if (activeRec.spec.document.modelGeneration == spec.document.modelGeneration &&
                activeRec.state != GeometryJobState::Completed &&
                activeRec.state != GeometryJobState::Cancelled &&
                activeRec.state != GeometryJobState::Failed) {
                return GeometryJobHandle(activeId, key);
            }
            // Newer generation: cancel older active job
            if (spec.document.modelGeneration > activeRec.spec.document.modelGeneration) {
                activeRec.state = GeometryJobState::Cancelled;
                invokeCallbackOnce(activeRec, activeId);
                _activeByKey.erase(it);
            }
        }
    }

    // Allocate new job ID
    GeometryJobId newId = _nextId++;
    spec.id = newId;

    JobRecord rec;
    rec.spec = spec;
    rec.state = GeometryJobState::Queued;

    _jobs[newId] = std::move(rec);
    _activeByKey[key] = newId;

    return GeometryJobHandle(newId, key);
}

void GeometryJobManager::cancel(GeometryJobId id, CancelReason reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        JobRecord& rec = it->second;
        if (rec.state != GeometryJobState::Completed &&
            rec.state != GeometryJobState::Cancelled) {
            rec.state = GeometryJobState::Cancelled;
            rec.result.success = false;
            rec.result.errorCode = "Cancelled";
            rec.result.errorMessage = "Job cancelled due to reason";
            invokeCallbackOnce(rec, id);
            _activeByKey.erase(rec.spec.key);
        }
    }
}

void GeometryJobManager::invalidateDocument(const DocumentRevisionToken& docToken, CancelReason reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [id, rec] : _jobs) {
        if (rec.spec.document.runtimeIncarnation == docToken.runtimeIncarnation) {
            if (rec.spec.document.modelGeneration < docToken.modelGeneration ||
                reason == CancelReason::DocumentClosed) {
                if (rec.state != GeometryJobState::Completed && rec.state != GeometryJobState::Cancelled) {
                    rec.state = (reason == CancelReason::DocumentClosed) ? GeometryJobState::DocumentClosed : GeometryJobState::Stale;
                    invokeCallbackOnce(rec, id);
                    _activeByKey.erase(rec.spec.key);
                }
            }
        }
    }
}

void GeometryJobManager::invalidateObject(const ObjectRevisionToken& objToken, CancelReason reason)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto& [id, rec] : _jobs) {
        if (rec.spec.target.objectId == objToken.objectId &&
            rec.spec.target.internalName == objToken.internalName) {
            if (rec.state != GeometryJobState::Completed && rec.state != GeometryJobState::Cancelled) {
                rec.state = GeometryJobState::Stale;
                invokeCallbackOnce(rec, id);
                _activeByKey.erase(rec.spec.key);
            }
        }
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
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        JobRecord& rec = it->second;
        rec.callback = callback;
        // If already completed or failed, invoke immediately
        if ((rec.state == GeometryJobState::Completed ||
             rec.state == GeometryJobState::Cancelled ||
             rec.state == GeometryJobState::Failed ||
             rec.state == GeometryJobState::Stale ||
             rec.state == GeometryJobState::DocumentClosed) &&
            !rec.callbackInvoked) {
            invokeCallbackOnce(rec, id);
        }
    }
}

void GeometryJobManager::setJobState(GeometryJobId id, GeometryJobState state, const DetachedGeometryResult& result)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        JobRecord& rec = it->second;
        rec.state = state;
        if (result.success || !result.errorCode.empty()) {
            rec.result = result;
        }
        if (state == GeometryJobState::Completed ||
            state == GeometryJobState::Cancelled ||
            state == GeometryJobState::Failed ||
            state == GeometryJobState::Stale ||
            state == GeometryJobState::DocumentClosed) {
            invokeCallbackOnce(rec, id);
            _activeByKey.erase(rec.spec.key);
        }
    }
}

void GeometryJobManager::updateProgress(GeometryJobId id, double fraction, const std::string& phase)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _jobs.find(id);
    if (it != _jobs.end()) {
        it->second.progress = fraction;
        it->second.progressPhase = phase;
    }
}

void GeometryJobManager::invokeCallbackOnce(JobRecord& rec, GeometryJobId id)
{
    if (!rec.callbackInvoked && rec.callback) {
        rec.callbackInvoked = true;
        JobCallback cb = rec.callback;
        GeometryJobState state = rec.state;
        DetachedGeometryResult res = rec.result;

        // Execute callback outside lock safety
        cb(id, state, res);
    }
}

void GeometryJobManager::setAllowInProcess(bool allow)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _allowInProcess = allow;
}

bool GeometryJobManager::isAllowInProcess() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _allowInProcess;
}

} // namespace App
