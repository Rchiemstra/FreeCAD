// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryRequestWorkspace.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <algorithm>
#include <cstring>
#include <limits>

namespace App
{

namespace
{
constexpr size_t kMaxWorkspaceSectionBytes = GeometryRequestWorkspace::maxWorkspaceSectionBytes();
constexpr qint64 kStreamChunkBytes = 1024 * 1024;
} // namespace

GeometryRequestWorkspace::GeometryRequestWorkspace(QString workspaceDir)
    : _workspaceDir(std::move(workspaceDir))
{
    QDir().mkpath(_workspaceDir);
    _request.insert(QStringLiteral("protocol"), QStringLiteral("FCGEO/1"));
    _request.insert(QStringLiteral("tempDir"), _workspaceDir);
    _request.insert(QStringLiteral("resultPath"), QStringLiteral("result.fcg"));
    // A reused manager workspace must never publish a stale request.json from a prior attempt.
    if (!clearPublishedRequest()) {
        markFailed("StaleRequestCleanupFailed",
                   "Failed to remove stale request.json during workspace initialization");
    }
}

bool GeometryRequestWorkspace::isTrustedRelativePath(const QString& relativePath)
{
    if (relativePath.isEmpty() || QFileInfo(relativePath).isAbsolute()) {
        return false;
    }
    const QString native = QDir::fromNativeSeparators(relativePath);
    const QStringList parts = native.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return !parts.contains(QStringLiteral(".."));
}

bool GeometryRequestWorkspace::removeWorkspaceEntry(const QString& relativePath, bool recordFailure)
{
    if (!isTrustedRelativePath(relativePath)) {
        if (recordFailure) {
            markFailed("UntrustedRelativePath", "Refusing to remove an untrusted relative path");
        }
        return false;
    }
    const QString absolute = QDir(_workspaceDir).filePath(relativePath);
    if (!QFileInfo::exists(absolute)) {
        return true;
    }
    const QFileInfo info(absolute);
    if (info.isDir()) {
        if (recordFailure && !_failed) {
            markFailed("WorkspaceReplaceFailed",
                       "Workspace path exists but is not a replaceable file");
        }
        return false;
    }
    if (!QFile::remove(absolute)) {
        if (recordFailure && !_failed) {
            markFailed("WorkspaceReplaceFailed",
                       "Failed to remove an existing workspace file before publish");
        }
        return false;
    }
    return true;
}

bool GeometryRequestWorkspace::clearPublishedRequest()
{
    return removeWorkspaceEntry(QStringLiteral("request.json"), /*recordFailure=*/true);
}

void GeometryRequestWorkspace::removePublishedRequestBestEffort()
{
    (void)removeWorkspaceEntry(QStringLiteral("request.json"), /*recordFailure=*/false);
}

bool GeometryRequestWorkspace::clearStagedFile(const QString& relativePath)
{
    return removeWorkspaceEntry(relativePath, /*recordFailure=*/true);
}

void GeometryRequestWorkspace::writeSection(const std::string& name, const std::vector<uint8_t>& data)
{
    if (_failed) {
        return;
    }
    if (data.size() > kMaxWorkspaceSectionBytes
        || data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        markFailed("SectionTooLarge", "Workspace section exceeds the configured archive limit");
        return;
    }
    _sections[name] = data;
}

void GeometryRequestWorkspace::writeString(const std::string& name, const std::string& value)
{
    _request.insert(QString::fromStdString(name), QString::fromStdString(value));
}

void GeometryRequestWorkspace::writeBytes(const std::string& name, const uint8_t* data, size_t size)
{
    if (_failed) {
        return;
    }
    if (size == 0) {
        // Empty payload is valid even when data is null.
        _sections[name] = {};
        return;
    }
    if (!data) {
        markFailed("NullWriteBytes", "writeBytes received a null pointer with nonzero size");
        return;
    }
    if (size > kMaxWorkspaceSectionBytes
        || size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        markFailed("SectionTooLarge", "Workspace section exceeds the configured archive limit");
        return;
    }
    std::vector<uint8_t> bytes(data, data + size);
    _sections[name] = std::move(bytes);
}

bool GeometryRequestWorkspace::writeFileAtomic(const QString& relativePath, const QByteArray& bytes)
{
    if (_failed) {
        return false;
    }
    if (!isTrustedRelativePath(relativePath)) {
        markFailed("UntrustedRelativePath", "Refusing to write an untrusted relative path");
        return false;
    }
    const QString absolute = QDir(_workspaceDir).filePath(relativePath);
    const QString tmp = absolute + QStringLiteral(".tmp");
    {
        QFile out(tmp);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            markFailed("WorkspaceWriteFailed", "Failed to open temporary staging file");
            return false;
        }
        if (out.write(bytes) != bytes.size()) {
            out.close();
            QFile::remove(tmp);
            markFailed("WorkspaceWriteFailed", "Failed to write temporary staging file");
            return false;
        }
        out.close();
    }
    if (!removeWorkspaceEntry(relativePath, /*recordFailure=*/true)) {
        QFile::remove(tmp);
        return false;
    }
    if (!QFile::rename(tmp, absolute)) {
        QFile::remove(tmp);
        markFailed("WorkspaceRenameFailed", "Failed to atomically publish staged file");
        return false;
    }
    return true;
}

