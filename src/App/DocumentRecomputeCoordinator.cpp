// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentRecomputeCoordinator.h"
#include "Document.h"
#include "DocumentObject.h"
#include "GeometryJobManager.h"
#include <Base/Console.h>

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

RecomputeHandle DocumentRecomputeCoordinator::request(RecomputeTargets targets, RecomputeOptions options)
{
    if (_document.testStatus(Document::Restoring) || _document.testStatus(Document::PartialRestore)) {
        return RecomputeHandle(0);
    }


    if (_isRecomputing) {
        // Union dirty roots into active session
        return RecomputeHandle(_activeSessionId);
    }

    _activeSessionId++;
    _isRecomputing = true;

    // Get current revision token
    DocumentRevisionToken docToken = _document.getRevisionToken();

    // Sliced processing of target objects
    for (long objId : targets.objectIds) {
        DocumentObject* obj = _document.getObjectByID(objId);
        if (!obj) {
            continue;
        }

        SnapshotContext snapCtx;
        snapCtx.jobId = _activeSessionId;
        snapCtx.docToken = docToken;
        snapCtx.objToken = obj->getRevisionToken();

        // Check if object supports detached recompute
        auto prepOpt = obj->prepareDetachedRecompute(snapCtx);
        if (prepOpt.has_value()) {
            GeometryJobSpec spec = prepOpt->spec;
            spec.key.documentIncarnation = docToken.runtimeIncarnation;
            spec.key.targetObjectId = obj->getID();
            spec.key.purpose = options.isPreview ? GeometryJobPurpose::Preview : GeometryJobPurpose::ModelRecompute;

            // Submit to manager
            GeometryJobHandle jobHandle = GeometryJobManager::instance().submit(spec);

            // Register callback to commit on main thread
            GeometryJobManager::instance().registerCallback(jobHandle.id(),
                [&doc = _document, targetId = objId](GeometryJobId jobId, GeometryJobState state, const DetachedGeometryResult& result) {
                    if (state == GeometryJobState::Completed && result.success) {
                        DocumentObject* targetObj = doc.getObjectByID(targetId);
                        if (targetObj) {
                            CommitContext commitCtx;
                            commitCtx.jobId = jobId;
                            commitCtx.docToken = doc.getRevisionToken();
                            commitCtx.objToken = targetObj->getRevisionToken();
                            commitCtx.result = result;

                            // Commit transaction inside GeometryCommitScope
                            targetObj->commitDetachedRecompute(result, commitCtx);
                        }
                    }
                });
        } else {
            // Standard synchronous execution fallback on main thread
            obj->recomputeFeature();
        }
    }

    _isRecomputing = false;
    return RecomputeHandle(_activeSessionId);
}

void DocumentRecomputeCoordinator::cancelCurrentSession(CancelReason reason)
{
    if (_isRecomputing && _activeSessionId != 0) {
        GeometryJobManager::instance().cancel(_activeSessionId, reason);
        _isRecomputing = false;
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
    if (_activeSessionId != 0) {
        DocumentRevisionToken docToken = _document.getRevisionToken();
        GeometryJobManager::instance().invalidateDocument(docToken, CancelReason::DocumentClosed);
        _isRecomputing = false;
        _activeSessionId = 0;
    }
}

void DocumentRecomputeCoordinator::onObjectRemoved(long objectId)
{
    ObjectRevisionToken objToken;
    objToken.objectId = objectId;
    GeometryJobManager::instance().invalidateObject(objToken, CancelReason::ObjectRemoved);
}

} // namespace App
