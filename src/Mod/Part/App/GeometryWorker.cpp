// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorker.h"
#include "BooleanGeometryOperation.h"
#include "FilletGeometryOperation.h"
#include "SweepGeometryOperation.h"
#include "GeometryWorkerRegistry.h"
#include "TopoShapeArchive.h"

#include <App/GeometryJob.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QCryptographicHash>

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <new>
#include <string>
#include <thread>

namespace Part
{

namespace
{

void emitControl(const QJsonObject& obj)
{
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    std::cout << "FCGEO/1 " << payload.constData() << std::endl;
}

void emitError(const std::string& code,
               const std::string& message,
               App::GeometryJobId jobId = 0)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("type"), QStringLiteral("error"));
    obj.insert(QStringLiteral("code"), QString::fromStdString(code));
    obj.insert(QStringLiteral("message"), QString::fromStdString(message));
    std::string jobIdWire;
    if (App::formatGeometryJobId(jobId, jobIdWire)) {
        obj.insert(QStringLiteral("jobId"), QString::fromStdString(jobIdWire));
    }
    emitControl(obj);
}

std::string sha256HexOfFile(const QString& path)
{
    return TopoShapeArchive::calculateSha256File(path.toStdString());
}

class ProcessWorkerContext : public App::GeometryWorkerContext
{
public:
    ProcessWorkerContext(std::string tempDir, std::chrono::steady_clock::time_point deadline)
        : _tempDir(std::move(tempDir))
        , _deadline(deadline)
    {
    }

    void reportProgress(double fraction, const std::string& phase = "") override
    {
        QJsonObject obj;
        obj.insert(QStringLiteral("type"), QStringLiteral("progress"));
        obj.insert(QStringLiteral("phase"), QString::fromStdString(phase));
        obj.insert(QStringLiteral("fraction"), fraction);
        emitControl(obj);
    }

    bool isCancelled() const override
    {
        return _cancelled.load();
    }

    void setCancelled(bool value)
    {
        _cancelled.store(value);
    }

    std::chrono::steady_clock::time_point deadline() const override
    {
        return _deadline;
    }

    std::string tempDir() const override
    {
        return _tempDir;
    }

private:
    std::string _tempDir;
    std::chrono::steady_clock::time_point _deadline;
    std::atomic<bool> _cancelled {false};
};

bool readJobId(const QJsonObject& obj, App::GeometryJobId& jobId, std::string& errorCode, std::string& errorMessage)
{
    if (!obj.contains(QStringLiteral("jobId"))) {
        errorCode = "missing_job_id";
        errorMessage = "jobId missing from request";
        return false;
    }
    const QJsonValue v = obj.value(QStringLiteral("jobId"));
    if (!v.isString()) {
        errorCode = "invalid_job_id";
        errorMessage = "jobId must be a canonical decimal JSON string";
        return false;
    }
    return App::parseGeometryJobId(v.toString().toStdString(), jobId, errorCode, errorMessage);
}

App::GeometryJobId launchedJobIdFromEnvironment()
{
    const QByteArray raw = qgetenv("FCGEO_LAUNCHED_JOB_ID");
    if (raw.isEmpty()) {
        return 0;
    }
    App::GeometryJobId id = 0;
    std::string errorCode;
    std::string errorMessage;
    if (!App::parseGeometryJobId(raw.toStdString(), id, errorCode, errorMessage)) {
        return 0;
    }
    return id;
}

#ifdef FC_GEOMETRY_WORKER_TEST_SEAMS

[[noreturn]] void injectWorkerFaultAbort()
{
    std::abort();
}

[[noreturn]] void injectWorkerFaultHang()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void injectWorkerFaultBadAlloc()
{
    throw std::bad_alloc();
}

