// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OpenGLViewportResize.h"

#include <algorithm>
#include <vector>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLWidget>
#include <QPointer>
#include <QResizeEvent>
#include <QWidget>
#include <QWindow>

namespace Gui
{
namespace
{
int paintFlushDepth = 0;
// Counted QWindow Resize deliveries, innermost last. handleResizeEvent
// forwards that same event object to the top-level QWidget before
// syncBackingStore; only that forward is before the texture list.
std::vector<const QEvent*> windowResizeEvents;
// How many of those window-Resize scopes are hidden by an active forward.
int forwardSuppress = 0;
// 1 if that forward hid a scope, so pop does not drop someone else's hide.
std::vector<int> forwardSuppressStack;
bool wakeQueued = false;
std::vector<QPointer<OpenGLViewportResizeGuard>> pendingGuards;

void notePending(OpenGLViewportResizeGuard* self)
{
    for (const QPointer<OpenGLViewportResizeGuard>& guard : pendingGuards) {
        if (guard.data() == self) {
            return;
        }
    }
    pendingGuards.emplace_back(self);
}

void dropPending(OpenGLViewportResizeGuard* self)
{
    std::erase_if(pendingGuards, [self](const QPointer<OpenGLViewportResizeGuard>& guard) {
        return guard.isNull() || guard.data() == self;
    });
}
}  // namespace

void enterTopLevelPaintFlush()
{
    ++paintFlushDepth;
}

void leaveTopLevelPaintFlush()
{
    if (paintFlushDepth <= 0) {
        return;
    }
    --paintFlushDepth;
    if (paintFlushDepth == 0) {
        OpenGLViewportResizeGuard::scheduleWake();
    }
}

void enterWindowResizeScope(const QEvent* event)
{
    windowResizeEvents.push_back(event);
}

void leaveWindowResizeScope()
{
    if (!windowResizeEvents.empty()) {
        windowResizeEvents.pop_back();
    }
}

void pushTopLevelPaintFlushBase(const QEvent* event)
{
    // Hide the innermost window-Resize scope only for its own forward: the
    // same QResizeEvent object delivered to the top-level widget. Any other
    // top-level Resize (QWidget::resize() from a paint, from aboutToCompose
    // during the sync, or for another window) is not before the texture
    // list and must keep the scope counted.
    const bool hideWindowScope =
        !windowResizeEvents.empty() && windowResizeEvents.back() == event;
    forwardSuppressStack.push_back(hideWindowScope ? 1 : 0);
    if (hideWindowScope) {
        ++forwardSuppress;
    }
}

void popTopLevelPaintFlushBase()
{
    if (forwardSuppressStack.empty()) {
        return;
    }
    if (forwardSuppressStack.back() != 0 && forwardSuppress > 0) {
        --forwardSuppress;
    }
    forwardSuppressStack.pop_back();
}

bool topLevelPaintFlushActive()
{
    return paintFlushDepth > forwardSuppress;
}

PaintFlushEventScope::PaintFlushEventScope(QObject* receiver, QEvent* event)
{
    const QEvent::Type type = event->type();
    windowResize = type == QEvent::Resize && qobject_cast<QWindow*>(receiver);
    flush = windowResize || type == QEvent::Expose
        || ((type == QEvent::UpdateRequest || type == QEvent::Paint) && receiver->isWidgetType());
    if (type == QEvent::Resize && !windowResize && receiver->isWidgetType()) {
        forward = static_cast<QWidget*>(receiver)->isWindow();
    }
    if (flush) {
        enterTopLevelPaintFlush();
    }
    if (windowResize) {
        enterWindowResizeScope(event);
    }
    if (forward) {
        pushTopLevelPaintFlushBase(event);
    }
}

PaintFlushEventScope::~PaintFlushEventScope()
{
    if (forward) {
        popTopLevelPaintFlushBase();
    }
    if (windowResize) {
        leaveWindowResizeScope();
    }
    if (flush) {
        leaveTopLevelPaintFlush();
    }
}

void OpenGLViewportResizeGuard::scheduleWake()
{
    if (wakeQueued || pendingGuards.empty() || !qApp) {
        return;
    }
    wakeQueued = true;
    const bool invoked = QMetaObject::invokeMethod(
        qApp,
        [] {
            wakeQueued = false;
            // A counted delivery is still on the stack (a nested loop drained
            // this call early, or this woke during a window Resize whose
            // forward has hidden that scope). Its leave queues another wake.
            // Hiding the window scope is not enough: syncBackingStore runs
            // after the forward, still inside the QWindow Resize. Do not poll.
            if (paintFlushDepth > 0) {
                return;
            }
            std::vector<QPointer<OpenGLViewportResizeGuard>> guards;
            guards.swap(pendingGuards);
            for (const QPointer<OpenGLViewportResizeGuard>& guard : guards) {
                if (guard) {
                    guard->deliver();
                }
            }
        },
        Qt::QueuedConnection
    );
    if (!invoked) {
        wakeQueued = false;
    }
}

OpenGLViewportResizeGuard::OpenGLViewportResizeGuard(QAbstractScrollArea* view)
    : QObject(view)
    , view(view)
{}

OpenGLViewportResizeGuard::~OpenGLViewportResizeGuard()
{
    stop();
}

void OpenGLViewportResizeGuard::stop()
{
    pending = false;
    deliveringEvent = nullptr;
    dropPending(this);
}

void OpenGLViewportResizeGuard::clearPending()
{
    pending = false;
    dropPending(this);
}

bool OpenGLViewportResizeGuard::interceptResize(QResizeEvent* event)
{
    if (!event || !view || event == deliveringEvent) {
        return false;
    }
    // A plain QWidget viewport does not take the framebuffer path. From Qt 6.6
    // the QOpenGLWidget path deletes its QRhiTexture wrapper in destroyFbos();
    // 6.4 and 6.5 reuse the wrapper, so holding is unnecessary there but harmless.
    if (!qobject_cast<QOpenGLWidget*>(view->viewport())) {
        clearPending();
        return false;
    }
    if (!topLevelPaintFlushActive()) {
        // This resize is really delivered. Drop any held replay so it cannot
        // run afterwards and put the Coin viewport / scene rect back.
        clearPending();
        return false;
    }
    // Several resizes can nest in one flush. Keep the first oldSize; the
    // widget geometry is already the latest size and the replay reads it then.
    if (!pending) {
        pendingOldSize = event->oldSize();
        pending = true;
    }
    notePending(this);
    return true;
}

void OpenGLViewportResizeGuard::deliver()
{
    // A viewport filter can delete the view during sendEvent or repaint. The
    // guard is a child of the view, so both QPointers go null and this must
    // not touch members after that.
    const QPointer<OpenGLViewportResizeGuard> self(this);
    const QPointer<QAbstractScrollArea> area(view);
    if (!self || !area || !pending) {
        return;
    }
    if (topLevelPaintFlushActive()) {
        notePending(this);
        return;
    }

    const QSize oldSize = pendingOldSize;
    pending = false;
    QWidget* viewport = area->viewport();
    if (!viewport) {
        return;
    }
    QResizeEvent event(viewport->size(), oldSize);
    QResizeEvent* const previousEvent = deliveringEvent;
    deliveringEvent = &event;
    QApplication::sendEvent(viewport, &event);
    if (!self || !area) {
        return;
    }
    deliveringEvent = previousEvent;
    if (pending || topLevelPaintFlushActive()) {
        notePending(this);
        return;
    }
    if (QWidget* top = area->window()) {
        // resizeViewportFramebuffer already called update(). A synchronous
        // repaint while this window is in a paint event recurses into
        // paintAndFlush (the nested window-resize case).
        if (!QCoreApplication::closingDown() && !top->testAttribute(Qt::WA_WState_InPaintEvent)) {
            top->repaint();
        }
    }
    if (!self || !area) {
        return;
    }
}

}  // namespace Gui
