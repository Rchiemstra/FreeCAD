// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <memory>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCommand.h"
#include "App/DocumentCommandHandle.h"
#include "App/DocumentExecutionLane.h"
#include "App/DocumentHandle.h"
#include "App/DocumentWouldBlock.h"
#include "App/FeatureTest.h"
#include "App/MainThreadSignal.h"
#include "App/RecomputeHandle.h"
#include "App/RecomputePhaseTrace.h"
#include "Base/Exception.h"
#include "Base/Interpreter.h"
#include "Base/Parameter.h"
#include <src/App/InitApplication.h>

namespace App
{

class PhaseTraceSecondPassFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceSecondPassFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        if (auto* document = getDocument()) {
            if (auto* peer = document->getObject("Peer")) {
                peer->touch();
            }
        }
        return FeatureTest::execute();
    }
};

class PhaseTraceReentrantFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceReentrantFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        if (!_nested && getDocument()) {
            _nested = true;
            getDocument()->recompute();
        }
        return FeatureTest::execute();
    }

private:
    bool _nested {false};
};

class PhaseTraceDiscardFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceDiscardFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        if (auto* document = getDocument()) {
            RecomputePhaseTrace::testing::discard(*document);
        }
        return FeatureTest::execute();
    }
};

class PhaseTraceRetryFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceRetryFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        if (auto* document = getDocument()) {
            if (auto* again = document->getObject("Again")) {
                again->touch();
            }
        }
        return FeatureTest::execute();
    }
};

class PhaseTraceToggleFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceToggleFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        if (group) {
            group->SetBool("RecomputePhaseTrace", false);
        }
        return FeatureTest::execute();
    }
};

class PhaseTraceClearDuringFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceClearDuringFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        if (group) {
            group->Clear(false);
        }
        return FeatureTest::execute();
    }
};

class PhaseTraceManyAttemptsFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceManyAttemptsFeature);

public:
    DocumentObjectExecReturn* execute() override
    {
        for (int attempt = 0; attempt < 5; ++attempt) {
            RecomputePhaseSpan span(RecomputePhase::Capture, this);
        }
        return FeatureTest::execute();
    }
};

/** Signals at execute() entry, then sleeps via FeatureTest::ExecuteDelay. */
class PhaseTraceDelayFeature: public FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::PhaseTraceDelayFeature);

public:
    static std::atomic<int> entered;

    DocumentObjectExecReturn* execute() override
    {
        entered.fetch_add(1, std::memory_order_release);
        return FeatureTest::execute();
    }
};

}  // namespace App

PROPERTY_SOURCE(App::PhaseTraceSecondPassFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceReentrantFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceDiscardFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceRetryFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceToggleFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceClearDuringFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceManyAttemptsFeature, App::FeatureTest)
PROPERTY_SOURCE(App::PhaseTraceDelayFeature, App::FeatureTest)

std::atomic<int> App::PhaseTraceDelayFeature::entered {0};

using namespace App;

namespace
{

const std::vector<RecomputePhaseInterval>& attemptsOf(const RecomputePhaseObjectTrace& object,
                                                      const RecomputePhase phase)
{
    return object.attempts[static_cast<std::size_t>(phase)];
}

const RecomputePhaseInterval& onlyAttempt(const RecomputePhaseObjectTrace& object,
                                          const RecomputePhase phase)
{
    const auto& attempts = attemptsOf(object, phase);
    static const RecomputePhaseInterval empty;
    EXPECT_EQ(attempts.size(), 1u) << object.name << " " << recomputePhaseTraceKey(phase);
    if (attempts.size() != 1) {
        return empty;
    }
    return attempts.front();
}

const RecomputePhaseObjectTrace* findObject(const RecomputePhaseTraceRecord& record, const char* name)
{
    for (const auto& object : record.objects) {
        if (object.name == name) {
            return &object;
        }
    }
    return nullptr;
}

void expectExecutedExactly(const RecomputePhaseTraceRecord& record,
                           const std::set<std::string>& expected)
{
    std::set<std::string> actual;
    for (const auto& object : record.objects) {
        if (object.executed) {
            actual.insert(object.name);
        }
    }
    EXPECT_EQ(actual, expected);
}

void expectExecutedSpans(const RecomputePhaseObjectTrace& object)
{
    ASSERT_TRUE(object.executed) << object.name;
    const auto& capture = onlyAttempt(object, RecomputePhase::Capture);
    const auto& execute = onlyAttempt(object, RecomputePhase::Execute);
    const auto& commit = onlyAttempt(object, RecomputePhase::Commit);
    const auto& presentation = onlyAttempt(object, RecomputePhase::Presentation);
    EXPECT_TRUE(attemptsOf(object, RecomputePhase::Transfer).empty()) << object.name;
    EXPECT_GE(capture.startNs, 0);
    EXPECT_LE(capture.startNs, capture.endNs);
    EXPECT_LE(capture.endNs, execute.startNs);
    EXPECT_LE(commit.startNs, presentation.startNs);
    EXPECT_LE(presentation.startNs, presentation.endNs);
    EXPECT_LE(presentation.endNs, execute.startNs);
    EXPECT_LE(execute.startNs, execute.endNs);
    EXPECT_LE(execute.endNs, commit.endNs);
}

int countTraceLines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    int lines = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        ++lines;
        EXPECT_NE(line.find("\"v\":1"), std::string::npos) << line;
        EXPECT_NE(line.find("\"io_errors\":"), std::string::npos) << line;
    }
    return lines;
}

int waitForTraceLines(const std::filesystem::path& path, const int minimum)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int lines = 0;
    do {
        lines = countTraceLines(path);
        if (lines >= minimum) {
            return lines;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return lines;
}

std::uint64_t jsonUint(const std::string& line, const char* key)
{
    const auto token = std::string("\"") + key + "\":";
    const auto found = line.find(token);
    if (found == std::string::npos) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::strtoull(line.c_str() + found + token.size(), nullptr, 10));
}

std::uint64_t droppedRecordsIn(const std::string& line)
{
    return jsonUint(line, "dropped_records");
}

std::uint64_t ioErrorsIn(const std::string& line)
{
    return jsonUint(line, "io_errors");
}

std::string jsonString(const std::string& line, const char* key)
{
    const auto token = std::string("\"") + key + "\":\"";
    const auto found = line.find(token);
    if (found == std::string::npos) {
        return {};
    }
    const auto start = found + token.size();
    const auto end = line.find('"', start);
    if (end == std::string::npos) {
        return {};
    }
    return line.substr(start, end - start);
}

