// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Base/Uuid.h>
#include <Base/Type.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <cstdint>
#include <optional>
#include <tuple>

namespace App
{

struct AppExport DocumentRevisionToken
{
    Base::Uuid documentUid;
    std::string internalName;
    uint64_t runtimeIncarnation {0};
    uint64_t modelGeneration {0};
};

struct AppExport ObjectRevisionToken
{
    std::string internalName;
    long objectId {0};
    Base::Type type;
};

enum class GeometryJobPurpose
{
    ModelRecompute,
    Preview,
    Tessellation,
    LegacyIsolatedRecompute
};

enum class GeometryBackend
{
    FreeCADCmd,
    VerifiedInProcess
};

enum class GeometryJobState
{
    Queued,
    Snapshotting,
    Running,
    Cancelling,
    Decoding,
    ReadyToCommit,
    Completed,
    Cancelled,
    TimedOut,
    Crashed,
    Failed,
    Stale,
    DocumentClosed
};

enum class CoalesceMode
{
    None,
    LatestWins,
    Union,
    SingleInstance
};

enum class CancelReason
{
    UserRequested,
    NewGeneration,
    SupersededByNewerGeneration = NewGeneration,
    DocumentClosing,
    DocumentClosed = DocumentClosing,
    ObjectRemoved,
    TimedOut
};

struct AppExport GeometryJobKey
{
    uint64_t documentIncarnation {0};
    long targetObjectId {0};
    GeometryJobPurpose purpose {GeometryJobPurpose::ModelRecompute};
    int previewChannel {0};

    //Convenience alias
    long objectId() const { return targetObjectId; }

    bool operator<(const GeometryJobKey& other) const
    {
        return std::tie(documentIncarnation, targetObjectId, purpose, previewChannel) <
               std::tie(other.documentIncarnation, other.targetObjectId, other.purpose, other.previewChannel);
    }
    bool operator==(const GeometryJobKey& other) const
    {
        return std::tie(documentIncarnation, targetObjectId, purpose, previewChannel) ==
               std::tie(other.documentIncarnation, other.targetObjectId, other.purpose, other.previewChannel);
    }
};

using GeometryJobId = uint64_t;

struct AppExport GeometryOperationTraits
{
    bool allowInProcess {false};
    bool supportsInProcess {false};
    bool supportsCooperativeCancel {true};
    uint32_t maxMemoryMb {2048};
    std::string operationName;
};

class GeometryWorkerContext
{
public:
    virtual ~GeometryWorkerContext() = default;
    virtual void reportProgress(double fraction, const std::string& phase = "") = 0;
    virtual bool isCancelled() const = 0;
    virtual std::chrono::steady_clock::time_point deadline() const = 0;
    virtual std::string tempDir() const = 0;
};

struct AppExport DetachedGeometryResult
{
    bool success {false};
    std::string resultArchivePath;
    std::string errorCode;
    std::string errorMessage;
    double executionTimeSeconds {0.0};
};

class AppExport GeometryArchiveWriter
{
public:
    virtual ~GeometryArchiveWriter() = default;
    virtual void writeSection(const std::string& name, const std::vector<uint8_t>& data) = 0;
    virtual void writeString(const std::string& name, const std::string& value) = 0;
    virtual void writeBytes(const std::string& name, const uint8_t* data, size_t size) = 0;
};

class AppExport GeometryArchiveReader
{
public:
    virtual ~GeometryArchiveReader() = default;
    virtual std::string readString(const std::string& name) const = 0;
    virtual void readBytes(const std::string& name, std::vector<uint8_t>& outData) const = 0;
};


class AppExport DetachedGeometryTask
{
public:
    virtual ~DetachedGeometryTask() = default;
    virtual std::string operationType() const = 0;
    virtual uint32_t codecVersion() const = 0;
    virtual GeometryOperationTraits traits() const = 0;
    virtual DetachedGeometryResult run(GeometryWorkerContext& ctx) const = 0;
    virtual void writeArchive(GeometryArchiveWriter& writer) const = 0;
};

struct AppExport GeometryJobSpec
{
    GeometryJobId id {0};
    DocumentRevisionToken document;
    ObjectRevisionToken target;
    GeometryJobKey key;
    GeometryJobPurpose purpose {GeometryJobPurpose::ModelRecompute};
    GeometryBackend backend {GeometryBackend::FreeCADCmd};
    std::chrono::steady_clock::time_point deadline;
    CoalesceMode coalescing {CoalesceMode::LatestWins};
    std::shared_ptr<const DetachedGeometryTask> task;
};

class AppExport GeometryJobHandle
{
public:
    GeometryJobHandle() = default;
    GeometryJobHandle(GeometryJobId id, GeometryJobKey key) : _id(id), _key(key) {}
    GeometryJobId id() const { return _id; }
    GeometryJobKey key() const { return _key; }
    bool isValid() const { return _id != 0; }

private:
    GeometryJobId _id {0};
    GeometryJobKey _key;
};

class Document;
class DocumentObject;

struct AppExport PreparedDetachedRecompute
{
    GeometryJobSpec spec;
};

struct AppExport SnapshotContext
{
    DocumentRevisionToken docToken;
    ObjectRevisionToken objToken;
    GeometryJobId jobId {0};
};

struct AppExport CommitContext
{
    DocumentRevisionToken docToken;
    ObjectRevisionToken objToken;
    GeometryJobId jobId {0};
    uint64_t modelGeneration {0};
    DetachedGeometryResult result;
};

class AppExport GeometryCommitScope
{
public:
    explicit GeometryCommitScope(Document* doc, DocumentObject* obj = nullptr);
    ~GeometryCommitScope();

private:
    Document* _doc {nullptr};
    DocumentObject* _obj {nullptr};
};

} // namespace App
