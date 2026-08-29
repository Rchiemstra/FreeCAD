// SPDX-License-Identifier: LGPL-2.1-or-later

#ifdef _WIN32
# ifndef NOMINMAX
#  define NOMINMAX
# endif
// App_tests_run defines DATADIR as a string literal. The Windows SDK uses
// DATADIR as an enum typedef in objidl.h; this translation unit does not use it.
# undef DATADIR
# include <windows.h>
#endif

#include <gtest/gtest.h>

#include <App/GeometryArchive.h>
#include <App/GeometryJobManager.h>
#include <App/GeometryProcessBackend.h>
#include <App/GeometryWorkerMain.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <thread>

#ifndef _WIN32
# include <cerrno>
# include <csignal>
# include <sys/types.h>
# include <unistd.h>
#endif

using namespace App;
using namespace std::chrono_literals;

namespace App::Internal
{

class GeometryProcessBackendTestAccess
{
public:
    static void start(GeometryJobManager& manager,
                      const std::filesystem::path& executable,
                      const std::filesystem::path& root,
                      const std::chrono::milliseconds startupHeartbeat = 5s,
                      const std::chrono::milliseconds heartbeat = 2s)
    {
        GeometryProcessBackendOptions options;
        options.executable = executable;
        options.workspaceRoot = root;
        options.pollInterval = 5ms;
        options.startupHeartbeatTimeout = startupHeartbeat;
        options.heartbeatTimeout = heartbeat;
        options.terminationGrace = 250ms;
        options.completedArtifactRetention = 1h;
        manager.startProcessBackend(std::move(options));
    }
};

}  // namespace App::Internal

namespace
{

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        static std::atomic<std::uint64_t> sequence {1};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
            / ("freecad-process-test-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

GeometryJobRequest request(const std::string& operation,
                           const std::chrono::milliseconds lifetime = 10s)
{
    GeometryJobRequest value;
    value.operationType = operation;
    value.coalescing = GeometryJobCoalescing::None;
    value.inputDigest.assign(64, 'a');
    value.deadline = std::chrono::steady_clock::now() + lifetime;
    return value;
}

GeometryArchive archive()
{
    GeometryArchive value;
    value.sections = {{"payload", {1, 3, 3, 7}}};
    return value;
}

bool terminal(const GeometryJobState state) noexcept
{
    return state == GeometryJobState::Completed || state == GeometryJobState::Cancelled
        || state == GeometryJobState::DeadlineExceeded
        || state == GeometryJobState::WorkerCrashed
        || state == GeometryJobState::WorkerOutOfMemory || state == GeometryJobState::Failed;
}

std::optional<GeometryJobStatus> waitForTerminal(GeometryJobManager& manager,
                                                 const GeometryJobId id,
                                                 const std::chrono::milliseconds timeout = 30s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = manager.status(id);
        if (status && terminal(status->state)) {
            return status;
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

bool waitForProgress(GeometryJobManager& manager,
                     const GeometryJobId id,
                     const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = manager.status(id);
        if (status && status->progress.fraction > 0.0) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

bool waitForNoJobDirectories(const std::filesystem::path& root,
                             const std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool found = false;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            if (iterator->is_directory(error)
                && iterator->path().filename().string().starts_with("job-")) {
                found = true;
                break;
            }
        }
        if (!found) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

std::optional<unsigned long> findChildPid(const std::filesystem::path& root,
                                          const std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
             !error && iterator != end;
             iterator.increment(error)) {
            if (iterator->is_regular_file(error)
                && iterator->path().filename() == "result.fcg.child-pid") {
                std::ifstream pid(iterator->path());
                unsigned long value = 0;
                if (pid >> value && value != 0) {
                    return value;
                }
            }
        }
        std::this_thread::sleep_for(5ms);
    }
    return std::nullopt;
}

bool processIsAlive(const unsigned long processId)
{
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return false;
    }
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
#endif
}

bool waitForDead(const unsigned long processId,
                 const std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processIsAlive(processId)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return !processIsAlive(processId);
}

std::uint64_t currentProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::filesystem::path installedWorkerPath()
{
    const char* overridePath = std::getenv("FREECAD_GEOMETRY_TEST_INSTALLED_WORKER");
    return overridePath && *overridePath ? std::filesystem::path(overridePath)
                                         : std::filesystem::path(GEOMETRY_INSTALLED_WORKER_PATH);
}

}  // namespace

TEST(GeometryProcessBackendTest, installedFreeCADCmdRoundTripsAValidatedProbe)
{
    TemporaryDirectory directory;
    const std::filesystem::path worker = installedWorkerPath();
    ASSERT_TRUE(std::filesystem::is_regular_file(worker)) << worker;
    const auto unicodeRoot = directory.path / std::filesystem::u8path("géométrie-路径");
    std::filesystem::create_directories(unicodeRoot);
    GeometryJobManager manager(1, 4);
    Internal::GeometryProcessBackendTestAccess::start(manager, worker, unicodeRoot);

    const auto id = manager.submit(request("FreeCAD.Internal.GeometryProbe"), archive());
    EXPECT_TRUE(waitForProgress(manager, id, 250ms));
    const auto status = waitForTerminal(manager, id);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(status->state, GeometryJobState::Completed) << status->diagnostic;
    auto result = manager.takeResult(id);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->state, GeometryJobState::Completed);
    EXPECT_FALSE(result->resultArtifact.empty());
    EXPECT_EQ(result->resultDigest.size(), 64U);

