// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <memory>

#include <QApplication>
#include <QDialog>
#include <QMainWindow>
#include <QtTest/QTest>

#include <Gui/ViewDropDocking.h>

namespace
{

class ViewDropDockingTest: public ::testing::Test
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
        main.setCentralWidget(new QWidget);
        main.setGeometry(100, 100, 800, 600);
        view.setGeometry(950, 100, 300, 200);
        main.show();
        view.show();
        QApplication::processEvents();
        controller = std::make_unique<Gui::ViewDropDocking>(
            main.centralWidget(), [this](QWidget* widget) { return eligible && widget == &view; },
            [this](QWidget* widget) { docked = widget; ++dockCount; }, &main,
            [this]() { return left; }, [this]() { return position; });
    }

    void TearDown() override
    {
        controller.reset();
        QApplication::processEvents();
    }

    void moveView(const QPoint& destination)
    {
        // QWidget::move uses frame coordinates. The offscreen backend supplies
        // a two-pixel frame; sample within it, above the client/navigation area.
        position = destination + QPoint(100, 1);
        view.move(destination);
        QApplication::processEvents();
    }

    void startInside()
    {
        left = true;
        moveView(main.centralWidget()->mapToGlobal(QPoint(80, 80)));
        QTest::qWait(60);
    }

    void release()
    {
        left = false;
        QTest::qWait(100);
    }

    void nonClientEvent(QEvent::Type type, Qt::MouseButton button)
    {
        QMouseEvent event(type, view.mapFromGlobal(position), position, button,
                          type == QEvent::NonClientAreaMouseButtonRelease ? Qt::NoButton : button,
                          Qt::NoModifier);
        QApplication::sendEvent(&view, &event);
    }

    QMainWindow main;
    QMainWindow view;
    std::unique_ptr<Gui::ViewDropDocking> controller;
    std::optional<bool> left {false};
    QPoint position;
    bool eligible {true};
    QPointer<QWidget> docked;
    int dockCount {0};
};

TEST_F(ViewDropDockingTest, docksExactDraggedViewOnlyAfterRelease)
{
    startInside();
    EXPECT_EQ(dockCount, 0);
    auto* hint = main.findChild<QRubberBand*>(QStringLiteral("FreeCADViewDropTarget"));
    ASSERT_NE(hint, nullptr);
    EXPECT_TRUE(hint->isVisible());
    QTest::qWait(120);
    EXPECT_EQ(dockCount, 0);
    release();
    EXPECT_EQ(dockCount, 1);
    EXPECT_EQ(docked, &view);
    EXPECT_FALSE(hint->isVisible());
    QTest::qWait(100);
    EXPECT_EQ(dockCount, 1);
}

TEST_F(ViewDropDockingTest, releaseOutsideMainModelAreaKeepsViewDetached)
{
    left = true;
    moveView(QPoint(1000, 400));
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, leavingTargetBeforeReleaseHidesHintAndDoesNotDock)
{
    startInside();
    moveView(QPoint(1000, 400));
    QTest::qWait(60);
    auto* hint = main.findChild<QRubberBand*>(QStringLiteral("FreeCADViewDropTarget"));
    ASSERT_NE(hint, nullptr);
    EXPECT_FALSE(hint->isVisible());
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, hoverAndProgrammaticMovesDoNotDock)
{
    moveView(main.centralWidget()->mapToGlobal(QPoint(80, 80)));
    QTest::qWait(100);
    left = true;
    QTest::qWait(100);
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, clientNavigationDoesNotArmWindowDrag)
{
    left = true;
    position = main.centralWidget()->mapToGlobal(QPoint(180, 180));
    view.move(main.centralWidget()->mapToGlobal(QPoint(80, 80)));
    QApplication::processEvents();
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, resizeCancelsWholeGesture)
{
    startInside();
    view.resize(350, 250);
    QApplication::processEvents();
    moveView(main.centralWidget()->mapToGlobal(QPoint(100, 100)));
    release();
    EXPECT_EQ(dockCount, 0);
    startInside();
    release();
    EXPECT_EQ(dockCount, 1);
}

