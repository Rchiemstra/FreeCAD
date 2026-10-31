// SPDX-License-Identifier: LGPL-2.1-or-later

#include "RecomputePhaseTrace.h"

#include "Application.h"
#include "DocumentRecomputeCoordinator.h"
#include "GeometryJobManager.h"
#include "PreparedEditExecutor.h"

#include <Base/Console.h>
#include <Base/Observer.h>
#include <Base/Parameter.h>
#include <Base/Version.h>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace App
{
namespace
{

constexpr std::size_t kAttemptsReserved = 4;
constexpr std::size_t kBoundariesReserved = 4;
constexpr std::size_t kMaxPendingTraceWrites = 64;

std::int64_t monotonicNs() noexcept
{
    return recomputePhaseMonotonicNs();
}

/** One non-blocking procfs read of resident pages. Not storage I/O. */
std::optional<std::uint64_t> readResidentKb() noexcept
{
#if !defined(__linux__)
    return std::nullopt;
#else
    const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::nullopt;
    }
    char buffer[64];
    const ssize_t count = ::read(fd, buffer, sizeof(buffer) - 1);
    ::close(fd);
    if (count <= 0) {
        return std::nullopt;
    }
    buffer[count] = '\0';
    char* end = nullptr;
    ::strtoul(buffer, &end, 10);
    if (!end || *end == '\0') {
        return std::nullopt;
    }
    const auto residentPages = ::strtoul(end, &end, 10);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(residentPages) * static_cast<std::uint64_t>(pageSize)
        / 1024ULL;
#endif
}

std::string freecadVersion()
{
    const char* major = Base::FCVersionInfo::VersionMajor();
    const char* minor = Base::FCVersionInfo::VersionMinor();
    const char* point = Base::FCVersionInfo::VersionPoint();
    const char* suffix = Base::FCVersionInfo::VersionSuffix();
    std::string version = std::string(major ? major : "") + "." + (minor ? minor : "") + "."
        + (point ? point : "");
    if (suffix && suffix[0] != '\0') {
        version += suffix;
    }
    return version;
}

std::string traceOutputPath()
{
    auto group = GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    if (!group) {
        return {};
    }
    return group->GetASCII("RecomputePhaseTracePath", "");
}

std::string repositoryHash()
{
    const auto& config = GetApplication().Config();
    const auto found = config.find("BuildRevisionHash");
    if (found == config.end() || found->second.empty()) {
        return {};
    }
    return found->second;
}

void appendEscaped(std::string& out, const std::string_view text)
{
    out.push_back('"');
    for (const unsigned char ch : text) {
        switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 0x0f]);
                }
                else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

class JsonSink
{
public:
    void beginMap()
    {
        open('{');
    }

    void endMap()
    {
        close('}');
    }

    void beginList()
    {
        open('[');
    }

    void endList()
    {
        close(']');
    }

    void key(const char* name)
    {
        if (_frames.back().comma) {
            _out += ',';
        }
        _frames.back().comma = true;
        appendEscaped(_out, name);
        _out += ':';
        _keyed = true;
    }

    void integer(const std::int64_t value)
    {
        literal(std::to_string(value));
    }

    void uinteger(const std::uint64_t value)
    {
        literal(std::to_string(value));
    }

    void boolean(const bool value)
    {
        literal(value ? "true" : "false");
    }

    void string(const std::string_view value)
    {
        beforeValue();
        appendEscaped(_out, value);
    }

    [[nodiscard]] std::string str() const
    {
        return _out;
    }

private:
    struct Frame
    {
        bool comma {false};
    };

    void beforeValue()
    {
        if (_keyed) {
            _keyed = false;
            return;
        }
        if (_frames.empty()) {
            return;
        }
        if (_frames.back().comma) {
            _out += ',';
        }
        _frames.back().comma = true;
    }

    void literal(const std::string& text)
    {
        beforeValue();
        _out += text;
    }

    void open(const char bracket)
    {
        beforeValue();
        _out.push_back(bracket);
        _frames.push_back({});
    }

    void close(const char bracket)
    {
        _frames.pop_back();
        _out.push_back(bracket);
    }

    std::string _out;
    std::vector<Frame> _frames;
    bool _keyed {false};
};

std::string toJson(const RecomputePhaseTraceRecord& record)
{
    JsonSink sink;
    writeRecomputePhaseTraceSchema(record, sink);
    return sink.str();
}

// Relaxed loads are safe: coordinator admission and lane dispatch synchronize the hand-off.
std::atomic<int> g_traceLive {0};
/** Documents that still have trace state. Distinct from live sessions so a
 *  sealed record can be dropped on close after the preference is turned off. */
std::atomic<int> g_traceStates {0};
std::atomic<std::uint64_t> g_holdPushes {0};
/** Negative means Document::recompute uses its production waits. */
std::atomic<std::int64_t> g_compatibilityWaitPrimaryMs {-1};
std::atomic<std::int64_t> g_compatibilityWaitFollowMs {-1};

bool traceInactive() noexcept
{
    return !recomputePhaseTraceEnabled()
        && g_traceLive.load(std::memory_order_relaxed) == 0;
}

struct TraceWriteItem
{
    std::string path;
    std::shared_ptr<const RecomputePhaseTraceRecord> record;
};

enum class IoKind : unsigned
{
    Destination = 1u << 0,
    Open = 1u << 1,
    Write = 1u << 2,
    Flush = 1u << 3,
    Close = 1u << 4,
};

const char* ioKindName(const unsigned kind)
{
    switch (kind) {
        case static_cast<unsigned>(IoKind::Destination):
            return "non-regular destination";
        case static_cast<unsigned>(IoKind::Open):
            return "open";
        case static_cast<unsigned>(IoKind::Write):
            return "write";
        case static_cast<unsigned>(IoKind::Flush):
            return "flush";
        case static_cast<unsigned>(IoKind::Close):
            return "close";
        default:
            return "write";
    }
}

bool destinationRefused(const std::string& path)
{
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return false;
    }
    return !S_ISREG(info.st_mode);
}

