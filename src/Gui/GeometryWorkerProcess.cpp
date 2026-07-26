// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorkerProcess.h"
#include <App/Application.h>
#include <App/GeometryJobManager.h>
#include <App/GeometryRequestWorkspace.h>
#include <App/MainThreadSignal.h>
#include <Base/Console.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStandardPaths>

#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace
{
constexpr qint64 kMaxTrustedResultBytes = 512LL * 1024 * 1024;
constexpr int kMaxPhaseChars = 256;
constexpr int kMaxErrorCodeChars = 128;
constexpr int kMaxErrorMessageChars = 4096;
constexpr int kSha256HexLen = 64;
/// executionTime is optional; when present it must be finite, >= 0, and <= 24h.
constexpr double kMaxExecutionTimeSeconds = 86400.0;

bool pathHasParentTraversal(const QString& relativePath)
{
    const QStringList parts = QDir::fromNativeSeparators(relativePath).split('/', Qt::SkipEmptyParts);
    return parts.contains(QStringLiteral(".."));
}

bool isFiniteUnitInterval(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

bool isHex64(const QString& value)
{
    if (value.size() != kSha256HexLen) {
        return false;
    }
    for (QChar ch : value) {
        if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('f'))
            && (ch < QLatin1Char('A') || ch > QLatin1Char('F'))) {
            return false;
        }
    }
    return true;
}

bool requireJsonString(const QJsonObject& obj,
                       const char* key,
                       QString& out,
                       int maxLen,
                       std::string& errorCode,
                       std::string& errorMessage)
{
    if (!obj.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = obj.value(QString::fromLatin1(key));
    if (!v.isString()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON string";
        return false;
    }
    out = v.toString();
    if (maxLen > 0 && out.size() > maxLen) {
        errorCode = "OversizedJsonString";
        errorMessage = std::string(key) + " exceeds the configured maximum length";
        return false;
    }
    return true;
}

bool requireJsonInt64(const QJsonObject& obj,
                      const char* key,
                      qint64& out,
                      std::string& errorCode,
                      std::string& errorMessage,
                      qint64 maxInclusive = std::numeric_limits<qint64>::max())
{
    if (!obj.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = obj.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    const double d = v.toDouble();
    if (!std::isfinite(d) || std::floor(d) != d) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be an integer";
        return false;
    }
    // JSON integers above 2^63-1 are representable as doubles but not as qint64.
    constexpr double kInt64ExclusiveMax = 9223372036854775808.0;
    constexpr double kInt64Min = -9223372036854775808.0;
    if (d < kInt64Min || d >= kInt64ExclusiveMax) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " is out of int64 range";
        return false;
    }
    if (d > static_cast<double>(maxInclusive)) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " exceeds the trusted maximum";
        return false;
    }
    out = static_cast<qint64>(d);
    if (static_cast<double>(out) != d) {
        errorCode = "OutOfRangeJsonNumber";
        errorMessage = std::string(key) + " cannot be represented exactly as int64";
        return false;
    }
    return true;
}