bool shouldInjectWorkerFault(App::GeometryJobId requestJobId, std::string& faultModeOut)
{
    const QByteArray modeRaw = qgetenv("FCGEO_TEST_FAULT_MODE");
    if (modeRaw.isEmpty()) {
        return false;
    }
    const QByteArray jobRaw = qgetenv("FCGEO_TEST_FAULT_JOB_ID");
    if (jobRaw.isEmpty()) {
        return false;
    }
    App::GeometryJobId faultJobId = 0;
    std::string errorCode;
    std::string errorMessage;
    if (!App::parseGeometryJobId(jobRaw.toStdString(), faultJobId, errorCode, errorMessage)) {
        return false;
    }
    const App::GeometryJobId launchedJobId = launchedJobIdFromEnvironment();
    if (faultJobId == 0 || faultJobId != requestJobId || launchedJobId == 0
        || launchedJobId != requestJobId) {
        return false;
    }
    const std::string mode = modeRaw.toStdString();
    if (mode == "abort" || mode == "hang" || mode == "bad_alloc") {
        faultModeOut = mode;
        return true;
    }
    return false;
}

void runInjectedWorkerFault(const std::string& mode)
{
    if (mode == "abort") {
        injectWorkerFaultAbort();
    }
    if (mode == "hang") {
        injectWorkerFaultHang();
    }
    if (mode == "bad_alloc") {
        injectWorkerFaultBadAlloc();
    }
}

#endif // FC_GEOMETRY_WORKER_TEST_SEAMS

} // namespace

int GeometryWorker::runWorkerProcess(const std::string& requestJsonPath)
{
    const App::GeometryJobId launchedJobId = launchedJobIdFromEnvironment();
    auto fail = [&](const std::string& code, const std::string& message, App::GeometryJobId jobId = 0) {
        if (jobId == 0) {
            jobId = launchedJobId;
        }
        emitError(code, message, jobId);
    };

    {
        QJsonObject hello;
        hello.insert(QStringLiteral("type"), QStringLiteral("hello"));
        hello.insert(QStringLiteral("version"), QStringLiteral("1.0"));
        hello.insert(QStringLiteral("protocol"), QStringLiteral("FCGEO/1"));
        emitControl(hello);
    }

    QFile reqFile(QString::fromStdString(requestJsonPath));
    if (!reqFile.open(QIODevice::ReadOnly)) {
        fail("request_file_not_found", "Failed to open request.json");
        return 1;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reqFile.readAll());
    reqFile.close();
    if (!doc.isObject()) {
        fail("malformed_request", "request.json is not a JSON object");
        return 1;
    }

    const QJsonObject obj = doc.object();
    if (obj.value(QStringLiteral("protocol")).toString() != QStringLiteral("FCGEO/1")) {
        fail("protocol_mismatch", "Unsupported request protocol");
        return 1;
    }

    App::GeometryJobId jobId = 0;
    std::string jobIdErrorCode;
    std::string jobIdErrorMessage;
    if (!readJobId(obj, jobId, jobIdErrorCode, jobIdErrorMessage)) {
        fail(jobIdErrorCode.empty() ? "invalid_job_id" : jobIdErrorCode,
             jobIdErrorMessage.empty() ? "Invalid jobId in request" : jobIdErrorMessage);
        return 1;
    }

    if (launchedJobId != 0 && jobId != launchedJobId) {
        fail("job_id_mismatch", "Request jobId does not match launched job identity");
        return 1;
    }

    const QString tempDir = obj.value(QStringLiteral("tempDir")).toString();
    const QString operationType = obj.value(QStringLiteral("operationType")).toString();
    QString relativeResultPath = obj.value(QStringLiteral("resultPath")).toString();
    if (relativeResultPath.isEmpty()) {
        relativeResultPath = QStringLiteral("result.fcg");
    }

    if (tempDir.isEmpty()) {
        emitError("missing_temp_dir", "tempDir missing from request", jobId);
        return 1;
    }
    if (QFileInfo(relativeResultPath).isAbsolute()
        || relativeResultPath.contains(QStringLiteral(".."))) {
        emitError("untrusted_result_path", "resultPath must be a relative workspace path", jobId);
        return 1;
    }

    const QString absoluteResultPath = QDir(tempDir).filePath(relativeResultPath);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    if (obj.contains(QStringLiteral("deadlineEpochMs"))) {
        const qint64 epochMs = obj.value(QStringLiteral("deadlineEpochMs")).toVariant().toLongLong();
        if (epochMs > 0) {
            deadline = std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(epochMs));
        }
    }
    ProcessWorkerContext ctx(tempDir.toStdString(), deadline);
    ctx.reportProgress(0.05, "worker.start");