unsigned writeRecord(const TraceWriteItem& item,
                     const std::uint64_t dropped,
                     const std::uint64_t ioErrors)
{
    if (!item.record || item.path.empty()) {
        return 0;
    }
    try {
        if (destinationRefused(item.path)) {
            return static_cast<unsigned>(IoKind::Destination);
        }
        auto copy = *item.record;
        copy.droppedRecords = dropped;
        copy.ioErrors = ioErrors;
        const std::string json = toJson(copy);
        std::ofstream out(item.path, std::ios::app);
        if (!out) {
            return static_cast<unsigned>(IoKind::Open);
        }
        out << json << '\n';
        unsigned failed = 0;
        if (!out) {
            failed |= static_cast<unsigned>(IoKind::Write);
        }
        else {
            out.flush();
            if (!out) {
                failed |= static_cast<unsigned>(IoKind::Flush);
            }
        }
        if (failed == 0) {
            out.close();
            if (out.fail()) {
                failed |= static_cast<unsigned>(IoKind::Close);
            }
        }
        else {
            out.close();
        }
        return failed;
    }
    catch (...) {
        return static_cast<unsigned>(IoKind::Write);
    }
}

class TraceWriter
{
public:
    void ensureStarted()
    {
        std::lock_guard lock(_mutex);
        startLocked();
    }

    void enqueue(std::string path, std::shared_ptr<const RecomputePhaseTraceRecord> record)
    {
        if (path.empty() || !record) {
            return;
        }
        {
            std::lock_guard lock(_mutex);
            if (_shutdown) {
                ++_dropped;
                return;
            }
            if (_queue.size() >= kMaxPendingTraceWrites) {
                _queue.pop_front();
                ++_dropped;
            }
            _queue.push_back(TraceWriteItem {std::move(path), std::move(record)});
            if (!_pause && !_joining) {
                startLocked();
            }
        }
        _cv.notify_one();
    }

    void setPause(const bool pause)
    {
        {
            std::lock_guard lock(_mutex);
            if (_shutdown) {
                return;
            }
            _pause = pause;
        }
        _cv.notify_all();
    }

    /** Preference-off stop. Drains the queue and leaves a later start possible. */
    void stopAndJoin()
    {
        std::unique_lock lock(_mutex);
        if (_shutdown) {
            if (_joining) {
                _cv.wait(lock, [&] {
                    return !_joining;
                });
            }
            return;
        }
        _pause = false;
        drainAndJoin(lock);
    }

    /** Permanent. After this returns, enqueue drops and nothing restarts the writer. */
    void shutdown()
    {
        std::unique_lock lock(_mutex);
        _shutdown = true;
        _pause = false;
        drainAndJoin(lock);
    }

    [[nodiscard]] std::uint64_t droppedRecords() const
    {
        std::lock_guard lock(_mutex);
        return _dropped;
    }

    [[nodiscard]] std::uint64_t ioErrors() const
    {
        std::lock_guard lock(_mutex);
        return _ioErrors;
    }

    ~TraceWriter()
    {
        try {
            shutdown();
        }
        catch (...) {
        }
    }

private:
    void startLocked()
    {
        if (_shutdown || _joining || _pause || _thread.joinable()) {
            return;
        }
        _stop = false;
        _warned = 0;
        _thread = std::thread([this] {
            run();
        });
    }

    void drainAndJoin(std::unique_lock<std::mutex>& lock)
    {
        for (;;) {
            if (_joining) {
                _cv.wait(lock, [&] {
                    return !_joining;
                });
                continue;
            }
            if (!_thread.joinable() && _queue.empty()) {
                return;
            }
            _joining = true;
            while (_thread.joinable() || !_queue.empty()) {
                if (!_thread.joinable()) {
                    _stop = false;
                    _warned = 0;
                    _thread = std::thread([this] {
                        run();
                    });
                }
                _stop = true;
                std::thread worker = std::move(_thread);
                lock.unlock();
                _cv.notify_all();
                worker.join();
                lock.lock();
                _stop = false;
            }
            _joining = false;
            _cv.notify_all();
        }
    }

    void run()
    {
        for (;;) {
            TraceWriteItem item;
            std::uint64_t dropped = 0;
            std::uint64_t ioErrors = 0;
            {
                std::unique_lock lock(_mutex);
                _cv.wait(lock, [&] {
                    return _stop || (!_pause && !_queue.empty());
                });
                if (_queue.empty()) {
                    if (_stop) {
                        return;
                    }
                    continue;
                }
                item = std::move(_queue.front());
                _queue.pop_front();
                dropped = _dropped;
                ioErrors = _ioErrors;
            }
            const unsigned failed = writeRecord(item, dropped, ioErrors);
            if (failed == 0) {
                continue;
            }
            unsigned fresh = 0;
            {
                std::lock_guard lock(_mutex);
                for (unsigned bit = 1u; bit <= static_cast<unsigned>(IoKind::Close); bit <<= 1u) {
                    if ((failed & bit) == 0) {
                        continue;
                    }
                    ++_ioErrors;
                    if ((_warned & bit) == 0) {
                        _warned |= bit;
                        fresh |= bit;
                    }
                }
            }
            for (unsigned bit = 1u; bit <= static_cast<unsigned>(IoKind::Close); bit <<= 1u) {
                if ((fresh & bit) == 0) {
                    continue;
                }
                Base::Console().warning(
                    "Recompute phase trace {} failed for '{}'\n",
                    ioKindName(bit),
                    item.path);
            }
        }
    }

    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<TraceWriteItem> _queue;
    std::uint64_t _dropped {0};
    std::uint64_t _ioErrors {0};
    unsigned _warned {0};
    bool _shutdown {false};
    bool _stop {false};
    bool _joining {false};
    bool _pause {false};
    std::thread _thread;
};

TraceWriter& traceWriter()
{
    static TraceWriter writer;
    return writer;
}

void enqueueTraceWrite(std::string path,
                       const std::shared_ptr<const RecomputePhaseTraceRecord>& record)
{
    traceWriter().enqueue(std::move(path), record);
}

std::string currentTracePath()
{
    return traceOutputPath();
}