std::vector<std::string> readTraceLines(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

/** Only the constructing thread is the GUI thread. The document owner is not. */
struct GuiThreadIdentity
{
    static GuiThreadIdentity* active;

    GuiThreadIdentity()
    {
        active = this;
        MainThreadSignalConfig::setHooks(&isMain, &invoke);
    }

    ~GuiThreadIdentity()
    {
        MainThreadSignalConfig::setHooks(nullptr, nullptr);
        active = nullptr;
    }

    static bool isMain()
    {
        return active && std::this_thread::get_id() == active->main;
    }

    static void invoke(std::function<void()>&& callback, bool)
    {
        callback();
    }

    const std::thread::id main {std::this_thread::get_id()};
};

GuiThreadIdentity* GuiThreadIdentity::active {nullptr};

struct AdmissionTimeoutOverride
{
    Base::Reference<ParameterGrp> group;
    long previous {500};

    explicit AdmissionTimeoutOverride(const long milliseconds)
        : group(GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document"))
        , previous(group->GetInt("GuiSyncAdmissionTimeoutMs", 500))
    {
        group->SetInt("GuiSyncAdmissionTimeoutMs", milliseconds);
    }

    ~AdmissionTimeoutOverride()
    {
        group->SetInt("GuiSyncAdmissionTimeoutMs", previous);
    }
};

struct CompatibilityWaitOverride
{
    CompatibilityWaitOverride(const std::chrono::milliseconds primary,
                              const std::chrono::milliseconds followUp)
    {
        RecomputePhaseTrace::testing::setCompatibilityWaits(primary, followUp);
    }

    ~CompatibilityWaitOverride()
    {
        RecomputePhaseTrace::testing::clearCompatibilityWaits();
    }
};

struct RecomputeOutcome
{
    bool wouldBlock {false};
    int value {-1};
    std::string message;
    std::chrono::milliseconds elapsed {0};
};

RecomputeOutcome callRecompute(Document& document)
{
    RecomputeOutcome outcome;
    const auto started = std::chrono::steady_clock::now();
    try {
        outcome.value = document.recompute();
    }
    catch (const DocumentWouldBlock& error) {
        outcome.wouldBlock = true;
        outcome.message = error.what();
    }
    outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    return outcome;
}

bool waitUntilDelayEntered(const int target, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (PhaseTraceDelayFeature::entered.load(std::memory_order_acquire) < target) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

DocumentCommand delayRecomputeCommand(Document& document, const char* featureName, const int nonce)
{
    DocumentCommand command;
    command.kind = DocumentCommandKind::Recompute;
    command.document = document.executionHandle().identity();
    command.recompute = DocumentCommandRecomputePayload {};
    command.recompute->featureIds = {featureName};
    command.recompute->coalescingKey =
        std::string("phase-trace-delay-") + featureName + "-" + std::to_string(nonce);
    return command;
}

bool waitForCommand(const DocumentCommandHandle& handle, const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (handle.status().terminal()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return handle.status().terminal();
}

/** Fail the process if `fn` does not return. A hung close never reaches EXPECT. */
template<typename Callable>
void expectPrompt(Callable&& fn)
{
    std::atomic<bool> done {false};
    std::thread watchdog([&done] {
        for (int tick = 0; tick < 80; ++tick) {
            if (done.load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load(std::memory_order_relaxed)) {
            std::_Exit(86);
        }
    });
    const auto started = std::chrono::steady_clock::now();
    try {
        fn();
    }
    catch (...) {
        done.store(true, std::memory_order_relaxed);
        watchdog.join();
        throw;
    }
    done.store(true, std::memory_order_relaxed);
    watchdog.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
}

void expectPythonTrace(Document& document, const RecomputePhaseTraceRecord* record)
{
    Base::PyGILStateLocker gil;
    Py::Object python(document.getPyObject(), true);
    Py::Object trace(python.callMemberFunction("lastRecomputeTrace"));
    if (!record) {
        EXPECT_TRUE(trace.isNone());
        return;
    }
    ASSERT_TRUE(PyDict_Check(trace.ptr()));
    PyObject* version = PyDict_GetItemString(trace.ptr(), "v");
    ASSERT_NE(version, nullptr);
    EXPECT_EQ(PyLong_AsLong(version), 1);
    PyObject* hash = PyDict_GetItemString(trace.ptr(), "git_hash");
    if (record->hasGitHash) {
        ASSERT_NE(hash, nullptr);
        EXPECT_TRUE(PyUnicode_Check(hash));
    }
    else {
        EXPECT_EQ(hash, nullptr);
    }
    PyObject* overlap = PyDict_GetItemString(trace.ptr(), "overlap");
    ASSERT_NE(overlap, nullptr);
    EXPECT_EQ(PyLong_AsLong(overlap), 1);
    PyObject* id = PyDict_GetItemString(trace.ptr(), "recompute_id");
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(PyLong_AsLongLong(id), static_cast<long long>(record->recomputeId));
    if (record->rssValid) {
        PyObject* rss = PyDict_GetItemString(trace.ptr(), "rss_kb");
        ASSERT_NE(rss, nullptr);
        PyObject* peak = PyDict_GetItemString(rss, "sampled_peak");
        ASSERT_NE(peak, nullptr);
        EXPECT_EQ(PyLong_AsUnsignedLongLong(peak), record->rssPeakKb);
    }
    const RecomputePhaseObjectTrace* executed = nullptr;
    for (const auto& object : record->objects) {
        if (object.executed) {
            executed = &object;
            break;
        }
    }
    if (!executed) {
        return;
    }
    const auto& attempts = attemptsOf(*executed, RecomputePhase::Execute);
    ASSERT_FALSE(attempts.empty());
    PyObject* objects = PyDict_GetItemString(trace.ptr(), "objects");
    ASSERT_NE(objects, nullptr);
    ASSERT_TRUE(PyList_Check(objects));
    bool found = false;
    for (Py_ssize_t index = 0; index < PyList_Size(objects); ++index) {
        PyObject* item = PyList_GetItem(objects, index);
        PyObject* name = PyDict_GetItemString(item, "name");
        if (!name || !PyUnicode_Check(name)
            || PyUnicode_AsUTF8(name) != executed->name) {
            continue;
        }
        found = true;
        PyObject* phases = PyDict_GetItemString(item, "phases");
        ASSERT_NE(phases, nullptr);
        PyObject* execute = PyDict_GetItemString(phases, "execute");
        ASSERT_TRUE(PyList_Check(execute));
        ASSERT_GT(PyList_Size(execute), 0);
        PyObject* interval = PyList_GetItem(execute, 0);
        PyObject* start = PyDict_GetItemString(interval, "start_ns");
        ASSERT_NE(start, nullptr);
        EXPECT_EQ(PyLong_AsLongLong(start), attempts.front().startNs);
    }
    EXPECT_TRUE(found);
}

}  // namespace

class RecomputePhaseTraceTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        PhaseTraceSecondPassFeature::init();
        PhaseTraceReentrantFeature::init();
        PhaseTraceDiscardFeature::init();
        PhaseTraceRetryFeature::init();
        PhaseTraceToggleFeature::init();
        PhaseTraceClearDuringFeature::init();
        PhaseTraceManyAttemptsFeature::init();
        PhaseTraceDelayFeature::init();
    }

    void SetUp() override
    {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        _previousEnabled = group->GetBool("RecomputePhaseTrace", false);
        _previousPath = group->GetASCII("RecomputePhaseTracePath", "");
        _name = "PhaseTrace" + std::to_string(++_sequence);
        _document = GetApplication().newDocument(_name.c_str(), "phase trace");
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        if (_document) {
            GetApplication().closeDocument(_document->getName());
            _document = nullptr;
        }
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        group->SetBool("RecomputePhaseTrace", _previousEnabled);
        group->SetASCII("RecomputePhaseTracePath", _previousPath);
    }

    void setTrace(const bool enabled, const std::string& path)
    {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        group->SetBool("RecomputePhaseTrace", enabled);
        group->SetASCII("RecomputePhaseTracePath", path);
    }

    FeatureTest* addFeature(const char* name)
    {
        auto* object = dynamic_cast<FeatureTest*>(_document->addObject("App::FeatureTest", name));
        EXPECT_NE(object, nullptr);
        if (object) {
            object->touch();
        }
        return object;
    }

    void expectCommon(const RecomputePhaseTraceRecord& record) const
    {
        EXPECT_EQ(record.version, 1);
        EXPECT_EQ(record.document, _document->getName());
        EXPECT_FALSE(record.freecadVersion.empty());
        EXPECT_EQ(record.overlap, 1);
        EXPECT_EQ(record.cpu.ownerThreads, 1u);
        EXPECT_EQ(record.cpu.hardwareConcurrency, std::thread::hardware_concurrency());
        EXPECT_FALSE(record.truncated);
        EXPECT_FALSE(_document->recomputePhaseTraceActive());
        ASSERT_FALSE(record.presentationBoundaries.empty());
        const auto& boundary = record.presentationBoundaries.back();
        EXPECT_GE(boundary.endNs, boundary.startNs);
#if defined(__linux__)
        EXPECT_TRUE(record.rssValid);
        EXPECT_GT(record.rssBeforeKb, 0u);
        EXPECT_GE(record.rssPeakKb, record.rssBeforeKb);
        EXPECT_GE(record.rssPeakKb, record.rssAfterKb);
        EXPECT_GT(record.rssAfterKb, 0u);
#endif
        for (const auto& object : record.objects) {
            if (!object.executed) {
                continue;
            }
            expectExecutedSpans(object);
            for (const auto& commit : attemptsOf(object, RecomputePhase::Commit)) {
                EXPECT_GE(boundary.startNs, commit.endNs) << object.name;
            }
        }
    }

    Document* _document {nullptr};
    std::string _name;
    bool _previousEnabled {false};
    std::string _previousPath;
    static int _sequence;
};

int RecomputePhaseTraceTest::_sequence = 0;

TEST_F(RecomputePhaseTraceTest, preferenceOffLeavesNoTrace)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-off.jsonl";
    std::filesystem::remove(path);
    setTrace(false, path.string());
    ASSERT_NE(addFeature("Box"), nullptr);
    _document->recompute();
    EXPECT_EQ(_document->lastRecomputeTrace(), nullptr);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    EXPECT_FALSE(std::filesystem::exists(path));
    expectPythonTrace(*_document, nullptr);
}

TEST_F(RecomputePhaseTraceTest, independentObjects)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-independent.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    ASSERT_NE(addFeature("Box1"), nullptr);
    ASSERT_NE(addFeature("Box2"), nullptr);
    ASSERT_NE(addFeature("Box3"), nullptr);
    _document->recompute();
    const auto first = _document->lastRecomputeTrace();
    ASSERT_NE(first, nullptr);
    expectCommon(*first);
    EXPECT_EQ(first->dag.objectCount, 3u);
    EXPECT_EQ(first->dag.edgeCount, 0u);
    EXPECT_EQ(first->dag.longestPath, 0u);
    EXPECT_EQ(first->dag.maxWidth, 3u);
    EXPECT_EQ(first->objects.size(), 3u);
    const auto firstId = first->recomputeId;
    expectExecutedExactly(*first, {"Box1", "Box2", "Box3"});
    expectPythonTrace(*_document, first.get());

    ASSERT_NE(findObject(*first, "Box1"), nullptr);
    _document->getObject("Box1")->touch();
    _document->recompute();
    const auto second = _document->lastRecomputeTrace();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->recomputeId, firstId);
    EXPECT_EQ(waitForTraceLines(path, 2), 2);
}

