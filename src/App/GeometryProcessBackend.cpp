// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryProcessBackend.h"

#include "GeometryArchive.h"
#include "GeometryJobManager.h"
#include "GeometryWorkerMain.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
# include <windows.h>
#else
# include <cerrno>
# include <csignal>
# include <sys/types.h>
# include <unistd.h>
#endif

using namespace App;
using namespace App::Internal;
using namespace std::chrono_literals;

namespace
{

constexpr auto WorkerProtocolEnvironment = "FREECAD_GEOMETRY_WORKER_PROTOCOL";
constexpr auto WorkerProtocolValue = "FCG/1";
constexpr auto RequestEnvironment = "FREECAD_GEOMETRY_REQUEST";
constexpr auto ResultEnvironment = "FREECAD_GEOMETRY_RESULT";
constexpr auto HeartbeatEnvironment = "FREECAD_GEOMETRY_HEARTBEAT";
constexpr auto StartGateEnvironment = "FREECAD_GEOMETRY_START_GATE";
constexpr auto CancelEnvironment = "FREECAD_GEOMETRY_CANCEL";
constexpr auto JobIdEnvironment = "FREECAD_GEOMETRY_JOB_ID";
constexpr auto OperationEnvironment = "FREECAD_GEOMETRY_OPERATION";
constexpr auto BuildEnvironment = "FREECAD_GEOMETRY_BUILD";
constexpr auto InputDigestEnvironment = "FREECAD_GEOMETRY_INPUT_DIGEST";
constexpr auto OwnerMarker = ".freecad-geometry-owner";
constexpr auto ResultPrefix = "result-";
constexpr int WorkerOutOfMemoryExitCode = 86;
constexpr std::size_t DiagnosticLimit = 8 * 1024;

std::filesystem::path fromQStringPath(const QString& value)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    const QByteArray encoded = value.toUtf8();
    return std::filesystem::path(encoded.constData());
#endif
}

QString toQStringPath(const std::filesystem::path& value)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromUtf8(value.native());
#endif
}

std::filesystem::path installedWorkerExecutable()
{
    auto executable = Internal::currentExecutablePath().parent_path();
#ifdef Q_OS_WIN
    executable /= "FreeCADCmd.exe";
#else
    executable /= "FreeCADCmd";
#endif
    return executable;
}

std::filesystem::path defaultWorkspaceRoot()
{
    QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cache.isEmpty()) {
        cache = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    return fromQStringPath(cache) / "geometry-jobs";
}

std::uint64_t currentProcessId() noexcept
{
#ifdef Q_OS_WIN
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

bool processIsAlive(const std::uint64_t processId) noexcept
{
    if (processId == 0 || processId > std::numeric_limits<unsigned long>::max()) {
        return false;
    }
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(processId));
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

std::optional<std::uint64_t> readOwner(const std::filesystem::path& directory)
{
    std::ifstream stream(directory / OwnerMarker);
    std::string value;
    if (!stream || !(stream >> value)) {
        return std::nullopt;
    }
    std::uint64_t processId = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), processId);
    if (error != std::errc {} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return processId;
}

void removeAll(const std::filesystem::path& path) noexcept
{
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

void runStartupJanitor(const GeometryProcessBackendOptions& options)
{
    std::error_code error;
    std::filesystem::create_directories(options.workspaceRoot, error);
    if (error || !std::filesystem::is_directory(options.workspaceRoot, error)) {
        return;
    }
#ifndef Q_OS_WIN
    std::filesystem::permissions(
        options.workspaceRoot,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        error);
    error.clear();
#endif
    const auto now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::directory_iterator iterator(options.workspaceRoot, error), end;
         !error && iterator != end;
         iterator.increment(error)) {
        const auto& entry = *iterator;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(error) && name.starts_with("job-")) {
            const auto owner = readOwner(entry.path());
            if (!owner || !processIsAlive(*owner)) {
                removeAll(entry.path());
            }
        }
        else if (entry.is_regular_file(error) && name.starts_with(ResultPrefix)) {
            const auto written = entry.last_write_time(error);
            if (!error && now - written > options.completedArtifactRetention) {
                std::filesystem::remove(entry.path(), error);
            }
        }
        error.clear();
    }
}

