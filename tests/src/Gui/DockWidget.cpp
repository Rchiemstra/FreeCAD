// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QFile>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QStyle>
#include <QStyleOption>
#include <QtTest/QTest>

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

    static void syncEvents()
    {
        // X11 configure replies arrive after the first event drain. Finish
        // them before checking geometry or sending the next resize gesture.
        QApplication::processEvents();
        QGuiApplication::sync();
        QApplication::processEvents();
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

    static QWidget* floatTabGroup(QMainWindow& window, QDockWidget& first, QDockWidget& second)
    {
        window.resize(800, 600);
        window.setDockOptions(window.dockOptions() | QMainWindow::GroupedDragging);
        first.setObjectName(QStringLiteral("FirstDock"));
        second.setObjectName(QStringLiteral("SecondDock"));
        first.setWindowTitle(QStringLiteral("First panel"));
        second.setWindowTitle(QStringLiteral("Second panel"));
        first.setWidget(new QWidget(&first));
        second.setWidget(new QWidget(&second));
        window.addDockWidget(Qt::LeftDockWidgetArea, &first);
        window.addDockWidget(Qt::LeftDockWidgetArea, &second);
        window.tabifyDockWidget(&first, &second);
        first.raise();
        window.show();
        syncEvents();

        // Exercise Qt's real grouped title-bar drag. setFloating(true) would
        // detach only one member and never create QDockWidgetGroupWindow.
        const QPoint start = first.mapToGlobal(QPoint(first.width() / 2, 8));
        const QPoint finish = start + QPoint(window.width() + 100, 200);
        const auto send = [&first](QEvent::Type type, const QPoint& global,
                                   Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, first.mapFromGlobal(global), global, button, buttons,
                              Qt::NoModifier);
            QApplication::sendEvent(&first, &event);
            syncEvents();
        };
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        send(QEvent::MouseMove, start + QPoint(QApplication::startDragDistance() + 20, 0),
             Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseMove, finish, Qt::NoButton, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, finish, Qt::LeftButton, Qt::NoButton);
        return first.parentWidget();
    }

    static QSizeGrip* resizeGrip(QWidget& window)
    {
        return window.findChild<QSizeGrip*>(QStringLiteral("FreeCADFloatingDockResizeGrip"),
                                            Qt::FindDirectChildrenOnly);
    }

    static void dragResizeGrip(QSizeGrip& grip, const QPoint& delta)
    {
        syncEvents();
        const QPoint start = grip.rect().center();
        const QPoint finish = grip.mapToGlobal(start) + delta;
        QTest::mousePress(&grip, Qt::LeftButton, Qt::NoModifier, start);
        QTest::mouseMove(&grip, grip.mapFromGlobal(finish), 1);
        QTest::mouseRelease(&grip, Qt::LeftButton, Qt::NoModifier, grip.mapFromGlobal(finish));
        syncEvents();
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

TEST_P(DockWidgetTest, framelessDragKeepsClientGrabPoint)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    dock.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
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

TEST_P(DockWidgetTest, framedDragKeepsWindowFramePosition)
{
    QMainWindow window;
    Gui::DockWidget dock(&window, GetParam());
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    dock.setWindowFlags(Qt::Window | Qt::WindowTitleHint);
    dock.show();
    dock.resize(250, 300);
    const QSize size = dock.size();
    ASSERT_FALSE(dock.windowFlags().testFlag(Qt::FramelessWindowHint));

    for (const QPoint position : {QPoint(400, 350), QPoint(-400, 100), QPoint(800, -200)}) {
        Gui::moveFloatingDockWidget(&dock, position);
        EXPECT_EQ(dock.frameGeometry().topLeft(), position);
        EXPECT_EQ(dock.size(), size);
        EXPECT_TRUE(dock.isVisible());
    }
}

TEST_P(DockWidgetTest, applicationFilterManagesPlainAddonDocksAndPreservesGeometry)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QMainWindow window;
    QDockWidget dock(&window);
    setup(window, dock);
    window.show();
    const auto features = dock.features();
    dock.hide();
    dock.setFloating(true);
    const QRect expected(120, 150, 250, 300);
    dock.setGeometry(expected);
    EXPECT_TRUE(dock.isHidden());

    dock.show();
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_EQ(dock.geometry(), expected);
    EXPECT_TRUE(dock.isFloating());
    EXPECT_TRUE(dock.isVisible());
    EXPECT_EQ(dock.features(), features);
    const QByteArray state = window.saveState();
    dock.setFloating(false);
    ASSERT_TRUE(window.restoreState(state));
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_EQ(dock.geometry(), expected);
    EXPECT_TRUE(dock.isFloating());

    dock.setFloating(false);
    EXPECT_EQ(dock.windowType(), Qt::Widget);
    EXPECT_EQ(window.dockWidgetArea(&dock), Qt::LeftDockWidgetArea);
    EXPECT_TRUE(dock.isVisible());
}

