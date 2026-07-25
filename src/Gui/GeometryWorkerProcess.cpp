// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerProcess.h"
#include <App/Application.h>
#include <App/GeometryJobManager.h>
#include <App/MainThreadSignal.h>
#include <Base/Console.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <mutex>
#include <unordered_map>

namespace
{
constexpr qint64 kMaxTrustedResultBytes = 512LL * 1024 * 1024;

bool pathHasParentTraversal(const QString& relativePath)
{
    const QStringList parts = QDir::fromNativeSeparators(relativePath).split('/', Qt::SkipEmptyParts);
    return parts.contains(QStringLiteral(".."));
}
} // namespace

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

namespace Gui
{

GeometryWorkerProcess::GeometryWorkerProcess(QObject* parent)
    : QObject(parent)
{
    _process = new QProcess(this);
    _deadlineTimer = new QTimer(this);
    _deadlineTimer->setSingleShot(true);
    _cancelTimer = new QTimer(this);

    connect(_process, &QProcess::readyReadStandardOutput, this, &GeometryWorkerProcess::onReadyReadStdout);
    connect(_process, &QProcess::readyReadStandardError, this, &GeometryWorkerProcess::onReadyReadStderr);
    connect(_process, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, &GeometryWorkerProcess::onProcessFinished);
    connect(_deadlineTimer, &QTimer::timeout, this, &GeometryWorkerProcess::onTimeout);
    connect(_cancelTimer, &QTimer::timeout, this, &GeometryWorkerProcess::onCooperativeCancelTimeout);
}

GeometryWorkerProcess::~GeometryWorkerProcess()
{
    // Never block the GUI thread with waitForFinished().
    // Disconnect signals first so late process events cannot re-enter this object.
    if (_process) {
        disconnect(_process, nullptr, this, nullptr);
        if (_process->state() != QProcess::NotRunning) {
            _process->kill();
            // Detach ownership: Qt will delete the QProcess with this QObject parent,
            // but we do not wait here. Workspace cleanup is deferred to a later janitor pass
            // when the process is known to have exited.
            _retainWorkspaceOnDestroy = true;
        }
    }
    if (_deadlineTimer) {
        _deadlineTimer->stop();
        disconnect(_deadlineTimer, nullptr, this, nullptr);
    }
    if (_cancelTimer) {
        _cancelTimer->stop();
        disconnect(_cancelTimer, nullptr, this, nullptr);
    }
    if (!_retainWorkspaceOnDestroy) {
        cleanupWorkspace();
    }
}

bool GeometryWorkerProcess::startJob(const App::GeometryJobSpec& spec)
{
    return startJob(spec, QString());
}

bool GeometryWorkerProcess::startJob(const App::GeometryJobSpec& spec, const QString& workspaceDir)
{
    _spec = spec;
    _cancelling = false;
    _cancelPhase = 0;
    _result = {};
    _claimedResultPath.clear();
    _claimedSha256.clear();
    _claimedResultSize = -1;
    _resultMessageSeen = false;
    _state = App::GeometryJobState::Running;
    // Manager-owned workspaces must outlive successful results until releaseJobArtifacts().
    _retainWorkspaceOnDestroy = !workspaceDir.isEmpty();

    if (!workspaceDir.isEmpty()) {
        _tempDir = workspaceDir;
        QDir().mkpath(_tempDir);
    }
    else {
        // Set up workspace directory under UserCache
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheDir.isEmpty()) {
            cacheDir = QDir::tempPath();
        }
        _tempDir = QString("%1/GeometryJobs/job_%2").arg(cacheDir).arg(_spec.id);
        QDir().mkpath(_tempDir);
    }

    // Write request.json
    QJsonObject reqObj;
    reqObj["jobId"] = static_cast<qint64>(_spec.id);
    reqObj["documentIncarnation"] = static_cast<qint64>(_spec.document.runtimeIncarnation);
    reqObj["modelGeneration"] = static_cast<qint64>(_spec.document.modelGeneration);
    reqObj["targetObjectId"] = static_cast<qint64>(_spec.target.objectId);
    reqObj["targetObjectName"] = QString::fromStdString(_spec.target.internalName);
    if (_spec.task) {
        reqObj["operationType"] = QString::fromStdString(_spec.task->operationType());
        reqObj["codecVersion"] = static_cast<qint64>(_spec.task->codecVersion());
    }
    reqObj["tempDir"] = _tempDir;
    // Child must report a workspace-relative result path; never trust absolute child paths.
    reqObj["resultPath"] = QStringLiteral("result.fcg");

