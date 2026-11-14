// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <QDockWidget>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QSizeGrip>
#include <QWindow>

namespace Gui
{

// Frameless dock dragging uses client coordinates even when the platform reports
// margins. Framed docks retain QWidget::move()'s window-frame positioning.
inline void moveFloatingDockWidget(QDockWidget* dock, const QPoint& clientPosition)
{
    if (dock->windowFlags().testFlag(Qt::FramelessWindowHint)) {
        QRect geometry = dock->geometry();
        geometry.moveTopLeft(clientPosition);
        dock->setGeometry(geometry);
    }
    else {
        dock->move(clientPosition);
    }
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

    void refresh()
    {
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
        const bool available = isFloatingDockWindow(window) && window->windowHandle()
            && window->windowHandle()->type() == Qt::Window
            && window->windowFlags().testFlag(Qt::FramelessWindowHint)
            && !window->windowFlags().testFlag(Qt::BypassWindowManagerHint)
            && !(window->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen));
        if (available) {
            move(qMax(0, window->width() - width()), qMax(0, window->height() - height()));
            raise();
        }
        setVisible(available);
    }

    QPointer<QWidget> window;
};

// WSLg records override_redirect when the X11 peer is created. Qt's temporary
// unmanaged drag peer cannot become a managed window merely by changing flags.
// Hide is delivered after Qt unmaps that peer. Retire it there and let Qt's
// subsequent drop/parent transition create the peer with its final flags.
inline void releaseUnmanagedFloatingDockWindow(QWidget* widget)
{
    if (!isFloatingDockWindow(widget) || widget->isVisible()
        || !widget->windowFlags().testFlag(Qt::BypassWindowManagerHint)) {
        return;
    }
    QWindow* native = widget->windowHandle();
    if (native && !native->isVisible() && native->handle()
        && native->flags().testFlag(Qt::BypassWindowManagerHint)) {
        native->destroy();
    }
}

inline bool normalizeFloatingDockWindow(QWidget* widget)
{
    if (!isFloatingDockWindow(widget)) {
        return false;
    }

    // Keep QWidget's Tool state intact: Qt's docking implementation owns its
    // drag transitions. Change only the platform window type before mapping,
    // without reparenting the widget or recreating its native surface. Preserve
    // the temporary Bypass flag too: the first drag map must already use the
    // normal platform type, before Qt completes the drop and removes Bypass.
    QWindow* native = widget->windowHandle();
    const bool normalized = native && widget->windowType() == Qt::Tool
        && native->type() == Qt::Tool;
    if (normalized) {
        native->setFlags((native->flags() & ~Qt::WindowType_Mask) | Qt::Window);
    }
    // Managed frameless windows have no reliable native resize border on
    // WSLg. A standard Qt grip supplies a visible mouse target and system
    // resize handling without changing the dock layout or tab membership.
    if (native && native->type() == Qt::Window
        && widget->windowFlags().testFlag(Qt::FramelessWindowHint)) {
        auto* grip = widget->findChild<QSizeGrip*>(QStringLiteral("FreeCADFloatingDockResizeGrip"),
                                                 Qt::FindDirectChildrenOnly);
        if (!grip) {
            new FloatingDockResizeGrip(widget);
        }
        else if (auto* floatingGrip = dynamic_cast<FloatingDockResizeGrip*>(grip)) {
            // The subclass normalizes after widget event filters have run.
            // Refresh a retained grip after that platform-type transition.
            floatingGrip->refresh();
        }
    }
    return normalized;
}

class FloatingDockWindowEventFilter: public QObject
{
public:
    explicit FloatingDockWindowEventFilter(QWidget* dockOwner,
                                           bool managedFloatingWindows = usesWslgX11())
        : dockOwner(dockOwner)
        , managedFloatingWindows(managedFloatingWindows)
    {}

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (managedFloatingWindows
            && (event->type() == QEvent::Show || event->type() == QEvent::Hide)
            && object->isWidgetType()) {
            auto* widget = static_cast<QWidget*>(object);
            // The application also hosts dialogs and independent main windows.
            // Only normalize docks belonging to this manager, including Qt's
            // floating tab groups. A destroyed owner must disable the filter.
            for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) {
                // QWidget::isAncestorOf stops at a window boundary, whereas
                // floating docks remain QObject children of their main window.
                if (dockOwner && parent == dockOwner) {
                    if (event->type() == QEvent::Hide) {
                        releaseUnmanagedFloatingDockWindow(widget);
                    }
                    else {
                        normalizeFloatingDockWindow(widget);
                    }
                    break;
                }
            }
        }
        return false;
    }

private:
    QPointer<QWidget> dockOwner;
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
        if (managedFloatingWindows) {
            if (event->type() == QEvent::Hide) {
                releaseUnmanagedFloatingDockWindow(this);
            }
            else if (event->type() == QEvent::Show) {
                normalizeFloatingDockWindow(this);
            }
        }
        return QDockWidget::event(event);
    }

private:
    const bool managedFloatingWindows;
};

}  // namespace Gui