#ifdef FC_GEOMETRY_WORKER_TEST_SEAMS
    try {
        std::string faultMode;
        if (shouldInjectWorkerFault(jobId, faultMode)) {
            runInjectedWorkerFault(faultMode);
        }
    }
    catch (const std::bad_alloc&) {
        emitError("OutOfMemory",
                  "Geometry worker rejected the job after a deterministic test allocation fault",
                  jobId);
        return 1;
    }
#endif

    if (operationType.isEmpty()) {
        emitError("missing_operation", "operationType missing from request", jobId);
        return 1;
    }

    if (!GeometryWorkerRegistry::instance().isOperationAllowed(operationType.toStdString())) {
        emitError("unsupported_operation",
                  "Operation is not registered in the trusted worker registry",
                  jobId);
        return 2;
    }

    std::shared_ptr<App::DetachedGeometryTask> task;
    if (operationType == QStringLiteral("Part::Boolean")) {
        std::string errorCode;
        std::string errorMessage;
        task = BooleanGeometryOperation::decodeFromRequest(obj,
                                                           tempDir,
                                                           errorCode,
                                                           errorMessage);
        if (!task) {
            emitError(errorCode.empty() ? "boolean_decode_failed" : errorCode,
                      errorMessage.empty() ? "Failed to decode Part::Boolean request"
                                           : errorMessage,
                      jobId);
            return 2;
        }
    }
    else if (operationType == QStringLiteral("Part::Fillet")) {
        std::string errorCode;
        std::string errorMessage;
        task = FilletGeometryOperation::decodeFromRequest(obj, tempDir, errorCode, errorMessage);
        if (!task) {
            emitError(errorCode.empty() ? "fillet_decode_failed" : errorCode,
                      errorMessage.empty() ? "Failed to decode Part::Fillet request" : errorMessage,
                      jobId);
            return 2;
        }
    }
    else if (operationType == QStringLiteral("Part::Sweep")) {
        std::string errorCode;
        std::string errorMessage;
        task = SweepGeometryOperation::decodeFromRequest(obj, tempDir, errorCode, errorMessage);
        if (!task) {
            emitError(errorCode.empty() ? "sweep_decode_failed" : errorCode,
                      errorMessage.empty() ? "Failed to decode Part::Sweep request" : errorMessage,
                      jobId);
            return 2;
        }
    }
    else {
        emitError("codec_not_implemented",
                  "Typed decode is not implemented for this operation in this slice",
                  jobId);
        return 2;
    }

    const App::DetachedGeometryResult result = task->run(ctx);
    if (!result.success) {
        emitError(result.errorCode.empty() ? "task_failed" : result.errorCode,
                  result.errorMessage.empty() ? "Geometry task failed" : result.errorMessage,
                  jobId);
        return 3;
    }

    QString publishedPath = QString::fromStdString(result.resultArchivePath);
    if (publishedPath.isEmpty()) {
        publishedPath = absoluteResultPath;
    }
    if (!QFileInfo::exists(publishedPath)) {
        emitError("missing_result", "Task succeeded without a result archive", jobId);
        return 3;
    }
    if (QFileInfo(publishedPath).absoluteFilePath()
        != QFileInfo(absoluteResultPath).absoluteFilePath()) {
        QFile::remove(absoluteResultPath);
        if (!QFile::rename(publishedPath, absoluteResultPath)
            && !QFile::copy(publishedPath, absoluteResultPath)) {
            emitError("result_publish_failed",
                      "Failed to publish result under the trusted path",
                      jobId);
            return 3;
        }
    }

    // Ensure atomic publish semantics: result.fcg must exist before result line.
    const qint64 size = QFileInfo(absoluteResultPath).size();
    const std::string digest = sha256HexOfFile(absoluteResultPath);
    std::string jobIdWire;
    App::formatGeometryJobId(jobId, jobIdWire);
    QJsonObject resultObj;
    resultObj.insert(QStringLiteral("type"), QStringLiteral("result"));
    resultObj.insert(QStringLiteral("path"), relativeResultPath);
    resultObj.insert(QStringLiteral("size"), size);
    resultObj.insert(QStringLiteral("sha256"), QString::fromStdString(digest));
    resultObj.insert(QStringLiteral("jobId"), QString::fromStdString(jobIdWire));
    emitControl(resultObj);
    return 0;
}

} // namespace Part