    // Write request.json atomically: request.json.tmp → request.json
    QString reqPath = _tempDir + "/request.json";
    QString reqTmpPath = reqPath + ".tmp";
    {
        QFile reqFile(reqTmpPath);
        if (!reqFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        QJsonDocument doc(reqObj);
        reqFile.write(doc.toJson());
        reqFile.close();
    }
    QFile::remove(reqPath);
    if (!QFile::rename(reqTmpPath, reqPath)) {
        return false;
    }

    // Determine FreeCADCmd executable and GeometryWorker.py script path
    QString appDir = QCoreApplication::applicationDirPath();
    QString cmdPath = appDir + "/FreeCADCmd";
#if defined(Q_OS_WIN)
    cmdPath += ".exe";
#endif
    if (!QFileInfo::exists(cmdPath)) {
        cmdPath = QCoreApplication::applicationFilePath();
    }

    QString scriptPath = appDir + "/Mod/Part/GeometryWorker.py";
    if (!QFileInfo::exists(scriptPath)) {
        scriptPath = appDir + "/../Mod/Part/GeometryWorker.py";
    }

    QStringList args;
    args << "--safe-mode" << scriptPath << reqPath;

    _process->setWorkingDirectory(_tempDir);
#if defined(Q_OS_UNIX)
    // Put the child in its own process group so cancel can kill the whole tree.
    _process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
            // Best-effort; launch still proceeds if setpgid fails.
        }
    });
#endif
    _process->start(cmdPath, args);
    if (!_process->waitForStarted(5000)) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        _result.errorCode = "ProcessStartFailed";
        _result.errorMessage = _process->errorString().toStdString();
        return false;
    }

    // Calculate deadline duration
    auto now = std::chrono::steady_clock::now();
    if (_spec.deadline > now) {
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(_spec.deadline - now);
        _deadlineTimer->start(static_cast<int>(dur.count()));
    } else {
        _deadlineTimer->start(120000); // Default 120s
    }

    return true;
}

void GeometryWorkerProcess::cancelJob(App::CancelReason /*reason*/)
{
    if (_cancelling || _state != App::GeometryJobState::Running) {
        return;
    }

    _cancelling = true;
    _cancelPhase = 1;
    _state = App::GeometryJobState::Cancelling;

    // 1. Cooperative cancel signal over process stdin
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->write("FCGEO/1 {\"type\":\"cancel\"}\n");
    }

    // 2. Schedule QProcess::terminate after 250 ms
    _cancelTimer->start(250);
}

void GeometryWorkerProcess::onCooperativeCancelTimeout()
{
    if (_cancelPhase == 1) {
        _cancelPhase = 2;
        if (_process && _process->state() != QProcess::NotRunning) {
#if defined(Q_OS_UNIX)
            const qint64 pid = _process->processId();
            if (pid > 0) {
                ::kill(static_cast<pid_t>(-pid), SIGTERM);
            }
            else {
                _process->terminate();
            }
#else
            _process->terminate();
#endif
        }
        // 3. Schedule process-group / process kill after another 750 ms (total 1 sec)
        _cancelTimer->start(750);
    } else if (_cancelPhase == 2) {
        _cancelTimer->stop();
        if (_process && _process->state() != QProcess::NotRunning) {
#if defined(Q_OS_UNIX)
            const qint64 pid = _process->processId();
            if (pid > 0) {
                ::kill(static_cast<pid_t>(-pid), SIGKILL);
            }
            else {
                _process->kill();
            }
#else
            _process->kill();
#endif
        }
    }
}

bool GeometryWorkerProcess::isRunning() const
{
    return _process && _process->state() != QProcess::NotRunning;
}

void GeometryWorkerProcess::onReadyReadStdout()
{
    _stdoutBuffer += QString::fromUtf8(_process->readAllStandardOutput());
    int idx;
    while ((idx = _stdoutBuffer.indexOf('\n')) != -1) {
        QString line = _stdoutBuffer.left(idx).trimmed();
        _stdoutBuffer.remove(0, idx + 1);
        processLine(line);
    }
}

void GeometryWorkerProcess::onReadyReadStderr()
{
    QByteArray errData = _process->readAllStandardError();
}

