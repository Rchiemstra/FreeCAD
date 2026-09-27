// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PresentationApplyScheduler.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace Gui
{

namespace
{

using Clock = std::chrono::steady_clock;

std::uint64_t elapsedMicros(const Clock::time_point start, const Clock::time_point end)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

}  // namespace

PresentationApplyScheduler::PresentationApplyScheduler(DocumentPresentationCache& cache)
    : _cache(cache)
{}

void PresentationApplyScheduler::enqueue(PresentationDelta&& delta)
{
    if (!delta.revision.valid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    if (!_cache.acceptsPacket(delta.revision)) {
        return;
    }

    if (_stagingPacket) {
        if (delta.revision.sequence <= _stagingPacket->revision.sequence) {
            return;
        }
    }

    _stagingPacket = std::move(delta);
    _stagingBuild = {};
    _stagingBuild.revision = _stagingPacket->revision;
    _stagingBuild.status = _stagingPacket->status;
    _stagingBuild.status.state = DocumentPresentationState::Pending;
    _nextSlice = 0;
    rebuildSlicePlan();

    DocumentPresentationStatus pending;
    pending.revision = _stagingPacket->revision;
    pending.state = DocumentPresentationState::Pending;
    pending.statusMessage = _stagingPacket->status.statusMessage;
    _cache.publishObservation(pending);
}

PresentationApplyPumpResult PresentationApplyScheduler::pump(const int budgetMs)
{
    PresentationApplyPumpResult result;
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_stagingPacket || _slicePlan.empty()) {
        return result;
    }

    DocumentPresentationStatus applying;
    applying.revision = _stagingPacket->revision;
    applying.state = DocumentPresentationState::Applying;
    applying.statusMessage = _stagingPacket->status.statusMessage;
    _cache.publishObservation(applying);

    const auto pumpStart = Clock::now();
    const auto deadline =
        budgetMs > 0 ? pumpStart + std::chrono::milliseconds(budgetMs) : Clock::time_point::max();

    while (_nextSlice < _slicePlan.size()) {
        const auto sliceStart = Clock::now();
        if (budgetMs > 0 && sliceStart >= deadline) {
            break;
        }

        applySlice(_slicePlan[_nextSlice]);
        const auto sliceEnd = Clock::now();
        result.sliceDurationsMicros.push_back(elapsedMicros(sliceStart, sliceEnd));
        ++result.slicesApplied;
        ++_nextSlice;

        if (budgetMs > 0 && Clock::now() >= deadline) {
            break;
        }
    }

    if (_nextSlice < _slicePlan.size()) {
        return result;
    }

    result.stagingComplete = true;
    _stagingBuild.status.revision = _stagingBuild.revision;
    _stagingBuild.status.state = DocumentPresentationState::Committed;

    const auto commitResult = _cache.tryCommit(std::move(_stagingBuild));
    result.committedRevision = commitResult == PresentationCommitResult::Accepted;

    _stagingPacket.reset();
    _stagingBuild = {};
    _slicePlan.clear();
    _nextSlice = 0;

    return result;
}

bool PresentationApplyScheduler::hasStagingWork() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _stagingPacket.has_value();
}

std::optional<PresentationRevision> PresentationApplyScheduler::stagingRevision() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_stagingPacket) {
        return std::nullopt;
    }
    return _stagingPacket->revision;
}

void PresentationApplyScheduler::rebuildSlicePlan()
{
    _slicePlan.clear();
    if (!_stagingPacket) {
        return;
    }

    _slicePlan.push_back({ApplySliceKind::Tree});
    _slicePlan.push_back({ApplySliceKind::Properties});
    _slicePlan.push_back({ApplySliceKind::Selection});

    for (std::size_t index = 0; index < _stagingPacket->renderBuffers.size(); ++index) {
        _slicePlan.push_back({ApplySliceKind::RenderBuffer, index});
    }
}

void PresentationApplyScheduler::applySlice(const ApplySlicePlan& slice)
{
    if (!_stagingPacket) {
        return;
    }

    switch (slice.kind) {
        case ApplySliceKind::Tree:
            _stagingBuild.tree = _stagingPacket->tree;
            break;
        case ApplySliceKind::Properties:
            _stagingBuild.properties = _stagingPacket->properties;
            break;
        case ApplySliceKind::Selection:
            _stagingBuild.selectionMappings = _stagingPacket->selectionMappings;
            break;
        case ApplySliceKind::RenderBuffer:
            if (slice.renderBufferIndex < _stagingPacket->renderBuffers.size()) {
                const auto& source = _stagingPacket->renderBuffers[slice.renderBufferIndex];
                touchBuffer(source);
                if (_stagingBuild.renderBuffers.size() <= slice.renderBufferIndex) {
                    _stagingBuild.renderBuffers.resize(slice.renderBufferIndex + 1);
                }
                _stagingBuild.renderBuffers[slice.renderBufferIndex] = source;
            }
            break;
    }
}

void PresentationApplyScheduler::touchBuffer(const PresentationRenderBuffer& buffer) const
{
    std::uint64_t checksum = 0;
    for (const float value : buffer.vertices) {
        checksum += static_cast<std::uint64_t>(value * 1000.0F);
    }
    for (const float value : buffer.normals) {
        checksum += static_cast<std::uint64_t>(value * 1000.0F);
    }
    for (const std::uint32_t index : buffer.indices) {
        checksum += index;
    }
    (void)checksum;
}

}  // namespace Gui
