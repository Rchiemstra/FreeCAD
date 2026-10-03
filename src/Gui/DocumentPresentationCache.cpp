// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentPresentationCache.h"

#include <Inventor/nodes/SoSeparator.h>

#include <stdexcept>
#include <utility>

namespace Gui
{

const char* presentationCommitResultName(const PresentationCommitResult result) noexcept
{
    switch (result) {
        case PresentationCommitResult::Accepted:
            return "Accepted";
        case PresentationCommitResult::Unbound:
            return "Unbound";
        case PresentationCommitResult::StaleDocumentIdentity:
            return "StaleDocumentIdentity";
        case PresentationCommitResult::StaleSequence:
            return "StaleSequence";
        case PresentationCommitResult::StaleSourceRevision:
            return "StaleSourceRevision";
        case PresentationCommitResult::InvalidRevision:
            return "InvalidRevision";
    }
    return "Unknown";
}

DocumentPresentationCache::DocumentPresentationCache()
{
    _status.state = DocumentPresentationState::Committed;
}

DocumentPresentationCache::~DocumentPresentationCache()
{
    if (_committedCoinRoot) {
        _committedCoinRoot->unref();
        _committedCoinRoot = nullptr;
    }
}

void DocumentPresentationCache::bindDocumentIdentity(
    const App::DocumentInstanceId documentInstanceId,
    const App::DocumentLifecycleEpoch lifecycleEpoch)
{
    if (documentInstanceId == 0 || lifecycleEpoch == 0) {
        throw std::invalid_argument(
            "document-presentation identity values must be nonzero");
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        _documentIdentity =
            App::DocumentRevisionIdentityBinding {documentInstanceId, lifecycleEpoch};
        _status.revision.documentInstanceId = documentInstanceId;
        _status.revision.lifecycleEpoch = lifecycleEpoch;
        return;
    }
    if (_documentIdentity->documentInstanceId != documentInstanceId) {
        throw std::logic_error(
            "document-presentation cache cannot be rebound to another document instance");
    }
    if (lifecycleEpoch < _documentIdentity->lifecycleEpoch) {
        throw std::invalid_argument(
            "document-presentation lifecycle epoch cannot rewind");
    }
    _documentIdentity->lifecycleEpoch = lifecycleEpoch;
    _status.revision.lifecycleEpoch = lifecycleEpoch;
}

std::optional<App::DocumentRevisionIdentityBinding>
DocumentPresentationCache::documentIdentity() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _documentIdentity;
}

bool DocumentPresentationCache::acceptsPacket(
    const PresentationRevision& revision) const noexcept
{
    if (!revision.valid()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        return false;
    }
    if (revision.documentInstanceId != _documentIdentity->documentInstanceId
        || revision.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
        return false;
    }
    if (revision.sequence <= _committedSequence) {
        return false;
    }
    if (revision.sourceModelRevision < _committedSourceRevision) {
        return false;
    }
    return true;
}

PresentationCommitResult DocumentPresentationCache::tryCommitLocked(PresentationDelta&& delta)
{
    if (!delta.revision.valid()) {
        return PresentationCommitResult::InvalidRevision;
    }
    if (!_documentIdentity) {
        return PresentationCommitResult::Unbound;
    }
    if (delta.revision.documentInstanceId != _documentIdentity->documentInstanceId
        || delta.revision.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
        return PresentationCommitResult::StaleDocumentIdentity;
    }
    if (delta.revision.sequence <= _committedSequence) {
        return PresentationCommitResult::StaleSequence;
    }
    if (delta.revision.sourceModelRevision < _committedSourceRevision) {
        return PresentationCommitResult::StaleSourceRevision;
    }

    auto committed = std::make_shared<PresentationDelta>(std::move(delta));
    committed->status.revision = committed->revision;
    if (committed->status.state != DocumentPresentationState::Error) {
        committed->status.state = DocumentPresentationState::Committed;
    }

    _committed = std::shared_ptr<const PresentationDelta>(std::move(committed));
    _committedSequence = _committed->revision.sequence;
    _committedSourceRevision = _committed->revision.sourceModelRevision;
    _committedSequenceAtomic.store(_committedSequence, std::memory_order_release);

    _status.revision = _committed->revision;
    _status.state = _committed->status.state;
    _status.statusMessage = _committed->status.statusMessage;
    _status.errorMessage = _committed->status.errorMessage;

    return PresentationCommitResult::Accepted;
}

void DocumentPresentationCache::activateCoinRootLocked(SoSeparator* coinRoot)
{
    if (_committedCoinRoot) {
        _committedCoinRoot->unref();
        _committedCoinRoot = nullptr;
    }
    if (coinRoot) {
        coinRoot->ref();
        _committedCoinRoot = coinRoot;
    }
}

PresentationCommitResult DocumentPresentationCache::tryCommit(PresentationDelta&& delta)
{
    std::lock_guard<std::mutex> lock(_mutex);
    return tryCommitLocked(std::move(delta));
}

PresentationCommitResult DocumentPresentationCache::tryCommitWithCoinRoot(
    PresentationDelta&& delta,
    SoSeparator* coinRoot)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto result = tryCommitLocked(std::move(delta));
    if (result == PresentationCommitResult::Accepted) {
        activateCoinRootLocked(coinRoot);
    }
    return result;
}

std::optional<PresentationDelta> DocumentPresentationCache::current() const
{
    std::shared_ptr<const PresentationDelta> snapshot;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        snapshot = _committed;
    }
    if (!snapshot) {
        return std::nullopt;
    }
    return *snapshot;
}

SoSeparator* DocumentPresentationCache::committedCoinRoot() const noexcept
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _committedCoinRoot;
}

DocumentPresentationStatus DocumentPresentationCache::status() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _status;
}

PresentationSequence DocumentPresentationCache::committedSequence() const noexcept
{
    return _committedSequenceAtomic.load(std::memory_order_acquire);
}

void DocumentPresentationCache::publishObservation(const DocumentPresentationStatus observation)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _status = observation;
    if (_committed) {
        _status.revision = _committed->revision;
    }
    else if (_documentIdentity) {
        _status.revision.documentInstanceId = _documentIdentity->documentInstanceId;
        _status.revision.lifecycleEpoch = _documentIdentity->lifecycleEpoch;
    }
}

}  // namespace Gui
