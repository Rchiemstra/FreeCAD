// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>
#include <App/GeometryJob.h>

#include <QObject>
#include <QProcess>
#include <QTimer>

#include <memory>
#include <string>

namespace Gui
{

class GuiExport GeometryWorkerProcess : public QObject
{
    Q_OBJECT

public:
    explicit GeometryWorkerProcess(QObject* parent = nullptr);
    ~GeometryWorkerProcess() override;

    bool startJob(const App::GeometryJobSpec& spec);
    /// Prefer manager-owned random workspace; falls back to a local cache dir if empty.
    bool startJob(const App::GeometryJobSpec& spec, const QString& workspaceDir);
    void cancelJob(App::CancelReason reason);
    bool isRunning() const;

    const App::DetachedGeometryResult& result() const { return _result; }

    /// Register this process controller as GeometryJobManager's FreeCADCmd backend.
    static void installManagerBackend();
    static void uninstallManagerBackend();

Q_SIGNALS:
    void progressUpdated(double fraction, const QString& phase);
    void jobFinished(App::GeometryJobId id, App::GeometryJobState state, const App::DetachedGeometryResult& result);

private Q_SLOTS:
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onTimeout();
    void onCooperativeCancelTimeout();

private:
    void processLine(const QString& line);
    void cleanupWorkspace();
    /// Resolve and verify a child-reported result path/size/digest under the workspace.
    bool acceptTrustedResult(const QString& relativePath,
                             qint64 claimedSize,
                             const QString& claimedSha256,
                             std::string& errorCode,
                             std::string& errorMessage);

    QProcess* _process {nullptr};
    QTimer* _deadlineTimer {nullptr};
    QTimer* _cancelTimer {nullptr};
    App::GeometryJobSpec _spec;
    App::DetachedGeometryResult _result;
    App::GeometryJobState _state {App::GeometryJobState::Queued};
    QString _tempDir;
    QString _stdoutBuffer;
    QString _claimedResultPath;
    QString _claimedSha256;
    qint64 _claimedResultSize {-1};
    bool _resultMessageSeen {false};
    bool _cancelling {false};
    int _cancelPhase {0};
    bool _retainWorkspaceOnDestroy {false};
};

} // namespace Gui
