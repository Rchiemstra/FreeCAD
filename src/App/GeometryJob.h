// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Base/Uuid.h>
#include <Base/Type.h>

#include <chrono>
#include <functional>
#include <limits>
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
    uint64_t documentIncarnation {0};
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

/// Maximum GeometryJobId representable on the FCGEO/1 wire (canonical decimal string).
inline constexpr GeometryJobId kMaxGeometryJobIdWire = std::numeric_limits<GeometryJobId>::max();

/**
 * Encode a nonzero GeometryJobId as a canonical decimal string for JSON.
 * Returns false for zero (job ids must be nonzero on the wire).
 */
AppExport bool formatGeometryJobId(GeometryJobId id, std::string& out);

/**
 * Parse a canonical decimal GeometryJobId string (full uint64_t domain).
 * Rejects empty, signed, non-decimal, leading zeros, overflow, and zero.
 */
AppExport bool parseGeometryJobId(const std::string& text,
                                  GeometryJobId& out,
                                  std::string& errorCode,
                                  std::string& errorMessage);

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

struct AppExport GeometryArchiveWriteResult
{
    bool success {false};
    std::string errorCode;
    std::string errorMessage;
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
    /// Stable digest of typed parameters for join/coalesce identity.
    virtual std::string parameterDigest() const
    {
        return {};
    }
    virtual GeometryOperationTraits traits() const = 0;
    virtual DetachedGeometryResult run(GeometryWorkerContext& ctx) const = 0;
    /// Stage typed request inputs under @p writer. Must not leave a successful
    /// publication path when returning failure.
    virtual GeometryArchiveWriteResult writeArchive(GeometryArchiveWriter& writer) const = 0;
    /**
     * Parent-side structural decode of a trusted child result archive.
     * Default rejects; codecs override. Must decode into a local candidate and
     * publish only after every check succeeds. Never reconstructs history from BREP.
     */
    virtual DetachedGeometryResult decodeResultArchive(const std::string& absolutePath) const
    {
        DetachedGeometryResult result;
        result.success = false;
        result.errorCode = "CodecDecodeNotImplemented";
        result.errorMessage = "Task does not implement parent-side result decoding";
        result.resultArchivePath = absolutePath;
        return result;
    }
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
    /// Stable fingerprint of inputs captured at prepare/snapshot time.
    std::string inputFingerprint;
};

struct AppExport SnapshotContext
{
    DocumentRevisionToken docToken;
    ObjectRevisionToken objToken;
    GeometryJobId jobId {0};
};

/**
 * Fences checked before applying a detached geometry result. All fields must
 * still match the live document/object at commit time.
 */
struct AppExport CommitFence
{
    GeometryJobId jobId {0};
    long objectId {0};
    std::string objectName;
    Base::Type objectType;
    uint64_t runtimeIncarnation {0};
    uint64_t modelGeneration {0};
    std::string inputFingerprint;
};

struct AppExport CommitContext
{
    DocumentRevisionToken docToken;
    ObjectRevisionToken objToken;
    GeometryJobId jobId {0};
    uint64_t modelGeneration {0};
    DetachedGeometryResult result;
    CommitFence fence;
};

class AppExport GeometryCommitScope
{
public:
    explicit GeometryCommitScope(Document* doc, DocumentObject* obj = nullptr);
    ~GeometryCommitScope();

    /// Call only after a successful commitDetachedRecompute; otherwise generation is not advanced.
    void markSucceeded();

private:
    Document* _doc {nullptr};
    DocumentObject* _obj {nullptr};
    bool _succeeded {false};
};

/// Build an input fingerprint from a task (operationType|codecVersion|parameterDigest).
AppExport std::string makeGeometryInputFingerprint(const DetachedGeometryTask* task);

/// True when live document/object still match the fence captured at submit.
AppExport bool commitFenceMatches(const CommitFence& fence,
                                  const Document& doc,
                                  const DocumentObject& obj,
                                  GeometryJobId jobId);

/**
 * True when @p relativePath is a safe workspace-relative result path:
 * non-empty, not absolute, and without ".." traversal segments.
 */
AppExport bool isTrustedRelativeResultPath(const std::string& relativePath);

} // namespace App