RecomputePhaseDag summarizeDag(const DocumentRecomputeRequest& request)
{
    RecomputePhaseDag dag;
    dag.objectCount = request.features.size();
    std::unordered_map<std::string, std::size_t> indexByName;
    indexByName.reserve(request.features.size());
    for (std::size_t index = 0; index < request.features.size(); ++index) {
        indexByName.emplace(request.features[index].featureId, index);
    }

    std::vector<std::vector<std::size_t>> dependents(request.features.size());
    std::vector<std::size_t> indegree(request.features.size(), 0);
    for (std::size_t index = 0; index < request.features.size(); ++index) {
        for (const auto& dependency : request.features[index].dependencies) {
            const auto found = indexByName.find(dependency);
            if (found == indexByName.end() || found->second == index) {
                continue;
            }
            dependents[found->second].push_back(index);
            ++indegree[index];
            ++dag.edgeCount;
        }
    }

    std::vector<std::size_t> ready;
    ready.reserve(request.features.size());
    std::vector<std::size_t> depth(request.features.size(), 0);
    for (std::size_t index = 0; index < indegree.size(); ++index) {
        if (indegree[index] == 0) {
            ready.push_back(index);
        }
    }
    std::size_t cursor = 0;
    while (cursor < ready.size()) {
        const auto levelEnd = ready.size();
        const auto width = levelEnd - cursor;
        if (width > dag.maxWidth) {
            dag.maxWidth = width;
        }
        for (; cursor < levelEnd; ++cursor) {
            const auto node = ready[cursor];
            if (depth[node] > dag.longestPath) {
                dag.longestPath = depth[node];
            }
            for (const auto child : dependents[node]) {
                const auto nextDepth = depth[node] + 1;
                if (nextDepth > depth[child]) {
                    depth[child] = nextDepth;
                }
                if (indegree[child] > 0) {
                    --indegree[child];
                }
                if (indegree[child] == 0) {
                    ready.push_back(child);
                }
            }
        }
    }
    return dag;
}

int overlapOf(const RecomputePhaseTraceRecord& record)
{
    struct Event
    {
        std::int64_t time {0};
        int delta {0};
    };
    std::vector<Event> events;
    const auto executeIndex = static_cast<std::size_t>(RecomputePhase::Execute);
    for (const auto& object : record.objects) {
        for (const auto& span : object.attempts[executeIndex]) {
            events.push_back({span.startNs, 1});
            events.push_back({span.endNs, -1});
        }
    }
    std::sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        if (left.time != right.time) {
            return left.time < right.time;
        }
        return left.delta < right.delta;
    });
    int current = 0;
    int maximum = 0;
    for (const auto& event : events) {
        current += event.delta;
        if (current < 0) {
            current = 0;
        }
        if (current > maximum) {
            maximum = current;
        }
    }
    if (maximum == 0 && !events.empty()) {
        maximum = 1;
    }
    return maximum;
}

struct TransparentHash
{
    using is_transparent = void;

    std::size_t operator()(const std::string_view text) const noexcept
    {
        return std::hash<std::string_view> {}(text);
    }
};

class PreferenceObserver: public Base::Observer<const char*>
{
public:
    ~PreferenceObserver() override
    {
        detach();
    }

    void OnChange(Base::Subject<const char*>& subject, const char* reason) override
    {
        static_cast<void>(subject);
        static_cast<void>(reason);
        if (!group) {
            return;
        }
        // Clear() and group removal notify without naming RecomputePhaseTrace.
        try {
            const bool enabled = group->GetBool("RecomputePhaseTrace", false);
            const auto path = group->GetASCII("RecomputePhaseTracePath", "");
            g_recomputePhaseTraceEnabled.store(enabled, std::memory_order_relaxed);
            if (!enabled) {
                // Joining waits for the writer. Skip that while a session is
                // still open: this observer runs on the recompute thread, and
                // the final seal still has to enqueue.
                if (g_traceLive.load(std::memory_order_relaxed) == 0) {
                    traceWriter().stopAndJoin();
                }
            }
            else if (!path.empty()) {
                traceWriter().ensureStarted();
            }
        }
        catch (...) {
        }
    }

    void detach() noexcept
    {
        if (!group) {
            return;
        }
        try {
            group->Detach(this);
        }
        catch (...) {
        }
        group = nullptr;
    }

    Base::Reference<ParameterGrp> group;
};

PreferenceObserver& preferenceObserver()
{
    static PreferenceObserver observer;
    return observer;
}

bool& preferenceInstalled()
{
    static bool installed = false;
    return installed;
}

struct PreparedObject
{
    std::string name;
    std::string operationId;
};

}  // namespace

struct PhaseOpen
{
    bool owns {false};
    std::uint32_t index {0};
};

class RecomputePhaseSession
{
public:
    /** `rssBefore` is sampled by the caller outside coordinator and lane locks. */
    RecomputePhaseSession(Document& document,
                          std::string path,
                          const std::optional<std::uint64_t>& rssBefore)
        : _document(&document)
        , _origin(monotonicNs())
        , _path(std::move(path))
    {
        g_traceLive.fetch_add(1, std::memory_order_relaxed);
        _liveHeld = true;
        const char* name = document.getName();
        _record.document = name ? name : "";
        _record.freecadVersion = freecadVersion();
        _record.gitHash = repositoryHash();
        _record.hasGitHash = !_record.gitHash.empty();
        _record.cpu = RecomputePhaseTrace::cpuBudget();
        _record.truncated = false;
        _record.presentationBoundaries.reserve(kBoundariesReserved);
        if (rssBefore) {
            _record.rssBeforeKb = *rssBefore;
            _record.rssAfterKb = *rssBefore;
            _record.rssPeakKb = *rssBefore;
            _record.rssValid = true;
        }
    }

    ~RecomputePhaseSession()
    {
        try {
            const auto published = forcePublish(std::nullopt);
            if (published.fresh) {
                enqueueTraceWrite(published.path, published.record);
            }
        }
        catch (...) {
        }
        releaseLive();
    }

    [[nodiscard]] Document* document() const noexcept
    {
        std::lock_guard lock(_mutex);
        return _document;
    }

    void detachDocument() noexcept
    {
        std::lock_guard lock(_mutex);
        _document = nullptr;
    }

    void addPass(const std::uint64_t id, const DocumentRecomputeRequest& request)
    {
        std::vector<PreparedObject> prepared;
        prepared.reserve(request.features.size());
        for (const auto& feature : request.features) {
            PreparedObject slot;
            slot.name = feature.featureId;
            slot.operationId = feature.operationId;
            prepared.push_back(std::move(slot));
        }
        const auto dag = summarizeDag(request);

        std::lock_guard lock(_mutex);
        if (_sealed) {
            return;
        }
        Pass pass;
        pass.id = id;
        pass.dag = dag;
        pass.features = request.features;
        _passes.push_back(std::move(pass));
        _passStack.push_back(id);
        _record.recomputeIds.push_back(id);
        if (_record.recomputeId == 0) {
            _record.recomputeId = id;
        }
        _record.objects.reserve(_record.objects.size() + prepared.size());
        _byObject.reserve(_record.objects.size() + prepared.size());
        _byOperation.reserve(_byOperation.size() + prepared.size());
        for (auto& slot : prepared) {
            addObjectLocked(std::move(slot));
        }
    }