bool requireJsonNumber(const QJsonObject& obj,
                       const char* key,
                       double& out,
                       std::string& errorCode,
                       std::string& errorMessage)
{
    if (!obj.contains(QString::fromLatin1(key))) {
        errorCode = "MissingJsonField";
        errorMessage = std::string(key) + " is required";
        return false;
    }
    const QJsonValue v = obj.value(QString::fromLatin1(key));
    if (!v.isDouble()) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be a JSON number";
        return false;
    }
    out = v.toDouble();
    if (!std::isfinite(out)) {
        errorCode = "WrongJsonType";
        errorMessage = std::string(key) + " must be finite";
        return false;
    }
    return true;
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
    if (_process) {
        disconnect(_process, nullptr, this, nullptr);
        if (_process->state() != QProcess::NotRunning) {
            _process->kill();
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

void GeometryWorkerProcess::prepareIdleJob(const App::GeometryJobSpec& spec,
                                           const QString& workspaceDir)
{
    _spec = spec;
    _cancelling = false;
    _cancelPhase = 0;
    _result = {};
    _claimedResultPath.clear();
    _claimedSha256.clear();
    _claimedResultSize = -1;
    _resultMessageSeen = false;
    _helloSeen = false;
    _terminalSeen = false;
    _protocolFailed = false;
    _errorTerminalSeen = false;
    _finishedHandled = false;
    _state = App::GeometryJobState::Running;
    _tempDir = workspaceDir;
    QDir().mkpath(_tempDir);
    _retainWorkspaceOnDestroy = true;
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
    _helloSeen = false;
    _terminalSeen = false;
    _protocolFailed = false;
    _errorTerminalSeen = false;
    _finishedHandled = false;
    _state = App::GeometryJobState::Running;
    _retainWorkspaceOnDestroy = !workspaceDir.isEmpty();

    if (!workspaceDir.isEmpty()) {
        _tempDir = workspaceDir;
        QDir().mkpath(_tempDir);
    }
    else {
        QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (cacheDir.isEmpty()) {
            cacheDir = QDir::tempPath();
        }
        _tempDir = QString("%1/GeometryJobs/job_%2").arg(cacheDir).arg(_spec.id);
        QDir().mkpath(_tempDir);
    }

    App::GeometryRequestWorkspace workspace(_tempDir);
    auto& reqObj = workspace.requestObject();
    std::string jobIdWire;
    if (!App::formatGeometryJobId(_spec.id, jobIdWire)) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        _result.errorCode = "InvalidJobId";
        _result.errorMessage = "Launched GeometryJobId must be nonzero";
        return false;
    }
    reqObj.insert(QStringLiteral("jobId"), QString::fromStdString(jobIdWire));
    reqObj.insert(QStringLiteral("documentIncarnation"),
                   QString::number(_spec.document.runtimeIncarnation));
    reqObj.insert(QStringLiteral("modelGeneration"),
                   QString::number(_spec.document.modelGeneration));
    reqObj.insert(QStringLiteral("targetObjectId"),
                   QString::number(static_cast<qulonglong>(_spec.target.objectId)));
    reqObj.insert(QStringLiteral("targetObjectName"),
                   QString::fromStdString(_spec.target.internalName));
    if (_spec.task) {
        reqObj.insert(QStringLiteral("operationType"),
                      QString::fromStdString(_spec.task->operationType()));
        reqObj.insert(QStringLiteral("codecVersion"),
                      static_cast<qint64>(_spec.task->codecVersion()));
        // Drop any stale publication before staging (reused manager workspaces).
        if (!workspace.clearPublishedRequest()) {
            _state = App::GeometryJobState::Failed;
            _result.success = false;
            _result.errorCode = workspace.failureCode().empty() ? "StaleRequestCleanupFailed"
                                                                : workspace.failureCode();
            _result.errorMessage = workspace.failureMessage().empty()
                ? "Failed to remove stale request.json before staging"
                : workspace.failureMessage();
            return false;
        }
        const App::GeometryArchiveWriteResult staged = _spec.task->writeArchive(workspace);
        if (!staged.success || workspace.hasFailed()) {
            workspace.removePublishedRequestBestEffort();
            _state = App::GeometryJobState::Failed;
            _result.success = false;
            _result.errorCode = !staged.errorCode.empty()
                ? staged.errorCode
                : (workspace.failureCode().empty() ? "RequestSerializeFailed"
                                                   : workspace.failureCode());
            _result.errorMessage = !staged.errorMessage.empty()
                ? staged.errorMessage
                : (workspace.failureMessage().empty()
                       ? "Task writeArchive failed before request publication"
                       : workspace.failureMessage());
            if (_result.errorCode.empty()) {
                _result.errorCode = "RequestSerializeFailed";
            }
            return false;
        }
    }
    reqObj.insert(QStringLiteral("tempDir"), _tempDir);
    reqObj.insert(QStringLiteral("resultPath"), QStringLiteral("result.fcg"));
    if (_spec.deadline.time_since_epoch().count() != 0) {
        const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 _spec.deadline.time_since_epoch())
                                 .count();
        reqObj.insert(QStringLiteral("deadlineEpochMs"), static_cast<qint64>(epochMs));
    }

    if (!workspace.publishRequestJson()) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        _result.errorCode = workspace.failureCode().empty() ? "RequestPublishFailed"
                                                            : workspace.failureCode();
        _result.errorMessage = workspace.failureMessage().empty()
            ? "Failed to atomically publish request.json"
            : workspace.failureMessage();
        return false;
    }

    const QString reqPath = _tempDir + QStringLiteral("/request.json");

    QString appDir = QCoreApplication::applicationDirPath();
    QString cmdPath = appDir + "/FreeCADCmd";
