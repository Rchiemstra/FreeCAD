// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include "Document.h"
#include "DocumentObject.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class ParameterManager;

namespace App
{

struct DocumentRecomputeRequest;
class RecomputePhaseSession;

/** Phases that exist on the document recompute path. `longest_path` elsewhere
 *  counts dependency edges, so independent objects report 0. */
enum class RecomputePhase : std::uint8_t
{
    Capture,
    Execute,
    Transfer,
    Commit,
    Presentation,
    Count
};

inline constexpr std::size_t kRecomputePhaseCount =
    static_cast<std::size_t>(RecomputePhase::Count);

inline const char* recomputePhaseTraceKey(RecomputePhase phase) noexcept
{
    switch (phase) {
        case RecomputePhase::Capture:
            return "capture";
        case RecomputePhase::Execute:
            return "execute";
        case RecomputePhase::Transfer:
            return "transfer";
        case RecomputePhase::Commit:
            return "commit";
        case RecomputePhase::Presentation:
            return "presentation";
        case RecomputePhase::Count:
            break;
    }
    return "";
}

struct RecomputePhaseInterval
{
    std::int64_t startNs {0};
    std::int64_t endNs {0};
    /** Coordinator request that opened this attempt. Zero when the pass is unknown. */
    std::uint64_t passId {0};
};

/** Nanoseconds from `std::chrono::steady_clock`. On Linux, Windows and macOS that
 *  clock is a system-wide monotonic counter, so a worker process and the parent
 *  stamp the same timeline. */
inline std::int64_t recomputePhaseMonotonicNs() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

struct RecomputePhaseObjectTrace
{
    std::string name;
    std::string type;
    bool executed {false};
    std::array<std::vector<RecomputePhaseInterval>, kRecomputePhaseCount> attempts {};
};

struct RecomputePhaseDag
{
    std::size_t objectCount {0};
    std::size_t edgeCount {0};
    std::size_t longestPath {0};
    std::size_t maxWidth {0};
};

struct RecomputePhasePass
{
    std::uint64_t id {0};
    RecomputePhaseDag dag;
};

struct RecomputePhaseCpu
{
    unsigned hardwareConcurrency {0};
    std::size_t preparedEditWorkers {0};
    std::size_t geometryJobActiveCapacity {0};
    unsigned ownerThreads {1};
};

/** Schema v1. Phase attempts grow with retries; `truncated` stays false because
 *  a record does not drop attempts. `dropped_records` is the writer's running
 *  count of whole records discarded because the JSONL queue was full or because
 *  the writer had already shut down. `io_errors` counts refused destinations
 *  and failed open/write/flush/close calls. Both stay 0 on the in-memory record;
 *  each JSONL line carries the counts at the time it is written. Each attempt
 *  carries `pass_id`, the coordinator request that opened it.
 *
 *  JSONL is appended by one background writer. A path that already exists and
 *  is not a regular file is refused. A stalled write to a regular file is not
 *  bounded: stopping the writer waits until that write returns.
 *
 *  RSS is one read of `/proc/self/statm` (resident pages times page size). That
 *  is a procfs read, not storage I/O. Samples stay at admission and at seal.
 *
 *  The trace covers document recompute phases only. Direct
 *  `DocumentObject::recompute()` outside a document recompute is not recorded.
 *  Dimension setters such as `Box::onChanged` in
 *  `Mod/Part/App/FeaturePartBox.cpp` run native execute at edit time, before
 *  any document recompute. The M0 probe times that setter around the assignment
 *  and reports it separately from recompute, so Box totals are not taken from
 *  this trace alone.
 *
 *  Object type is filled when a span first sees the feature. Admission records
 *  only the request's feature ids and dependencies, so a feature that never
 *  spans keeps an empty type.
 */
struct RecomputePhaseTraceRecord
{
    int version {1};
    std::string freecadVersion;
    bool hasGitHash {false};
    std::string gitHash;
    std::string document;
    std::uint64_t recomputeId {0};
    std::vector<std::uint64_t> recomputeIds;
    std::vector<RecomputePhasePass> passes;
    RecomputePhaseCpu cpu;
    bool rssValid {false};
    std::uint64_t rssBeforeKb {0};
    std::uint64_t rssAfterKb {0};
    std::uint64_t rssPeakKb {0};
    RecomputePhaseDag dag;
    int overlap {0};
    /** False: attempts are kept past the initial reservation. There is no silent drop. */
    bool truncated {false};
    /** JSONL only. See the schema note above. */
    std::uint64_t droppedRecords {0};
    /** JSONL only. See the schema note above. */
    std::uint64_t ioErrors {0};
    std::vector<RecomputePhaseObjectTrace> objects;
    std::vector<RecomputePhaseInterval> presentationBoundaries;
};

/** One place lists the JSONL and Python fields. `Sink` provides `beginMap`,
 *  `endMap`, `beginList`, `endList`, `key`, `integer`, `uinteger`, `boolean`
 *  and `string`. */
template<typename Sink>
void writeRecomputePhaseTraceSchema(const RecomputePhaseTraceRecord& record, Sink& sink)
{
    auto intervals = [&](const std::vector<RecomputePhaseInterval>& spans) {
        sink.beginList();
        for (const auto& span : spans) {
            sink.beginMap();
            sink.key("start_ns");
            sink.integer(span.startNs);
            sink.key("end_ns");
            sink.integer(span.endNs);
            sink.key("pass_id");
            sink.uinteger(span.passId);
            sink.endMap();
        }
        sink.endList();
    };
    auto dag = [&](const RecomputePhaseDag& value) {
        sink.beginMap();
        sink.key("object_count");
        sink.uinteger(static_cast<std::uint64_t>(value.objectCount));
        sink.key("edge_count");
        sink.uinteger(static_cast<std::uint64_t>(value.edgeCount));
        sink.key("longest_path");
        sink.uinteger(static_cast<std::uint64_t>(value.longestPath));
        sink.key("max_width");
        sink.uinteger(static_cast<std::uint64_t>(value.maxWidth));
        sink.endMap();
    };

    sink.beginMap();
    sink.key("v");
    sink.integer(record.version);
    sink.key("version");
    sink.string(record.freecadVersion);
    if (record.hasGitHash) {
        sink.key("git_hash");
        sink.string(record.gitHash);
    }
    sink.key("document");
    sink.string(record.document);
    sink.key("recompute_id");
    sink.uinteger(record.recomputeId);
    sink.key("recompute_ids");
    sink.beginList();
    for (const auto id : record.recomputeIds) {
        sink.uinteger(id);
    }
    sink.endList();
    sink.key("passes");
    sink.beginList();
    for (const auto& pass : record.passes) {
        sink.beginMap();
        sink.key("id");
        sink.uinteger(pass.id);
        sink.key("dag");
        dag(pass.dag);
        sink.endMap();
    }
    sink.endList();
    sink.key("cpu");
    sink.beginMap();
    sink.key("hardware_concurrency");
    sink.uinteger(record.cpu.hardwareConcurrency);
    sink.key("prepared_edit_workers");
    sink.uinteger(static_cast<std::uint64_t>(record.cpu.preparedEditWorkers));
    sink.key("geometry_job_active_capacity");
    sink.uinteger(static_cast<std::uint64_t>(record.cpu.geometryJobActiveCapacity));
    sink.key("owner_threads");
    sink.uinteger(record.cpu.ownerThreads);
    sink.endMap();
    if (record.rssValid) {
        sink.key("rss_kb");
        sink.beginMap();
        sink.key("before");
        sink.uinteger(record.rssBeforeKb);
        sink.key("after");
        sink.uinteger(record.rssAfterKb);
        sink.key("sampled_peak");
        sink.uinteger(record.rssPeakKb);
        sink.endMap();
    }
    sink.key("dag");
    dag(record.dag);
    sink.key("overlap");
    sink.integer(record.overlap);
    sink.key("truncated");
    sink.boolean(record.truncated);
    sink.key("dropped_records");
    sink.uinteger(record.droppedRecords);
    sink.key("io_errors");
    sink.uinteger(record.ioErrors);
    sink.key("objects");
    sink.beginList();
    for (const auto& object : record.objects) {
        sink.beginMap();
        sink.key("name");
        sink.string(object.name);
        sink.key("type");
        sink.string(object.type);
        sink.key("executed");
        sink.boolean(object.executed);
        sink.key("phases");
        sink.beginMap();
        for (std::size_t index = 0; index < kRecomputePhaseCount; ++index) {
            const auto& attempts = object.attempts[index];
            if (attempts.empty()) {
                continue;
            }
            const char* name = recomputePhaseTraceKey(static_cast<RecomputePhase>(index));
            if (!name || name[0] == '\0') {
                continue;
            }
            sink.key(name);
            intervals(attempts);
        }
        sink.endMap();
        sink.endMap();
    }
    sink.endList();
    if (!record.presentationBoundaries.empty()) {
        sink.key("presentation_boundary");
        intervals(record.presentationBoundaries);
    }
    sink.endMap();
}

/** Creation gate only. An admitted recompute captures this once and passes the
 *  session pointer down; per-object helpers test that pointer. */
inline std::atomic<bool> g_recomputePhaseTraceEnabled {false};

inline bool recomputePhaseTraceEnabled() noexcept
{
    return g_recomputePhaseTraceEnabled.load(std::memory_order_relaxed);
}

inline thread_local const DocumentObject* g_recomputePhaseCommittingObject = nullptr;

inline const DocumentObject* recomputePhaseCommittingObject() noexcept
{
    return g_recomputePhaseCommittingObject;
}

inline void setRecomputePhaseCommittingObject(const DocumentObject* object) noexcept
{
    g_recomputePhaseCommittingObject = object;
}

class AppExport RecomputePhaseTrace
{
public:
    static void installPreferenceCache(::ParameterManager& userParameters) noexcept;
    static void removePreferenceCache() noexcept;
    [[nodiscard]] static RecomputePhaseCpu cpuBudget() noexcept;
    /** True while a hold, a defer, or an unsealed session exists. */
    [[nodiscard]] static bool hasLiveState() noexcept;
    /** Record one admitted request from the feature ids and dependencies the
     *  request already carries. No document-object lookup and no owner hop.
     *  The returned pointer keeps the session alive for the coordinator job. */
    [[nodiscard]] static std::shared_ptr<void> noteAdmitted(
        Document& document,
        std::uint64_t id,
        const DocumentRecomputeRequest& request) noexcept;
    /** The admitted request reached a terminal state, including failure, cancel
     *  and exception. Closes that pass. Idempotent. */
    static void onOwnerTerminal(Document& document, std::uint64_t id) noexcept;
    /** Seal the document's session when every pass is closed and nothing is
     *  held open. Samples RSS here, outside the coordinator. */
    static void publishIfIdle(Document& document) noexcept;
    /** True when this thread's current document binding is tracing. No preference load. */
    [[nodiscard]] static bool recording() noexcept;
    /** Bind this activation frame to the document's session. A nested frame for
     *  another document does not keep the outer binding. */
    static void bindCurrent(Document& document) noexcept;
    [[nodiscard]] static bool pushHold(Document& document) noexcept;
    static void popHold(Document& document) noexcept;
    /** Blocks seal until the matching pop, so presentation can finish first. */
    [[nodiscard]] static bool pushDefer(Document& document) noexcept;
    static void popDefer(Document& document) noexcept;
    static void forgetDocument(Document& document) noexcept;
    static void noteIsolatedExecute(Document& document,
                                    const DocumentObject* object,
                                    std::int64_t startMonotonicNs,
                                    std::int64_t endMonotonicNs) noexcept;
    [[nodiscard]] static const DocumentObject* objectForOperation(
        Document& document,
        std::string_view operationId) noexcept;
    [[nodiscard]] static bool active(const Document& document) noexcept;
    [[nodiscard]] static std::shared_ptr<const RecomputePhaseTraceRecord> published(
        const Document& document) noexcept;

