// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>
#include <QObject>
#include <QSize>

class QAbstractScrollArea;
class QEvent;
class QResizeEvent;

namespace Gui
{

// Deliveries that can flush the backing store's QPlatformTextureList. See
// GUIApplication::notify. Expose is any receiver: QWidgetWindow syncs from
// handleExposeEvent. Resize is counted only for a QWindow receiver:
// QWidgetWindow::handleResizeEvent calls syncBackingStore on that delivery.
// A QWidget Resize is not itself a flush. UpdateRequest and Paint are
// counted for QWidget receivers.
//
// The QWindow Resize is forwarded to the top-level widget before that sync.
// Qt 6.4.2 handleResizeEvent (qwidgetwindow.cpp) calls
// QGuiApplication::forwardEvent and then syncBackingStore (line 778, then
// 784 or 786). Qt 6.11 uses the same order (forward at 870, then sync at
// 876 or 878). The forward lays the viewport out before the texture list is
// built, so that resize must be delivered, not held. While that top-level
// QWidget Resize runs inside a counted QWindow Resize, that one scope is
// hidden and topLevelPaintFlushActive() is depth > hidden. A nested
// UpdateRequest or Paint still counts, and so does a Paint that was already
// active: QWidget::resize() from inside a paint also delivers a top-level
// widget Resize, but Qt has already stored the new rect, so it is not this
// forward. Hiding that Paint lets the viewport delete the wrapper the paint
// captured. After the forward returns, the window-Resize scope counts again
// for syncBackingStore, and a resize inside that sync is held.
// QWidgetWindow's own UpdateRequest calls QWidget::repaint(), which sends a
// widget UpdateRequest, and that delivery is the one that syncs.
//
// Not counted: QPlatformTextureListWatcher::onLockStatusChanged (Qt 6.11
// qwidgetrepaintmanager.cpp) calls sync() from the texture list's locked()
// signal. The xcb RHI compositor never locks that list. The OpenGL compositor
// used by eglfs unlocks from QOpenGLCompositor::handleRenderAllRequest, a
// timer callback, so that sync is outside the deliveries above. A resize that
// lands in that sync after the list is built, and not inside a nested counted
// delivery, is not held.
//
// The deleted object is the QRhiTexture wrapper. Qt 6.4 and 6.5 keep it across
// recreateFbo() (setPixelSize + createFrom). From Qt 6.6, destroyFbos() calls
// resetRhiDependentResources() and deletes it. The stale bind exists only on
// Qt >= 6.6. QWidgetRepaintManager::isDirty() includes
// dirtyRenderToTextureWidgets, so a resize outside a flush is safe: the next
// sync rebuilds the list.
//
// A synchronous top-level repaint() cannot repair a flush that is already
// running; Qt ignores it while paintAndFlush is active.
// WindowAboutToChangeInternal deletes the wrapper that list already holds.
//
// While a counted delivery is active the viewport resize is held. Pending
// guards are woken by a queued call when the outermost scope exits, not by a
// polling timer. The replay keeps the first held oldSize and uses the
// viewport's size at delivery. A resize that is delivered with no scope active
// cancels a pending replay, so a held size cannot replace a newer one.
//
// A modal loop entered from inside a counted delivery keeps the depth above
// zero for the whole dialog. A resize held in that session is delivered when
// the dialog returns, not while it is open.
void enterTopLevelPaintFlush();
void leaveTopLevelPaintFlush();
void enterWindowResizeScope(const QEvent* event);
void leaveWindowResizeScope();
// Hide the enclosing QWindow Resize while its forward runs. Nests.
void pushTopLevelPaintFlushBase(const QEvent* event);
void popTopLevelPaintFlushBase();
bool topLevelPaintFlushActive();

// One per GUIApplication::notify call. It counts the deliveries that contain
// a flush: UpdateRequest and Paint to a QWidget, Expose to any receiver (the
// QWidgetWindow syncs from handleExposeEvent), and Resize to a QWindow
// (handleResizeEvent syncs after forwarding the size). The forward itself is
// a Resize of the top-level QWidget carrying the same event object (Qt 6.4.2
// qwidgetwindow.cpp:778 then :784/:786; Qt 6.11 :870 then :876/:878); it
// hides that window scope, because the layout it runs is before the texture
// list exists. Other QWidget Resizes are not counted: they are the viewport
// events the guard may hold.
class PaintFlushEventScope
{
public:
    PaintFlushEventScope(QObject* receiver, QEvent* event);
    ~PaintFlushEventScope();
    PaintFlushEventScope(const PaintFlushEventScope&) = delete;
    PaintFlushEventScope& operator=(const PaintFlushEventScope&) = delete;

private:
    bool flush {false};
    bool windowResize {false};
    bool forward {false};
};

class GuiExport OpenGLViewportResizeGuard: public QObject
{
public:
    explicit OpenGLViewportResizeGuard(QAbstractScrollArea* view);
    ~OpenGLViewportResizeGuard() override;

    // True when the caller must not deliver this resize yet.
    bool interceptResize(QResizeEvent* event);
    void stop();

private:
    friend void leaveTopLevelPaintFlush();

    static void scheduleWake();
    void deliver();
    void clearPending();

    QAbstractScrollArea* view;
    QResizeEvent* deliveringEvent {nullptr};
    bool pending {false};
    QSize pendingOldSize;
};

}  // namespace Gui