TEST_F(RecomputePhaseTraceTest, dependencyChain)
{
    setTrace(true, "");
    auto* first = addFeature("A");
    auto* second = addFeature("B");
    auto* third = addFeature("C");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);
    second->Source1.setValue(first);
    third->Source1.setValue(second);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectCommon(*record);
    expectExecutedExactly(*record, {"A", "B", "C"});
    EXPECT_EQ(record->dag.objectCount, 3u);
    EXPECT_EQ(record->dag.edgeCount, 2u) << record->dag.longestPath << " " << record->dag.maxWidth;
    EXPECT_EQ(record->dag.longestPath, 2u);
    EXPECT_EQ(record->dag.maxWidth, 1u);
}

TEST_F(RecomputePhaseTraceTest, dependencyDiamond)
{
    setTrace(true, "");
    auto* apex = addFeature("A");
    auto* left = addFeature("B");
    auto* right = addFeature("C");
    auto* join = addFeature("D");
    ASSERT_NE(apex, nullptr);
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_NE(join, nullptr);
    left->Source1.setValue(apex);
    right->Source1.setValue(apex);
    join->Source1.setValue(left);
    join->Source2.setValue(right);
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectCommon(*record);
    expectExecutedExactly(*record, {"A", "B", "C", "D"});
    EXPECT_EQ(record->dag.objectCount, 4u);
    EXPECT_EQ(record->dag.edgeCount, 4u) << record->dag.longestPath << " " << record->dag.maxWidth;
    EXPECT_EQ(record->dag.longestPath, 2u);
    EXPECT_EQ(record->dag.maxWidth, 2u);
}

TEST_F(RecomputePhaseTraceTest, failingFeatureStillRecordsSpans)
{
    setTrace(true, "");
    auto* failing = dynamic_cast<FeatureTestException*>(
        _document->addObject("App::FeatureTestException", "Bad"));
    ASSERT_NE(failing, nullptr);
    failing->touch();
    bool hasError = false;
    _document->recompute({}, false, &hasError);
    EXPECT_TRUE(hasError);
    EXPECT_TRUE(failing->isError());
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectExecutedExactly(*record, {"Bad"});
    EXPECT_EQ(record->objects.size(), 1u);
    EXPECT_EQ(record->dag.objectCount, 1u);
    EXPECT_EQ(record->dag.edgeCount, 0u);
    EXPECT_EQ(record->dag.longestPath, 0u);
    EXPECT_EQ(record->dag.maxWidth, 1u);
    EXPECT_EQ(record->overlap, 1);
    const auto* object = findObject(*record, "Bad");
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->type, "App::FeatureTestException");
    expectExecutedSpans(*object);
    const auto failedId = record->recomputeId;
    auto* good = addFeature("Good");
    ASSERT_NE(good, nullptr);
    good->touch();
    _document->recompute({good});
    const auto second = _document->lastRecomputeTrace();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->recomputeId, failedId);
    expectExecutedExactly(*second, {"Good"});
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

namespace
{

struct PreferenceSnapshot
{
    struct Group
    {
        std::string name;
        std::unique_ptr<PreferenceSnapshot> values;
    };

    std::vector<std::pair<std::string, bool>> bools;
    std::vector<std::pair<std::string, long>> ints;
    std::vector<std::pair<std::string, unsigned long>> unsigneds;
    std::vector<std::pair<std::string, double>> floats;
    std::vector<std::pair<std::string, std::string>> asciis;
    std::vector<std::pair<std::string, Base::Color>> colors;
    std::vector<Group> groups;

