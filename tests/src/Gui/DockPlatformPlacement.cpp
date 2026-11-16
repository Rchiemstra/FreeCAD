// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QDockWidget>
#include <QLibrary>
#include <QLineEdit>
#include <QMainWindow>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QScopeGuard>
#include <QScreen>
#include <QVBoxLayout>
#include <QWindow>
#include <QtTest/QTest>

#include <Gui/DockWidget.h>

#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
#include <X11/Xlib.h>
#undef None
#undef Bool
#endif

namespace
{

class DockPlatformPlacementTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        }
        static int argc = 1;
        static char executable[] = "DockPlacementTest";
        static char* argv[] = {executable, nullptr};
        if (!QApplication::instance()) {
            new QApplication(argc, argv);
        }
    }

    static QLineEdit* setup(QMainWindow& main, QDockWidget& dock, bool frameless = true)
    {
        main.resize(700, 500);
        main.setWindowTitle(QStringLiteral("FreeCAD Dock Placement Test"));
        dock.setObjectName(QStringLiteral("PlacementDock"));
        dock.setWindowTitle(QStringLiteral("Placement fixture"));
        if (frameless) {
            dock.setTitleBarWidget(new QWidget(&dock));
        }
        auto* content = new QWidget(&dock);
        auto* layout = new QVBoxLayout(content);
        auto* editor = new QLineEdit(QStringLiteral("Preserved draft"), content);
        layout->addWidget(editor);
        layout->addStretch();
        dock.setWidget(content);
        main.addDockWidget(Qt::LeftDockWidgetArea, &dock);
        main.show();
        return editor;
    }
};

class DockSurfaceObserver: public QObject
{
public:
    explicit DockSurfaceObserver(QDockWidget* dock)
        : dock(dock)
    {}

    int shows = 0;
    int destroyed = 0;

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (dock && object == dock && event->type() == QEvent::Show) {
            ++shows;
        }
        if (dock && object == dock->windowHandle() && event->type() == QEvent::PlatformSurface
            && static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            ++destroyed;
        }
        return false;
    }

private:
    QPointer<QDockWidget> dock;
};

TEST_F(DockPlatformPlacementTest, changesOnlyThePlatformTypeWithoutAnExtraShowOrNewSurface)
{
    QMainWindow main;
    QDockWidget dock(&main);
    auto* editor = setup(main, dock);
    Gui::FloatingDockWindowEventFilter filter(&main, true);
    qApp->installEventFilter(&filter);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&filter); });
    dock.hide();
    dock.setFloating(true);
    const auto flags = dock.windowFlags();
    ASSERT_EQ(dock.windowType(), Qt::Tool);
    ASSERT_NE(dock.windowHandle(), nullptr);
    const auto nativeId = dock.windowHandle()->winId();
    DockSurfaceObserver observer(&dock);
    qApp->installEventFilter(&observer);
    const auto removeObserver = qScopeGuard([&] { qApp->removeEventFilter(&observer); });

    dock.show();

    EXPECT_EQ(dock.windowFlags(), flags);
    EXPECT_EQ(dock.windowType(), Qt::Tool);
    EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
    EXPECT_EQ(dock.windowHandle()->winId(), nativeId);
    EXPECT_FALSE(dock.testAttribute(Qt::WA_NativeWindow));
    EXPECT_EQ(observer.shows, 1);
    EXPECT_EQ(observer.destroyed, 0);
    EXPECT_EQ(editor->text(), QStringLiteral("Preserved draft"));
    editor->setText(QStringLiteral("Still editable"));
    EXPECT_EQ(editor->text(), QStringLiteral("Still editable"));
}

TEST_F(DockPlatformPlacementTest, preservesQtUnplugFlagsUntilTheFinalShow)
{
    QMainWindow main;
    QDockWidget dock(&main);
    setup(main, dock);
    Gui::FloatingDockWindowEventFilter filter(&main, true);
    qApp->installEventFilter(&filter);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&filter); });
    dock.hide();
    dock.setFloating(true);
    dock.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);

    dock.show();

    EXPECT_EQ(dock.windowType(), Qt::Tool);
    EXPECT_TRUE(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint));
    EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
    EXPECT_TRUE(dock.windowHandle()->flags().testFlag(Qt::BypassWindowManagerHint));
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        ASSERT_TRUE(QTest::qWaitForWindowExposed(dock.windowHandle()));
        QTest::qWait(150);
    }
    dock.hide();
    dock.setWindowFlags(dock.windowFlags() & ~Qt::BypassWindowManagerHint);
    const auto nativeId = dock.windowHandle()->winId();

    dock.show();

    EXPECT_EQ(dock.windowType(), Qt::Tool);
    EXPECT_FALSE(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint));
    EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
    EXPECT_EQ(dock.windowHandle()->winId(), nativeId);
}