TEST_P(DockWidgetTest, applicationFilterManagesActualFloatingTabGroups)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QMainWindow window;
    QDockWidget first(&window);
    QDockWidget second(&window);
    QPointer<QWidget> group = floatTabGroup(window, first, second);
    ASSERT_NE(group, nullptr);
    ASSERT_TRUE(group->inherits("QDockWidgetGroupWindow"));
    EXPECT_EQ(first.parentWidget(), second.parentWidget());
    EXPECT_FALSE(first.isFloating());
    EXPECT_FALSE(second.isFloating());
    EXPECT_EQ(group->windowType(), floatingType());
    EXPECT_TRUE(group->isVisible());
    EXPECT_FALSE(group->windowFlags().testFlag(Qt::BypassWindowManagerHint));
    EXPECT_TRUE(window.tabifiedDockWidgets(&first).contains(&second));

    // A new Show must normalize Qt's reset flags while keeping the client
    // rectangle and members. Qt can still resize the real group's layout.
    group->hide();
    group->setWindowFlags((group->windowFlags() & ~Qt::WindowType_Mask) | Qt::Tool);
    const QRect expected(120, 150, 350, 260);
    group->setGeometry(expected);
    group->show();
    EXPECT_EQ(group->windowType(), floatingType());
    EXPECT_EQ(group->geometry(), expected);
    EXPECT_EQ(first.parentWidget(), group);
    EXPECT_EQ(second.parentWidget(), group);
    group->resize(440, 330);
    syncEvents();
    EXPECT_EQ(group->size(), QSize(440, 330));
    EXPECT_GT(first.width(), 350);

    const QRect savedGeometry = group->geometry();
    const QByteArray state = window.saveState();
    window.addDockWidget(Qt::LeftDockWidgetArea, &first);
    window.addDockWidget(Qt::LeftDockWidgetArea, &second);
    EXPECT_EQ(first.parentWidget(), &window);
    EXPECT_EQ(second.parentWidget(), &window);
    ASSERT_TRUE(window.restoreState(state));
    syncEvents();
    group = first.parentWidget();
    ASSERT_NE(group, nullptr);
    ASSERT_TRUE(group->inherits("QDockWidgetGroupWindow"));
    EXPECT_EQ(group->windowType(), floatingType());
    // Qt restores a group's saved rectangle as its frame position. Keep that
    // native convention, including the offscreen platform's frame margins.
    EXPECT_EQ(group->size(), savedGeometry.size());
    EXPECT_EQ(group->frameGeometry().topLeft(), savedGeometry.topLeft());
    EXPECT_EQ(first.parentWidget(), second.parentWidget());
    EXPECT_TRUE(window.tabifiedDockWidgets(&first).contains(&second));

    window.addDockWidget(Qt::LeftDockWidgetArea, &first);
    window.addDockWidget(Qt::LeftDockWidgetArea, &second);
    EXPECT_EQ(window.dockWidgetArea(&first), Qt::LeftDockWidgetArea);
    EXPECT_EQ(window.dockWidgetArea(&second), Qt::LeftDockWidgetArea);
    first.show();
    second.show();
    EXPECT_TRUE(first.isVisible());
    EXPECT_TRUE(second.isVisible());
}

TEST_P(DockWidgetTest, applicationFilterLeavesUnrelatedToolWindowsAlone)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QWidget tool(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
    tool.setObjectName(QStringLiteral("Unrelated_groupWindow"));
    const auto flags = tool.windowFlags();
    const QRect expected(120, 150, 250, 300);
    tool.setGeometry(expected);
    tool.show();
    EXPECT_EQ(tool.windowFlags(), flags);
    EXPECT_EQ(tool.geometry(), expected);
    EXPECT_TRUE(tool.isVisible());
    EXPECT_EQ(resizeGrip(tool), nullptr);
}