    PreferenceSnapshot();
    ~PreferenceSnapshot();
    PreferenceSnapshot(PreferenceSnapshot&&) noexcept;
    PreferenceSnapshot& operator=(PreferenceSnapshot&&) noexcept;
    PreferenceSnapshot(const PreferenceSnapshot&) = delete;
    PreferenceSnapshot& operator=(const PreferenceSnapshot&) = delete;
};

PreferenceSnapshot::PreferenceSnapshot() = default;
PreferenceSnapshot::~PreferenceSnapshot() = default;
PreferenceSnapshot::PreferenceSnapshot(PreferenceSnapshot&&) noexcept = default;
PreferenceSnapshot& PreferenceSnapshot::operator=(PreferenceSnapshot&&) noexcept = default;

PreferenceSnapshot capturePreferences(ParameterGrp& group)
{
    PreferenceSnapshot snapshot;
    snapshot.bools = group.GetBoolMap();
    snapshot.ints = group.GetIntMap();
    snapshot.unsigneds = group.GetUnsignedMap();
    snapshot.floats = group.GetFloatMap();
    snapshot.asciis = group.GetASCIIMap();
    snapshot.colors = group.GetColorMap();
    for (const auto& child : group.GetGroups()) {
        PreferenceSnapshot::Group nested;
        nested.name = child->GetGroupName();
        nested.values = std::make_unique<PreferenceSnapshot>(capturePreferences(*child));
        snapshot.groups.push_back(std::move(nested));
    }
    return snapshot;
}

void restorePreferences(ParameterGrp& group, const PreferenceSnapshot& snapshot)
{
    for (const auto& [name, value] : snapshot.bools) {
        group.SetBool(name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.ints) {
        group.SetInt(name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.unsigneds) {
        group.SetUnsigned(name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.floats) {
        group.SetFloat(name.c_str(), value);
    }
    for (const auto& [name, value] : snapshot.asciis) {
        group.SetASCII(name.c_str(), value.c_str());
    }
    for (const auto& [name, value] : snapshot.colors) {
        group.SetColor(name.c_str(), value);
    }
    for (const auto& child : snapshot.groups) {
        restorePreferences(*group.GetGroup(child.name.c_str()), *child.values);
    }
}

}  // namespace

TEST_F(RecomputePhaseTraceTest, recomputeAsyncPublishesATrace)
{
    setTrace(true, "");
    auto* feature = addFeature("Async");
    ASSERT_NE(feature, nullptr);
    auto handle = _document->recomputeAsync({feature});
    ASSERT_NE(handle, nullptr);
    const auto snapshot = handle->wait(std::chrono::seconds(60));
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectExecutedExactly(*record, {"Async"});
    expectCommon(*record);
}

TEST_F(RecomputePhaseTraceTest, isolatedWorkerCarriesExecuteAndTransfer)
{
    setTrace(true, "");
    auto* feature = addFeature("Worker");
    ASSERT_NE(feature, nullptr);
    feature->ExecuteDelay.setValue(150);
    auto handle = _document->recomputeAsync(
        {feature}, false, 0, RecomputeVenue::Isolated);
    ASSERT_NE(handle, nullptr);
    const auto snapshot = handle->wait(std::chrono::seconds(120));
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, DocumentRecomputeState::Completed) << snapshot.diagnostic;
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    const auto* object = findObject(*record, "Worker");
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(object->executed);
    const auto& execute = attemptsOf(*object, RecomputePhase::Execute);
    const auto& transfer = attemptsOf(*object, RecomputePhase::Transfer);
    const auto& commit = attemptsOf(*object, RecomputePhase::Commit);
    ASSERT_EQ(execute.size(), 1u);
    ASSERT_EQ(transfer.size(), 1u);
    ASSERT_FALSE(commit.empty());
    const auto executeSpan = execute.front().endNs - execute.front().startNs;
    const auto transferSpan = transfer.front().endNs - transfer.front().startNs;
    EXPECT_GE(executeSpan, 150'000'000);
    EXPECT_GT(transferSpan, 0);
    EXPECT_GT(executeSpan, transferSpan);
    EXPECT_LE(execute.front().endNs, transfer.front().startNs);
    EXPECT_LE(transfer.front().endNs, commit.front().startNs);
    EXPECT_NE(execute.front().passId, 0u);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, cancelledRecomputeRecordsTheExecutedObject)
{
    setTrace(true, "");
    FeatureTestAsyncBlocker::resetBlocker();
    auto* blocker = dynamic_cast<FeatureTestAsyncBlocker*>(
        _document->addObject("App::FeatureTestAsyncBlocker", "Blocker"));
    ASSERT_NE(blocker, nullptr);
    blocker->touch();
    auto handle = _document->recomputeAsync({blocker});
    ASSERT_NE(handle, nullptr);
    std::atomic<bool> sawStart {false};
    std::thread helper([&] {
        sawStart = FeatureTestAsyncBlocker::waitUntilStarted(std::chrono::seconds(20));
        if (sawStart) {
            static_cast<void>(handle->cancel("phase trace cancel"));
        }
        FeatureTestAsyncBlocker::releaseBlocker();
    });
    const auto snapshot = handle->wait(std::chrono::seconds(60));
    helper.join();
    ASSERT_TRUE(sawStart);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, DocumentRecomputeState::Cancelled) << snapshot.diagnostic;
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectExecutedExactly(*record, {"Blocker"});
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    const auto cancelledId = record->recomputeId;
    ASSERT_NE(addFeature("Next"), nullptr);
    _document->recompute();
    const auto second = _document->lastRecomputeTrace();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(second->recomputeId, cancelledId);
    EXPECT_EQ(second->passes.size(), 1u);
    expectExecutedExactly(*second, {"Next"});
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    FeatureTestAsyncBlocker::releaseBlocker();
}

TEST_F(RecomputePhaseTraceTest, secondPassKeepsEachRequest)
{
    setTrace(false, "");
    auto* toucher = dynamic_cast<PhaseTraceSecondPassFeature*>(
        _document->addObject("App::PhaseTraceSecondPassFeature", "Toucher"));
    auto* peer = addFeature("Peer");
    ASSERT_NE(toucher, nullptr);
    ASSERT_NE(peer, nullptr);
    toucher->touch();
    _document->recompute();
    EXPECT_EQ(_document->lastRecomputeTrace(), nullptr);

    setTrace(true, "");
    toucher->touch();
    EXPECT_FALSE(peer->isTouched());
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(record->passes.size(), 2u);
    EXPECT_NE(record->passes[0].id, record->passes[1].id);
    EXPECT_EQ(record->recomputeIds.size(), 2u);
    EXPECT_EQ(record->passes[0].dag.objectCount, 1u);
    EXPECT_EQ(record->dag.objectCount, 2u);
    expectExecutedExactly(*record, {"Toucher", "Peer"});
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, reentrantRecomputeKeepsOuterExecute)
{
    setTrace(true, "");
    auto* feature = dynamic_cast<PhaseTraceReentrantFeature*>(
        _document->addObject("App::PhaseTraceReentrantFeature", "Outer"));
    ASSERT_NE(feature, nullptr);
    feature->touch();
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    const auto* object = findObject(*record, "Outer");
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(feature->isError());
    EXPECT_TRUE(object->executed);
    const auto& execute = attemptsOf(*object, RecomputePhase::Execute);
    const auto& commit = attemptsOf(*object, RecomputePhase::Commit);
    ASSERT_EQ(execute.size(), 1u);
    ASSERT_FALSE(commit.empty());
    // The rejected inner recompute runs inside the already-open commit span.
    EXPECT_LE(commit.front().startNs, execute.front().startNs);
    EXPECT_LE(execute.front().endNs, commit.front().endNs);
    ASSERT_FALSE(record->presentationBoundaries.empty());
    for (const auto& boundary : record->presentationBoundaries) {
        EXPECT_LE(boundary.startNs, boundary.endNs);
        EXPECT_LE(execute.front().endNs, boundary.startNs);
    }
    for (const auto& traced : record->objects) {
        for (const auto& attempts : traced.attempts) {
            for (const auto& interval : attempts) {
                EXPECT_GE(interval.startNs, 0) << traced.name;
                EXPECT_LE(interval.startNs, interval.endNs) << traced.name;
            }
        }
    }
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, discardDuringExecuteKeepsTheSpanAlive)
{
    setTrace(true, "");
    auto* feature = dynamic_cast<PhaseTraceDiscardFeature*>(
        _document->addObject("App::PhaseTraceDiscardFeature", "Closing"));
    ASSERT_NE(feature, nullptr);
    feature->touch();
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    const auto* object = findObject(*record, "Closing");
    ASSERT_NE(object, nullptr);
    EXPECT_TRUE(object->executed);
    const auto& execute = attemptsOf(*object, RecomputePhase::Execute);
    const auto& commit = attemptsOf(*object, RecomputePhase::Commit);
    const auto& presentation = attemptsOf(*object, RecomputePhase::Presentation);
    ASSERT_FALSE(execute.empty());
    ASSERT_FALSE(commit.empty());
    ASSERT_FALSE(presentation.empty());
    // Commit and presentation are already open when execute runs. Discard
    // force-closes them there, so they do not outlive the execute interval.
    EXPECT_LE(commit.front().endNs, execute.front().endNs);
    EXPECT_LE(presentation.front().endNs, execute.front().endNs);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, preferenceClearDisablesTheTrace)
{
    setTrace(true, "");
    auto group = GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ASSERT_TRUE(group);
    auto saved = capturePreferences(*group);
    struct Restore
    {
        ParameterGrp* group {nullptr};
        PreferenceSnapshot saved;
        ~Restore()
        {
            if (group) {
                restorePreferences(*group, saved);
            }
        }
    } restore {group, std::move(saved)};

    group->Clear(false);
    EXPECT_FALSE(recomputePhaseTraceEnabled());
    ASSERT_NE(addFeature("Cleared"), nullptr);
    _document->recompute();
    EXPECT_EQ(_document->lastRecomputeTrace(), nullptr);
}

TEST_F(RecomputePhaseTraceTest, jsonlWritesWithoutReadingTheGetter)
{
    const auto path = std::filesystem::temp_directory_path()
        / "freecad-phase-trace-auto.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    ASSERT_NE(addFeature("Auto"), nullptr);
    _document->recompute();
    EXPECT_EQ(waitForTraceLines(path, 1), 1);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    expectExecutedExactly(*record, {"Auto"});
}

TEST_F(RecomputePhaseTraceTest, retriedObjectKeepsBothAttempts)
{
    setTrace(true, "");
    auto* again = addFeature("Again");
    auto* retrier = dynamic_cast<PhaseTraceRetryFeature*>(
        _document->addObject("App::PhaseTraceRetryFeature", "Retrier"));
    ASSERT_NE(again, nullptr);
    ASSERT_NE(retrier, nullptr);
    retrier->Source1.setValue(again);
    again->touch();
    retrier->touch();
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->truncated);
    ASSERT_GE(record->passes.size(), 2u);
    const auto* object = findObject(*record, "Again");
    ASSERT_NE(object, nullptr);
    const auto& execute = attemptsOf(*object, RecomputePhase::Execute);
    ASSERT_EQ(execute.size(), 2u);
    EXPECT_NE(execute[0].passId, execute[1].passId);
    EXPECT_EQ(execute[0].passId, record->passes[0].id);
    EXPECT_EQ(execute[1].passId, record->passes[1].id);
    EXPECT_LE(execute[0].startNs, execute[0].endNs);
    EXPECT_LE(execute[1].startNs, execute[1].endNs);
}

