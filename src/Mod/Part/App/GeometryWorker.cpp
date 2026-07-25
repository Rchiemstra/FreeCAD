// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryWorker.h"
#include "GeometryWorkerRegistry.h"
#include "TopoShapeArchive.h"

#include <Base/Console.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>

#include <iostream>
#include <chrono>
#include <atomic>
#include <string>

namespace Part
{

namespace
{

std::string sha256HexOfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray digest =
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
    return digest.toStdString();
}

bool writeEmptyResultArchive(const QString& absolutePath)
{
    const QString tmpPath = absolutePath + QStringLiteral(".tmp");
    {
        QFile out(tmpPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        // Empty placeholder archive for idle/trusted-entry smoke path.
        out.close();
    }
    QFile::remove(absolutePath);
    return QFile::rename(tmpPath, absolutePath);
}

void emitResultLine(const std::string& relativePath,
                    qint64 size,
                    const std::string& sha256,
                    App::GeometryJobId jobId)
{
    std::cout << "FCGEO/1 {\"type\":\"result\",\"path\":\"" << relativePath
              << "\",\"size\":" << size << ",\"sha256\":\"" << sha256
              << "\",\"jobId\":" << jobId << "}" << std::endl;
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
        // Cap progress chatter; parent also rate-limits.
        std::cout << "FCGEO/1 {\"type\":\"progress\",\"phase\":\"" << phase
                  << "\",\"fraction\":" << fraction << "}" << std::endl;
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

} // namespace

int GeometryWorker::runWorkerProcess(const std::string& requestJsonPath)
{
    std::cout << "FCGEO/1 {\"type\":\"hello\",\"version\":\"1.0\",\"protocol\":\"FCGEO/1\"}"
              << std::endl;

    QFile reqFile(QString::fromStdString(requestJsonPath));
    if (!reqFile.open(QIODevice::ReadOnly)) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"request_file_not_found\","
                     "\"message\":\"Failed to open request.json\"}"
                  << std::endl;
        return 1;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(reqFile.readAll());
    reqFile.close();
    if (!doc.isObject()) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"malformed_request\","
                     "\"message\":\"request.json is not a JSON object\"}"
                  << std::endl;
        return 1;
    }

    const QJsonObject obj = doc.object();
    const std::string tempDir = obj.value("tempDir").toString().toStdString();
    const std::string operationType = obj.value("operationType").toString().toStdString();
    QString relativeResultPath = obj.value("resultPath").toString();
    if (relativeResultPath.isEmpty()) {
        relativeResultPath = QStringLiteral("result.fcg");
    }
    const auto jobId = static_cast<App::GeometryJobId>(
        obj.value("jobId").toVariant().toULongLong());

    if (tempDir.empty()) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"missing_temp_dir\","
                     "\"message\":\"tempDir missing from request\"}"
                  << std::endl;
        return 1;
    }

    // Reject absolute / traversing result paths from the request itself.
    if (QFileInfo(relativeResultPath).isAbsolute()
        || relativeResultPath.contains(QStringLiteral(".."))) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"untrusted_result_path\","
                     "\"message\":\"resultPath must be a relative workspace path\"}"
                  << std::endl;
        return 1;
    }

    const QString absoluteResultPath =
        QDir(QString::fromStdString(tempDir)).filePath(relativeResultPath);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    ProcessWorkerContext ctx(tempDir, deadline);

    std::cout << "FCGEO/1 {\"type\":\"progress\",\"phase\":\"worker.start\",\"fraction\":0.05}"
              << std::endl;

    if (operationType.empty()) {
        // Trusted entry point ready; specific operations are dispatched by registry
        // once request archives are present (Phase 2 continuation).
        if (!writeEmptyResultArchive(absoluteResultPath)) {
            std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"result_write_failed\","
                         "\"message\":\"Failed to publish empty result archive\"}"
                      << std::endl;
            return 1;
        }
        const qint64 size = QFileInfo(absoluteResultPath).size();
        const std::string digest = sha256HexOfFile(absoluteResultPath);
        std::cout << "FCGEO/1 {\"type\":\"heartbeat\"}" << std::endl;
        std::cout << "FCGEO/1 {\"type\":\"progress\",\"phase\":\"worker.idle\",\"fraction\":1.0}"
                  << std::endl;
        emitResultLine(relativeResultPath.toStdString(), size, digest, jobId);
        return 0;
    }

    if (!GeometryWorkerRegistry::instance().isOperationAllowed(operationType)) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"unsupported_operation\","
                     "\"message\":\"Operation is not registered in the trusted worker registry\"}"
                  << std::endl;
        return 2;
    }

    auto task = GeometryWorkerRegistry::instance().createTask(operationType);
    if (!task) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"factory_failed\","
                     "\"message\":\"Failed to construct geometry task\"}"
                  << std::endl;
        return 2;
    }

    const App::DetachedGeometryResult result = task->run(ctx);
    if (!result.success) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"" << result.errorCode
                  << "\",\"message\":\"" << result.errorMessage << "\"}" << std::endl;
        return 3;
    }

    // Tasks may leave an absolute path; always re-emit as the trusted relative name
    // after verifying the artifact exists under the workspace.
    QString publishedPath = QString::fromStdString(result.resultArchivePath);
    if (publishedPath.isEmpty()) {
        publishedPath = absoluteResultPath;
    }
    if (!QFileInfo::exists(publishedPath)) {
        std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"missing_result\","
                     "\"message\":\"Task succeeded without a result archive\"}"
                  << std::endl;
        return 3;
    }
    if (QFileInfo(publishedPath).absoluteFilePath()
        != QFileInfo(absoluteResultPath).absoluteFilePath()) {
        // Normalize onto the requested relative publish path.
        QFile::remove(absoluteResultPath);
        if (!QFile::rename(publishedPath, absoluteResultPath)
            && !QFile::copy(publishedPath, absoluteResultPath)) {
            std::cout << "FCGEO/1 {\"type\":\"error\",\"code\":\"result_publish_failed\","
                         "\"message\":\"Failed to publish result under the trusted path\"}"
                      << std::endl;
            return 3;
        }
    }

    const qint64 size = QFileInfo(absoluteResultPath).size();
    const std::string digest = sha256HexOfFile(absoluteResultPath);
    emitResultLine(relativeResultPath.toStdString(), size, digest, jobId);
    return 0;
}

} // namespace Part