TEST_F(ViewDropDockingTest, escapeCancelsWithoutRearmingOnRemainingMoveEvents)
{
    startInside();
    QTest::keyClick(&view, Qt::Key_Escape);
    moveView(main.centralWidget()->mapToGlobal(QPoint(110, 110)));
    release();
    EXPECT_EQ(dockCount, 0);
    startInside();
    release();
    EXPECT_EQ(dockCount, 1);
}

TEST_F(ViewDropDockingTest, cancelledNativeMoveReturningToOriginDoesNotDock)
{
    view.move(main.centralWidget()->mapToGlobal(QPoint(80, 80)));
    QApplication::processEvents();
    const auto origin = view.pos();
    left = true;
    moveView(origin + QPoint(100, 50));
    moveView(origin);
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, modeChangeAndHiddenTargetCancelPendingDrag)
{
    startInside();
    eligible = false;
    release();
    EXPECT_EQ(dockCount, 0);
    eligible = true;
    startInside();
    main.hide();
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, fullscreenAndMaximizedViewsAreNotDropCandidates)
{
    for (auto state : {Qt::WindowFullScreen, Qt::WindowMaximized}) {
        view.setWindowState(state);
        QApplication::processEvents();
        startInside();
        release();
        EXPECT_EQ(dockCount, 0);
        view.setWindowState(Qt::WindowNoState);
        QApplication::processEvents();
    }
}

TEST_F(ViewDropDockingTest, floatingPanelOverTargetPreventsDockingThroughIt)
{
    startInside();
    QWidget panel(nullptr, Qt::Tool);
    panel.setGeometry(QRect(position - QPoint(20, 20), QSize(150, 100)));
    panel.show();
    QApplication::processEvents();
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, modalDialogPreventsDocking)
{
    startInside();
    QDialog dialog(&main);
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.show();
    QApplication::processEvents();
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, closedViewDoesNotRunQueuedDock)
{
    startInside();
    left = false;
    nonClientEvent(QEvent::NonClientAreaMouseButtonRelease, Qt::LeftButton);
    view.close();
    QApplication::processEvents();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, newPressBeforeQueuedDockInvalidatesRelease)
{
    startInside();
    left = false;
    nonClientEvent(QEvent::NonClientAreaMouseButtonRelease, Qt::LeftButton);
    left = true;
    QApplication::processEvents();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, deletingTargetDuringDragIsSafe)
{
    startInside();
    delete main.takeCentralWidget();
    release();
    EXPECT_EQ(dockCount, 0);
}

TEST_F(ViewDropDockingTest, deletingControllerCancelsQueuedDockAndRemovesHint)
{
    startInside();
    left = false;
    nonClientEvent(QEvent::NonClientAreaMouseButtonRelease, Qt::LeftButton);
    controller.reset();
    QApplication::processEvents();
    EXPECT_EQ(dockCount, 0);
    EXPECT_EQ(main.findChild<QRubberBand*>(QStringLiteral("FreeCADViewDropTarget")), nullptr);
}

TEST_F(ViewDropDockingTest, unknownNativeStateNeverBecomesAnAssumedRelease)
{
    startInside();
    left = std::nullopt;
    QTest::qWait(120);
    EXPECT_EQ(dockCount, 0);
    nonClientEvent(QEvent::NonClientAreaMouseButtonRelease, Qt::LeftButton);
    QApplication::processEvents();
    EXPECT_EQ(dockCount, 1);
}

TEST_F(ViewDropDockingTest, explicitNonClientEventsWorkWithoutNativePointerQuery)
{
    left = std::nullopt;
    position = QPoint(view.geometry().center().x(), view.frameGeometry().top() + 1);
    nonClientEvent(QEvent::NonClientAreaMouseButtonPress, Qt::LeftButton);
    moveView(main.centralWidget()->mapToGlobal(QPoint(80, 80)));
    QTest::qWait(60);
    EXPECT_EQ(dockCount, 0);
    nonClientEvent(QEvent::NonClientAreaMouseButtonRelease, Qt::LeftButton);
    QApplication::processEvents();
    EXPECT_EQ(dockCount, 1);
}

}  // namespace