    void closePass(const std::uint64_t id)
    {
        std::lock_guard lock(_mutex);
        closePassLocked(id);
    }

    [[nodiscard]] bool isSealed() const
    {
        std::lock_guard lock(_mutex);
        return _sealed;
    }

    [[nodiscard]] const DocumentObject* findOperation(const std::string_view operationId) const
    {
        std::lock_guard lock(_mutex);
        const auto found = _byOperation.find(operationId);
        if (found == _byOperation.end()) {
            return nullptr;
        }
        return found->second;
    }

    PhaseOpen begin(const RecomputePhase phase,
                    const DocumentObject& object,
                    const std::int64_t stamp)
    {
        std::lock_guard lock(_mutex);
        if (_sealed) {
            return {};
        }
        const auto found = indexOf(object);
        if (!found) {
            return {};
        }
        const auto index = *found;
        const auto phaseIndex = static_cast<std::size_t>(phase);
        if (_open[index][phaseIndex]) {
            return {false, index};
        }
        auto& attempts = _record.objects[index].attempts[phaseIndex];
        const auto relative = relativeTo(stamp);
        attempts.push_back(RecomputePhaseInterval {relative, relative, activePassLocked()});
        _open[index][phaseIndex] = true;
        ++_openSpans;
        if (phase == RecomputePhase::Execute) {
            _record.objects[index].executed = true;
        }
        if (phase == RecomputePhase::Commit) {
            setRecomputePhaseCommittingObject(&object);
        }
        return {true, index};
    }

    void end(const RecomputePhase phase, const std::uint32_t index, const std::int64_t stamp)
    {
        std::lock_guard lock(_mutex);
        if (_sealed || index >= _record.objects.size()) {
            return;
        }
        const auto phaseIndex = static_cast<std::size_t>(phase);
        if (!_open[index][phaseIndex]) {
            return;
        }
        _open[index][phaseIndex] = false;
        auto& attempts = _record.objects[index].attempts[phaseIndex];
        if (!attempts.empty()) {
            auto& span = attempts.back();
            const auto relative = relativeTo(stamp);
            span.endNs = relative < span.startNs ? span.startNs : relative;
        }
        if (_openSpans > 0) {
            --_openSpans;
        }
        if (phase == RecomputePhase::Commit) {
            setRecomputePhaseCommittingObject(nullptr);
        }
    }

    void noteExecute(const DocumentObject& object,
                     const std::int64_t startMonotonicNs,
                     const std::int64_t endMonotonicNs)
    {
        std::lock_guard lock(_mutex);
        if (_sealed) {
            return;
        }
        const auto found = indexOf(object);
        if (!found) {
            return;
        }
        const auto index = *found;
        auto& attempts = _record.objects[index].attempts[static_cast<std::size_t>(
            RecomputePhase::Execute)];
        // Worker stamps are already on this clock. Keep them even when they
        // fall outside the later decode/apply span.
        auto start = relativeTo(startMonotonicNs);
        auto end = relativeTo(endMonotonicNs);
        if (end < start) {
            end = start;
        }
        attempts.push_back(RecomputePhaseInterval {start, end, activePassLocked()});
        _record.objects[index].executed = true;
    }

    [[nodiscard]] bool beginBoundary(const std::int64_t stamp)
    {
        std::lock_guard lock(_mutex);
        if (_sealed || _boundaryOpen) {
            return false;
        }
        const auto relative = relativeTo(stamp);
        _record.presentationBoundaries.push_back(
            RecomputePhaseInterval {relative, relative, activePassLocked()});
        _boundaryOpen = true;
        ++_openSpans;
        return true;
    }

    void endBoundary(const std::int64_t stamp)
    {
        std::lock_guard lock(_mutex);
        if (_sealed || !_boundaryOpen || _record.presentationBoundaries.empty()) {
            return;
        }
        _boundaryOpen = false;
        auto& span = _record.presentationBoundaries.back();
        const auto relative = relativeTo(stamp);
        span.endNs = relative < span.startNs ? span.startNs : relative;
        if (_openSpans > 0) {
            --_openSpans;
        }
    }

    struct PublishedTrace
    {
        std::shared_ptr<const RecomputePhaseTraceRecord> record;
        std::string path;
        bool fresh {false};
    };

    [[nodiscard]] bool canSeal() const
    {
        std::lock_guard lock(_mutex);
        return readyLocked();
    }

    /** Publish only when every admitted pass is closed and no span is open. */
    [[nodiscard]] std::optional<PublishedTrace> publishIfReady(
        const std::optional<std::uint64_t>& rssAfter)
    {
        std::lock_guard lock(_mutex);
        if (_sealed) {
            return std::nullopt;
        }
        if (!readyLocked()) {
            return std::nullopt;
        }
        return sealLocked(rssAfter);
    }

    /** Close whatever is still open. The destructor and discard use this. */
    [[nodiscard]] PublishedTrace forcePublish(const std::optional<std::uint64_t>& rssAfter)
    {
        std::lock_guard lock(_mutex);
        if (_sealed) {
            return {};
        }
        return sealLocked(rssAfter);
    }

private:
    struct Pass
    {
        std::uint64_t id {0};
        bool closed {false};
        RecomputePhaseDag dag;
        std::vector<DocumentRecomputeFeatureRequest> features;
    };

    [[nodiscard]] std::uint64_t activePassLocked() const
    {
        return _passStack.empty() ? 0 : _passStack.back();
    }

    void closePassLocked(const std::uint64_t id)
    {
        for (auto& pass : _passes) {
            if (pass.id != id) {
                continue;
            }
            pass.closed = true;
            const auto stacked = std::find(_passStack.begin(), _passStack.end(), id);
            if (stacked != _passStack.end()) {
                _passStack.erase(stacked);
            }
            return;
        }
    }

    [[nodiscard]] std::int64_t relativeTo(const std::int64_t stamp) const
    {
        if (stamp < _origin) {
            return 0;
        }
        return stamp - _origin;
    }

    [[nodiscard]] bool readyLocked() const
    {
        return !_sealed && _openSpans == 0 && !_boundaryOpen && !_passes.empty()
            && std::all_of(_passes.begin(), _passes.end(), [](const Pass& pass) {
                   return pass.closed;
               });
    }

