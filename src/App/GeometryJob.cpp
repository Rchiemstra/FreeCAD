// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryJob.h"
#include "Document.h"
#include "DocumentObject.h"
#include "StringHasher.h"

namespace App
{

std::string makeGeometryInputFingerprint(const DetachedGeometryTask* task)
{
    if (!task) {
        return {};
    }
    return task->operationType() + "|" + std::to_string(task->codecVersion()) + "|"
        + task->parameterDigest();
}

bool commitFenceMatches(const CommitFence& fence,
                        const Document& doc,
                        const DocumentObject& obj,
                        GeometryJobId jobId)
{
    if (fence.jobId != 0 && fence.jobId != jobId) {
        return false;
    }
    if (fence.objectId != 0 && fence.objectId != obj.getID()) {
        return false;
    }
    const char* liveName = obj.getNameInDocument();
    if (!fence.objectName.empty()
        && (!liveName || fence.objectName != liveName)) {
        return false;
    }
    if (!fence.objectType.isBad() && obj.getTypeId() != fence.objectType) {
        return false;
    }
    const DocumentRevisionToken live = doc.getRevisionToken();
    if (fence.runtimeIncarnation != 0
        && fence.runtimeIncarnation != live.runtimeIncarnation) {
        return false;
    }
    if (fence.modelGeneration != 0 && fence.modelGeneration != live.modelGeneration) {
        return false;
    }
    return true;
}

bool isTrustedRelativeResultPath(const std::string& relativePath)
{
    if (relativePath.empty()) {
        return false;
    }
    // Reject absolute paths (Unix '/', Windows drive, UNC).
    if (relativePath[0] == '/' || relativePath[0] == '\\') {
        return false;
    }
    if (relativePath.size() >= 2 && relativePath[1] == ':') {
        return false;
    }
    // Reject any ".." path segment.
    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t end = relativePath.find_first_of("/\\", start);
        const size_t partEnd = (end == std::string::npos) ? relativePath.size() : end;
        if (partEnd > start) {
            const std::string part = relativePath.substr(start, partEnd - start);
            if (part == "..") {
                return false;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

GeometryCommitScope::GeometryCommitScope(Document* doc, DocumentObject* obj)
    : _doc(doc), _obj(obj)
{
    if (_doc) {
        _doc->setCommittingGeometryJob(true);
    }
}

void GeometryCommitScope::markSucceeded()
{
    _succeeded = true;
}

GeometryCommitScope::~GeometryCommitScope()
{
    if (_doc) {
        _doc->setCommittingGeometryJob(false);
        // Advance generation only after a successful commit; never cancel same-session
        // jobs that the coordinator may still have queued.
        if (_succeeded) {
            _doc->advanceModelGeneration(/*invalidatePendingJobs=*/false);
            // Advance the document hasher revision so detached hasher deltas captured
            // against the pre-commit revision cannot be merged again.
            if (auto hasher = _doc->getStringHasher()) {
                hasher->advanceRevision();
            }
        }
    }
}

} // namespace App
