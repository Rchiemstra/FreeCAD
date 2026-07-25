// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentRecomputeCoordinator.h"
#include "Document.h"
#include "DocumentObject.h"
#include "GeometryJobManager.h"
#include "GuiResponsivenessProbe.h"
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Base/Type.h>

#include <algorithm>
#include <unordered_set>

namespace App
{

DocumentRecomputeCoordinator::DocumentRecomputeCoordinator(Document& doc)
    : _document(doc)
{
}

DocumentRecomputeCoordinator::~DocumentRecomputeCoordinator()
{
    onDocumentClosed();
}

void DocumentRecomputeCoordinator::expandTargets(RecomputeTargets& targets) const
{
    if (!targets.objectIds.empty() && !targets.forceAll) {
        return;
    }

    targets.objectIds.clear();
    const auto& objects = _document.getObjects();
    for (DocumentObject* obj : objects) {
        if (!obj) {
            continue;
        }
        if (targets.forceAll || obj->isTouched() || obj->mustExecute()) {
            targets.objectIds.push_back(obj->getID());
        }
    }
}

std::vector<long> DocumentRecomputeCoordinator::dependencyOrderedIds(
    const std::vector<long>& objectIds) const
{
    std::vector<DocumentObject*> roots;
    roots.reserve(objectIds.size());
    std::unordered_set<long> wanted(objectIds.begin(), objectIds.end());

    for (long id : objectIds) {
        if (DocumentObject* obj = _document.getObjectByID(id)) {
            roots.push_back(obj);
        }
    }

    std::vector<long> ordered;
    if (roots.empty()) {
        return ordered;
    }

    try {
        const auto deps = Document::getDependencyList(roots, Document::DepSort);
        for (DocumentObject* obj : deps) {
            if (obj && wanted.contains(obj->getID())) {
                ordered.push_back(obj->getID());
            }
        }
    }
    catch (const Base::BadGraphError&) {
        ordered = objectIds;
    }

    // Preserve any IDs that dependency sorting omitted.
    std::unordered_set<long> seen(ordered.begin(), ordered.end());
    for (long id : objectIds) {
        if (!seen.contains(id)) {
            ordered.push_back(id);
        }
    }
    return ordered;
}

bool DocumentRecomputeCoordinator::sliceBudgetExhausted() const
{
    if (_options.maxGuiSlice.count() <= 0) {
        return false;
    }
    return std::chrono::steady_clock::now() >= _sliceDeadline;
}

RecomputeHandle DocumentRecomputeCoordinator::request(RecomputeTargets targets,
                                                      RecomputeOptions options)
{
    GuiResponsivenessProbe::ScopedCallback slice("DocumentRecomputeCoordinator::request");
    assertDocumentThread("DocumentRecomputeCoordinator::request");

    if (_document.testStatus(Document::Restoring) || _document.testStatus(Document::PartialRestore)) {
        return RecomputeHandle(0);
    }

    // One model-recompute session per document: join while in flight.
    if (_isRecomputing) {
        if (options.isPreview == _isPreview) {
            // Union newly requested roots into the remaining queue.
            expandTargets(targets);
            std::unordered_set<long> pending(_remainingTargets.begin(), _remainingTargets.end());
            for (long id : targets.objectIds) {
                if (!pending.contains(id)) {
                    _remainingTargets.push_back(id);
                    pending.insert(id);
                }
            }
        }
        return RecomputeHandle(_activeSessionId);
    }

    expandTargets(targets);
    auto ordered = dependencyOrderedIds(targets.objectIds);

    _activeSessionId++;
    _isRecomputing = true;
    _isPreview = options.isPreview;
    _options = options;
    _sliceDeadline = std::chrono::steady_clock::now() + options.maxGuiSlice;
    _pendingJobs.clear();
    _unsupportedObjects.clear();
    _remainingTargets.assign(ordered.begin(), ordered.end());

    if (_remainingTargets.empty() || !submitNext()) {
        _isRecomputing = false;
    }

    return RecomputeHandle(_activeSessionId);
}

bool DocumentRecomputeCoordinator::submitNext()
{
    while (!_remainingTargets.empty()) {
        if (sliceBudgetExhausted()) {
            // Leave remaining targets queued; caller / next request resumes.
            Base::Console().log(
                "DocumentRecomputeCoordinator: GUI slice budget exhausted; deferring %zu targets\n",
                _remainingTargets.size());
            return !_pendingJobs.empty();
        }

        const long objId = _remainingTargets.front();
        _remainingTargets.pop_front();

        DocumentObject* obj = _document.getObjectByID(objId);
        if (!obj) {
            continue;
        }

        const DocumentRevisionToken docToken = _document.getRevisionToken();

        SnapshotContext snapCtx;
        snapCtx.jobId = _activeSessionId;
        snapCtx.docToken = docToken;
        snapCtx.objToken = obj->getRevisionToken();

        auto prepOpt = obj->prepareDetachedRecompute(snapCtx);
        if (!prepOpt.has_value()) {
            const char* name = obj->getNameInDocument() ? obj->getNameInDocument() : "<unnamed>";
            _unsupportedObjects.emplace_back(name);
            Base::Console().warning(
                "DocumentRecomputeCoordinator: object '%s' has no detached recompute adapter; "
                "skipping (no GUI-thread OCC fallback)\n",
                name);
            continue;
        }

        GeometryJobSpec spec = prepOpt->spec;
        spec.document = docToken;
        spec.target = snapCtx.objToken;
        if (spec.target.objectId == 0) {
            spec.target.objectId = obj->getID();
        }
        if (spec.target.internalName.empty() && obj->getNameInDocument()) {
            spec.target.internalName = obj->getNameInDocument();
        }
        if (spec.target.type.isBad()) {
            spec.target.type = obj->getTypeId();
        }
        spec.key.documentIncarnation = docToken.runtimeIncarnation;
        spec.key.targetObjectId = obj->getID();
        spec.key.purpose = _isPreview ? GeometryJobPurpose::Preview
                                      : GeometryJobPurpose::ModelRecompute;
        if (_isPreview) {
            spec.coalescing = CoalesceMode::LatestWins;
        }
        else {
            spec.coalescing = CoalesceMode::SingleInstance;
        }

        if (prepOpt->inputFingerprint.empty() && spec.task) {
            prepOpt->inputFingerprint = makeGeometryInputFingerprint(spec.task.get());
        }

        CommitFence fence;
        fence.jobId = 0; // filled after submit assigns id
        fence.objectId = obj->getID();
        fence.objectName = obj->getNameInDocument() ? obj->getNameInDocument() : std::string();
        fence.objectType = obj->getTypeId();
        fence.runtimeIncarnation = docToken.runtimeIncarnation;
        fence.modelGeneration = docToken.modelGeneration;
        fence.inputFingerprint = prepOpt->inputFingerprint;

        GeometryJobHandle jobHandle = GeometryJobManager::instance().submit(spec);
        if (!jobHandle.isValid()) {
            continue;
        }
        fence.jobId = jobHandle.id();

        _pendingJobs.insert(jobHandle.id());

        const std::string expectedFingerprint = fence.inputFingerprint;
        GeometryJobManager::instance().registerCallback(
            jobHandle.id(),
            [this, targetId = objId, fence, expectedFingerprint](
                GeometryJobId jobId, GeometryJobState state, const DetachedGeometryResult& result) {
                GuiResponsivenessProbe::ScopedCallback cb(
                    "DocumentRecomputeCoordinator::commitCallback");
                assertDocumentThread("DocumentRecomputeCoordinator::commitCallback");

                if (state == GeometryJobState::Completed && result.success) {
                    DocumentObject* targetObj = _document.getObjectByID(targetId);
                    if (!targetObj
                        || !commitFenceMatches(fence, _document, *targetObj, jobId)) {
                        ++_rejectedCommits;
                        Base::Console().log(
                            "DocumentRecomputeCoordinator: rejecting fenced result for job %llu\n",
                            static_cast<unsigned long long>(jobId));
                    }
                    else {
                        // Optional result-side fingerprint must match prepare-time fence when set.
                        if (!expectedFingerprint.empty()
                            && !result.resultArchivePath.empty()) {
                            // Archive path alone is not a fingerprint; adapters may stash
                            // digest in errorMessage when using synthetic results in tests.
                        }

                        CommitContext commitCtx;
                        commitCtx.jobId = jobId;
                        commitCtx.docToken = _document.getRevisionToken();
                        commitCtx.objToken = targetObj->getRevisionToken();
                        commitCtx.modelGeneration = fence.modelGeneration;
                        commitCtx.result = result;
                        commitCtx.fence = fence;

                        bool committed = false;
                        _document.openTransaction("DetachedGeometryCommit");
                        try {
                            GeometryCommitScope scope(&_document, targetObj);
                            DocumentObjectExecReturn* ret =
                                targetObj->commitDetachedRecompute(result, commitCtx);
                            if (ret && ret != DocumentObject::StdReturn) {
                                Base::Console().warning(
                                    "DocumentRecomputeCoordinator: commit failed for '%s': %s\n",
                                    fence.objectName.c_str(),
                                    ret->Why.c_str());
                                delete ret;
                                _document.abortTransaction();
                            }
                            else {
                                scope.markSucceeded();
                                committed = true;
                                _document.commitTransaction();
                            }
                        }
                        catch (const Base::Exception& ex) {
                            Base::Console().warning(
                                "DocumentRecomputeCoordinator: commit exception for '%s': %s\n",
                                fence.objectName.c_str(),
                                ex.what());
                            _document.abortTransaction();
                        }
                        catch (...) {
                            Base::Console().warning(
                                "DocumentRecomputeCoordinator: unknown commit exception for '%s'\n",
                                fence.objectName.c_str());
                            _document.abortTransaction();
                        }
                        if (!committed) {
                            // Generation must not advance on failed commit (scope skipped markSucceeded).
                        }
                    }
                }

                onJobFinished(jobId, state);
            });

        return true;
    }

    return false;
}

void DocumentRecomputeCoordinator::onJobFinished(GeometryJobId jobId, GeometryJobState /*state*/)
{
    _pendingJobs.erase(jobId);
    if (!_remainingTargets.empty()) {
        // Refresh slice deadline for the next planning/snapshot burst on this thread.
        _sliceDeadline = std::chrono::steady_clock::now() + _options.maxGuiSlice;
        if (!submitNext() && _pendingJobs.empty()) {
            _isRecomputing = false;
        }
        return;
    }
    if (_pendingJobs.empty()) {
        _isRecomputing = false;
    }
}

void DocumentRecomputeCoordinator::cancelCurrentSession(CancelReason reason)
{
    assertDocumentThread("DocumentRecomputeCoordinator::cancelCurrentSession");
    if (_activeSessionId == 0) {
        return;
    }

    // Copy IDs first: cancel callbacks must not invalidate this iteration.
    const std::vector<GeometryJobId> pending(_pendingJobs.begin(), _pendingJobs.end());
    _remainingTargets.clear();
    _pendingJobs.clear();
    _isRecomputing = false;

    for (GeometryJobId id : pending) {
        GeometryJobManager::instance().cancel(id, reason);
    }
}

bool DocumentRecomputeCoordinator::isRecomputing() const
{
    return _isRecomputing;
}

uint64_t DocumentRecomputeCoordinator::activeSessionId() const
{
    return _activeSessionId;
}

void DocumentRecomputeCoordinator::onDocumentClosed()
{
    DocumentRevisionToken docToken = _document.getRevisionToken();
    GeometryJobManager::instance().invalidateDocument(docToken, CancelReason::DocumentClosed);
    _remainingTargets.clear();
    _pendingJobs.clear();
    _isRecomputing = false;
    _activeSessionId = 0;
}

void DocumentRecomputeCoordinator::onObjectRemoved(long objectId)
{
    ObjectRevisionToken objToken;
    objToken.objectId = objectId;
    objToken.documentIncarnation = _document.getRuntimeIncarnation();
    GeometryJobManager::instance().invalidateObject(objToken, CancelReason::ObjectRemoved);

    std::deque<long> kept;
    for (long id : _remainingTargets) {
        if (id != objectId) {
            kept.push_back(id);
        }
    }
    _remainingTargets.swap(kept);
}

} // namespace App
