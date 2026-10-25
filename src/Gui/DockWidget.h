// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QDockWidget>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QSizeGrip>

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

inline bool usesWslgX11()
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

// Qt creates floating tab groups itself; their member docks are neither
// top-level windows nor isFloating(). Include that private widget by its
// runtime type without depending on Qt's private headers.
inline bool isFloatingDockWindow(QWidget* widget)
{
    if (!widget->isWindow()) {
        return false;
    }
    auto* dock = qobject_cast<QDockWidget*>(widget);
    return (dock && dock->isFloating()) || widget->inherits("QDockWidgetGroupWindow");
}

class FloatingDockResizeGrip: public QSizeGrip
{
public:
    explicit FloatingDockResizeGrip(QWidget* window)
        : QSizeGrip(window)
        , window(window)
    {
        setObjectName(QStringLiteral("FreeCADFloatingDockResizeGrip"));
        setFixedSize(sizeHint().expandedTo(QSize(18, 18)));
        window->installEventFilter(this);
        updatePosition();
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        const bool filtered = QSizeGrip::eventFilter(object, event);
        if (window && object == window
            && (event->type() == QEvent::Show || event->type() == QEvent::Resize
                || event->type() == QEvent::WindowStateChange
                || event->type() == QEvent::ParentChange)) {
            updatePosition();
        }
        return filtered;
    }

private:
    void updatePosition()
    {
        if (!window) {
            return;
        }
        const bool available = isFloatingDockWindow(window) && window->windowType() == Qt::Window
            && window->windowFlags().testFlag(Qt::FramelessWindowHint)
            && !(window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen));
        if (available) {
            move(qMax(0, window->width() - width()), qMax(0, window->height() - height()));
            raise();
        }
        setVisible(available);
    }

    QPointer<QWidget> window;
};

inline bool normalizeFloatingDockWindow(QWidget* widget)
{
    if (!isFloatingDockWindow(widget)) {
        return false;
    }

    // Qt resets these flags when floating, restoring a layout, or replacing a
    // title bar. Show runs before the native window maps. Preserve client
    // geometry, including on platforms reporting margins for frameless docks.
    const bool normalized = widget->windowType() == Qt::Tool;
    if (normalized) {
        const QRect floatingGeometry = widget->geometry();
        widget->setWindowFlags(
            (widget->windowFlags() & ~Qt::WindowType_Mask & ~Qt::BypassWindowManagerHint) | Qt::Window
        );
        widget->setGeometry(floatingGeometry);
        widget->show();
    }
    // Managed frameless windows have no reliable native resize border on
    // WSLg. A standard Qt grip supplies a visible mouse target and system
    // resize handling without changing the dock layout or tab membership.
    if (widget->windowType() == Qt::Window
        && widget->windowFlags().testFlag(Qt::FramelessWindowHint)
        && !widget->findChild<QSizeGrip*>(QStringLiteral("FreeCADFloatingDockResizeGrip"),
                                         Qt::FindDirectChildrenOnly)) {
        new FloatingDockResizeGrip(widget);
    }
    return normalized;
}

class FloatingDockWindowEventFilter: public QObject
{
public:
    explicit FloatingDockWindowEventFilter(bool managedFloatingWindows = usesWslgX11())
        : managedFloatingWindows(managedFloatingWindows)
    {}

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (managedFloatingWindows && event->type() == QEvent::Show && object->isWidgetType()) {
            normalizeFloatingDockWindow(static_cast<QWidget*>(object));
        }
        return false;
    }

private:
    const bool managedFloatingWindows;
};

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
        if (managedFloatingWindows && event->type() == QEvent::Show) {
            normalizeFloatingDockWindow(this);
        }
        return QDockWidget::event(event);
    }

private:
    const bool managedFloatingWindows;
};

}  // namespace Gui