TEST_F(RecomputePhaseTraceTest, preferenceToggledDuringRecomputeStillCloses)
{
    const auto path = std::filesystem::temp_directory_path()
        / "freecad-phase-trace-toggle.jsonl";
    std::filesystem::remove(path);
    auto group = GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ASSERT_TRUE(group);
    auto saved = capturePreferences(*group);
    struct Restore
    {
        ParameterGrp* group {nullptr};
        PreferenceSnapshot saved;
        ~Restore()
        {
            if (group) {
                restorePreferences(*group, saved);
            }
        }
    } restore {group, std::move(saved)};

    setTrace(true, path.string());
    auto* feature = dynamic_cast<PhaseTraceToggleFeature*>(
        _document->addObject("App::PhaseTraceToggleFeature", "Toggle"));
    ASSERT_NE(feature, nullptr);
    feature->touch();
    _document->recompute();
    EXPECT_FALSE(recomputePhaseTraceEnabled());
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    EXPECT_EQ(waitForTraceLines(path, 1), 1);
    ASSERT_NE(addFeature("After"), nullptr);
    _document->recompute();
    EXPECT_EQ(waitForTraceLines(path, 1), 1);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, preferenceClearDuringActiveSessionStillCloses)
{
    const auto path = std::filesystem::temp_directory_path()
        / "freecad-phase-trace-clear.jsonl";
    std::filesystem::remove(path);
    auto group = GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Document");
    ASSERT_TRUE(group);
    auto saved = capturePreferences(*group);
    struct Restore
    {
        ParameterGrp* group {nullptr};
        PreferenceSnapshot saved;
        ~Restore()
        {
            if (group) {
                restorePreferences(*group, saved);
            }
        }
    } restore {group, std::move(saved)};

    setTrace(true, path.string());
    auto* feature = dynamic_cast<PhaseTraceClearDuringFeature*>(
        _document->addObject("App::PhaseTraceClearDuringFeature", "Clearing"));
    ASSERT_NE(feature, nullptr);
    feature->touch();
    _document->recompute();
    EXPECT_FALSE(recomputePhaseTraceEnabled());
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    EXPECT_EQ(waitForTraceLines(path, 1), 1);
    ASSERT_NE(addFeature("After"), nullptr);
    _document->recompute();
    EXPECT_EQ(waitForTraceLines(path, 1), 1);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
}

TEST_F(RecomputePhaseTraceTest, closeAllDocumentsWithAbandonedTraceReturns)
{
    setTrace(true, "");
    DocumentRecomputeRequest request;
    DocumentRecomputeFeatureRequest feature;
    feature.featureId = "Abandoned";
    feature.operationId = "generic-recompute:Abandoned";
    feature.intent.operationType = "App::FeatureTest";
    feature.provenance = "phase-trace-test";
    request.features.push_back(std::move(feature));
    ASSERT_NE(RecomputePhaseTrace::noteAdmitted(*_document, 1, request), nullptr);
    ASSERT_TRUE(_document->recomputePhaseTraceActive());
    expectPrompt([&] {
        GetApplication().closeAllDocuments();
    });
    EXPECT_EQ(GetApplication().getDocument(_name.c_str()), nullptr);
    _document = nullptr;
}

TEST_F(RecomputePhaseTraceTest, closeDocumentWhileTracedRecomputeIsPending)
{
    setTrace(true, "");
    ASSERT_NE(addFeature("Pending"), nullptr);
    auto handle = _document->recomputeAsync({_document->getObject("Pending")});
    ASSERT_NE(handle, nullptr);
    expectPrompt([&] {
        EXPECT_TRUE(GetApplication().closeDocument(_document->getName()));
    });
    _document = nullptr;
}

