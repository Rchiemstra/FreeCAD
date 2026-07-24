// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryProgressController.h"
#include <Base/Console.h>

namespace Gui
{

GeometryProgressController& GeometryProgressController::instance()
{
    static GeometryProgressController ctrl;
    return ctrl;
}

GeometryProgressController::GeometryProgressController(QObject* parent)
    : QObject(parent)
{
}

GeometryProgressController::~GeometryProgressController() = default;

void GeometryProgressController::onJobStarted(App::GeometryJobId id, const std::string& description)
{
    _activeJobId = id;
    _currentFraction = 0.0;
    _currentPhase = QString::fromStdString(description);
    Q_EMIT progressChanged(id, 0.0, _currentPhase);
}

void GeometryProgressController::onJobProgress(App::GeometryJobId id, double fraction, const std::string& phase)
{
    _activeJobId = id;
    _currentFraction = fraction;
    _currentPhase = QString::fromStdString(phase);
    Q_EMIT progressChanged(id, fraction, _currentPhase);
}

void GeometryProgressController::onJobFinished(App::GeometryJobId id, App::GeometryJobState state)
{
    if (_activeJobId == id) {
        _activeJobId = 0;
        _currentFraction = 1.0;
    }
    Q_EMIT progressFinished(id, state);
}

} // namespace Gui