bool GeometryRequestWorkspace::stageFileAtomic(const QString& relativePath,
                                               const QString& absoluteSourcePath)
{
    if (_failed) {
        return false;
    }
    if (!isTrustedRelativePath(relativePath)) {
        markFailed("UntrustedRelativePath", "Refusing to stage an untrusted relative path");
        return false;
    }

    const QFileInfo srcInfo(absoluteSourcePath);
    if (!srcInfo.exists() || !srcInfo.isFile()) {
        markFailed("WorkspaceReadFailed", "Source file for staging is missing or not a file");
        return false;
    }
    const qint64 srcSize = srcInfo.size();
    if (srcSize < 0
        || static_cast<uint64_t>(srcSize) > static_cast<uint64_t>(kMaxWorkspaceSectionBytes)) {
        markFailed("SectionTooLarge", "Workspace section exceeds the configured archive limit");
        return false;
    }

    const QString absolute = QDir(_workspaceDir).filePath(relativePath);
    const QString tmp = absolute + QStringLiteral(".tmp");

    QFile in(absoluteSourcePath);
    if (!in.open(QIODevice::ReadOnly)) {
        markFailed("WorkspaceReadFailed", "Failed to read source file for staging");
        return false;
    }

    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        in.close();
        markFailed("WorkspaceWriteFailed", "Failed to open temporary staging file");
        return false;
    }

    qint64 remaining = srcSize;
    while (remaining > 0) {
        const qint64 chunk =
            std::min(remaining, static_cast<qint64>(kStreamChunkBytes));
        const QByteArray buf = in.read(chunk);
        if (buf.size() != chunk) {
            in.close();
            out.close();
            QFile::remove(tmp);
            markFailed("WorkspaceReadFailed", "Short read while staging source file");
            return false;
        }
        if (out.write(buf) != buf.size()) {
            in.close();
            out.close();
            QFile::remove(tmp);
            markFailed("WorkspaceWriteFailed", "Failed to write temporary staging file");
            return false;
        }
        remaining -= chunk;
    }
    in.close();
    out.close();

    if (!removeWorkspaceEntry(relativePath, /*recordFailure=*/true)) {
        QFile::remove(tmp);
        return false;
    }
    if (!QFile::rename(tmp, absolute)) {
        QFile::remove(tmp);
        markFailed("WorkspaceRenameFailed", "Failed to atomically publish staged file");
        return false;
    }
    return true;
}

void GeometryRequestWorkspace::markFailed(std::string errorCode, std::string errorMessage)
{
    if (_failed) {
        return;
    }
    _failed = true;
    _failureCode = std::move(errorCode);
    _failureMessage = std::move(errorMessage);
    // Never leave a usable request.json after staging/serialization failure.
    removePublishedRequestBestEffort();
}

bool GeometryRequestWorkspace::publishRequestJson()
{
    if (_failed) {
        removePublishedRequestBestEffort();
        return false;
    }
    // Flush any opaque sections as sibling files when named like paths.
    for (const auto& section : _sections) {
        const QString rel = QString::fromStdString(section.first);
        if (!isTrustedRelativePath(rel)) {
            markFailed("UntrustedRelativePath", "Section name is not a trusted relative path");
            return false;
        }
        if (section.second.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            markFailed("SectionTooLarge", "Workspace section exceeds QByteArray size limits");
            return false;
        }
        QByteArray bytes;
        bytes.resize(static_cast<int>(section.second.size()));
        if (!section.second.empty()) {
            memcpy(bytes.data(), section.second.data(), section.second.size());
        }
        if (!writeFileAtomic(rel, bytes)) {
            return false;
        }
    }

    const QByteArray json = QJsonDocument(_request).toJson(QJsonDocument::Compact);
    return writeFileAtomic(QStringLiteral("request.json"), json);
}

} // namespace App