void GeometryWorkerProcess::processLine(const QString& line)
{
    if (!line.startsWith("FCGEO/1 ")) {
        return;
    }

    QString jsonStr = line.mid(8);
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8());
    if (!doc.isObject()) {
        return;
    }

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "progress") {
        double fraction = obj["fraction"].toDouble();
        QString phase = obj["phase"].toString();
        Q_EMIT progressUpdated(fraction, phase);
    } else if (type == "result") {
        // Defer path/size/digest trust checks until the process exits normally.
        _resultMessageSeen = true;
        _claimedResultPath = obj.value(QStringLiteral("path")).toString();
        _claimedResultSize = obj.value(QStringLiteral("size")).toVariant().toLongLong();
        _claimedSha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
        _result.executionTimeSeconds = obj.value(QStringLiteral("executionTime")).toDouble();
        if (obj.contains(QStringLiteral("jobId"))) {
            const auto reportedJobId = static_cast<App::GeometryJobId>(
                obj.value(QStringLiteral("jobId")).toVariant().toULongLong());
            if (reportedJobId != 0 && reportedJobId != _spec.id) {
                _resultMessageSeen = false;
                _result.success = false;
                _result.errorCode = "JobIdMismatch";
                _result.errorMessage = "Result jobId does not match the launched job";
            }
        }
    } else if (type == "error") {
        _result.success = false;
        _result.errorCode = obj["code"].toString().toStdString();
        _result.errorMessage = obj["message"].toString().toStdString();
    }
}

bool GeometryWorkerProcess::acceptTrustedResult(const QString& relativePath,
                                                qint64 claimedSize,
                                                const QString& claimedSha256,
                                                std::string& errorCode,
                                                std::string& errorMessage)
{
    if (relativePath.isEmpty() || QFileInfo(relativePath).isAbsolute()
        || pathHasParentTraversal(relativePath)
        || !App::isTrustedRelativeResultPath(relativePath.toStdString())) {
        errorCode = "UntrustedResultPath";
        errorMessage = "Result path must be a relative path under the job workspace";
        return false;
    }
    if (claimedSize < 0) {
        errorCode = "MissingResultSize";
        errorMessage = "Result message omitted a non-negative size";
        return false;
    }
    if (claimedSize > kMaxTrustedResultBytes) {
        errorCode = "OversizedResult";
        errorMessage = "Result size exceeds the trusted maximum";
        return false;
    }

    const QDir workspace(_tempDir);
    const QString absolutePath = QFileInfo(workspace.filePath(relativePath)).absoluteFilePath();
    const QString workspaceRoot = QFileInfo(_tempDir).absoluteFilePath();
    if (!absolutePath.startsWith(workspaceRoot + QLatin1Char('/'))
        && absolutePath != workspaceRoot) {
        errorCode = "UntrustedResultPath";
        errorMessage = "Resolved result path escaped the job workspace";
        return false;
    }

    QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile()) {
        errorCode = "MissingResult";
        errorMessage = "Worker reported success but result archive is missing";
        return false;
    }
    if (info.size() != claimedSize) {
        errorCode = "ResultSizeMismatch";
        errorMessage = "Result file size does not match the claimed size";
        return false;
    }

    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        errorCode = "ResultReadFailed";
        errorMessage = "Failed to open result archive for digest verification";
        return false;
    }
    const QByteArray digest =
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    file.close();

    if (claimedSha256.isEmpty()) {
        if (claimedSize != 0) {
            errorCode = "MissingResultDigest";
            errorMessage = "Non-empty result requires a sha256 digest";
            return false;
        }
    }
    else if (claimedSha256 != QString::fromLatin1(digest)) {
        errorCode = "ResultDigestMismatch";
        errorMessage = "Result sha256 does not match the on-disk archive";
        return false;
    }

    _result.resultArchivePath = absolutePath.toStdString();
    return true;
}

void GeometryWorkerProcess::onTimeout()
{
    _state = App::GeometryJobState::TimedOut;
    _result.success = false;
    _result.errorCode = "TimedOut";
    _result.errorMessage = "Worker process exceeded execution deadline";

    if (_process && _process->state() != QProcess::NotRunning) {
#if defined(Q_OS_UNIX)
        const qint64 pid = _process->processId();
        if (pid > 0) {
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
        }
        else {
            _process->kill();
        }
#else
        _process->kill();
#endif
    }
}

