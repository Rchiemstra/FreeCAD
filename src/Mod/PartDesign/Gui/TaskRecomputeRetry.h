// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <utility>

#include <QObject>
#include <QTimer>
#include <QVariant>

namespace PartDesignGui
{

// A Busy admission is temporary. Keep one retry per task action, and cancel
// it automatically when the task is destroyed (Cancel, resetEdit or close).
inline void retryTaskAction(QObject* task, const char* pending, std::function<void()> action)
{
    if (task->property(pending).toBool()) {
        return;
    }
    task->setProperty(pending, true);
    QTimer::singleShot(10, task, [task, pending, action = std::move(action)] {
        task->setProperty(pending, false);
        action();
    });
}

}  // namespace PartDesignGui