#if defined(Q_OS_WIN)
    cmdPath += ".exe";
#endif
    if (!QFileInfo::exists(cmdPath)) {
        cmdPath = appDir + QStringLiteral("/../bin/FreeCADCmd");
#if defined(Q_OS_WIN)
        cmdPath += QStringLiteral(".exe");
#endif
    }
    if (!QFileInfo::exists(cmdPath)) {
        cmdPath = QCoreApplication::applicationFilePath();
    }

    QString scriptPath = appDir + "/Mod/Part/GeometryWorker.py";
    if (!QFileInfo::exists(scriptPath)) {
        scriptPath = appDir + "/../Mod/Part/GeometryWorker.py";
    }

    QStringList args;
    args << "--safe-mode" << scriptPath << "--pass" << reqPath;

    _process->setWorkingDirectory(_tempDir);
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("FCGEO_LAUNCHED_JOB_ID"), QString::fromStdString(jobIdWire));
        _process->setProcessEnvironment(env);
    }
#if defined(Q_OS_UNIX)
    _process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
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

    auto now = std::chrono::steady_clock::now();
    if (_spec.deadline > now) {
        auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(_spec.deadline - now);
        _deadlineTimer->start(static_cast<int>(dur.count()));
    }
    else {
        _deadlineTimer->start(120000);
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

    if (_process && _process->state() != QProcess::NotRunning) {
        _process->write("FCGEO/1 {\"type\":\"cancel\"}\n");
    }

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
        _cancelTimer->start(750);
    }
    else if (_cancelPhase == 2) {
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
    int idx = 0;
    while ((idx = _stdoutBuffer.indexOf('\n')) != -1) {
        QString line = _stdoutBuffer.left(idx).trimmed();
        _stdoutBuffer.remove(0, idx + 1);
        processLine(line);
    }
}

void GeometryWorkerProcess::onReadyReadStderr()
{
    (void)_process->readAllStandardError();
}

void GeometryWorkerProcess::injectStdoutLine(const QString& line)
{
    processLine(line);
}

void GeometryWorkerProcess::injectProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    onProcessFinished(exitCode, exitStatus);
}

void GeometryWorkerProcess::markProtocolFailed(const std::string& errorCode,
                                               const std::string& errorMessage)
{
    _protocolFailed = true;
    _result.success = false;
    _result.errorCode = errorCode;
    _result.errorMessage = errorMessage;
}