std::filesystem::path createWorkspace(const GeometryProcessBackendOptions& options,
                                      const GeometryJobId id)
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto nonce = QRandomGenerator::system()->generate64();
        const auto name = "job-" + std::to_string(currentProcessId()) + "-"
            + std::to_string(id) + "-" + QString::number(nonce, 16).toStdString();
        auto path = options.workspaceRoot / name;
        std::error_code error;
        if (!std::filesystem::create_directory(path, error)) {
            if (!error) {
                continue;
            }
            throw std::runtime_error("cannot create isolated geometry workspace");
        }
#ifndef Q_OS_WIN
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace,
                                     error);
        if (error) {
            removeAll(path);
            throw std::runtime_error("cannot secure isolated geometry workspace");
        }
#endif
        std::ofstream owner(path / OwnerMarker, std::ios::out | std::ios::trunc);
        owner << currentProcessId() << '\n';
        owner.close();
        if (!owner) {
            removeAll(path);
            throw std::runtime_error("cannot publish geometry workspace owner marker");
        }
        return path;
    }
    throw std::runtime_error("cannot allocate unique isolated geometry workspace");
}

class WorkspaceCleanup
{
public:
    explicit WorkspaceCleanup(std::filesystem::path path)
        : workspace(std::move(path))
    {}
    ~WorkspaceCleanup()
    {
        removeAll(workspace);
    }

private:
    std::filesystem::path workspace;
};

std::int64_t deadlineEpochMilliseconds(
    const std::chrono::steady_clock::time_point deadline)
{
    const auto nowSteady = std::chrono::steady_clock::now();
    const auto remaining = deadline > nowSteady ? deadline - nowSteady
                                                : std::chrono::steady_clock::duration::zero();
    const auto deadlineSystem = std::chrono::system_clock::now()
        + std::chrono::duration_cast<std::chrono::system_clock::duration>(remaining);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               deadlineSystem.time_since_epoch())
        .count();
}

std::string boundedDiagnostic(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::string result(DiagnosticLimit, '\0');
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    result.resize(static_cast<std::size_t>(stream.gcount()));
    return result;
}

#ifdef Q_OS_WIN
class WindowsJob
{
public:
    ~WindowsJob()
    {
        if (handle) {
            CloseHandle(handle);
        }
    }

    bool assign(const qint64 processId)
    {
        handle = CreateJobObjectW(nullptr, nullptr);
        if (!handle) {
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle,
                                     JobObjectExtendedLimitInformation,
                                     &limits,
                                     sizeof(limits))) {
            return false;
        }
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE
                                         | PROCESS_QUERY_LIMITED_INFORMATION,
                                     FALSE,
                                     static_cast<DWORD>(processId));
        if (!process) {
            return false;
        }
        const bool assigned = AssignProcessToJobObject(handle, process) != FALSE;
        CloseHandle(process);
        return assigned;
    }

    void terminate() noexcept
    {
        if (handle) {
            TerminateJobObject(handle, 70);
        }
    }

private:
    HANDLE handle {nullptr};
};
#endif

void terminateProcessTree(QProcess& process,
#ifdef Q_OS_WIN
                          WindowsJob& job,
#endif
                          const std::chrono::milliseconds grace)
{
    if (process.state() == QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_WIN
    job.terminate();
#else
    const auto processId = static_cast<pid_t>(process.processId());
    if (processId > 0) {
        ::kill(-processId, SIGTERM);
    }
#endif
    if (!process.waitForFinished(static_cast<int>(grace.count()))) {
#ifndef Q_OS_WIN
        const auto processId = static_cast<pid_t>(process.processId());
        if (processId > 0) {
            ::kill(-processId, SIGKILL);
        }
#endif
        process.kill();
        process.waitForFinished(static_cast<int>(grace.count()));
    }
}

GeometryJobState classifyExit(const QProcess& process)
{
    const auto exitCode = process.exitCode();
    if (exitCode == WorkerOutOfMemoryExitCode) {
        return GeometryJobState::WorkerOutOfMemory;
    }
#ifdef Q_OS_WIN
    const auto status = static_cast<unsigned long>(exitCode);
    if (status == 0xc0000017UL || status == 0xc000009aUL) {
        return GeometryJobState::WorkerOutOfMemory;
    }
#endif
    if (process.exitStatus() == QProcess::CrashExit) {
        return GeometryJobState::WorkerCrashed;
    }
    return GeometryJobState::Failed;
}

bool isManagerTerminal(const GeometryJobState state) noexcept
{
    return state == GeometryJobState::Completed || state == GeometryJobState::Cancelled
        || state == GeometryJobState::DeadlineExceeded
        || state == GeometryJobState::WorkerCrashed
        || state == GeometryJobState::WorkerOutOfMemory || state == GeometryJobState::Failed;
}

}  // namespace

