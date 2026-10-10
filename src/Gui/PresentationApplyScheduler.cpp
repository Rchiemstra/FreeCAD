// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PresentationApplyScheduler.h"

#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoIndexedFaceSet.h>
#include <Inventor/nodes/SoNormal.h>
#include <Inventor/nodes/SoSeparator.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
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

SoSeparator* buildCoinSubtree(const PresentationRenderBuffer& buffer)
{
    auto* root = new SoSeparator;
    root->ref();

    if (!buffer.vertices.empty()) {
        auto* coords = new SoCoordinate3;
        const int pointCount = static_cast<int>(buffer.vertices.size() / 3);
        coords->point.setNum(pointCount);
        SbVec3f* points = coords->point.startEditing();
        for (int i = 0; i < pointCount; ++i) {
            points[i].setValue(
                buffer.vertices[static_cast<std::size_t>(i) * 3U],
                buffer.vertices[static_cast<std::size_t>(i) * 3U + 1U],
                buffer.vertices[static_cast<std::size_t>(i) * 3U + 2U]);
        }
        coords->point.finishEditing();
        root->addChild(coords);
    }

    if (!buffer.normals.empty()) {
        auto* normals = new SoNormal;
        const int normalCount = static_cast<int>(buffer.normals.size() / 3);
        normals->vector.setNum(normalCount);
        SbVec3f* vectors = normals->vector.startEditing();
        for (int i = 0; i < normalCount; ++i) {
            vectors[i].setValue(
                buffer.normals[static_cast<std::size_t>(i) * 3U],
                buffer.normals[static_cast<std::size_t>(i) * 3U + 1U],
                buffer.normals[static_cast<std::size_t>(i) * 3U + 2U]);
        }
        normals->vector.finishEditing();
        root->addChild(normals);
    }

    if (!buffer.indices.empty()) {
        auto* faces = new SoIndexedFaceSet;
        const int indexCount = static_cast<int>(buffer.indices.size());
        // Coin face sets expect -1 sentinels between triangles when using
        // indexed triangles; append one sentinel after every three indices.
        const int coinIndexCount = indexCount + (indexCount / 3);
        faces->coordIndex.setNum(coinIndexCount);
        int32_t* indices = faces->coordIndex.startEditing();
        int out = 0;
        for (int i = 0; i < indexCount; ++i) {
            indices[out++] = static_cast<int32_t>(buffer.indices[static_cast<std::size_t>(i)]);
            if ((i % 3) == 2) {
                indices[out++] = -1;
            }
        }
        faces->coordIndex.finishEditing();
        root->addChild(faces);
    }

    return root;
}

}  // namespace

PresentationApplyScheduler::PresentationApplyScheduler(DocumentPresentationCache& cache)
    : _cache(cache)
{}

PresentationApplyScheduler::~PresentationApplyScheduler()
{
    if (_stagingCoinRoot) {
        _stagingCoinRoot->unref();
        _stagingCoinRoot = nullptr;
    }
}

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

    if (_stagingCoinRoot) {
        _stagingCoinRoot->unref();
        _stagingCoinRoot = nullptr;
    }

    _stagingPacket = std::move(delta);
    _stagingBuild = {};
    _stagingBuild.revision = _stagingPacket->revision;
    _stagingBuild.status = _stagingPacket->status;
    _stagingBuild.status.state = DocumentPresentationState::Pending;
    _nextSlice = 0;
    // Detached staging root: old committed root stays navigable until commit.
    _stagingCoinRoot = new SoSeparator;
    _stagingCoinRoot->ref();
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

    SoSeparator* rootToCommit = _stagingCoinRoot;
    _stagingCoinRoot = nullptr;
    const auto commitResult =
        _cache.tryCommitWithCoinRoot(std::move(_stagingBuild), rootToCommit);
    if (rootToCommit) {
        rootToCommit->unref();
    }
    result.committedRevision = commitResult == PresentationCommitResult::Accepted;
    if (!result.committedRevision) {
        // Keep the previous committed presentation and surface the apply error.
        DocumentPresentationStatus observation = _cache.status();
        observation.state = DocumentPresentationState::Error;
        observation.errorMessage = std::string("presentation commit rejected: ")
            + presentationCommitResultName(commitResult);
        _cache.publishObservation(observation);
    }

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
                if (_stagingCoinRoot) {
                    SoSeparator* subtree = buildCoinSubtree(source);
                    _stagingCoinRoot->addChild(subtree);
                    subtree->unref();
                }
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