    [[nodiscard]] std::optional<std::uint32_t> indexOf(const DocumentObject& object)
    {
        const auto cached = _byObject.find(&object);
        if (cached != _byObject.end()) {
            return cached->second;
        }
        const char* name = object.getNameInDocument();
        if (!name || name[0] == '\0') {
            return std::nullopt;
        }
        const auto named = _byName.find(name);
        if (named == _byName.end()) {
            return std::nullopt;
        }
        const auto index = named->second;
        _byObject.emplace(&object, index);
        if (index < _record.objects.size() && _record.objects[index].type.empty()) {
            _record.objects[index].type = object.getTypeId().getName();
        }
        const auto operations = _operationsByName.find(name);
        if (operations != _operationsByName.end()) {
            for (const auto& operationId : operations->second) {
                _byOperation.emplace(operationId, &object);
            }
        }
        return index;
    }

    void addObjectLocked(PreparedObject&& prepared)
    {
        if (prepared.name.empty()) {
            return;
        }
        if (const auto existing = _byName.find(prepared.name); existing != _byName.end()) {
            rememberOperationLocked(prepared.operationId, prepared.name);
            return;
        }
        const auto index = static_cast<std::uint32_t>(_record.objects.size());
        RecomputePhaseObjectTrace slot;
        slot.name = std::move(prepared.name);
        for (auto& attempts : slot.attempts) {
            attempts.reserve(kAttemptsReserved);
        }
        _record.objects.push_back(std::move(slot));
        _open.push_back({});
        _byName.emplace(_record.objects.back().name, index);
        rememberOperationLocked(prepared.operationId, _record.objects.back().name);
    }

    void rememberOperationLocked(const std::string& operationId, const std::string& name)
    {
        if (operationId.empty() || name.empty()) {
            return;
        }
        _operationsByName[name].push_back(operationId);
    }

