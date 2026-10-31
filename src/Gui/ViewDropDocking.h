// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QKeyEvent>
#include <QLibrary>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPointer>
#include <QRubberBand>
#include <QTimer>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Gui
{

namespace ViewDropDockingInternal
{
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
struct X11Pointer
{
    using Window = unsigned long;
    using RootWindow = Window (*)(Display*);
    using QueryPointer = int (*)(
        Display*, Window, Window*, Window*, int*, int*, int*, int*, unsigned int*
    );
    using QueryTree = int (*)(Display*, Window, Window*, Window*, Window**, unsigned int*);
    using Free = int (*)(void*);

    struct State
    {
        Display* display;
        Window root;
        Window child;
        unsigned int mask;
    };

    std::optional<State> query() const
    {
        auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        if (native && rootWindow && queryPointer) {
            State state {native->display(), 0, 0, 0};
            int rootX = 0, rootY = 0, localX = 0, localY = 0;
            if (queryPointer(state.display, rootWindow(state.display), &state.root, &state.child,
                             &rootX, &rootY, &localX, &localY, &state.mask)) {
                return state;
            }
        }
        return std::nullopt;
    }

    bool ownsFrame(QWidget* widget) const
    {
        const auto state = query();
        if (!state || !queryTree || !freeMemory) {
            return false;
        }
        Window window = widget->winId();
        for (int depth = 0; window && window != state->root && depth < 16; ++depth) {
            if (window == state->child) {
                return true;
            }
            Window root = 0, parent = 0, *children = nullptr;
            unsigned int count = 0;
            const bool found = queryTree(state->display, window, &root, &parent, &children, &count);
            if (children) {
                freeMemory(children);
            }
            if (!found) {
                return false;
            }
            window = parent;
        }
        return false;
    }

    QLibrary library {QStringLiteral("X11"), 6};
    const RootWindow rootWindow =
        reinterpret_cast<RootWindow>(library.resolve("XDefaultRootWindow"));
    const QueryPointer queryPointer =
        reinterpret_cast<QueryPointer>(library.resolve("XQueryPointer"));
    const QueryTree queryTree = reinterpret_cast<QueryTree>(library.resolve("XQueryTree"));
    const Free freeMemory = reinterpret_cast<Free>(library.resolve("XFree"));
};

inline X11Pointer& x11Pointer()
{
    static X11Pointer pointer;
    return pointer;
}
#endif
}  // namespace ViewDropDockingInternal

// A window manager owns title-bar drags. Qt's mouseButtons() can therefore
// retain its pre-drag state, including "released" while the button is held.
// Query native state only during an observed move, never from an idle timer.
inline std::optional<bool> viewDragLeftButtonDown()
{
#ifdef Q_OS_WIN
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
#elif defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    if (const auto state = ViewDropDockingInternal::x11Pointer().query()) {
        return (state->mask & (1U << 8)) != 0;  // X11 Button1Mask
    }
#endif
    // Other backends can use explicit non-client press/release events. An
    // unavailable native query is not evidence that the user released a drag.
    return std::nullopt;
}

inline bool viewDragOwnsNativeFrame(QWidget* view, const QPoint& point)
{
    if (point != QCursor::pos() || !view->frameGeometry().contains(point)) {
        return false;
    }
#ifdef Q_OS_WIN
    POINT nativePoint;
    return GetCursorPos(&nativePoint)
        && GetAncestor(WindowFromPoint(nativePoint), GA_ROOT)
        == reinterpret_cast<HWND>(view->winId());
#elif defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    return ViewDropDockingInternal::x11Pointer().ownsFrame(view);
#else
    return false;
#endif
}

// Independent of document/model state. The callbacks identify the movable view
// and dock that exact view through its existing mode-change path.
class ViewDropDocking: public QObject
{
public:
    using Eligible = std::function<bool(QWidget*)>;
    using Dock = std::function<void(QWidget*)>;
    using ButtonQuery = std::function<std::optional<bool>()>;
    using PositionQuery = std::function<QPoint()>;

    ViewDropDocking(QWidget* target, Eligible eligible, Dock dock, QObject* parent,
                    ButtonQuery buttonQuery = viewDragLeftButtonDown,
                    PositionQuery positionQuery = []() { return QCursor::pos(); })
        : QObject(parent)
        , target(target)
        , eligible(std::move(eligible))
        , dock(std::move(dock))
        , buttonQuery(std::move(buttonQuery))
        , positionQuery(std::move(positionQuery))
    {
        setObjectName(QStringLiteral("FreeCADViewDropDocking"));
        timer.setInterval(40);
        connect(&timer, &QTimer::timeout, this, [this]() { poll(); });
        qApp->installEventFilter(this);
    }