TEST_F(RecomputePhaseTraceTest, cancelFromAnotherThreadDoesNotBlockClose)
{
    setTrace(true, "");
    ASSERT_NE(addFeature("Cancel"), nullptr);
    auto handle = _document->recomputeAsync({_document->getObject("Cancel")});
    ASSERT_NE(handle, nullptr);
    expectPrompt([&] {
        std::thread canceller([&handle] {
            static_cast<void>(handle->cancel("phase trace off-owner cancel"));
        });
        canceller.join();
        EXPECT_TRUE(GetApplication().closeDocument(_document->getName()));
    });
    _document = nullptr;
}

TEST_F(RecomputePhaseTraceTest, writerDrainsWhenPreferenceTurnsOff)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-writer-off.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::pauseWriter(true);
    struct Resume
    {
        ~Resume()
        {
            RecomputePhaseTrace::testing::pauseWriter(false);
        }
    } resume;
    RecomputePhaseTrace::testing::enqueue(
        path.string(), std::make_shared<RecomputePhaseTraceRecord>());
    EXPECT_EQ(countTraceLines(path), 0);
    setTrace(false, path.string());
    EXPECT_EQ(countTraceLines(path), 1);
}

TEST_F(RecomputePhaseTraceTest, overflowIncrementsDroppedRecords)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-overflow.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::pauseWriter(true);
    struct Resume
    {
        ~Resume()
        {
            RecomputePhaseTrace::testing::pauseWriter(false);
        }
    } resume;
    constexpr unsigned queued = 70;
    for (unsigned index = 0; index < queued; ++index) {
        RecomputePhaseTrace::testing::enqueue(
            path.string(), std::make_shared<RecomputePhaseTraceRecord>());
    }
    setTrace(false, path.string());
    std::ifstream input(path);
    ASSERT_TRUE(input);
    int lines = 0;
    std::uint64_t dropped = 0;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        ++lines;
        dropped = droppedRecordsIn(line);
        EXPECT_EQ(dropped, queued - 64) << line;
    }
    EXPECT_EQ(lines, 64);
    EXPECT_EQ(dropped, queued - 64);
}

TEST_F(RecomputePhaseTraceTest, nestedCrossDocumentRecomputeKeepsSeparateRecords)
{
    setTrace(true, "");
    auto* other = GetApplication().newDocument("PhaseTraceOther", "phase trace other");
    ASSERT_NE(other, nullptr);
    auto* lane = other->executionLane();
    ASSERT_NE(lane, nullptr);
    auto* inner = dynamic_cast<FeatureTest*>(other->addObject("App::FeatureTest", "Inner"));
    auto* outer = addFeature("Outer");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(outer, nullptr);
    inner->touch();

    // Each document's lane thread is its owner, so a recompute started inside
    // the outer feature hops threads and never nests the binding. Run the
    // inner recompute on its owner while that same thread already holds the
    // outer session: that is the same-thread cross-document nest.
    DocumentRecomputeRequest outerRequest;
    DocumentRecomputeFeatureRequest outerFeature;
    outerFeature.featureId = "Outer";
    outerFeature.operationId = "generic-recompute:Outer";
    outerFeature.intent.operationType = "App::FeatureTest";
    outerFeature.provenance = "phase-trace-test";
    outerRequest.features.push_back(std::move(outerFeature));
    constexpr std::uint64_t outerPass = 7;
    ASSERT_NE(RecomputePhaseTrace::noteAdmitted(*_document, outerPass, outerRequest), nullptr);

    lane->dispatchToOwner([&] {
        RecomputePhaseTraceActivation outerFrame;
        RecomputePhaseTrace::bindCurrent(*_document);
        RecomputePhaseSpan outerSpan(RecomputePhase::Execute, outer);
        other->recompute();
    });
    RecomputePhaseTrace::onOwnerTerminal(*_document, outerPass);
    RecomputePhaseTrace::publishIfIdle(*_document);

    const auto outerRecord = _document->lastRecomputeTrace();
    const auto innerRecord = other->lastRecomputeTrace();
    ASSERT_NE(outerRecord, nullptr);
    ASSERT_NE(innerRecord, nullptr);
    EXPECT_EQ(outerRecord->document, _document->getName());
    EXPECT_EQ(innerRecord->document, other->getName());
    EXPECT_NE(outerRecord->recomputeId, innerRecord->recomputeId);
    EXPECT_EQ(findObject(*outerRecord, "Inner"), nullptr);
    EXPECT_EQ(findObject(*innerRecord, "Outer"), nullptr);
    expectExecutedExactly(*outerRecord, {"Outer"});
    expectExecutedExactly(*innerRecord, {"Inner"});
    GetApplication().closeDocument(other->getName());
}

TEST_F(RecomputePhaseTraceTest, moreThanFourAttemptsAreKept)
{
    setTrace(true, "");
    auto* feature = dynamic_cast<PhaseTraceManyAttemptsFeature*>(
        _document->addObject("App::PhaseTraceManyAttemptsFeature", "Many"));
    ASSERT_NE(feature, nullptr);
    feature->touch();
    _document->recompute();
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->truncated);
    const auto* object = findObject(*record, "Many");
    ASSERT_NE(object, nullptr);
    EXPECT_GE(attemptsOf(*object, RecomputePhase::Capture).size(), 5u);
}

TEST_F(RecomputePhaseTraceTest, nestedActivationRestoresTheOuterFrame)
{
    setTrace(true, "");
    const auto otherName = _name + "Nest";
    auto* other = GetApplication().newDocument(otherName.c_str(), "phase trace nest");
    ASSERT_NE(other, nullptr);
    auto* lane = other->executionLane();
    ASSERT_NE(lane, nullptr);
    auto* inner = dynamic_cast<FeatureTest*>(other->addObject("App::FeatureTest", "Inner"));
    auto* outer = addFeature("Outer");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(outer, nullptr);
    inner->touch();

    DocumentRecomputeRequest outerRequest;
    DocumentRecomputeFeatureRequest outerFeature;
    outerFeature.featureId = "Outer";
    outerFeature.operationId = "generic-recompute:Outer";
    outerFeature.intent.operationType = "App::FeatureTest";
    outerFeature.provenance = "phase-trace-test";
    outerRequest.features.push_back(std::move(outerFeature));
    constexpr std::uint64_t outerPass = 11;
    ASSERT_NE(RecomputePhaseTrace::noteAdmitted(*_document, outerPass, outerRequest), nullptr);

    lane->dispatchToOwner([&] {
        RecomputePhaseTraceActivation outerFrame;
        RecomputePhaseTrace::bindCurrent(*_document);
        other->recompute();
        RecomputePhaseSpan after(RecomputePhase::Capture, outer);
    });
    RecomputePhaseTrace::onOwnerTerminal(*_document, outerPass);
    RecomputePhaseTrace::publishIfIdle(*_document);

    const auto outerRecord = _document->lastRecomputeTrace();
    const auto innerRecord = other->lastRecomputeTrace();
    ASSERT_NE(outerRecord, nullptr);
    ASSERT_NE(innerRecord, nullptr);
    const auto* outerObject = findObject(*outerRecord, "Outer");
    ASSERT_NE(outerObject, nullptr);
    EXPECT_FALSE(attemptsOf(*outerObject, RecomputePhase::Capture).empty());
    EXPECT_EQ(findObject(*innerRecord, "Outer"), nullptr);
    GetApplication().closeDocument(other->getName());
}

