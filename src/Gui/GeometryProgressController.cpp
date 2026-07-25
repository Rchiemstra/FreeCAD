// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GeometryProgressController.h"
#include "ProgressBar.h"

#include <App/GeometryJobManager.h>
#include <App/GuiResponsivenessProbe.h>

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

void GeometryProgressController::installManagerHooks()
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.setProgressListener([](App::GeometryJobId id, double fraction, const std::string& phase) {
        GeometryProgressController::instance().onJobProgress(id, fraction, phase);
    });
    mgr.setStateListener([](App::GeometryJobId id, App::GeometryJobState state) {
        switch (state) {
            case App::GeometryJobState::Running:
                GeometryProgressController::instance().onJobStarted(id, "geometry");
                break;
            case App::GeometryJobState::Completed:
            case App::GeometryJobState::Failed:
            case App::GeometryJobState::Cancelled:
            case App::GeometryJobState::Crashed:
            case App::GeometryJobState::TimedOut:
                GeometryProgressController::instance().onJobFinished(id, state);
                break;
            default:
                break;
        }
    });
}

void GeometryProgressController::uninstallManagerHooks()
{
    App::GeometryJobManager::instance().clearProgressListeners();
}

void GeometryProgressController::onJobStarted(App::GeometryJobId id, const std::string& description)
{
    App::GuiResponsivenessProbe::ScopedCallback slice("GeometryProgressController::onJobStarted");
    _activeJobId = id;
    _currentFraction = 0.0;
    _currentPhase = QString::fromStdString(description);
    SequencerBar::instance()->setGeometryJobProgress(0.0, _currentPhase);
    Q_EMIT progressChanged(id, 0.0, _currentPhase);
}

void GeometryProgressController::onJobProgress(App::GeometryJobId id, double fraction, const std::string& phase)
{
    App::GuiResponsivenessProbe::ScopedCallback slice("GeometryProgressController::onJobProgress");
    if (_activeJobId != 0 && _activeJobId != id) {
        return;
    }
    _activeJobId = id;
    _currentFraction = fraction;
    if (!phase.empty()) {
        _currentPhase = QString::fromStdString(phase);
    }
    SequencerBar::instance()->setGeometryJobProgress(fraction, _currentPhase);
    Q_EMIT progressChanged(id, fraction, _currentPhase);
}

void GeometryProgressController::onJobFinished(App::GeometryJobId id, App::GeometryJobState state)
{
    App::GuiResponsivenessProbe::ScopedCallback slice("GeometryProgressController::onJobFinished");
    if (_activeJobId == id) {
        _activeJobId = 0;
        _currentFraction = 1.0;
        SequencerBar::instance()->clearGeometryJobProgress();
    }
    Q_EMIT progressFinished(id, state);
}

} // namespace Gui