class GeometryJobProcessBackend::Impl
{
public:
    struct Task
    {
        std::atomic_bool done {false};
        std::jthread thread;
    };

    Impl(GeometryJobManager& managerValue, GeometryProcessBackendOptions optionsValue)
        : manager(managerValue)
        , options(std::move(optionsValue))
    {
        if (options.executable.empty()) {
            options.executable = installedWorkerExecutable();
        }
        if (options.workspaceRoot.empty()) {
            options.workspaceRoot = defaultWorkspaceRoot();
        }
        if (options.pollInterval <= 0ms || options.startupHeartbeatTimeout <= 0ms
            || options.heartbeatTimeout <= 0ms || options.terminationGrace <= 0ms
            || options.completedArtifactRetention <= std::chrono::hours::zero()) {
            throw std::invalid_argument("geometry process backend time bounds must be positive");
        }
        runStartupJanitor(options);
        dispatcher = std::jthread([this](const std::stop_token stopToken) {
            dispatch(stopToken);
        });
    }

    ~Impl()
    {
        dispatcher.request_stop();
        if (dispatcher.joinable()) {
            dispatcher.join();
        }
        for (auto& task : tasks) {
            task->thread.request_stop();
        }
        for (auto& task : tasks) {
            if (task->thread.joinable()) {
                task->thread.join();
            }
        }
    }

private:
    void publishTerminal(GeometryJobResult result) noexcept
    {
        static_cast<void>(manager.finish(std::move(result)));
    }

    void publishProgress(const GeometryJobId id, GeometryJobProgress progress) noexcept
    {
        static_cast<void>(manager.reportProgress(id, std::move(progress)));
    }

    void dispatch(const std::stop_token stopToken)
    {
        while (!stopToken.stop_requested()) {
            tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const auto& task) {
                            return task->done.load(std::memory_order_acquire);
                        }),
                        tasks.end());
            while (tasks.size() < manager.activeCapacity()) {
                GeometryArchive input;
                auto next = manager.takeNext(input);
                if (!next) {
                    break;
                }
                auto task = std::make_unique<Task>();
                Task* taskPointer = task.get();
                task->thread = std::jthread(
                    [this,
                     dispatchPacket = std::move(*next),
                     archive = std::move(input),
                     taskPointer](const std::stop_token workerStop) mutable {
                        run(std::move(dispatchPacket), std::move(archive), workerStop);
                        taskPointer->done.store(true, std::memory_order_release);
                    });
                tasks.push_back(std::move(task));
            }
            std::this_thread::sleep_for(options.pollInterval);
        }
    }

    void run(GeometryJobDispatch dispatchPacket,
             GeometryArchive input,
             const std::stop_token stopToken) noexcept
    {
        try {
            runChecked(std::move(dispatchPacket), std::move(input), stopToken);
        }
        catch (const std::bad_alloc&) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::WorkerOutOfMemory,
                                              {},
                                              {},
                                              "geometry process backend exhausted memory"});
        }
        catch (const std::exception& exception) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              exception.what()});
        }
        catch (...) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              "unknown geometry process backend failure"});
        }
    }

    void runChecked(GeometryJobDispatch dispatchPacket,
                    GeometryArchive input,
                    const std::stop_token stopToken)
    {
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(options.executable, filesystemError)) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              "trusted FreeCADCmd worker is missing"});
            return;
        }
        const std::string buildFingerprint =
            Internal::geometryWorkerBuildFingerprint(options.executable);
        if (buildFingerprint.empty()) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              "cannot fingerprint trusted FreeCADCmd worker"});
            return;
        }
        const auto workspace = createWorkspace(options, dispatchPacket.id);
        WorkspaceCleanup cleanup(workspace);
        const auto requestPath = workspace / "request.fcg";
        const auto workerResultPath = workspace / "result.fcg";
        const auto heartbeatPath = workspace / "heartbeat";
        const auto startGatePath = workspace / "start";
        const auto cancelPath = workspace / "cancel";
        const auto stdoutPath = workspace / "stdout.log";
        const auto stderrPath = workspace / "stderr.log";

        input.metadata.protocolVersion = GeometryArchiveProtocolVersion;
        input.metadata.kind = GeometryArchiveKind::Request;
        input.metadata.jobId = dispatchPacket.id;
        input.metadata.policy = PreparationPolicy::IsolatedProcess;
        input.metadata.deadlineEpochMilliseconds = deadlineEpochMilliseconds(
            dispatchPacket.request.deadline);
        input.metadata.operationType = dispatchPacket.request.operationType;
        input.metadata.buildFingerprint = buildFingerprint;
        input.metadata.inputDigest = dispatchPacket.request.inputDigest;
        input.archiveDigest.clear();
        const auto requestWrite = GeometryArchiveCodec::writeAtomic(requestPath, input);
        if (!requestWrite.success) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              requestWrite.error.code + ": "
                                                  + requestWrite.error.message});
            return;
        }

        QProcess process;
        process.setProgram(toQStringPath(options.executable));
        process.setWorkingDirectory(toQStringPath(workspace));
        process.setStandardOutputFile(toQStringPath(stdoutPath),
                                      QIODeviceBase::Truncate);
        process.setStandardErrorFile(toQStringPath(stderrPath),
                                     QIODeviceBase::Truncate);
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(WorkerProtocolEnvironment, WorkerProtocolValue);
        environment.insert(RequestEnvironment, toQStringPath(requestPath));
        environment.insert(ResultEnvironment,
                           toQStringPath(workerResultPath));
        environment.insert(HeartbeatEnvironment,
                           toQStringPath(heartbeatPath));
        environment.insert(StartGateEnvironment,
                           toQStringPath(startGatePath));
        environment.insert(CancelEnvironment,
                           toQStringPath(cancelPath));
        environment.insert(JobIdEnvironment,
                           QString::number(static_cast<qulonglong>(dispatchPacket.id)));
        environment.insert(OperationEnvironment,
                           QString::fromStdString(dispatchPacket.request.operationType));
        environment.insert(BuildEnvironment, QString::fromStdString(buildFingerprint));
        environment.insert(InputDigestEnvironment,
                           QString::fromStdString(dispatchPacket.request.inputDigest));
        process.setProcessEnvironment(environment);