    /** Test-only seams. */
    struct AppExport testing
    {
        static void pauseWriter(bool pause) noexcept;
        static void enqueue(std::string path,
                            std::shared_ptr<const RecomputePhaseTraceRecord> record) noexcept;
        [[nodiscard]] static std::uint64_t droppedRecords() noexcept;
        [[nodiscard]] static std::uint64_t ioErrors() noexcept;
        [[nodiscard]] static std::uint64_t holdPushes() noexcept;
        static void discard(Document& document) noexcept;
        /** Negative storage means Document::recompute keeps its 6 min / 30 s waits. */
        static void setCompatibilityWaits(std::chrono::milliseconds primary,
                                          std::chrono::milliseconds followUp) noexcept;
        static void clearCompatibilityWaits() noexcept;
        [[nodiscard]] static std::chrono::milliseconds compatibilityWaitPrimary() noexcept;
        [[nodiscard]] static std::chrono::milliseconds compatibilityWaitFollowUp() noexcept;
    };

private:
    friend class DocumentRecomputeCoordinator;
    static void releaseSealedJobSession(std::shared_ptr<void>& session) noexcept;
};

/** Keeps one synchronous multi-pass recompute in a single record.
 *  Creation follows the preference. Once pushed, the destructor always pops,
 *  even if the preference was turned off in between.
 *  The hold copies the document session under the trace-state mutex and
 *  shares it with the job. It does not read or write session fields. The
 *  last remaining reference seals, on whichever side drops it. */
class AppExport RecomputePhaseTraceHold
{
public:
    explicit RecomputePhaseTraceHold(Document& document) noexcept;
    ~RecomputePhaseTraceHold() noexcept;