TEST_F(RecomputePhaseTraceTest, concurrentPreferenceToggleWhileSealingJoinsOnce)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-toggle-stress.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    const auto droppedBefore = RecomputePhaseTrace::testing::droppedRecords();
    std::atomic<bool> run {true};
    auto toggle = [&run] {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        while (run.load(std::memory_order_relaxed)) {
            group->SetBool("RecomputePhaseTrace", false);
            group->SetBool("RecomputePhaseTrace", true);
        }
    };
    std::thread first(toggle);
    std::thread second(toggle);
    struct StopToggles
    {
        std::atomic<bool>& run;
        std::thread& first;
        std::thread& second;
        ~StopToggles()
        {
            run.store(false, std::memory_order_relaxed);
            if (first.joinable()) {
                first.join();
            }
            if (second.joinable()) {
                second.join();
            }
        }
    } stopToggles {run, first, second};
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    constexpr int total = 32;
    std::atomic<bool> done {false};
    std::thread watchdog([&done] {
        for (int tick = 0; tick < 200; ++tick) {
            if (done.load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load(std::memory_order_relaxed)) {
            std::_Exit(86);
        }
    });
    for (int index = 0; index < total; ++index) {
        auto record = std::make_shared<RecomputePhaseTraceRecord>();
        record->version = 1;
        record->document = "stress";
        RecomputePhaseTrace::testing::enqueue(path.string(), record);
    }
    run.store(false, std::memory_order_relaxed);
    first.join();
    second.join();
    setTrace(false, path.string());
    done.store(true, std::memory_order_relaxed);
    watchdog.join();
    const auto dropped = RecomputePhaseTrace::testing::droppedRecords() - droppedBefore;
    EXPECT_EQ(countTraceLines(path), total - static_cast<int>(dropped));
}

TEST_F(RecomputePhaseTraceTest, fifoDestinationIsRefused)
{
    const auto directory = std::filesystem::temp_directory_path() / "prc-m0-1-fifo";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directory(directory);
    const auto path = directory / "trace.fifo";
    ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0) << path;
    const auto errorsBefore = RecomputePhaseTrace::testing::ioErrors();
    expectPrompt([&] {
        setTrace(true, path.string());
        RecomputePhaseTrace::testing::enqueue(
            path.string(), std::make_shared<RecomputePhaseTraceRecord>());
        setTrace(false, path.string());
    });
    struct stat info {};
    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_TRUE(S_ISFIFO(info.st_mode));
    EXPECT_GT(RecomputePhaseTrace::testing::ioErrors(), errorsBefore);
    std::filesystem::remove_all(directory);
}

TEST_F(RecomputePhaseTraceTest, guiRecomputeWhileLaneBusyMatchesTracingOff)
{
    auto* feature = dynamic_cast<PhaseTraceDelayFeature*>(
        _document->addObject("App::PhaseTraceDelayFeature", "Delay"));
    ASSERT_NE(feature, nullptr);
    feature->ExecuteDelay.setValue(800);
    int nonce = 0;
    const auto occupyAndCall = [&](const bool trace) {
        setTrace(trace, "");
        feature->touch();
        PhaseTraceDelayFeature::entered.store(0, std::memory_order_release);
        const auto submitted = _document->executionHandle().trySubmit(
            delayRecomputeCommand(*_document, "Delay", ++nonce));
        if (!submitted.accepted()) {
            ADD_FAILURE() << submitted.diagnostic;
            return RecomputeOutcome {};
        }
        DocumentCommandHandle command(submitted.commandId,
                                      _document->executionHandle().identity());
        struct Finish
        {
            DocumentCommandHandle& command;
            ~Finish()
            {
                static_cast<void>(waitForCommand(command, std::chrono::seconds(10)));
            }
        } finish {command};
        if (!waitUntilDelayEntered(1, std::chrono::seconds(5))) {
            ADD_FAILURE() << "delay recompute did not enter execute";
            return RecomputeOutcome {};
        }
        const CompatibilityWaitOverride waits(std::chrono::seconds(3), std::chrono::seconds(3));
        const AdmissionTimeoutOverride admission(80);
        const GuiThreadIdentity gui;
        static_cast<void>(gui);
        return callRecompute(*_document);
    };

    const auto off = occupyAndCall(false);
    const auto pushesBefore = RecomputePhaseTrace::testing::holdPushes();
    const auto on = occupyAndCall(true);
    // recomputeAsync() uses the same GUI admission budget with the trace on or
    // off, so a busy lane throws DocumentWouldBlock either way. The hold is
    // already armed when that throw unwinds; restoring the pre-hold hop leaves
    // holdPushes unchanged.
    EXPECT_TRUE(off.wouldBlock) << off.message;
    EXPECT_TRUE(on.wouldBlock) << on.message;
    EXPECT_EQ(off.wouldBlock, on.wouldBlock);
    EXPECT_EQ(off.value, on.value) << off.message << " / " << on.message;
    EXPECT_EQ(off.message, on.message);
    EXPECT_NE(on.message.find("GUI admission timeout"), std::string::npos) << on.message;
    const auto delta = off.elapsed > on.elapsed ? off.elapsed - on.elapsed : on.elapsed - off.elapsed;
    EXPECT_LT(delta, std::chrono::milliseconds(400))
        << off.elapsed.count() << " ms vs " << on.elapsed.count() << " ms";
    EXPECT_GT(RecomputePhaseTrace::testing::holdPushes(), pushesBefore);
}

TEST_F(RecomputePhaseTraceTest, guiHoldSurvivesOwnerSpanAfterWaitTimeout)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-gui-hold.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::pauseWriter(false);
    auto* feature = dynamic_cast<PhaseTraceDelayFeature*>(
        _document->addObject("App::PhaseTraceDelayFeature", "Delay"));
    ASSERT_NE(feature, nullptr);
    feature->ExecuteDelay.setValue(800);
    feature->touch();
    PhaseTraceDelayFeature::entered.store(0, std::memory_order_release);
    const auto submitted = _document->executionHandle().trySubmit(
        delayRecomputeCommand(*_document, "Delay", 1));
    ASSERT_TRUE(submitted.accepted()) << submitted.diagnostic;
    DocumentCommandHandle command(submitted.commandId, _document->executionHandle().identity());
    struct FinishCommand
    {
        DocumentCommandHandle& command;
        ~FinishCommand()
        {
            static_cast<void>(waitForCommand(command, std::chrono::seconds(10)));
        }
    } finishCommand {command};
    ASSERT_TRUE(waitUntilDelayEntered(1, std::chrono::seconds(5)));
    const auto execBefore = feature->ExecCount.getValue();
    RecomputeOutcome outcome;
    {
        const CompatibilityWaitOverride waits(std::chrono::milliseconds(40),
                                              std::chrono::milliseconds(40));
        const AdmissionTimeoutOverride admission(80);
        const GuiThreadIdentity gui;
        static_cast<void>(gui);
        const auto pushesBefore = RecomputePhaseTrace::testing::holdPushes();
        outcome = callRecompute(*_document);
        EXPECT_GT(RecomputePhaseTrace::testing::holdPushes(), pushesBefore);
    }
    EXPECT_EQ(feature->ExecCount.getValue(), execBefore) << outcome.message;
    EXPECT_TRUE(PhaseTraceDelayFeature::entered.load(std::memory_order_acquire) >= 1);
    EXPECT_TRUE(RecomputePhaseTrace::hasLiveState());
    EXPECT_TRUE(_document->recomputePhaseTraceActive());
    EXPECT_EQ(_document->lastRecomputeTrace(), nullptr);
    EXPECT_EQ(countTraceLines(path), 0);
    ASSERT_TRUE(waitForCommand(command, std::chrono::seconds(10)));
    EXPECT_GT(feature->ExecCount.getValue(), execBefore);
    ASSERT_GE(waitForTraceLines(path, 1), 1);
    EXPECT_FALSE(_document->recomputePhaseTraceActive());
    EXPECT_FALSE(RecomputePhaseTrace::hasLiveState());
    const auto record = _document->lastRecomputeTrace();
    ASSERT_NE(record, nullptr);
    const auto* object = findObject(*record, "Delay");
    ASSERT_NE(object, nullptr);
    const auto& execute = attemptsOf(*object, RecomputePhase::Execute);
    ASSERT_EQ(execute.size(), 1u);
    EXPECT_GE(execute.front().endNs - execute.front().startNs, 500'000'000);
    EXPECT_EQ(countTraceLines(path), 1);
    std::filesystem::remove(path);
}

