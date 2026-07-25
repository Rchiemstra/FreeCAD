// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "GeometryJob.h"
#include <vector>
#include <deque>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace App
{

class Document;

struct AppExport RecomputeTargets
{
    std::vector<long> objectIds;
    bool forceAll {false};
};

struct AppExport RecomputeOptions
{
    bool isPreview {false};
    bool allowAsync {true};
    std::chrono::milliseconds maxGuiSlice {33};
};

class AppExport RecomputeHandle
{
public:
    RecomputeHandle() = default;
    explicit RecomputeHandle(uint64_t id) : _id(id) {}
    uint64_t id() const { return _id; }
    bool isValid() const { return _id != 0; }

private:
    uint64_t _id {0};
};

/**
 * @brief Per-document coordinator for detached geometry recompute sessions.
 *
 * Captures and commits features in dependency order. Only one feature is
 * in-flight at a time so a commit generation advance cannot stale siblings.
 * Unsupported objects are recorded and skipped (no GUI-thread OCC fallback).
 */
class AppExport DocumentRecomputeCoordinator
{
public:
    explicit DocumentRecomputeCoordinator(Document& doc);
    ~DocumentRecomputeCoordinator();

    RecomputeHandle request(RecomputeTargets targets, RecomputeOptions options);
    void cancelCurrentSession(CancelReason reason);
    bool isRecomputing() const;
    uint64_t activeSessionId() const;

    const std::vector<std::string>& unsupportedObjects() const { return _unsupportedObjects; }

    /// Number of commits rejected by fence mismatch (tests / diagnostics).
    uint64_t rejectedCommitCount() const { return _rejectedCommits; }

    void onDocumentClosed();
    void onObjectRemoved(long objectId);

private:
    void expandTargets(RecomputeTargets& targets) const;
    std::vector<long> dependencyOrderedIds(const std::vector<long>& objectIds) const;
    bool submitNext();
    void onJobFinished(GeometryJobId jobId, GeometryJobState state);
    bool sliceBudgetExhausted() const;

    Document& _document;
    uint64_t _activeSessionId {0};
    bool _isRecomputing {false};
    bool _isPreview {false};
    RecomputeOptions _options;
    std::chrono::steady_clock::time_point _sliceDeadline {};
    std::deque<long> _remainingTargets;
    std::unordered_set<GeometryJobId> _pendingJobs;
    std::vector<std::string> _unsupportedObjects;
    uint64_t _rejectedCommits {0};
};

} // namespace App
