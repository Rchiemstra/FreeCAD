// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerMain.h"

#include "GeometryArchive.h"

#include <QCryptographicHash>
#include <QFile>
#include <QString>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
# include <windows.h>
#elif defined(Q_OS_LINUX)
# include <unistd.h>
#elif defined(Q_OS_MACOS)
# include <mach-o/dyld.h>
#endif

using namespace App;
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

std::string environment(const char* name)
{
    return qEnvironmentVariable(name).toStdString();
}

std::filesystem::path pathEnvironment(const char* name)
{
    const QString value = qEnvironmentVariable(name);
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

bool parseJobId(const std::string& value, GeometryJobId& id) noexcept
{
    if (value.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
    return error == std::errc {} && end == value.data() + value.size() && id != 0;
}

std::filesystem::path workerRuntimeLibrary(const std::filesystem::path& executable)
{
    const auto directory = executable.parent_path();
#ifdef Q_OS_WIN
    return directory / "FreeCADApp.dll";
#elif defined(Q_OS_MACOS)
    for (const auto& candidate : {directory / "libFreeCADApp.dylib",
                                  directory.parent_path() / "lib" / "libFreeCADApp.dylib"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
#else
    for (const auto& candidate : {directory / "libFreeCADApp.so",
                                  directory.parent_path() / "lib" / "libFreeCADApp.so",
                                  directory.parent_path() / "lib64" / "libFreeCADApp.so"}) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
#endif
    return {};
}

bool addFingerprintFile(QCryptographicHash& hash,
                        const QByteArray& label,
                        const std::filesystem::path& path)
{
    QFile file(toQStringPath(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    hash.addData(label);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
            return false;
        }
        hash.addData(chunk);
    }
    return true;
}

bool isWorkspaceFile(const std::filesystem::path& path, const char* expectedName)
{
    std::error_code error;
    const auto current = std::filesystem::weakly_canonical(
        std::filesystem::current_path(error), error);
    if (error || current.empty() || path.filename() != expectedName) {
        return false;
    }
    const auto parent = std::filesystem::weakly_canonical(path.parent_path(), error);
    return !error && parent == current;
}

bool waitForStartGate(const std::filesystem::path& gate)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::error_code error;
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::is_regular_file(gate, error)) {
            return true;
        }
        error.clear();
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

void writeHeartbeat(const std::filesystem::path& heartbeat)
{
    std::ofstream stream(heartbeat, std::ios::out | std::ios::trunc);
    if (stream) {
        stream << std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count()
               << '\n';
    }
}

class Heartbeat
{
public:
    explicit Heartbeat(std::filesystem::path path)
        : heartbeatPath(std::move(path))
        , thread([this] {
            while (!stopping.load(std::memory_order_acquire)) {
                writeHeartbeat(heartbeatPath);
                std::this_thread::sleep_for(50ms);
            }
        })
    {}

    ~Heartbeat()
    {
        stopping.store(true, std::memory_order_release);
        if (thread.joinable()) {
            thread.join();
        }
    }

private:
    std::filesystem::path heartbeatPath;
    std::atomic_bool stopping {false};
    std::thread thread;
};

}  // namespace

bool App::Internal::geometryWorkerRequested() noexcept
{
    return qEnvironmentVariable(WorkerProtocolEnvironment) == WorkerProtocolValue;
}

std::filesystem::path App::Internal::currentExecutablePath()
{
#ifdef Q_OS_WIN
    std::wstring buffer(1024, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(Q_OS_LINUX)
    std::string buffer(1024, '\0');
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(Q_OS_MACOS)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()));
#else
    return {};
#endif
}

std::string App::Internal::geometryWorkerBuildFingerprint(
    const std::filesystem::path& workerExecutable)
{
    const auto runtimeLibrary = workerRuntimeLibrary(workerExecutable);
    if (workerExecutable.empty() || runtimeLibrary.empty()) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!addFingerprintFile(hash, QByteArray("FreeCADCmd\0", 11), workerExecutable)
        || !addFingerprintFile(hash,
                               QByteArray("FreeCADApp\0", 11),
                               runtimeLibrary)) {
        return {};
    }
    return hash.result().toHex().toStdString();
}

int App::Internal::runGeometryWorkerMain() noexcept
{
    try {
        const std::filesystem::path requestPath = pathEnvironment(RequestEnvironment);
        const std::filesystem::path resultPath = pathEnvironment(ResultEnvironment);
        const std::filesystem::path heartbeatPath = pathEnvironment(HeartbeatEnvironment);
        const std::filesystem::path startGatePath = pathEnvironment(StartGateEnvironment);
        const std::filesystem::path cancelPath = pathEnvironment(CancelEnvironment);
        const std::string operation = environment(OperationEnvironment);
        const std::string expectedBuild = environment(BuildEnvironment);
        const std::string inputDigest = environment(InputDigestEnvironment);
        GeometryJobId jobId = 0;
        if (!parseJobId(environment(JobIdEnvironment), jobId)
            || operation.empty() || expectedBuild.empty() || inputDigest.empty()
            || !isWorkspaceFile(requestPath, "request.fcg")
            || !isWorkspaceFile(resultPath, "result.fcg")
            || !isWorkspaceFile(heartbeatPath, "heartbeat")
            || !isWorkspaceFile(startGatePath, "start")
            || !isWorkspaceFile(cancelPath, "cancel")) {
            std::cerr << "invalid FCG worker environment\n";
            return 10;
        }
        const std::string actualBuild = Internal::geometryWorkerBuildFingerprint(
            Internal::currentExecutablePath());
        if (actualBuild != expectedBuild) {
            std::cerr << "FCG worker binary fingerprint mismatch: expected="
                      << expectedBuild << " actual=" << actualBuild << " path="
                      << toQStringPath(Internal::currentExecutablePath()).toStdString() << '\n';
            return 10;
        }
        if (!waitForStartGate(startGatePath)) {
            std::cerr << "FCG worker start gate timed out\n";
            return 11;
        }

        Heartbeat heartbeat(heartbeatPath);
        GeometryArchiveExpectation expectation;
        expectation.kind = GeometryArchiveKind::Request;
        expectation.jobId = jobId;
        expectation.operationType = operation;
        expectation.buildFingerprint = expectedBuild;
        expectation.inputDigest = inputDigest;
        auto input = GeometryArchiveCodec::readValidated(requestPath, expectation);
        if (!input.success()) {
            std::cerr << input.error.code << ": " << input.error.message << '\n';
            return 12;
        }

        // CC-WP08 supplies a transport probe only. Native OCC adapters are
        // registered explicitly by CC-WP09; unsupported operations fail closed.
        if (operation != "FreeCAD.Internal.GeometryProbe") {
            std::cerr << "unsupported isolated geometry operation\n";
            return 13;
        }
        GeometryArchive output = std::move(*input.archive);
        output.metadata.kind = GeometryArchiveKind::Result;
        output.archiveDigest.clear();
        const auto written = GeometryArchiveCodec::writeAtomic(resultPath, output);
        if (!written.success) {
            std::cerr << written.error.code << ": " << written.error.message << '\n';
            return 14;
        }
        return 0;
    }
    catch (const std::bad_alloc&) {
        std::cerr << "geometry worker out of memory\n";
        return 86;
    }
    catch (const std::exception& exception) {
        std::cerr << "geometry worker failed: " << exception.what() << '\n';
        return 15;
    }
    catch (...) {
        std::cerr << "geometry worker failed with an unknown exception\n";
        return 15;
    }
}