void GeometryWorkerProcess::processLine(const QString& line)
{
    if (!line.startsWith(QStringLiteral("FCGEO/1 "))) {
        return;
    }

    if (_protocolFailed) {
        return;
    }

    if (_terminalSeen) {
        markProtocolFailed("PostTerminalMessage",
                           "Worker emitted a control message after a terminal result/error");
        return;
    }

    const QString jsonStr = line.mid(8);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        markProtocolFailed("MalformedControlJson", "Worker control line is not valid JSON");
        return;
    }

    const QJsonObject obj = doc.object();
    std::string errorCode;
    std::string errorMessage;
    QString type;
    if (!requireJsonString(obj, "type", type, 64, errorCode, errorMessage)) {
        markProtocolFailed(errorCode, errorMessage);
        return;
    }

    if (type == QStringLiteral("hello")) {
        if (_helloSeen) {
            markProtocolFailed("DuplicateHello", "Worker emitted more than one hello");
            return;
        }
        QString protocol;
        QString version;
        if (!requireJsonString(obj, "protocol", protocol, 32, errorCode, errorMessage)
            || !requireJsonString(obj, "version", version, 32, errorCode, errorMessage)) {
            markProtocolFailed(errorCode, errorMessage);
            return;
        }
        if (protocol != QStringLiteral("FCGEO/1")) {
            markProtocolFailed("ProtocolMismatch", "Worker hello protocol is unsupported");
            return;
        }
        if (version != QStringLiteral("1.0")) {
            markProtocolFailed("ProtocolVersionMismatch",
                               "Worker hello version is unsupported");
            return;
        }
        _helloSeen = true;
        return;
    }

    if (!_helloSeen) {
        markProtocolFailed("MissingHello",
                           "Worker emitted progress/result/error before hello");
        return;
    }

    if (type == QStringLiteral("progress")) {
        double fraction = 0.0;
        QString phase;
        if (!requireJsonNumber(obj, "fraction", fraction, errorCode, errorMessage)
            || !requireJsonString(obj, "phase", phase, kMaxPhaseChars, errorCode, errorMessage)) {
            markProtocolFailed(errorCode, errorMessage);
            return;
        }
        if (!isFiniteUnitInterval(fraction)) {
            markProtocolFailed("InvalidProgressFraction",
                               "progress.fraction must be finite and in [0, 1]");
            return;
        }
        Q_EMIT progressUpdated(fraction, phase);
        return;
    }

    if (type == QStringLiteral("result")) {
        if (_resultMessageSeen || _errorTerminalSeen) {
            markProtocolFailed("DuplicateTerminal",
                               "Worker emitted more than one terminal result/error");
            _terminalSeen = true;
            return;
        }

        QString path;
        QString sha;
        QString jobIdText;
        qint64 size = -1;
        if (!requireJsonString(obj, "path", path, 512, errorCode, errorMessage)
            || !requireJsonInt64(obj, "size", size, errorCode, errorMessage, kMaxTrustedResultBytes)
            || !requireJsonString(obj, "sha256", sha, kSha256HexLen, errorCode, errorMessage)
            || !requireJsonString(obj, "jobId", jobIdText, 32, errorCode, errorMessage)) {
            markProtocolFailed(errorCode, errorMessage);
            _terminalSeen = true;
            return;
        }
        App::GeometryJobId jobId = 0;
        if (!App::parseGeometryJobId(jobIdText.toStdString(), jobId, errorCode, errorMessage)
            || jobId != _spec.id) {
            markProtocolFailed(errorCode.empty() ? "JobIdMismatch" : errorCode,
                               errorMessage.empty()
                                   ? "Result jobId does not match the launched job"
                                   : errorMessage);
            _terminalSeen = true;
            return;
        }
        sha = sha.trimmed().toLower();
        if (!isHex64(sha)) {
            markProtocolFailed("InvalidResultDigest",
                               "Result sha256 must be a 64-character hexadecimal string");
            _terminalSeen = true;
            return;
        }
        if (size < 0 || size > kMaxTrustedResultBytes) {
            markProtocolFailed("InvalidResultSize",
                               "Result size is negative or exceeds the trusted maximum");
            _terminalSeen = true;
            return;
        }

        if (obj.contains(QStringLiteral("executionTime"))) {
            double execTime = 0.0;
            if (!requireJsonNumber(obj, "executionTime", execTime, errorCode, errorMessage)) {
                markProtocolFailed(errorCode, errorMessage);
                _terminalSeen = true;
                return;
            }
            if (!(std::isfinite(execTime) && execTime >= 0.0
                  && execTime <= kMaxExecutionTimeSeconds)) {
                markProtocolFailed("InvalidExecutionTime",
                                   "executionTime must be finite, non-negative, and <= 86400");
                _terminalSeen = true;
                return;
            }
            _result.executionTimeSeconds = execTime;
        }

        _resultMessageSeen = true;
        _terminalSeen = true;
        _claimedResultPath = path;
        _claimedResultSize = size;
        _claimedSha256 = sha;
        return;
    }

    if (type == QStringLiteral("error")) {
        if (_resultMessageSeen || _errorTerminalSeen) {
            markProtocolFailed("DuplicateTerminal",
                               "Worker emitted more than one terminal result/error");
            _terminalSeen = true;
            return;
        }
        QString code;
        QString message;
        QString jobIdText;
        if (!requireJsonString(obj, "code", code, kMaxErrorCodeChars, errorCode, errorMessage)
            || !requireJsonString(obj,
                                  "message",
                                  message,
                                  kMaxErrorMessageChars,
                                  errorCode,
                                  errorMessage)
            || !requireJsonString(obj, "jobId", jobIdText, 32, errorCode, errorMessage)) {
            markProtocolFailed(errorCode, errorMessage);
            _terminalSeen = true;
            return;
        }
        if (code.trimmed().isEmpty() || message.trimmed().isEmpty()) {
            markProtocolFailed("EmptyErrorTerminal",
                               "error.code and error.message must be non-empty");
            _terminalSeen = true;
            return;
        }
        App::GeometryJobId jobId = 0;
        if (!App::parseGeometryJobId(jobIdText.toStdString(), jobId, errorCode, errorMessage)
            || jobId != _spec.id) {
            markProtocolFailed(errorCode.empty() ? "JobIdMismatch" : errorCode,
                               errorMessage.empty()
                                   ? "Error jobId does not match the launched job"
                                   : errorMessage);
            _terminalSeen = true;
            return;
        }
        _errorTerminalSeen = true;
        _terminalSeen = true;
        _result.success = false;
        _result.errorCode = code.toStdString();
        _result.errorMessage = message.toStdString();
        return;
    }

    markProtocolFailed("UnknownControlType",
                       "Worker emitted an unsupported control message type");
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
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex().toLower();
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

