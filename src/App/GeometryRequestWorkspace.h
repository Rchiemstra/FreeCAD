// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>
#include <App/GeometryJob.h>

#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace App
{

/**
 * Concrete GeometryArchiveWriter that stages named sections under a workspace
 * and builds the typed request.json envelope. Archives are written via .tmp
 * rename before request.json is published.
 */
class AppExport GeometryRequestWorkspace : public GeometryArchiveWriter
{
public:
    explicit GeometryRequestWorkspace(QString workspaceDir);

    void writeSection(const std::string& name, const std::vector<uint8_t>& data) override;
    void writeString(const std::string& name, const std::string& value) override;
    void writeBytes(const std::string& name, const uint8_t* data, size_t size) override;

    /// Write @p relativePath under the workspace atomically (.tmp → final).
    bool writeFileAtomic(const QString& relativePath, const QByteArray& bytes);

    /// Copy an existing file into the workspace under @p relativePath atomically.
    bool stageFileAtomic(const QString& relativePath, const QString& absoluteSourcePath);

    QJsonObject& requestObject()
    {
        return _request;
    }
    const QJsonObject& requestObject() const
    {
        return _request;
    }

    QString workspaceDir() const
    {
        return _workspaceDir;
    }

    /// Publish request.json last, after all staged files exist.
    /// Returns false if a prior staging failure was recorded or the write fails.
    bool publishRequestJson();

    /// Remove any previously published request.json (reused-workspace safety).
    /// Missing target is success; an existing entry that cannot be removed is failure.
    bool clearPublishedRequest();

    /// Best-effort removal after a recorded failure (does not change failure state).
    void removePublishedRequestBestEffort();

    /// Remove a staged workspace file if present; fail when it exists but cannot be removed.
    bool clearStagedFile(const QString& relativePath);

    /// Record a staging/serialization failure; blocks publishRequestJson() and clears
    /// any published request.json. Orphaned `*.tmp` files are left for the janitor.
    void markFailed(std::string errorCode, std::string errorMessage);

    bool hasFailed() const
    {
        return _failed;
    }
    const std::string& failureCode() const
    {
        return _failureCode;
    }
    const std::string& failureMessage() const
    {
        return _failureMessage;
    }

    static bool isTrustedRelativePath(const QString& relativePath);

    /// Maximum bytes for a single staged section (matches trusted result cap).
    static constexpr size_t maxWorkspaceSectionBytes()
    {
        return 512ull * 1024ull * 1024ull;
    }

private:
    bool removeWorkspaceEntry(const QString& relativePath, bool recordFailure);

    QString _workspaceDir;
    QJsonObject _request;
    std::unordered_map<std::string, std::vector<uint8_t>> _sections;
    bool _failed {false};
    std::string _failureCode;
    std::string _failureMessage;
};

} // namespace App
