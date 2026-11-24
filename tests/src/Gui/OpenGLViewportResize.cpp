// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QOpenGLWidget>
#include <QResizeEvent>

#include <Gui/OpenGLViewportResize.h>

namespace
{
class ResizeEvents: public QObject
{
public:
    ResizeEvents(QWidget* viewport, Gui::OpenGLViewportResizeGuard& guard)
        : viewport(viewport), guard(guard)
    {}

    int wakes {0};
    int held {0};
    int delivered {0};
    QSize deliveredSize;

    bool eventFilter(QObject* receiver, QEvent* event) override
    {
        if (receiver == qApp && event->type() == QEvent::MetaCall) {
            ++wakes;
        }
        if (receiver == viewport && event->type() == QEvent::Resize) {
            auto* resize = static_cast<QResizeEvent*>(event);
            if (guard.interceptResize(resize)) {
                ++held;
            }
            else {
                ++delivered;
                deliveredSize = resize->size();
            }
            // Count the guard's real sendEvent without requiring an OpenGL
            // context. The Python GUI regression checks framebuffer behavior.
            return true;
        }
        return false;
    }

private:
    QWidget* viewport;
    Gui::OpenGLViewportResizeGuard& guard;
};

class OpenGLViewportResizeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "ViewDropDocking_tests_run";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
    }

    void SetUp() override
    {
        QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    }
};
}  // namespace

TEST_F(OpenGLViewportResizeTest, heldPaintDoesNotRepostQueuedWake)
{
    QAbstractScrollArea area;
    auto* viewport = new QOpenGLWidget;
    area.setViewport(viewport);
    Gui::OpenGLViewportResizeGuard guard(&area);
    ResizeEvents events(viewport, guard);
    qApp->installEventFilter(&events);
    QEvent paint(QEvent::Paint);
    QResizeEvent resize(viewport->size(), QSize(10, 10));
    {
        Gui::PaintFlushEventScope flush(&area, &paint);
        QApplication::sendEvent(viewport, &resize);
        ASSERT_EQ(events.held, 1);
        ASSERT_EQ(events.delivered, 0);
    } // Queues the one wake for the held resize.

    {
        // A new paint can start before that queued wake is delivered. A nested
        // loop must consume that wake once and leave retry to flush completion.
        Gui::PaintFlushEventScope nested(&area, &paint);
        QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
        ASSERT_EQ(events.wakes, 1);
        for (int turn = 0; turn < 5; ++turn) {
            QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
            EXPECT_EQ(events.wakes, 1) << "resize wake reposted during a held paint";
            EXPECT_EQ(events.delivered, 0);
        }
    }

    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    EXPECT_EQ(events.wakes, 2);
    EXPECT_EQ(events.delivered, 1);
    EXPECT_EQ(events.deliveredSize, viewport->size());
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    EXPECT_EQ(events.wakes, 2);
    EXPECT_EQ(events.delivered, 1);
}

TEST_F(OpenGLViewportResizeTest, stopDropsQueuedResize)
{
    QAbstractScrollArea area;
    auto* viewport = new QOpenGLWidget;
    area.setViewport(viewport);
    Gui::OpenGLViewportResizeGuard guard(&area);
    ResizeEvents events(viewport, guard);
    qApp->installEventFilter(&events);
    QEvent paint(QEvent::Paint);
    QResizeEvent resize(viewport->size(), QSize(10, 10));
    {
        Gui::PaintFlushEventScope flush(&area, &paint);
        QApplication::sendEvent(viewport, &resize);
        ASSERT_EQ(events.held, 1);
    }
    guard.stop();
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    EXPECT_EQ(events.wakes, 1);
    EXPECT_EQ(events.delivered, 0);
    QCoreApplication::sendPostedEvents(qApp, QEvent::MetaCall);
    EXPECT_EQ(events.wakes, 1);
    EXPECT_EQ(events.delivered, 0);
}
