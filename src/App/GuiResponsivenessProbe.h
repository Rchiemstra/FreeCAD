// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace App
{

/**
 * @brief 10 ms Qt heartbeat and GUI-callback duration probe for responsiveness contracts.
 *
 * Acceptance targets from the non-blocking geometry plan:
 * - instrumented GUI slices <= 33 ms
 * - heartbeat gap <= 100 ms
 */
class AppExport GuiResponsivenessProbe : public QObject
{
    Q_OBJECT

public:
    static GuiResponsivenessProbe& instance();

    explicit GuiResponsivenessProbe(QObject* parent = nullptr);
    ~GuiResponsivenessProbe() override;

    void start(std::chrono::milliseconds interval = std::chrono::milliseconds(10));
    void stop();
    bool isRunning() const;

    /** Record a GUI-thread slice duration (ms). */
    void recordCallbackDuration(double durationMs);

    /** RAII helper that records the scope duration when destroyed. */
    class AppExport ScopedCallback
    {
    public:
        explicit ScopedCallback(const char* label = nullptr);
        ~ScopedCallback();

        ScopedCallback(const ScopedCallback&) = delete;
        ScopedCallback& operator=(const ScopedCallback&) = delete;

    private:
        QElapsedTimer _timer;
        const char* _label {nullptr};
    };

    double maxHeartbeatGapMs() const;
    double maxCallbackDurationMs() const;
    std::uint64_t heartbeatCount() const;
    void resetStats();

    /** True when max heartbeat gap and max callback duration meet plan budgets. */
    bool meetsAcceptanceBudgets(double maxGapMs = 100.0, double maxSliceMs = 33.0) const;

private Q_SLOTS:
    void onHeartbeat();

private:
    QTimer* _timer {nullptr};
    QElapsedTimer _lastBeat;
    mutable std::mutex _mutex;
    double _maxHeartbeatGapMs {0.0};
    double _maxCallbackDurationMs {0.0};
    std::atomic<std::uint64_t> _heartbeatCount {0};
    bool _running {false};
};

/** Assert current thread is the document/GUI owning thread when hooks are installed. */
AppExport void assertDocumentThread(const char* context);

} // namespace App