    ~ViewDropDocking() override
    {
        delete hint;
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            abortDrag();
        }
        if (object == movingView
            && (event->type() == QEvent::Resize || event->type() == QEvent::Hide
                || event->type() == QEvent::Close || event->type() == QEvent::Destroy
                || event->type() == QEvent::ParentChange
                || event->type() == QEvent::WindowStateChange)) {
            abortDrag();
        }
        if (target && (object == target || object == target->window())
            && (event->type() == QEvent::Hide || event->type() == QEvent::Close)) {
            cancel();
        }

        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget) {
            return false;
        }
        if (event->type() == QEvent::NonClientAreaMouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && available(widget)
                && onTitleBar(widget, mouse->globalPosition().toPoint())) {
                ignoreUntilRelease = false;
                begin(widget, widget->pos());
            }
        }
        else if (event->type() == QEvent::Move && available(widget)) {
            const auto left = buttonQuery();
            if (!ignoreUntilRelease && left.value_or(false)
                && onTitleBar(widget, positionQuery())) {
                if (movingView != widget) {
                    begin(widget, static_cast<QMoveEvent*>(event)->oldPos());
                }
                updateHint();
            }
        }
        else if (event->type() == QEvent::NonClientAreaMouseButtonRelease
                 || event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && buttonQuery() != true) {
                if (ignoreUntilRelease) {
                    ignoreUntilRelease = false;
                    cancel();
                }
                else if (object == movingView) {
                    finish(mouse->globalPosition().toPoint());
                }
            }
        }
        return false;
    }

private:
    bool available(QWidget* view) const
    {
        return view && target && view->isWindow() && view->isVisible()
            && !view->isMinimized() && !view->isMaximized() && !view->isFullScreen()
            && target->isVisible() && !target->window()->isMinimized()
            && !QApplication::activeModalWidget() && eligible(view);
    }

    static bool onTitleBar(QWidget* view, const QPoint& point)
    {
        // Exclude client navigation and the left/right resize borders. Both
        // rectangles use Qt logical global coordinates, including mixed DPI.
        const QRect client(view->mapToGlobal(QPoint()), view->size());
        const QRect frame = view->frameGeometry();
        return point.x() >= client.left() && point.x() <= client.right()
            && point.y() >= frame.top() && point.y() < client.top();
    }

    bool overTarget(const QPoint& point, QWidget* view) const
    {
        if (!available(view) || !target->rect().contains(target->mapFromGlobal(point))) {
            return false;
        }
        // Do not dock through another application, a dialog or a floating dock.
        auto* top = QApplication::topLevelAt(point);
        // On X11/WSLg the native title bar can have no Qt widget at all.
        // Admit it only after verifying ownership of the native frame; a
        // geometric overlap alone could dock through another application.
        return top == view || top == target->window()
            || (!top && viewDragOwnsNativeFrame(view, point));
    }

    bool movedEnough() const
    {
        return movingView && movingView->size() == dragSize
            && (movingView->pos() - startPosition).manhattanLength()
            >= QApplication::startDragDistance();
    }

    void begin(QWidget* view, const QPoint& position)
    {
        cancel();
        movingView = view;
        startPosition = position;
        dragSize = view->size();
        timer.start();
    }

    void cancel()
    {
        ++generation;
        timer.stop();
        movingView.clear();
        if (hint) {
            hint->hide();
        }
    }

    void abortDrag()
    {
        const bool wasMoving = movingView || ignoreUntilRelease;
        cancel();
        if (wasMoving) {
            ignoreUntilRelease = true;
            timer.start();
        }
    }

    void updateHint()
    {
        const bool show = movedEnough() && overTarget(positionQuery(), movingView);
        if (show && !hint) {
            hint = new QRubberBand(QRubberBand::Rectangle, target);
            hint->setObjectName(QStringLiteral("FreeCADViewDropTarget"));
            hint->setAttribute(Qt::WA_TransparentForMouseEvents);
        }
        if (hint) {
            hint->setGeometry(target->rect().adjusted(4, 4, -4, -4));
            hint->setVisible(show);
            if (show) {
                hint->raise();
            }
        }
    }

    void poll()
    {
        if (ignoreUntilRelease) {
            if (buttonQuery() == false) {
                ignoreUntilRelease = false;
                cancel();
            }
            return;
        }
        if (!available(movingView) || movingView->size() != dragSize) {
            cancel();
            return;
        }
        const auto left = buttonQuery();
        if (left && !*left) {
            finish(positionQuery());
        }
        else {
            updateHint();
        }
    }

    void finish(const QPoint& point)
    {
        QPointer<QWidget> view = movingView;
        const bool shouldDock = movedEnough() && overTarget(point, view);
        const QPoint releasedPosition = view ? view->pos() : QPoint();
        const QSize releasedSize = dragSize;
        cancel();
        if (!shouldDock) {
            return;
        }
        const auto ticket = generation;
        // Reparent/clone after the native move/release handler has unwound.
        QTimer::singleShot(0, this, [this, view, point, releasedPosition, releasedSize, ticket]() {
            if (generation == ticket && view && view->pos() == releasedPosition
                && view->size() == releasedSize && buttonQuery() != true
                && overTarget(point, view)) {
                dock(view);
            }
        });
    }

    QPointer<QWidget> target;
    QPointer<QWidget> movingView;
    QPointer<QRubberBand> hint;
    Eligible eligible;
    Dock dock;
    ButtonQuery buttonQuery;
    PositionQuery positionQuery;
    QTimer timer;
    QPoint startPosition;
    QSize dragSize;
    unsigned int generation {0};
    bool ignoreUntilRelease {false};
};

}  // namespace Gui