    GeometryArchiveExpectation expected;
    expected.kind = GeometryArchiveKind::Result;
    expected.jobId = id;
    expected.operationType = "FreeCAD.Internal.GeometryProbe";
    expected.buildFingerprint = Internal::geometryWorkerBuildFingerprint(worker);
    expected.inputDigest.assign(64, 'a');
    const auto decoded = GeometryArchiveCodec::readValidated(result->resultArtifact, expected);
    ASSERT_TRUE(decoded.success()) << decoded.error.code << ": " << decoded.error.message;
    ASSERT_EQ(decoded.archive->sections.size(), 1U);
    EXPECT_EQ(decoded.archive->sections.front().bytes,
              (std::vector<std::uint8_t> {1, 3, 3, 7}));
    EXPECT_TRUE(waitForNoJobDirectories(unicodeRoot));
}

TEST(GeometryProcessBackendTest, janitorRemovesOnlyDeadOwnedWorkspaces)
{
    TemporaryDirectory directory;
    const auto stale = directory.path / "job-stale";
    const auto live = directory.path / "job-live";
    std::filesystem::create_directories(stale);
    std::filesystem::create_directories(live);
    {
        std::ofstream owner(stale / ".freecad-geometry-owner");
        owner << std::numeric_limits<unsigned long>::max() << '\n';
    }
    {
        std::ofstream owner(live / ".freecad-geometry-owner");
        owner << currentProcessId() << '\n';
    }
    GeometryJobManager manager(1, 1);
    Internal::GeometryProcessBackendTestAccess::start(
        manager, GEOMETRY_PROCESS_TEST_WORKER_PATH, directory.path);
    EXPECT_FALSE(std::filesystem::exists(stale));
    EXPECT_TRUE(std::filesystem::is_directory(live));
}

TEST(GeometryProcessBackendTest, classifiesCrashOomTimeoutAndLostHeartbeat)
{
    TemporaryDirectory directory;
    GeometryJobManager manager(1, 8);
    Internal::GeometryProcessBackendTestAccess::start(
        manager, GEOMETRY_PROCESS_TEST_WORKER_PATH, directory.path, 150ms, 2s);

    const auto crash = manager.submit(request("Test.Crash"), archive());
    auto state = waitForTerminal(manager, crash);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, GeometryJobState::WorkerCrashed);
    ASSERT_TRUE(manager.takeResult(crash).has_value());

    const auto oom = manager.submit(request("Test.OutOfMemory"), archive());
    state = waitForTerminal(manager, oom);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, GeometryJobState::WorkerOutOfMemory);
    ASSERT_TRUE(manager.takeResult(oom).has_value());

    const auto timeout = manager.submit(request("Test.Hang", 150ms), archive());
    state = waitForTerminal(manager, timeout);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, GeometryJobState::DeadlineExceeded);
    ASSERT_TRUE(manager.takeResult(timeout).has_value());

    const auto noHeartbeat = manager.submit(request("Test.NoHeartbeat", 5s), archive());
    state = waitForTerminal(manager, noHeartbeat);
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->state, GeometryJobState::WorkerCrashed);
    EXPECT_NE(state->diagnostic.find("heartbeat"), std::string::npos);
    ASSERT_TRUE(manager.takeResult(noHeartbeat).has_value());
    EXPECT_TRUE(waitForNoJobDirectories(directory.path));
}

TEST(GeometryProcessBackendTest, cancellationTerminatesTheCompleteWorkerTree)
{
    TemporaryDirectory directory;
    GeometryJobManager manager(1, 2);
    Internal::GeometryProcessBackendTestAccess::start(
        manager, GEOMETRY_PROCESS_TEST_WORKER_PATH, directory.path);
    const auto id = manager.submit(request("Test.ChildHang"), archive());
    const auto child = findChildPid(directory.path);
    ASSERT_TRUE(child.has_value());
    ASSERT_TRUE(processIsAlive(*child));
    ASSERT_TRUE(manager.cancel(id));
    const auto status = waitForTerminal(manager, id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, GeometryJobState::Cancelled);
    ASSERT_TRUE(manager.takeResult(id).has_value());
    EXPECT_TRUE(waitForDead(*child));
    EXPECT_TRUE(waitForNoJobDirectories(directory.path));
}

TEST(GeometryProcessBackendTest, cancellationOffersABoundedCooperativeExitFirst)
{
    TemporaryDirectory directory;
    GeometryJobManager manager(1, 2);
    Internal::GeometryProcessBackendTestAccess::start(
        manager, GEOMETRY_PROCESS_TEST_WORKER_PATH, directory.path);
    const auto id = manager.submit(request("Test.CooperativeCancel"), archive());
    ASSERT_TRUE(waitForProgress(manager, id, 1s));
    ASSERT_TRUE(manager.cancel(id));
    const auto status = waitForTerminal(manager, id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, GeometryJobState::Cancelled);
    EXPECT_NE(status->diagnostic.find("cooperatively"), std::string::npos);
    ASSERT_TRUE(manager.takeResult(id).has_value());
    EXPECT_TRUE(waitForNoJobDirectories(directory.path));
}

TEST(GeometryProcessBackendTest, missingTrustedWorkerFailsClosedWithoutLeak)
{
    TemporaryDirectory directory;
    GeometryJobManager manager(1, 1);
    Internal::GeometryProcessBackendTestAccess::start(
        manager, directory.path / "missing-FreeCADCmd", directory.path);
    const auto id = manager.submit(request("FreeCAD.Internal.GeometryProbe"), archive());
    const auto status = waitForTerminal(manager, id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, GeometryJobState::Failed);
    EXPECT_NE(status->diagnostic.find("missing"), std::string::npos);
    EXPECT_TRUE(waitForNoJobDirectories(directory.path));
}