bool GeometryWorkerProcess::decodeTrustedResult(std::string& errorCode, std::string& errorMessage)
{
    if (!_spec.task) {
        errorCode = "MissingTask";
        errorMessage = "Cannot decode result without a typed DetachedGeometryTask";
        return false;
    }
    _state = App::GeometryJobState::Decoding;
    const App::DetachedGeometryResult decoded =
        _spec.task->decodeResultArchive(_result.resultArchivePath);
    if (!decoded.success) {
        errorCode = decoded.errorCode.empty() ? "ResultDecodeFailed" : decoded.errorCode;
        errorMessage = decoded.errorMessage.empty() ? "Task-aware result decode failed"
                                                    : decoded.errorMessage;
        return false;
    }
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
    if (_finishedHandled) {
        return;
    }
    _finishedHandled = true;
    _deadlineTimer->stop();
    _cancelTimer->stop();

    if (_cancelling) {
        _state = App::GeometryJobState::Cancelled;
        _result.success = false;
        _result.errorCode = "Cancelled";
        _result.errorMessage = "Job was cancelled";
    }
    else if (exitStatus == QProcess::CrashExit) {
        _state = App::GeometryJobState::Crashed;
        _result.success = false;
        _result.errorCode = "Crashed";
        _result.errorMessage =
            QString("Worker process crashed with exit code %1").arg(exitCode).toStdString();
    }
    else if (_protocolFailed) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        // errorCode/message already set by markProtocolFailed
    }
    else if (_resultMessageSeen && exitCode == 0 && _helloSeen && !_errorTerminalSeen) {
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
        else if (!decodeTrustedResult(errorCode, errorMessage)) {
            _state = App::GeometryJobState::Failed;
            _result.success = false;
            _result.errorCode = errorCode;
            _result.errorMessage = errorMessage;
        }
        else {
            _result.success = true;
            _state = App::GeometryJobState::ReadyToCommit;
        }
    }
    else if (_resultMessageSeen && exitCode != 0) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        _result.errorCode = "NonZeroExitAfterResult";
        _result.errorMessage =
            QString("Worker emitted result but exited with code %1").arg(exitCode).toStdString();
    }
    else if (_errorTerminalSeen && exitCode != 0) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        // Keep child error code/message.
    }
    else if (_state != App::GeometryJobState::TimedOut) {
        _state = App::GeometryJobState::Failed;
        _result.success = false;
        if (_result.errorCode.empty()) {
            if (!_helloSeen) {
                _result.errorCode = "MissingHello";
                _result.errorMessage = "Worker exited without a valid hello message";
            }
            else {
                _result.errorCode = "MissingTerminal";
                _result.errorMessage = "Worker exited without a trusted terminal result/error";
            }
        }
    }

    Q_EMIT jobFinished(_spec.id, _state, _result);
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
                    // Decode already succeeded; expose Completed until commit fencing lands.
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
            // startJob already emitted Failed when serialization/publish fails.
            if (worker->state() != App::GeometryJobState::Failed) {
                App::DetachedGeometryResult fail;
                fail.success = false;
                fail.errorCode = "ProcessStartFailed";
                fail.errorMessage = "GeometryWorkerProcess failed to start";
                App::GeometryJobManager::instance().setJobState(
                    req.id, App::GeometryJobState::Failed, fail);
            }
            else {
                App::GeometryJobManager::instance().setJobState(
                    req.id, worker->state(), worker->result());
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
