// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Gui/GuiExport.h>
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
    void cancelJob(App::CancelReason reason);
    bool isRunning() const;

    const App::DetachedGeometryResult& result() const { return _result; }

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

    QProcess* _process {nullptr};
    QTimer* _deadlineTimer {nullptr};
    QTimer* _cancelTimer {nullptr};
    App::GeometryJobSpec _spec;
    App::DetachedGeometryResult _result;
    App::GeometryJobState _state {App::GeometryJobState::Queued};
    QString _tempDir;
    QString _stdoutBuffer;
    bool _cancelling {false};
    int _cancelPhase {0};
};

} // namespace Gui