TEST_P(DockWidgetTest, framelessAddonDockResizesByDraggingItsNativeGrip)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QMainWindow window;
    QDockWidget dock(&window);
    setup(window, dock);
    window.show();
    const QSize contentMinimum = dock.widget()->minimumSize();
    const QSize titleMinimum = dock.titleBarWidget()->minimumSize();
    const auto features = dock.features();
    dock.hide();
    dock.setFloating(true);
    dock.setGeometry(120, 150, 350, 260);
    dock.show();
    syncEvents();
    auto* grip = resizeGrip(dock);
    if (!GetParam()) {
        EXPECT_EQ(grip, nullptr);
        return;
    }
    ASSERT_NE(grip, nullptr);
    ASSERT_TRUE(grip->isVisible());
    EXPECT_GE(grip->width(), 18);
    EXPECT_GE(grip->height(), 18);
    EXPECT_EQ(grip->geometry().bottomRight(), dock.rect().bottomRight());
    const QSize minimum = dock.minimumSize();
    const QSize before = dock.size();
    dragResizeGrip(*grip, QPoint(48, 36));
    EXPECT_EQ(dock.size(), before + QSize(48, 36));
    EXPECT_EQ(grip->geometry().bottomRight(), dock.rect().bottomRight());
    EXPECT_EQ(dock.minimumSize(), minimum);
    EXPECT_EQ(dock.widget()->minimumSize(), contentMinimum);
    EXPECT_EQ(dock.titleBarWidget()->minimumSize(), titleMinimum);
    EXPECT_EQ(dock.features(), features);

    dock.setFloating(false);
    syncEvents();
    EXPECT_TRUE(grip->isHidden());
    dock.setFloating(true);
    syncEvents();
    EXPECT_EQ(resizeGrip(dock), grip);
    EXPECT_TRUE(grip->isVisible());
    const QSize refloated = dock.size();
    dragResizeGrip(*grip, QPoint(24, 18));
    EXPECT_EQ(dock.size(), refloated + QSize(24, 18));
}

TEST_P(DockWidgetTest, framelessTabGroupResizesByDraggingItsNativeGrip)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QMainWindow window;
    QDockWidget first(&window);
    QDockWidget second(&window);
    QPointer<QWidget> group = floatTabGroup(window, first, second);
    ASSERT_NE(group, nullptr);
    ASSERT_TRUE(group->inherits("QDockWidgetGroupWindow"));
    // FreeCAD supplies custom title bars; Qt then keeps the whole group
    // frameless instead of restoring native group decorations on Show.
    first.setTitleBarWidget(new QWidget(&first));
    second.setTitleBarWidget(new QWidget(&second));
    group->hide();
    group->setWindowFlags((group->windowFlags() & ~Qt::WindowType_Mask)
                          | Qt::Tool | Qt::FramelessWindowHint);
    group->setGeometry(120, 150, 350, 260);
    group->show();
    syncEvents();
    auto* grip = resizeGrip(*group);
    if (!GetParam()) {
        EXPECT_EQ(grip, nullptr);
        return;
    }
    ASSERT_NE(grip, nullptr) << "group flags " << int(group->windowFlags());
    ASSERT_TRUE(grip->isVisible());
    EXPECT_GE(grip->width(), 18);
    EXPECT_GE(grip->height(), 18);
    const QSize before = group->size();
    dragResizeGrip(*grip, QPoint(48, 36));
    EXPECT_EQ(group->size(), before + QSize(48, 36));
    EXPECT_EQ(grip->geometry().bottomRight(), group->rect().bottomRight());
    EXPECT_EQ(first.parentWidget(), group);
    EXPECT_EQ(second.parentWidget(), group);
    EXPECT_TRUE(window.tabifiedDockWidgets(&first).contains(&second));

    group->showMaximized();
    syncEvents();
    EXPECT_TRUE(grip->isHidden());
    group->showNormal();
    syncEvents();
    EXPECT_TRUE(grip->isVisible());
    EXPECT_EQ(grip->geometry().bottomRight(), group->rect().bottomRight());
    group->showFullScreen();
    syncEvents();
    EXPECT_TRUE(grip->isHidden());
    group->showNormal();
    syncEvents();
    EXPECT_TRUE(grip->isVisible());
}

