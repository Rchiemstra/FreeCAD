// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Gui/GuiExport.h>
#include <App/GeometryJob.h>

#include <QObject>
#include <QString>

namespace Gui
{

class GuiExport GeometryProgressController : public QObject
{
    Q_OBJECT

public:
    explicit GeometryProgressController(QObject* parent = nullptr);
    ~GeometryProgressController() override;

    static GeometryProgressController& instance();

    void onJobStarted(App::GeometryJobId id, const std::string& description);
    void onJobProgress(App::GeometryJobId id, double fraction, const std::string& phase);
    void onJobFinished(App::GeometryJobId id, App::GeometryJobState state);

Q_SIGNALS:
    void progressChanged(App::GeometryJobId id, double fraction, const QString& phase);
    void progressFinished(App::GeometryJobId id, App::GeometryJobState state);

private:
    App::GeometryJobId _activeJobId {0};
    double _currentFraction {0.0};
    QString _currentPhase;
};

} // namespace Gui
