// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QDockWidget>
#include <QScopeGuard>

#include <App/Application.h>
#include <Gui/Application.h>
#include <Gui/DockWindowManager.h>
#include <Gui/MainWindow.h>
#include <src/App/InitApplication.h>

TEST(MainWindowShutdown, notificationDockDoesNotReenterDestroyedMainWindow)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    static int argc = 1;
    static char executable[] = "GuiShutdown_tests_run";
    static char* argv[] = {executable, nullptr};
    QApplication qtApplication(argc, argv);

    tests::initApplication();
    Gui::Application::initApplication();
    Gui::Application::initOpenInventor();
    Gui::Application guiApplication(true);

    auto preferences = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/NotificationArea");
    const bool notificationAreaWasEnabled =
        preferences->GetBool("NotificationAreaEnabled", true);
    const auto restorePreference = qScopeGuard([&] {
        preferences->SetBool("NotificationAreaEnabled", notificationAreaWasEnabled);
    });
    preferences->SetBool("NotificationAreaEnabled", true);

    auto* mainWindow = new Gui::MainWindow;
    ASSERT_EQ(Gui::MainWindow::getInstance(), mainWindow);

    auto* notifications =
        Gui::DockWindowManager::instance()->findRegisteredDockWindow("Std_NotificationView");
    ASSERT_NE(notifications, nullptr);
    auto* dock = Gui::DockWindowManager::instance()->addDockWindow(
        "Notifications", notifications, Qt::BottomDockWidgetArea);
    ASSERT_NE(dock, nullptr);
    ASSERT_EQ(dock->widget(), notifications);

    // MainWindow's derived destructor clears the singleton before QMainWindow
    // destroys QStatusBar and its NotificationArea child.  The notification
    // cleanup must not call removeDockWidget through that cleared singleton.
    delete mainWindow;
    EXPECT_EQ(Gui::MainWindow::getInstance(), nullptr);
}