TEST_F(DockPlatformPlacementTest, retiresTheFirstUnmanagedPeerOnlyAfterQtHidesIt)
{
    QMainWindow main;
    main.resize(700, 500);
    Gui::DockWidget dock(&main, true);
    dock.setObjectName(QStringLiteral("FirstUnmanagedDock"));
    dock.setTitleBarWidget(new QWidget(&dock));
    dock.setWidget(new QWidget(&dock));
    main.addDockWidget(Qt::LeftDockWidgetArea, &dock);
    Gui::FloatingDockWindowEventFilter filter(&main, true);
    qApp->installEventFilter(&filter);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&filter); });
    dock.hide();
    dock.setFloating(true);
    dock.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
    // The first XCreate must carry Bypass, like the real unplug gesture. A
    // managed native peer created before setting Bypass would hide the defect.
    ASSERT_EQ(dock.windowHandle(), nullptr);
    ASSERT_FALSE(dock.testAttribute(Qt::WA_WState_Created));
    main.show();
    dock.show();
    ASSERT_NE(dock.windowHandle(), nullptr);
    QPointer<QWindow> native = dock.windowHandle();
    const auto unmanagedId = native->winId();
    EXPECT_EQ(native->type(), Qt::Window);
    EXPECT_TRUE(native->flags().testFlag(Qt::BypassWindowManagerHint));
    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        ASSERT_TRUE(QTest::qWaitForWindowExposed(native));
        QTest::qWait(150);
    }
    Gui::releaseUnmanagedFloatingDockWindow(&dock);
    EXPECT_NE(native->handle(), nullptr);  // A visible drag peer is untouched.
    DockSurfaceObserver observer(&dock);
    qApp->installEventFilter(&observer);
    const auto removeObserver = qScopeGuard([&] { qApp->removeEventFilter(&observer); });

    dock.hide();

    EXPECT_EQ(dock.windowHandle(), native);
    EXPECT_EQ(native->handle(), nullptr);
    EXPECT_EQ(observer.destroyed, 1);  // App filter plus subclass retire once.
    EXPECT_TRUE(dock.windowFlags().testFlag(Qt::BypassWindowManagerHint));
    dock.setWindowFlags(dock.windowFlags() & ~Qt::BypassWindowManagerHint);
    ASSERT_NE(dock.windowHandle(), nullptr);
    const auto managedId = dock.windowHandle()->winId();
    EXPECT_NE(managedId, unmanagedId);

    dock.show();

    EXPECT_EQ(dock.windowType(), Qt::Tool);
    EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
    EXPECT_EQ(dock.windowHandle()->winId(), managedId);
    EXPECT_FALSE(dock.windowHandle()->flags().testFlag(Qt::BypassWindowManagerHint));
    EXPECT_EQ(observer.destroyed, 1);
    dock.setFloating(false);
    EXPECT_EQ(dock.windowHandle(), nullptr);
    EXPECT_FALSE(dock.testAttribute(Qt::WA_NativeWindow));
}

TEST_F(DockPlatformPlacementTest, standaloneSubclassRefreshesItsExistingGripOnRefloat)
{
    QMainWindow main;
    Gui::DockWidget dock(&main, true);
    setup(main, dock);
    // Deliberately omit the application filter: the subclass receives Show
    // after the resize grip's own event filter has seen the old Tool type.
    dock.setFloating(true);
    auto* originalGrip = dock.findChild<QSizeGrip*>(
        QStringLiteral("FreeCADFloatingDockResizeGrip"), Qt::FindDirectChildrenOnly);
    ASSERT_NE(originalGrip, nullptr);
    ASSERT_TRUE(originalGrip->isVisible());

    for (int cycle = 0; cycle < 3; ++cycle) {
        SCOPED_TRACE(cycle);
        dock.setFloating(false);
        EXPECT_FALSE(originalGrip->isVisible());
        dock.setFloating(true);

        EXPECT_EQ(dock.windowType(), Qt::Tool);
        ASSERT_NE(dock.windowHandle(), nullptr);
        EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
        EXPECT_EQ(dock.findChild<QSizeGrip*>(QStringLiteral("FreeCADFloatingDockResizeGrip"),
                                            Qt::FindDirectChildrenOnly), originalGrip);
        EXPECT_TRUE(originalGrip->isVisible());
        EXPECT_EQ(originalGrip->geometry().bottomRight(), dock.rect().bottomRight());
        EXPECT_EQ(dock.findChildren<QSizeGrip*>(QStringLiteral("FreeCADFloatingDockResizeGrip"),
                                               Qt::FindDirectChildrenOnly).size(), 1);
    }
}