TEST_F(RecomputePhaseTraceTest, concurrentPreferenceToggleWhileRealRecomputesSealOnce)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-toggle-real.jsonl";
    std::filesystem::remove(path);
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::pauseWriter(false);
    const auto droppedBefore = RecomputePhaseTrace::testing::droppedRecords();
    const auto ioBefore = RecomputePhaseTrace::testing::ioErrors();
    const auto leftName = _name + "RealA";
    const auto rightName = _name + "RealB";
    auto* left = GetApplication().newDocument(leftName.c_str(), "phase trace real a");
    auto* right = GetApplication().newDocument(rightName.c_str(), "phase trace real b");
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    struct CloseExtra
    {
        std::string left;
        std::string right;
        ~CloseExtra()
        {
            if (GetApplication().getDocument(left.c_str())) {
                GetApplication().closeDocument(left.c_str());
            }
            if (GetApplication().getDocument(right.c_str())) {
                GetApplication().closeDocument(right.c_str());
            }
        }
    } closeExtra {leftName, rightName};
    ASSERT_NE(left->addObject("App::FeatureTest", "Left"), nullptr);
    ASSERT_NE(right->addObject("App::FeatureTest", "Right"), nullptr);

    std::mutex errorMutex;
    std::string error;
    std::atomic<bool> run {true};
    auto toggle = [&run, &errorMutex, &error] {
        auto group = GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document");
        try {
            while (run.load(std::memory_order_relaxed)) {
                group->SetBool("RecomputePhaseTrace", true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                group->SetBool("RecomputePhaseTrace", false);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        catch (const Base::Exception& caught) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = caught.what();
            }
        }
        catch (const std::exception& caught) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = caught.what();
            }
        }
        catch (...) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = "unknown exception on the preference thread";
            }
        }
    };
    std::thread toggler(toggle);
    struct StopToggle
    {
        std::atomic<bool>& run;
        std::thread& toggler;
        ~StopToggle()
        {
            run.store(false, std::memory_order_relaxed);
            if (toggler.joinable()) {
                toggler.join();
            }
        }
    } stopToggle {run, toggler};

    auto recomputeDocument = [&errorMutex, &error](Document* document) {
        try {
            for (int index = 0; index < 8; ++index) {
                // Two documents can overlap in the process-wide presentation
                // gate. Retry that refusal; it is not a trace failure.
                bool admitted = false;
                for (int attempt = 0; attempt < 40 && !admitted; ++attempt) {
                    try {
                        static_cast<void>(document->recompute({}, true));
                        admitted = true;
                    }
                    catch (const Base::RuntimeError& caught) {
                        const std::string message = caught.what();
                        if (message.find("atomic presentation") == std::string::npos) {
                            throw;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                if (!admitted) {
                    throw Base::RuntimeError(
                        "recompute stayed inside an atomic presentation callback");
                }
            }
        }
        catch (const Base::Exception& caught) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = caught.what();
            }
        }
        catch (const std::exception& caught) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = caught.what();
            }
        }
        catch (...) {
            std::lock_guard lock(errorMutex);
            if (error.empty()) {
                error = "unknown exception on a recompute thread";
            }
        }
    };
    std::atomic<bool> done {false};
    std::thread watchdog([&done] {
        for (int tick = 0; tick < 300; ++tick) {
            if (done.load(std::memory_order_relaxed)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done.load(std::memory_order_relaxed)) {
            std::_Exit(86);
        }
    });
    std::thread leftThread(recomputeDocument, left);
    std::thread rightThread(recomputeDocument, right);
    leftThread.join();
    rightThread.join();
    run.store(false, std::memory_order_relaxed);
    toggler.join();
    setTrace(false, path.string());
    setTrace(false, path.string());
    done.store(true, std::memory_order_relaxed);
    watchdog.join();
    EXPECT_TRUE(error.empty()) << error;

    const auto lines = readTraceLines(path);
    EXPECT_GT(lines.size(), 0u);
    std::set<std::string> keys;
    std::uint64_t previousDropped = 0;
    std::uint64_t previousIo = 0;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const auto& line = lines[index];
        EXPECT_NE(line.find("\"v\":1"), std::string::npos) << line;
        const auto dropped = droppedRecordsIn(line);
        const auto io = ioErrorsIn(line);
        if (index == 0) {
            EXPECT_GE(dropped, droppedBefore);
            EXPECT_GE(io, ioBefore);
        }
        else {
            EXPECT_GE(dropped, previousDropped);
            EXPECT_GE(io, previousIo);
        }
        previousDropped = dropped;
        previousIo = io;
        keys.insert(jsonString(line, "document") + ":"
                    + std::to_string(jsonUint(line, "recompute_id")));
    }
    EXPECT_EQ(keys.size(), lines.size());
    if (!lines.empty()) {
        EXPECT_GE(RecomputePhaseTrace::testing::droppedRecords(), previousDropped);
        EXPECT_GE(RecomputePhaseTrace::testing::ioErrors(), previousIo);
    }
    EXPECT_FALSE(RecomputePhaseTrace::hasLiveState());
    std::filesystem::remove(path);
}

TEST_F(RecomputePhaseTraceTest, enqueueAfterShutdownDropsAndCounts)
{
    const auto path = std::filesystem::temp_directory_path() / "prc-m0-1-writer-shutdown.jsonl";
    std::filesystem::remove(path);
    struct RestoreCache
    {
        ~RestoreCache()
        {
            RecomputePhaseTrace::installPreferenceCache(GetApplication().GetUserParameter());
        }
    } restoreCache;
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::pauseWriter(true);
    RecomputePhaseTrace::testing::enqueue(
        path.string(), std::make_shared<RecomputePhaseTraceRecord>());
    EXPECT_EQ(countTraceLines(path), 0);
    const auto droppedBefore = RecomputePhaseTrace::testing::droppedRecords();
    RecomputePhaseTrace::removePreferenceCache();
    EXPECT_EQ(countTraceLines(path), 1);
    RecomputePhaseTrace::testing::enqueue(
        path.string(), std::make_shared<RecomputePhaseTraceRecord>());
    EXPECT_EQ(RecomputePhaseTrace::testing::droppedRecords(), droppedBefore + 1);
    EXPECT_EQ(countTraceLines(path), 1);
    RecomputePhaseTrace::installPreferenceCache(GetApplication().GetUserParameter());
    setTrace(true, path.string());
    RecomputePhaseTrace::testing::enqueue(
        path.string(), std::make_shared<RecomputePhaseTraceRecord>());
    EXPECT_EQ(RecomputePhaseTrace::testing::droppedRecords(), droppedBefore + 2);
    EXPECT_EQ(countTraceLines(path), 1);
}
