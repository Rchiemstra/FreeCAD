// SPDX-License-Identifier: LGPL-2.1-or-later

#include "GuiResponsivenessProbe.h"
#include "MainThreadSignal.h"

#include <Base/Console.h>

#include <stdexcept>

namespace App
{

GuiResponsivenessProbe& GuiResponsivenessProbe::instance()
{
    static GuiResponsivenessProbe probe;
    return probe;
}

GuiResponsivenessProbe::GuiResponsivenessProbe(QObject* parent)
    : QObject(parent)
{
    _timer = new QTimer(this);
    _timer->setTimerType(Qt::PreciseTimer);
    connect(_timer, &QTimer::timeout, this, &GuiResponsivenessProbe::onHeartbeat);
}

GuiResponsivenessProbe::~GuiResponsivenessProbe()
{
    stop();
}

void GuiResponsivenessProbe::start(std::chrono::milliseconds interval)
{
    assertDocumentThread("GuiResponsivenessProbe::start");
    resetStats();
    _lastBeat.start();
    _running = true;
    _timer->start(static_cast<int>(interval.count()));
}

void GuiResponsivenessProbe::stop()
{
    if (_timer) {
        _timer->stop();
    }
    _running = false;
}

bool GuiResponsivenessProbe::isRunning() const
{
    return _running;
}

void GuiResponsivenessProbe::recordCallbackDuration(double durationMs)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (durationMs > _maxCallbackDurationMs) {
        _maxCallbackDurationMs = durationMs;
    }
}

GuiResponsivenessProbe::ScopedCallback::ScopedCallback(const char* label)
    : _label(label)
{
    _timer.start();
}

GuiResponsivenessProbe::ScopedCallback::~ScopedCallback()
{
    const double ms = static_cast<double>(_timer.nsecsElapsed()) / 1.0e6;
    GuiResponsivenessProbe::instance().recordCallbackDuration(ms);
    if (ms > 33.0) {
        Base::Console().warning(
            "GuiResponsivenessProbe: callback '%s' took %.1f ms (budget 33 ms)\n",
            _label ? _label : "(unnamed)",
            ms);
    }
}

double GuiResponsivenessProbe::maxHeartbeatGapMs() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _maxHeartbeatGapMs;
}

double GuiResponsivenessProbe::maxCallbackDurationMs() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _maxCallbackDurationMs;
}

std::uint64_t GuiResponsivenessProbe::heartbeatCount() const
{
    return _heartbeatCount.load();
}

void GuiResponsivenessProbe::resetStats()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _maxHeartbeatGapMs = 0.0;
    _maxCallbackDurationMs = 0.0;
    _heartbeatCount.store(0);
}

bool GuiResponsivenessProbe::meetsAcceptanceBudgets(double maxGapMs, double maxSliceMs) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _maxHeartbeatGapMs <= maxGapMs && _maxCallbackDurationMs <= maxSliceMs;
}

void GuiResponsivenessProbe::onHeartbeat()
{
    const double gapMs = static_cast<double>(_lastBeat.nsecsElapsed()) / 1.0e6;
    _lastBeat.restart();
    _heartbeatCount.fetch_add(1);

    std::lock_guard<std::mutex> lock(_mutex);
    if (gapMs > _maxHeartbeatGapMs) {
        _maxHeartbeatGapMs = gapMs;
    }
}

void assertDocumentThread(const char* context)
{
    if (!MainThreadSignalConfig::hasHooks()) {
        return;
    }
    if (!MainThreadSignalConfig::isMainThread()) {
        throw std::runtime_error(
            std::string("Document-thread affinity violation in ") + (context ? context : "(unknown)"));
    }
}

} // namespace App
