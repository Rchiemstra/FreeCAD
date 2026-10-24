// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QDockWidget>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>

namespace Gui
{

// QWidget::move() positions the window frame. Dock dragging works with client
// coordinates, including for frameless windows whose platform reports margins.
inline void moveFloatingDockWidget(QDockWidget* dock, const QPoint& clientPosition)
{
    QRect geometry = dock->geometry();
    geometry.moveTopLeft(clientPosition);
    dock->setGeometry(geometry);
}

// Keep floating docks managed by WSLg. Its X11 window manager loses input and
// reports incorrect coordinates for Qt::Tool windows (microsoft/wslg#1153).
class DockWidget: public QDockWidget
{
public:
    explicit DockWidget(QWidget* parent = nullptr)
        : DockWidget(parent, usesWslgX11())
    {}

    DockWidget(QWidget* parent, bool managedFloatingWindows)
        : QDockWidget(parent)
        , managedFloatingWindows(managedFloatingWindows)
    {}

protected:
    bool event(QEvent* event) override
    {
        if (managedFloatingWindows && event->type() == QEvent::Show && windowType() == Qt::Tool) {
            // Qt resets these flags when floating, restoring a layout, or
            // replacing a title bar. Show runs before the native window maps.
            const QRect floatingGeometry = geometry();
            setWindowFlags(
                (windowFlags() & ~Qt::WindowType_Mask & ~Qt::BypassWindowManagerHint) | Qt::Window
            );
            setGeometry(floatingGeometry);
            show();
        }
        return QDockWidget::event(event);
    }

private:
    static bool usesWslgX11()
    {
#ifdef Q_OS_LINUX
        const auto display = qgetenv("DISPLAY");
        return QGuiApplication::platformName() == QLatin1String("xcb")
            && (display == ":0" || display == ":0.0" || display == "unix:0"
                || display == "unix:0.0")
            && QFileInfo::exists(QStringLiteral("/mnt/wslg/.X11-unix/X0"));
#else
        return false;
#endif
    }

    const bool managedFloatingWindows;
};

}  // namespace Gui