void GeometryWorkerProcess::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    _deadlineTimer->stop();
    _cancelTimer->stop();

    if (_cancelling) {
        _state = App::GeometryJobState::Cancelled;
        _result.success = false;
        _result.errorCode = "Cancelled";
        _result.errorMessage = "Job was cancelled";
    } else if (exitStatus == QProcess::CrashExit) {
        _state = App::GeometryJobState::Crashed;
        _result.success = false;
        _result.errorCode = "Crashed";
        _result.errorMessage = QString("Worker process crashed with exit code %1").arg(exitCode).toStdString();
    } else if (_resultMessageSeen && exitCode == 0
               && _result.errorCode != "JobIdMismatch") {
        // Trust only workspace-relative path + size + digest; never absolute child paths.
        std::string errorCode;
        std::string errorMessage;
        if (!acceptTrustedResult(_claimedResultPath,
                                 _claimedResultSize,
                                 _claimedSha256,
                                 errorCode,
                                 errorMessage)) {
            _state = App::GeometryJobState::Failed;
            _result.success = false;
            _result.errorCode = errorCode;
            _result.errorMessage = errorMessage;
        }
        else {
            _result.success = true;
            _state = App::GeometryJobState::ReadyToCommit;
        }
    } else if (_resultMessageSeen && exitCode != 0) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        _result.errorCode = "NonZeroExitAfterResult";
        _result.errorMessage =
            QString("Worker emitted result but exited with code %1").arg(exitCode).toStdString();
    } else if (_state != App::GeometryJobState::TimedOut) {
        _state = App::GeometryJobState::Failed;
        if (_result.errorCode.empty()) {
            _result.success = false;
            _result.errorCode = "MissingResult";
            _result.errorMessage = "Worker exited without a trusted result message";
        }
    }

    Q_EMIT jobFinished(_spec.id, _state, _result);
    // Retain successful result artifacts until decode/commit consumes them.
    // Manager-owned workspaces are never deleted here (releaseJobArtifacts owns cleanup).
    // Clean failed/cancelled/crashed local (non-manager) workspaces promptly.
    if (_state != App::GeometryJobState::ReadyToCommit && !_retainWorkspaceOnDestroy) {
        cleanupWorkspace();
    }
}

void GeometryWorkerProcess::cleanupWorkspace()
{
    if (!_tempDir.isEmpty()) {
        QDir dir(_tempDir);
        dir.removeRecursively();
        _tempDir.clear();
    }
}

namespace
{

std::mutex& workerMapMutex()
{
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<App::GeometryJobId, GeometryWorkerProcess*>& workerMap()
{
    static std::unordered_map<App::GeometryJobId, GeometryWorkerProcess*> map;
    return map;
}

bool launchForManager(const App::GeometryProcessLaunchRequest& req)
{
    auto startOnMain = [&req]() -> bool {
        auto* worker = new GeometryWorkerProcess();
        QObject::connect(
            worker,
            &GeometryWorkerProcess::progressUpdated,
            worker,
            [id = req.id](double fraction, const QString& phase) {
                App::GeometryJobManager::instance().updateProgress(
                    id, fraction, phase.toStdString());
            });
        QObject::connect(
            worker,
            &GeometryWorkerProcess::jobFinished,
            worker,
            [id = req.id, worker](App::GeometryJobId,
                                  App::GeometryJobState state,
                                  const App::DetachedGeometryResult& result) {
                {
                    std::lock_guard<std::mutex> lock(workerMapMutex());
                    workerMap().erase(id);
                }
                App::GeometryJobState deliver = state;
                if (deliver == App::GeometryJobState::ReadyToCommit) {
                    // Commit fencing still lands in a later step; expose success as Completed
                    // so coordinator observers can observe terminal delivery.
                    deliver = App::GeometryJobState::Completed;
                }
                App::GeometryJobManager::instance().setJobState(id, deliver, result);
                worker->deleteLater();
            });

        {
            std::lock_guard<std::mutex> lock(workerMapMutex());
            workerMap()[req.id] = worker;
        }

        const QString workspace = QString::fromStdString(req.workspaceDir);
        if (!worker->startJob(req.spec, workspace)) {
            {
                std::lock_guard<std::mutex> lock(workerMapMutex());
                workerMap().erase(req.id);
            }
            worker->deleteLater();
            return false;
        }
        return true;
    };

    if (App::MainThreadSignalConfig::hasHooks()
        && !App::MainThreadSignalConfig::isMainThread()) {
        bool ok = false;
        App::MainThreadSignalConfig::invoke(
            [&]() { ok = startOnMain(); },
            /*blocking=*/true);
        return ok;
    }
    return startOnMain();
}

void cancelForManager(App::GeometryJobId id, App::CancelReason reason)
{
    auto cancelOnMain = [id, reason]() {
        GeometryWorkerProcess* worker = nullptr;
        {
            std::lock_guard<std::mutex> lock(workerMapMutex());
            auto it = workerMap().find(id);
            if (it != workerMap().end()) {
                worker = it->second;
            }
        }
        if (worker) {
            worker->cancelJob(reason);
        }
    };

    if (App::MainThreadSignalConfig::hasHooks()
        && !App::MainThreadSignalConfig::isMainThread()) {
        App::MainThreadSignalConfig::invoke(cancelOnMain, /*blocking=*/false);
        return;
    }
    cancelOnMain();
}

} // namespace

void GeometryWorkerProcess::installManagerBackend()
{
    App::GeometryJobManager::instance().setProcessBackend(&launchForManager, &cancelForManager);
}

void GeometryWorkerProcess::uninstallManagerBackend()
{
    App::GeometryJobManager::instance().clearProcessBackend();
    std::lock_guard<std::mutex> lock(workerMapMutex());
    workerMap().clear();
}

} // namespace Gui