TEST_F(DockPlatformPlacementTest, preservesPlatformPeerAndClientPlacementAcrossScreens)
{
#if defined(Q_OS_LINUX) && QT_CONFIG(xcb)
    if (QGuiApplication::platformName() != QStringLiteral("xcb")) {
        GTEST_SKIP() << "Native X11 placement requires the xcb platform";
    }
    for (auto* screen : QGuiApplication::screens()) {
        if (screen->devicePixelRatio() != 1.0) {
            GTEST_SKIP() << "This native-coordinate assertion covers unit-scale screens";
        }
    }
    QLibrary x11(QStringLiteral("X11"), 6);
    using Open = Display* (*)(const char*);
    using Close = int (*)(Display*);
    using Root = ::Window (*)(Display*);
    using Translate = int (*)(Display*, ::Window, ::Window, int, int, int*, int*, ::Window*);
    const auto open = reinterpret_cast<Open>(x11.resolve("XOpenDisplay"));
    const auto close = reinterpret_cast<Close>(x11.resolve("XCloseDisplay"));
    const auto root = reinterpret_cast<Root>(x11.resolve("XDefaultRootWindow"));
    const auto translate = reinterpret_cast<Translate>(x11.resolve("XTranslateCoordinates"));
    ASSERT_TRUE(open && close && root && translate);
    Display* display = open(nullptr);
    ASSERT_NE(display, nullptr);
    const auto closeDisplay = qScopeGuard([&] { close(display); });

    for (bool frameless : {true, false}) {
        SCOPED_TRACE(frameless);
        QMainWindow main;
        QDockWidget dock(&main);
        auto* editor = setup(main, dock, frameless);
        Gui::FloatingDockWindowEventFilter filter(&main, true);
        qApp->installEventFilter(&filter);
        const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&filter); });
        for (auto* screen : QGuiApplication::screens()) {
            SCOPED_TRACE(screen->name().toStdString());
            for (int cycle = 0; cycle < 3; ++cycle) {
                SCOPED_TRACE(cycle);
                dock.hide();
                dock.setFloating(true);
                const auto flags = dock.windowFlags();
                ASSERT_EQ(dock.windowType(), Qt::Tool);
                ASSERT_NE(dock.windowHandle(), nullptr);
                const auto nativeId = dock.windowHandle()->winId();
                const QRect expected(screen->availableGeometry().topLeft() + QPoint(80, 80),
                                     QSize(280, 250));
                dock.setGeometry(expected);

                dock.show();

                ASSERT_TRUE(QTest::qWaitForWindowExposed(dock.windowHandle()));
                EXPECT_EQ(dock.windowFlags(), flags);
                EXPECT_EQ(dock.windowHandle()->type(), Qt::Window);
                EXPECT_EQ(dock.windowHandle()->winId(), nativeId);
                EXPECT_FALSE(dock.testAttribute(Qt::WA_NativeWindow));
                QPoint nativePosition;
                const auto matches = [&] {
                    int x = 0;
                    int y = 0;
                    ::Window child = 0;
                    if (!translate(display, static_cast<::Window>(nativeId), root(display), 0, 0,
                                   &x, &y, &child)) {
                        return false;
                    }
                    nativePosition = QPoint(x, y);
                    return nativePosition == expected.topLeft() && dock.geometry() == expected;
                };
                EXPECT_TRUE(QTest::qWaitFor(matches))
                    << "Native client " << nativePosition.x() << ',' << nativePosition.y()
                    << "; requested " << expected.x() << ',' << expected.y();
                // Drain late ConfigureNotify events after the first matching geometry.
                QTest::qWait(150);
                EXPECT_TRUE(matches())
                    << "Late native client " << nativePosition.x() << ',' << nativePosition.y();
                EXPECT_EQ(editor->text(), QStringLiteral("Preserved draft"));
                dock.setFloating(false);
                QApplication::processEvents();
                EXPECT_EQ(dock.windowHandle(), nullptr);
                EXPECT_FALSE(dock.testAttribute(Qt::WA_NativeWindow));
                dock.show();
            }
        }
    }
#else
    GTEST_SKIP() << "Native X11 placement is unavailable";
#endif
}

}  // namespace
