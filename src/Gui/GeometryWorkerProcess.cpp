// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerProcess.h"
#include <App/Application.h>
#include <Mod/Part/App/TopoShapeArchive.h>
#include <Base/Console.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <thread>

#if defined(Q_OS_WIN)
#include <windows.h>
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
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->kill();
        _process->waitForFinished(500);
    }
    cleanupWorkspace();
}

bool GeometryWorkerProcess::startJob(const App::GeometryJobSpec& spec)
{
    _spec = spec;
    _cancelling = false;
    _cancelPhase = 0;
    _result = {};
    _state = App::GeometryJobState::Running;

    // Set up workspace directory under UserCache
    QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty()) {
        cacheDir = QDir::tempPath();
    }
    _tempDir = QString("%1/GeometryJobs/job_%2").arg(cacheDir).arg(_spec.id);
    QDir().mkpath(_tempDir);

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
    reqObj["resultPath"] = _tempDir + "/result.fcg";

    QString reqPath = _tempDir + "/request.json";
    QFile reqFile(reqPath);
    if (reqFile.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(reqObj);
        reqFile.write(doc.toJson());
        reqFile.close();
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
    _process->start(cmdPath, args);

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

void GeometryWorkerProcess::cancelJob(App::CancelReason reason)
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
            _process->terminate();
        }
        // 3. Schedule QProcess::kill after another 750 ms (total 1 sec)
        _cancelTimer->start(750);
    } else if (_cancelPhase == 2) {
        _cancelTimer->stop();
        if (_process && _process->state() != QProcess::NotRunning) {
            _process->kill();
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
        _result.success = true;
        _result.resultArchivePath = obj["path"].toString().toStdString();
        _result.executionTimeSeconds = obj["executionTime"].toDouble();
    } else if (type == "error") {
        _result.success = false;
        _result.errorCode = obj["code"].toString().toStdString();
        _result.errorMessage = obj["message"].toString().toStdString();
    }
}

void GeometryWorkerProcess::onTimeout()
{
    _state = App::GeometryJobState::TimedOut;
    _result.success = false;
    _result.errorCode = "TimedOut";
    _result.errorMessage = "Worker process exceeded execution deadline";

    if (_process && _process->state() != QProcess::NotRunning) {
        _process->kill();
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
    } else if (_result.success) {
        // Off-thread decoding and checksum validation
        std::string resultPath = _result.resultArchivePath;
        std::thread decodeThread([resultPath]() {
            Part::FrozenTopoShapeBundle bundle;
            bool ok = Part::TopoShapeArchive::readArchive(resultPath, bundle);
            if (!ok) {
                Base::Console().Error("Off-thread decode failure for result archive: %s\n", resultPath.c_str());
            }
        });
        decodeThread.join();

        _state = App::GeometryJobState::Completed;
    } else if (_state != App::GeometryJobState::TimedOut) {
        _state = App::GeometryJobState::Failed;
    }

    Q_EMIT jobFinished(_spec.id, _state, _result);
    cleanupWorkspace();
}

void GeometryWorkerProcess::cleanupWorkspace()
{
    if (!_tempDir.isEmpty()) {
        QDir dir(_tempDir);
        dir.removeRecursively();
        _tempDir.clear();
    }
}

} // namespace Gui
