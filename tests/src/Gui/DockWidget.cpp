// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QMainWindow>

#include <Gui/DockWidget.h>

namespace
{

class DockWidgetTest: public ::testing::TestWithParam<bool>
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "Gui_tests_run";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
    }

    Qt::WindowType floatingType() const
    {
        return GetParam() ? Qt::Window : Qt::Tool;
    }

    static void setup(QMainWindow& window, QDockWidget& dock)
    {
        window.resize(800, 600);
        dock.setObjectName(QStringLiteral("TestDock"));
        dock.setWindowTitle(QStringLiteral("Panel"));
        dock.setTitleBarWidget(new QWidget(&dock));
        dock.setWidget(new QWidget(&dock));
        window.addDockWidget(Qt::LeftDockWidgetArea, &dock);
    }
};

TEST_P(DockWidgetTest, repeatedFloatingPreservesVisibilityAndDocking)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    const auto features = dock.features();
    int transitions = 0;
    QObject::connect(&dock, &QDockWidget::topLevelChanged, [&transitions](bool) { ++transitions; });

    for (int cycle = 0; cycle < 3; ++cycle) {
        dock.setFloating(true);
        EXPECT_EQ(dock.windowType(), floatingType());
        EXPECT_TRUE(dock.isFloating());
        EXPECT_TRUE(dock.isVisible());
        EXPECT_EQ(dock.features(), features);
        EXPECT_TRUE(dock.windowFlags().testFlag(Qt::FramelessWindowHint));
        EXPECT_FALSE(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint));

        dock.setFloating(false);
        EXPECT_FALSE(dock.isFloating());
        EXPECT_TRUE(dock.isVisible());
        EXPECT_EQ(window.dockWidgetArea(&dock), Qt::LeftDockWidgetArea);
    }
    EXPECT_EQ(transitions, 6);
}

TEST_P(DockWidgetTest, hiddenFloatingDockStaysHiddenUntilShown)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.hide();
    dock.setFloating(true);
    const QRect expected(120, 150, 250, 300);
    dock.setGeometry(expected);
    EXPECT_TRUE(dock.isHidden());

    dock.show();
    EXPECT_TRUE(dock.isVisible());
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_EQ(dock.geometry(), expected);
    EXPECT_EQ(dock.parentWidget(), &window);
}

TEST_P(DockWidgetTest, titleBarReplacementKeepsFloatingWindowManaged)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    const QRect expected(120, 150, 250, 300);
    dock.setGeometry(expected);

    auto* oldTitle = dock.titleBarWidget();
    dock.setTitleBarWidget(new QWidget(&dock));
    delete oldTitle;
    EXPECT_TRUE(dock.isVisible());
    EXPECT_TRUE(dock.isFloating());
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_FALSE(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint));
    EXPECT_EQ(dock.geometry(), expected);
}

TEST_P(DockWidgetTest, restoresFloatingLayoutAndCanDockAgain)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    const QRect expected(120, 150, 250, 300);
    dock.setGeometry(expected);
    const QByteArray state = window.saveState();

    dock.setFloating(false);
    dock.hide();
    ASSERT_TRUE(window.restoreState(state));
    EXPECT_TRUE(dock.isVisible());
    EXPECT_TRUE(dock.isFloating());
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_EQ(dock.geometry(), expected);

    dock.setFloating(false);
    EXPECT_FALSE(dock.isFloating());
    EXPECT_TRUE(dock.isVisible());
    EXPECT_EQ(window.dockWidgetArea(&dock), Qt::LeftDockWidgetArea);
}

TEST_P(DockWidgetTest, dragKeepsClientGrabPointWhenWindowHasFrameMargins)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    // Exercise real Qt frame margins, independently of the platform's floating
    // dock decorations. WSLg reports margins even for a frameless window.
    dock.setWindowFlags(Qt::Window | Qt::WindowTitleHint);
    dock.show();
    dock.resize(250, 300);
    const QSize size = dock.size();
    const QPoint grabPoint(120, 8);

    for (const QPoint cursor : {QPoint(400, 350), QPoint(-400, 100), QPoint(800, -200)}) {
        Gui::moveFloatingDockWidget(&dock, cursor - grabPoint);
        EXPECT_EQ(dock.mapToGlobal(grabPoint), cursor);
        EXPECT_EQ(dock.size(), size);
        EXPECT_TRUE(dock.isVisible());
    }
}

INSTANTIATE_TEST_SUITE_P(FloatingWindows, DockWidgetTest, ::testing::Bool());

}  // namespace