TEST_P(DockWidgetTest, framedFloatingDocksDoNotGainAnExtraResizeGrip)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    QMainWindow window;
    QDockWidget dock(&window);
    setup(window, dock);
    auto* oldTitle = dock.titleBarWidget();
    dock.setTitleBarWidget(nullptr);
    delete oldTitle;
    window.show();
    dock.setFloating(true);
    dock.hide();
    dock.setWindowFlags(Qt::Tool | Qt::WindowTitleHint);
    // On X11 Qt initially floats docks frameless even without a custom title
    // bar. Install the filter only after preparing the framed window under test.
    qApp->installEventFilter(&filter);
    dock.show();
    EXPECT_EQ(dock.windowType(), floatingType());
    EXPECT_EQ(resizeGrip(dock), nullptr);
}

TEST_P(DockWidgetTest, nativeResizeGripDiesWithItsDock)
{
    Gui::FloatingDockWindowEventFilter filter(GetParam());
    qApp->installEventFilter(&filter);
    QMainWindow window;
    auto* dock = new QDockWidget(&window);
    setup(window, *dock);
    window.show();
    dock->setFloating(true);
    QPointer<QSizeGrip> grip = resizeGrip(*dock);
    EXPECT_EQ(!grip.isNull(), GetParam());
    delete dock;
    syncEvents();
    EXPECT_TRUE(grip.isNull());
}

TEST_P(DockWidgetTest, destroyedApplicationFilterDoesNotAffectLaterDocks)
{
    {
        Gui::FloatingDockWindowEventFilter filter(GetParam());
        qApp->installEventFilter(&filter);
    }
    QMainWindow window;
    QDockWidget dock(&window);
    setup(window, dock);
    window.show();
    dock.setFloating(true);
    EXPECT_EQ(dock.windowType(), Qt::Tool);
    EXPECT_TRUE(dock.isFloating());
    EXPECT_TRUE(dock.isVisible());
}

TEST_P(DockWidgetTest, bundledThemePreservesDockSeparatorSizes)
{
    QFile theme(QStringLiteral(TEST_SOURCE_DIR "/src/Gui/Stylesheets/FreeCAD.qss"));
    ASSERT_TRUE(theme.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(theme.readAll());
    const QRegularExpression rule(QStringLiteral("QMainWindow::separator[^\\{]*\\{[^}]*\\}"));
    auto matches = rule.globalMatch(content);
    QString separators;
    while (matches.hasNext()) {
        separators += matches.next().captured() + QLatin1Char('\n');
    }
    ASSERT_FALSE(separators.isEmpty());
    QMainWindow window;
    window.setStyleSheet(separators);
    QStyleOption option;
    option.initFrom(&window);
    for (const bool horizontal : {false, true}) {
        option.state.setFlag(QStyle::State_Horizontal, horizontal);
        EXPECT_EQ(window.style()->pixelMetric(QStyle::PM_DockWidgetSeparatorExtent, &option,
                                              &window), 6);
    }

    auto* central = new QWidget(&window);
    central->setMinimumSize(100, 100);
    window.setCentralWidget(central);
    QDockWidget dock(&window);
    dock.setWidget(new QWidget(&dock));
    dock.widget()->setMinimumSize(100, 100);
    window.addDockWidget(Qt::LeftDockWidgetArea, &dock);
    window.resize(800, 600);
    window.show();
    window.resizeDocks({&dock}, {200}, Qt::Horizontal);
    syncEvents();
    const int before = dock.width();
    const QPoint start(dock.geometry().right() + 1, dock.geometry().center().y());
    QTest::mouseMove(&window, start);
    QTest::mousePress(&window, Qt::LeftButton, Qt::NoModifier, start);
    for (int step = 1; step <= 4; ++step) {
        QTest::mouseMove(&window, start + QPoint(12 * step, 0), 1);
        // QMainWindow applies separator movement through a zero-delay timer.
        // Drain it while the button is held; releasing first discards the move.
        syncEvents();
    }
    const int widthBeforeRelease = dock.width();
    QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, start + QPoint(48, 0));
    syncEvents();
    EXPECT_EQ(widthBeforeRelease, before + 48);
    EXPECT_EQ(dock.width(), before + 48);
    EXPECT_FALSE(dock.isFloating());
    EXPECT_EQ(window.dockWidgetArea(&dock), Qt::LeftDockWidgetArea);

    window.resizeDocks({&dock}, {before}, Qt::Horizontal);
    syncEvents();
    EXPECT_EQ(dock.width(), before);
}

INSTANTIATE_TEST_SUITE_P(FloatingWindows, DockWidgetTest, ::testing::Bool());

}  // namespace