#ifndef Q_OS_WIN
        process.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession
                                         | QProcess::UnixProcessFlag::CloseFileDescriptors);
#endif
        publishProgress(dispatchPacket.id, {0.02, "worker-starting"});
        process.start(QIODeviceBase::ReadOnly);
        if (!process.waitForStarted(static_cast<int>(
                std::min(options.startupHeartbeatTimeout,
                         std::chrono::milliseconds(30s))
                    .count()))) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              "trusted FreeCADCmd worker failed to start"});
            return;
        }

#ifdef Q_OS_WIN
        WindowsJob windowsJob;
        if (!windowsJob.assign(process.processId())) {
            terminateProcessTree(process, windowsJob, options.terminationGrace);
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              "cannot assign FreeCADCmd worker to a Job Object"});
            return;
        }
#endif
        {
            std::ofstream start(startGatePath, std::ios::out | std::ios::trunc);
            start << "assigned\n";
            start.close();
            if (!start) {
                terminateProcessTree(process,
#ifdef Q_OS_WIN
                                     windowsJob,
#endif
                                     options.terminationGrace);
                publishTerminal(GeometryJobResult {dispatchPacket.id,
                                                  GeometryJobState::Failed,
                                                  {},
                                                  {},
                                                  "cannot release FreeCADCmd worker start gate"});
                return;
            }
        }

        const auto processStarted = std::chrono::steady_clock::now();
        auto lastHeartbeatSeen = processStarted;
        std::filesystem::file_time_type heartbeatStamp {};
        bool heartbeatObserved = false;
        bool forcedTermination = false;
        bool cancellationSignalled = false;
        auto cancellationSignalledAt = std::chrono::steady_clock::time_point {};
        while (process.state() != QProcess::NotRunning) {
            process.waitForFinished(static_cast<int>(options.pollInterval.count()));
            auto status = manager.status(dispatchPacket.id);
            if (status && status->cancellationRequested && !cancellationSignalled) {
                std::ofstream cancel(cancelPath, std::ios::out | std::ios::trunc);
                cancel << "cancel\n";
                cancel.close();
                cancellationSignalled = true;
                cancellationSignalledAt = std::chrono::steady_clock::now();
            }
            if (cancellationSignalled && process.state() != QProcess::NotRunning
                && std::chrono::steady_clock::now() - cancellationSignalledAt
                    >= options.terminationGrace) {
                forcedTermination = true;
                terminateProcessTree(process,
#ifdef Q_OS_WIN
                                     windowsJob,
#endif
                                     options.terminationGrace);
                publishTerminal(GeometryJobResult {dispatchPacket.id,
                                                  GeometryJobState::Cancelled,
                                                  {},
                                                  {},
                                                  "isolated geometry worker required forced cancellation"});
                break;
            }
            if (stopToken.stop_requested() || !status
                || (isManagerTerminal(status->state)
                    && status->state != GeometryJobState::Cancelled)) {
                forcedTermination = true;
                terminateProcessTree(process,
#ifdef Q_OS_WIN
                                     windowsJob,
#endif
                                     options.terminationGrace);
                break;
            }
            filesystemError.clear();
            const auto stamp = std::filesystem::last_write_time(heartbeatPath,
                                                                 filesystemError);
            if (!filesystemError && (!heartbeatObserved || stamp != heartbeatStamp)) {
                heartbeatObserved = true;
                heartbeatStamp = stamp;
                lastHeartbeatSeen = std::chrono::steady_clock::now();
                publishProgress(dispatchPacket.id, {0.1, "worker-heartbeat"});
            }
            const auto now = std::chrono::steady_clock::now();
            const bool startupLost = !heartbeatObserved
                && now - processStarted > options.startupHeartbeatTimeout;
            const bool heartbeatLost = heartbeatObserved
                && now - lastHeartbeatSeen > options.heartbeatTimeout;
            if (startupLost || heartbeatLost) {
                forcedTermination = true;
                terminateProcessTree(process,
#ifdef Q_OS_WIN
                                     windowsJob,
#endif
                                     options.terminationGrace);
                publishTerminal(GeometryJobResult {dispatchPacket.id,
                                                  GeometryJobState::WorkerCrashed,
                                                  {},
                                                  {},
                                                  startupLost
                                                      ? "FreeCADCmd heartbeat did not start"
                                                      : "FreeCADCmd heartbeat stopped"});
                break;
            }
        }
        if (forcedTermination) {
            return;
        }
        if (cancellationSignalled) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Cancelled,
                                              {},
                                              {},
                                              "isolated geometry worker cancelled cooperatively"});
            return;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            auto diagnostic = boundedDiagnostic(stderrPath);
            if (diagnostic.empty()) {
                diagnostic = "FreeCADCmd worker exited without a result";
            }
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              classifyExit(process),
                                              {},
                                              {},
                                              std::move(diagnostic)});
            return;
        }

        GeometryArchiveExpectation expectation;
        expectation.kind = GeometryArchiveKind::Result;
        expectation.jobId = dispatchPacket.id;
        expectation.operationType = dispatchPacket.request.operationType;
        expectation.buildFingerprint = buildFingerprint;
        expectation.inputDigest = dispatchPacket.request.inputDigest;
        auto workerResult = GeometryArchiveCodec::readValidated(workerResultPath, expectation);
        if (!workerResult.success()) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              workerResult.error.code + ": "
                                                  + workerResult.error.message});
            return;
        }
        const auto nonce = QRandomGenerator::system()->generate64();
        const auto completedPath = options.workspaceRoot
            / (std::string(ResultPrefix) + std::to_string(currentProcessId()) + "-"
               + std::to_string(dispatchPacket.id) + "-"
               + QString::number(nonce, 16).toStdString() + ".fcg");
        auto published = GeometryArchiveCodec::writeAtomic(completedPath,
                                                            *workerResult.archive);
        if (!published.success) {
            publishTerminal(GeometryJobResult {dispatchPacket.id,
                                              GeometryJobState::Failed,
                                              {},
                                              {},
                                              published.error.code + ": "
                                                  + published.error.message});
            return;
        }
        publishTerminal(GeometryJobResult {dispatchPacket.id,
                                          GeometryJobState::Completed,
                                          completedPath,
                                          published.archiveDigest,
                                          {},
                                          dispatchPacket.request.operationType,
                                          buildFingerprint,
                                          dispatchPacket.request.inputDigest});
    }

    GeometryJobManager& manager;
    GeometryProcessBackendOptions options;
    std::jthread dispatcher;
    std::vector<std::unique_ptr<Task>> tasks;
};

GeometryJobProcessBackend::GeometryJobProcessBackend(
    GeometryJobManager& manager,
    GeometryProcessBackendOptions options)
    : _impl(std::make_unique<Impl>(manager, std::move(options)))
{}

GeometryJobProcessBackend::~GeometryJobProcessBackend() = default;
