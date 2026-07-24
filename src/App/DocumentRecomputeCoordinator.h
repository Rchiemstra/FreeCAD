// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include "GeometryJob.h"
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>

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

class AppExport DocumentRecomputeCoordinator
{
public:
    explicit DocumentRecomputeCoordinator(Document& doc);
    ~DocumentRecomputeCoordinator();

    RecomputeHandle request(RecomputeTargets targets, RecomputeOptions options);
    void cancelCurrentSession(CancelReason reason);
    bool isRecomputing() const;
    uint64_t activeSessionId() const;

    void onDocumentClosed();
    void onObjectRemoved(long objectId);

private:
    Document& _document;
    uint64_t _activeSessionId {0};
    bool _isRecomputing {false};
};

} // namespace App