    void releaseLive() noexcept
    {
        if (_liveHeld.exchange(false, std::memory_order_relaxed)) {
            g_traceLive.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] PublishedTrace sealLocked(const std::optional<std::uint64_t>& rssAfter)
    {
        forceCloseLocked(monotonicNs());
        finishRecordLocked(rssAfter);
        _record.truncated = false;
        _sealed = true;
        releaseLive();
        PublishedTrace published;
        published.record = std::make_shared<const RecomputePhaseTraceRecord>(std::move(_record));
        published.path = _path;
        published.fresh = true;
        releaseHeavyLocked();
        return published;
    }

    void releaseHeavyLocked()
    {
        _record = {};
        _passes.clear();
        _passes.shrink_to_fit();
        _passStack.clear();
        _passStack.shrink_to_fit();
        _byName.clear();
        _operationsByName.clear();
        _byObject.clear();
        _byOperation.clear();
        _open.clear();
        _open.shrink_to_fit();
        _path.clear();
        _path.shrink_to_fit();
    }

    void forceCloseLocked(const std::int64_t stamp)
    {
        const auto relative = relativeTo(stamp);
        for (std::uint32_t index = 0; index < _record.objects.size(); ++index) {
            for (std::size_t phase = 0; phase < kRecomputePhaseCount; ++phase) {
                if (!_open[index][phase]) {
                    continue;
                }
                _open[index][phase] = false;
                auto& attempts = _record.objects[index].attempts[phase];
                if (!attempts.empty() && attempts.back().endNs < attempts.back().startNs) {
                    attempts.back().endNs = attempts.back().startNs;
                }
                else if (!attempts.empty()) {
                    const auto end = relative < attempts.back().startNs
                        ? attempts.back().startNs
                        : relative;
                    attempts.back().endNs = end;
                }
                if (static_cast<RecomputePhase>(phase) == RecomputePhase::Commit) {
                    setRecomputePhaseCommittingObject(nullptr);
                }
            }
        }
        if (_boundaryOpen && !_record.presentationBoundaries.empty()) {
            auto& span = _record.presentationBoundaries.back();
            span.endNs = relative < span.startNs ? span.startNs : relative;
            _boundaryOpen = false;
        }
        _openSpans = 0;
    }

    void finishRecordLocked(const std::optional<std::uint64_t>& rssAfter)
    {
        DocumentRecomputeRequest combined;
        std::unordered_map<std::string, std::size_t> indexByName;
        _record.passes.clear();
        _record.passes.reserve(_passes.size());
        for (const auto& pass : _passes) {
            _record.passes.push_back(RecomputePhasePass {pass.id, pass.dag});
            for (const auto& feature : pass.features) {
                const auto found = indexByName.find(feature.featureId);
                if (found == indexByName.end()) {
                    indexByName.emplace(feature.featureId, combined.features.size());
                    combined.features.push_back(feature);
                    continue;
                }
                auto& existing = combined.features[found->second].dependencies;
                for (const auto& dependency : feature.dependencies) {
                    if (std::find(existing.begin(), existing.end(), dependency) == existing.end()) {
                        existing.push_back(dependency);
                    }
                }
            }
        }
        _record.dag = summarizeDag(combined);
        _record.overlap = overlapOf(_record);
        if (rssAfter) {
            if (!_record.rssValid) {
                _record.rssBeforeKb = *rssAfter;
                _record.rssValid = true;
            }
            _record.rssAfterKb = *rssAfter;
            _record.rssPeakKb = std::max(_record.rssBeforeKb, _record.rssAfterKb);
        }
    }

    mutable std::mutex _mutex;
    Document* _document {nullptr};
    std::int64_t _origin {0};
    std::string _path;
    RecomputePhaseTraceRecord _record;
    std::vector<Pass> _passes;
    std::vector<std::uint64_t> _passStack;
    std::unordered_map<std::string, std::uint32_t, TransparentHash, std::equal_to<>> _byName;
    std::unordered_map<std::string, std::vector<std::string>, TransparentHash, std::equal_to<>>
        _operationsByName;
    std::unordered_map<const DocumentObject*, std::uint32_t> _byObject;
    std::unordered_map<std::string, const DocumentObject*, TransparentHash, std::equal_to<>>
        _byOperation;
    std::vector<std::array<bool, kRecomputePhaseCount>> _open;
    int _openSpans {0};
    bool _boundaryOpen {false};
    bool _sealed {false};
    std::atomic<bool> _liveHeld {false};
};

namespace
{

thread_local RecomputePhaseTraceActivation* g_traceActivation = nullptr;

}  // namespace

struct DocumentTraceState
{
    std::mutex mutex;
    int hold {0};
    int defer {0};
    std::shared_ptr<RecomputePhaseSession> session;
    std::shared_ptr<const RecomputePhaseTraceRecord> published;
};

std::mutex& documentTraceMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const Document*, std::shared_ptr<DocumentTraceState>>& documentTraceStates()
{
    static std::unordered_map<const Document*, std::shared_ptr<DocumentTraceState>> states;
    return states;
}

std::shared_ptr<DocumentTraceState> stateFor(const Document& document, const bool create)
{
    std::lock_guard lock(documentTraceMutex());
    auto& states = documentTraceStates();
    const auto found = states.find(&document);
    if (found != states.end()) {
        return found->second;
    }
    if (!create) {
        return nullptr;
    }
    auto created = std::make_shared<DocumentTraceState>();
    states.emplace(&document, created);
    g_traceStates.fetch_add(1, std::memory_order_release);
    return created;
}

std::shared_ptr<RecomputePhaseSession> sessionPointer(const Document& document)
{
    const auto state = stateFor(document, false);
    if (!state) {
        return nullptr;
    }
    std::lock_guard lock(state->mutex);
    return state->session;
}

void tryPublish(const Document& document, const std::optional<std::uint64_t>& rssAfter)
{
    const auto state = stateFor(document, false);
    if (!state) {
        return;
    }
    std::optional<RecomputePhaseSession::PublishedTrace> published;
    {
        std::lock_guard lock(state->mutex);
        // Defer stays up through presentation, which runs after the job is
        // already terminal. Sealing on the terminal hook alone would drop it.
        if (state->hold > 0 || state->defer > 0 || !state->session) {
            return;
        }
        published = state->session->publishIfReady(rssAfter);
        if (!published) {
            return;
        }
        state->published = published->record;
        state->session.reset();
    }
    if (published->fresh) {
        enqueueTraceWrite(published->path, published->record);
    }
}

bool readyToPublish(const Document& document)
{
    const auto state = stateFor(document, false);
    if (!state) {
        return false;
    }
    std::lock_guard lock(state->mutex);
    return state->hold == 0 && state->defer == 0 && state->session && state->session->canSeal();
}

void RecomputePhaseSpan::open() noexcept
{
    try {
        const auto began = _session->begin(_phase, *_object, monotonicNs());
        _index = began.index;
        _open = began.owns;
    }
    catch (...) {
        _open = false;
        if (_phase == RecomputePhase::Commit) {
            setRecomputePhaseCommittingObject(nullptr);
        }
    }
}

void RecomputePhaseSpan::close() noexcept
{
    try {
        if (_session) {
            _session->end(_phase, _index, monotonicNs());
        }
    }
    catch (...) {
        if (_phase == RecomputePhase::Commit) {
            setRecomputePhaseCommittingObject(nullptr);
        }
    }
    _open = false;
}

void RecomputePhaseBoundarySpan::open() noexcept
{
    try {
        _open = _session->beginBoundary(monotonicNs());
    }
    catch (...) {
        _open = false;
    }
}

void RecomputePhaseBoundarySpan::close() noexcept
{
    try {
        if (_session) {
            _session->endBoundary(monotonicNs());
        }
    }
    catch (...) {
    }
    _open = false;
}

RecomputePhaseSpan::RecomputePhaseSpan(const RecomputePhase phase,
                                       const DocumentObject* object) noexcept
    : _phase(phase)
{
    if (!g_traceActivation || !g_traceActivation->_session || !object) {
        return;
    }
    _session = g_traceActivation->_session;
    _object = object;
    open();
}

RecomputePhaseSpan::~RecomputePhaseSpan() noexcept
{
    if (_open) {
        close();
    }
}

RecomputePhaseBoundarySpan::RecomputePhaseBoundarySpan(Document& document) noexcept
{
    static_cast<void>(document);
    if (!g_traceActivation || !g_traceActivation->_session) {
        return;
    }
    if (g_traceActivation->_document && g_traceActivation->_document != &document) {
        return;
    }
    _session = g_traceActivation->_session;
    open();
}

RecomputePhaseBoundarySpan::~RecomputePhaseBoundarySpan() noexcept
{
    if (_open) {
        close();
    }
}

RecomputePhaseTraceActivation::RecomputePhaseTraceActivation() noexcept
{
    _previous = g_traceActivation;
    g_traceActivation = this;
    _engaged = true;
}

RecomputePhaseTraceActivation::~RecomputePhaseTraceActivation() noexcept
{
    if (!_engaged) {
        return;
    }
    g_traceActivation = _previous;
}

bool RecomputePhaseTrace::hasLiveState() noexcept
{
    return g_traceLive.load(std::memory_order_relaxed) != 0;
}

bool RecomputePhaseTrace::recording() noexcept
{
    return g_traceActivation && static_cast<bool>(g_traceActivation->_session);
}

void RecomputePhaseTrace::bindCurrent(Document& document) noexcept
{
    if (!g_traceActivation) {
        return;
    }
    if (g_traceActivation->_document == &document && g_traceActivation->_session) {
        return;
    }
    if (traceInactive() && !g_traceActivation->_session) {
        return;
    }
    try {
        auto session = sessionPointer(document);
        if (!session && traceInactive()) {
            return;
        }
        g_traceActivation->_document = &document;
        g_traceActivation->_session = std::move(session);
    }
    catch (...) {
    }
}

std::shared_ptr<void> RecomputePhaseTrace::noteAdmitted(
    Document& document,
    const std::uint64_t id,
    const DocumentRecomputeRequest& request) noexcept
{
    if (traceInactive()) {
        return nullptr;
    }
    try {
        const bool enabled = recomputePhaseTraceEnabled();
        auto state = stateFor(document, false);
        std::shared_ptr<RecomputePhaseSession> existing;
        if (state) {
            std::lock_guard lock(state->mutex);
            existing = state->session;
        }
        if (!enabled && !existing) {
            return nullptr;
        }
        // RSS is sampled before the session is published, and this runs after
        // the coordinator has released its locks.
        const auto rssBefore = existing ? std::nullopt : readResidentKb();
        const auto path = existing ? std::string {} : currentTracePath();
        if (!state) {
            state = stateFor(document, true);
        }
        std::shared_ptr<RecomputePhaseSession> session;
        {
            std::lock_guard lock(state->mutex);
            if (!state->session) {
                if (!enabled) {
                    return nullptr;
                }
                state->session = std::make_shared<RecomputePhaseSession>(
                    document, path, rssBefore);
            }
            session = state->session;
        }
        if (!session) {
            return nullptr;
        }
        session->addPass(id, request);
        if (g_traceActivation
            && (!g_traceActivation->_document || g_traceActivation->_document == &document)) {
            g_traceActivation->_document = &document;
            g_traceActivation->_session = session;
        }
        return std::static_pointer_cast<void>(session);
    }
    catch (...) {
        return nullptr;
    }
}

void RecomputePhaseTrace::onOwnerTerminal(Document& document, const std::uint64_t id) noexcept
{
    if (traceInactive()) {
        return;
    }
    try {
        const auto state = stateFor(document, false);
        if (!state) {
            return;
        }
        std::shared_ptr<RecomputePhaseSession> session;
        {
            std::lock_guard lock(state->mutex);
            session = state->session;
        }
        if (session) {
            session->closePass(id);
        }
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::publishIfIdle(Document& document) noexcept
{
    if (traceInactive()) {
        return;
    }
    try {
        if (readyToPublish(document)) {
            // Outside the coordinator and the lane: callers publish after those
            // locks, or from the hold/defer destructor.
            const auto rssAfter = readResidentKb();
            tryPublish(document, rssAfter);
        }
        document.recomputeCoordinator().releaseSealedTraceSessions();
    }
    catch (...) {
    }
}

bool RecomputePhaseTrace::pushHold(Document& document) noexcept
{
    if (traceInactive()) {
        return false;
    }
    try {
        const bool enabled = recomputePhaseTraceEnabled();
        auto state = stateFor(document, false);
        if (!enabled && !hasLiveState()) {
            return false;
        }
        if (!state) {
            state = stateFor(document, true);
        }
        std::lock_guard lock(state->mutex);
        ++state->hold;
        g_traceLive.fetch_add(1, std::memory_order_relaxed);
        g_holdPushes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    catch (...) {
        return false;
    }
}

void RecomputePhaseTrace::popHold(Document& document) noexcept
{
    if (traceInactive()) {
        return;
    }
    try {
        const auto state = stateFor(document, false);
        if (!state) {
            return;
        }
        bool released = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->hold > 0) {
                --state->hold;
                released = true;
            }
        }
        if (released) {
            g_traceLive.fetch_sub(1, std::memory_order_relaxed);
        }
        publishIfIdle(document);
    }
    catch (...) {
    }
}

namespace
{

std::shared_ptr<void> retainSession(Document& document)
{
    const auto state = stateFor(document, false);
    if (!state) {
        return {};
    }
    std::lock_guard lock(state->mutex);
    return state->session;
}

}  // namespace

RecomputePhaseTraceHold::RecomputePhaseTraceHold(Document& document) noexcept
{
    if (RecomputePhaseTrace::pushHold(document)) {
        _document = &document;
        retain();
    }
}

RecomputePhaseTraceHold::~RecomputePhaseTraceHold() noexcept
{
    if (!_document) {
        return;
    }
    retain();
    auto* document = _document;
    _document = nullptr;
    RecomputePhaseTrace::popHold(*document);
    _session.reset();
}

void RecomputePhaseTraceHold::retain() noexcept
{
    if (!_document) {
        return;
    }
    try {
        if (auto session = retainSession(*_document)) {
            _session = std::move(session);
        }
    }
    catch (...) {
    }
}

bool RecomputePhaseTrace::pushDefer(Document& document) noexcept
{
    if (traceInactive()) {
        return false;
    }
    try {
        const bool enabled = recomputePhaseTraceEnabled();
        auto state = stateFor(document, false);
        if (!enabled && !hasLiveState()) {
            return false;
        }
        if (!state) {
            state = stateFor(document, true);
        }
        std::lock_guard lock(state->mutex);
        ++state->defer;
        g_traceLive.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    catch (...) {
        return false;
    }
}

void RecomputePhaseTrace::popDefer(Document& document) noexcept
{
    if (traceInactive()) {
        return;
    }
    try {
        const auto state = stateFor(document, false);
        if (!state) {
            return;
        }
        bool released = false;
        {
            std::lock_guard lock(state->mutex);
            if (state->defer > 0) {
                --state->defer;
                released = true;
            }
        }
        if (released) {
            g_traceLive.fetch_sub(1, std::memory_order_relaxed);
        }
        publishIfIdle(document);
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::testing::discard(Document& document) noexcept
{
    if (traceInactive()) {
        return;
    }
    try {
        const auto state = stateFor(document, false);
        if (!state) {
            return;
        }
        std::shared_ptr<RecomputePhaseSession> session;
        {
            std::lock_guard lock(state->mutex);
            session = std::move(state->session);
        }
        if (!session) {
            return;
        }
        if (g_traceActivation && g_traceActivation->_session == session) {
            g_traceActivation->_session.reset();
        }
        // Execute may still hold the coordinator lock. Do not read /proc here.
        auto published = session->forcePublish(std::nullopt);
        if (published.fresh) {
            enqueueTraceWrite(published.path, published.record);
        }
        {
            std::lock_guard lock(state->mutex);
            state->published = published.record;
        }
        document.recomputeCoordinator().releaseSealedTraceSessions();
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::forgetDocument(Document& document) noexcept
{
    // Preference-off with no retained state returns before the map lock.
    // A sealed record still has to be erased, or the next document at this
    // address would inherit it.
    if (g_traceStates.load(std::memory_order_acquire) == 0) {
        return;
    }
    try {
        std::shared_ptr<DocumentTraceState> state;
        {
            std::lock_guard lock(documentTraceMutex());
            auto& states = documentTraceStates();
            const auto found = states.find(&document);
            if (found == states.end()) {
                return;
            }
            state = found->second;
            states.erase(found);
        }
        g_traceStates.fetch_sub(1, std::memory_order_release);
        if (!state) {
            return;
        }
        std::shared_ptr<RecomputePhaseSession> session;
        {
            std::lock_guard lock(state->mutex);
            session = std::move(state->session);
            state->published.reset();
        }
        if (!session) {
            return;
        }
        session->detachDocument();
        if (g_traceActivation && g_traceActivation->_session == session) {
            g_traceActivation->_session.reset();
            g_traceActivation->_document = nullptr;
        }
        // The job still owns the session when one is in flight. Dropping this
        // reference seals only when nothing else holds it, on this thread.
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::noteIsolatedExecute(Document& document,
                                              const DocumentObject* object,
                                              const std::int64_t startMonotonicNs,
                                              const std::int64_t endMonotonicNs) noexcept
{
    if (!object || !g_traceActivation || !g_traceActivation->_session
        || g_traceActivation->_document != &document) {
        return;
    }
    try {
        g_traceActivation->_session->noteExecute(*object, startMonotonicNs, endMonotonicNs);
    }
    catch (...) {
    }
}

const DocumentObject* RecomputePhaseTrace::objectForOperation(
    Document& document, const std::string_view operationId) noexcept
{
    if (!g_traceActivation || !g_traceActivation->_session) {
        return nullptr;
    }
    try {
        if (const auto* object = g_traceActivation->_session->findOperation(operationId)) {
            return object;
        }
        constexpr std::string_view prefix = "generic-recompute:";
        if (!operationId.starts_with(prefix)) {
            return nullptr;
        }
        const std::string name(operationId.substr(prefix.size()));
        return document.getObject(name.c_str());
    }
    catch (...) {
        return nullptr;
    }
}

bool RecomputePhaseTrace::active(const Document& document) noexcept
{
    if (traceInactive()) {
        return false;
    }
    const auto state = stateFor(document, false);
    if (!state) {
        return false;
    }
    std::lock_guard lock(state->mutex);
    return static_cast<bool>(state->session);
}

std::shared_ptr<const RecomputePhaseTraceRecord> RecomputePhaseTrace::published(
    const Document& document) noexcept
{
    if (!recomputePhaseTraceEnabled()) {
        return nullptr;
    }
    const auto state = stateFor(document, false);
    if (!state) {
        return nullptr;
    }
    std::lock_guard lock(state->mutex);
    return state->published;
}

RecomputePhaseCpu RecomputePhaseTrace::cpuBudget() noexcept
{
    RecomputePhaseCpu cpu;
    cpu.hardwareConcurrency = std::thread::hardware_concurrency();
    cpu.ownerThreads = 1;
    cpu.preparedEditWorkers = 0;
    cpu.geometryJobActiveCapacity = 0;
    try {
        auto& application = GetApplication();
        if (application._preparedEditExecutor) {
            cpu.preparedEditWorkers = application._preparedEditExecutor->workerCount();
        }
        if (application._geometryJobManager) {
            cpu.geometryJobActiveCapacity = application._geometryJobManager->activeCapacity();
        }
    }
    catch (...) {
    }
    return cpu;
}

void RecomputePhaseTrace::installPreferenceCache(::ParameterManager& userParameters) noexcept
{
    try {
        auto& observer = preferenceObserver();
        if (!preferenceInstalled()) {
            observer.group = userParameters.GetGroup("BaseApp/Preferences/Document");
            if (observer.group) {
                observer.group->Attach(&observer);
                preferenceInstalled() = true;
            }
        }
        if (observer.group) {
            const bool enabled = observer.group->GetBool("RecomputePhaseTrace", false);
            g_recomputePhaseTraceEnabled.store(enabled, std::memory_order_relaxed);
            if (enabled && !observer.group->GetASCII("RecomputePhaseTracePath", "").empty()) {
                traceWriter().ensureStarted();
            }
        }
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::removePreferenceCache() noexcept
{
    try {
        g_recomputePhaseTraceEnabled.store(false, std::memory_order_relaxed);
        traceWriter().shutdown();
        preferenceObserver().detach();
        preferenceInstalled() = false;
    }
    catch (...) {
    }
}

std::shared_ptr<const RecomputePhaseTraceRecord> Document::lastRecomputeTrace() const noexcept
{
    if (!recomputePhaseTraceEnabled()) {
        return nullptr;
    }
    return RecomputePhaseTrace::published(*this);
}

void RecomputePhaseTrace::testing::pauseWriter(const bool pause) noexcept
{
    try {
        traceWriter().setPause(pause);
    }
    catch (...) {
    }
}

void RecomputePhaseTrace::testing::enqueue(
    std::string path,
    std::shared_ptr<const RecomputePhaseTraceRecord> record) noexcept
{
    try {
        traceWriter().enqueue(std::move(path), std::move(record));
    }
    catch (...) {
    }
}

std::uint64_t RecomputePhaseTrace::testing::droppedRecords() noexcept
{
    try {
        return traceWriter().droppedRecords();
    }
    catch (...) {
        return 0;
    }
}

std::uint64_t RecomputePhaseTrace::testing::ioErrors() noexcept
{
    try {
        return traceWriter().ioErrors();
    }
    catch (...) {
        return 0;
    }
}

std::uint64_t RecomputePhaseTrace::testing::holdPushes() noexcept
{
    return g_holdPushes.load(std::memory_order_relaxed);
}

void RecomputePhaseTrace::testing::setCompatibilityWaits(
    const std::chrono::milliseconds primary,
    const std::chrono::milliseconds followUp) noexcept
{
    g_compatibilityWaitPrimaryMs.store(primary.count(), std::memory_order_relaxed);
    g_compatibilityWaitFollowMs.store(followUp.count(), std::memory_order_relaxed);
}

void RecomputePhaseTrace::testing::clearCompatibilityWaits() noexcept
{
    g_compatibilityWaitPrimaryMs.store(-1, std::memory_order_relaxed);
    g_compatibilityWaitFollowMs.store(-1, std::memory_order_relaxed);
}

std::chrono::milliseconds RecomputePhaseTrace::testing::compatibilityWaitPrimary() noexcept
{
    const auto milliseconds = g_compatibilityWaitPrimaryMs.load(std::memory_order_relaxed);
    if (milliseconds < 0) {
        return std::chrono::minutes(6);
    }
    return std::chrono::milliseconds(milliseconds);
}

std::chrono::milliseconds RecomputePhaseTrace::testing::compatibilityWaitFollowUp() noexcept
{
    const auto milliseconds = g_compatibilityWaitFollowMs.load(std::memory_order_relaxed);
    if (milliseconds < 0) {
        return std::chrono::seconds(30);
    }
    return std::chrono::milliseconds(milliseconds);
}

void RecomputePhaseTrace::releaseSealedJobSession(std::shared_ptr<void>& session) noexcept
{
    if (!session) {
        return;
    }
    const auto held = std::static_pointer_cast<RecomputePhaseSession>(session);
    if (held && held->isSealed()) {
        session.reset();
    }
}


bool Document::recomputePhaseTraceActive() const noexcept
{
    return RecomputePhaseTrace::active(*this);
}

}  // namespace App