    /** Copy the session admission stored for this document, if one exists. */
    void retain() noexcept;

    RecomputePhaseTraceHold(const RecomputePhaseTraceHold&) = delete;
    RecomputePhaseTraceHold& operator=(const RecomputePhaseTraceHold&) = delete;

private:
    Document* _document {nullptr};
    std::shared_ptr<void> _session;
};

/** One stack frame of document → session bindings. A nested recompute of
 *  another document pushes its own frame and the destructor restores the
 *  outer one. Per-object helpers read the top frame and do not load the
 *  preference. */
class AppExport RecomputePhaseTraceActivation
{
public:
    RecomputePhaseTraceActivation() noexcept;
    ~RecomputePhaseTraceActivation() noexcept;

    RecomputePhaseTraceActivation(const RecomputePhaseTraceActivation&) = delete;
    RecomputePhaseTraceActivation& operator=(const RecomputePhaseTraceActivation&) = delete;

private:
    friend class RecomputePhaseTrace;
    friend class RecomputePhaseSpan;
    friend class RecomputePhaseBoundarySpan;
    RecomputePhaseTraceActivation* _previous {nullptr};
    const Document* _document {nullptr};
    std::shared_ptr<RecomputePhaseSession> _session;
    bool _engaged {false};
};

class AppExport RecomputePhaseSpan
{
public:
    RecomputePhaseSpan(RecomputePhase phase, const DocumentObject* object) noexcept;
    ~RecomputePhaseSpan() noexcept;

    RecomputePhaseSpan(const RecomputePhaseSpan&) = delete;
    RecomputePhaseSpan& operator=(const RecomputePhaseSpan&) = delete;

private:
    void open() noexcept;
    void close() noexcept;

    RecomputePhase _phase {};
    const DocumentObject* _object {nullptr};
    std::shared_ptr<RecomputePhaseSession> _session;
    std::uint32_t _index {0};
    bool _open {false};
};

class AppExport RecomputePhaseBoundarySpan
{
public:
    explicit RecomputePhaseBoundarySpan(Document& document) noexcept;
    ~RecomputePhaseBoundarySpan() noexcept;

    RecomputePhaseBoundarySpan(const RecomputePhaseBoundarySpan&) = delete;
    RecomputePhaseBoundarySpan& operator=(const RecomputePhaseBoundarySpan&) = delete;

private:
    void open() noexcept;
    void close() noexcept;

    std::shared_ptr<RecomputePhaseSession> _session;
    bool _open {false};
};

}  // namespace App
