// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <Base/Console.h>
#include <Base/Parameter.h>
#include <Gui/Application.h>
#include <Gui/MainWindow.h>
#include <src/App/InitApplication.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QScopeGuard>
#include <QStatusBar>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{

// A deadlocked GUI thread cannot fail an assertion, so end the process if the
// guarded block does not return in time.
class DeadlockWatchdog
{
public:
    explicit DeadlockWatchdog(std::chrono::seconds limit)
        : thread([this, limit] {
            std::unique_lock<std::mutex> lock(mutex);
            if (!finished.wait_for(lock, limit, [this] { return done; })) {
                std::fputs(
                    "NotificationAreaReentrancy: the GUI thread did not return, "
                    "the notification area deadlocked\n",
                    stderr
                );
                std::fflush(stderr);
                std::_Exit(EXIT_FAILURE);
            }
        })
    {}

    ~DeadlockWatchdog()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        finished.notify_one();
        thread.join();
    }

    DeadlockWatchdog(const DeadlockWatchdog&) = delete;
    DeadlockWatchdog(DeadlockWatchdog&&) = delete;
    DeadlockWatchdog& operator=(const DeadlockWatchdog&) = delete;
    DeadlockWatchdog& operator=(DeadlockWatchdog&&) = delete;

private:
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    std::thread thread;
};

// Raises one console warning from the Show event of the notification popup,
// that is from inside NotificationBox::showText(). On platforms without
// raise() support (offscreen, minimal) Qt itself warns at this point.
class WarnWhileNotificationShows: public QObject
{
public:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!warned && event->type() == QEvent::Show && watched->inherits("Gui::NotificationLabel")) {
            warned = true;
            Base::Console().warning(
                "NotificationAreaReentrancy: warning raised while a notification shows\n"
            );
        }
        return QObject::eventFilter(watched, event);
    }

    bool warned = false;
};

bool hasNotificationPopup()
{
    const auto widgets = QApplication::topLevelWidgets();
    return std::any_of(widgets.begin(), widgets.end(), [](const QWidget* widget) {
        return widget->inherits("Gui::NotificationLabel");
    });
}

// Waits until no notification popup is left. Hiding a popup by hand is not
// enough: it keeps its expire timer and deletes itself later, inside another
// test. The quiet period also covers a batch the area has not shown yet.
bool waitForNotificationPopupsToClose(int milliseconds)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QElapsedTimer quiet;
    quiet.start();
    while (elapsed.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        if (hasNotificationPopup()) {
            quiet.restart();
        }
        else if (quiet.elapsed() >= 500) {
            return true;
        }
    }
    return false;
}

bool hasNotificationArea(const Gui::MainWindow* mainWindow)
{
    // NotificationArea has no Q_OBJECT; MainWindow registers it under this name.
    return mainWindow->statusBar()->findChild<QWidget*>(QStringLiteral("notificationArea"))
        != nullptr;
}

void processEventsFor(int milliseconds, const bool* stopWhenTrue = nullptr)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds && !(stopWhenTrue && *stopWhenTrue)) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

class NotificationAreaReentrancyTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "NotificationAreaReentrancy";
            static char* argv[] = {name, nullptr};
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (!Gui::Application::Instance) {
            Gui::Application::initApplication();
            Gui::Application::initOpenInventor();
            new Gui::Application(true);
        }
        if (!Gui::MainWindow::getInstance()) {
            new Gui::MainWindow();
        }
    }

    void SetUp() override
    {
        preferences = App::GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/NotificationArea"
        );
        nonIntrusiveWasEnabled = preferences->GetBool("NonIntrusiveNotificationsEnabled", true);
        preventWasEnabled
            = preferences->GetBool("PreventNonIntrusiveNotificationsWhenWindowNotActive", true);
        preferences->SetBool("NonIntrusiveNotificationsEnabled", true);
        // No window is active under xvfb or offscreen; show the popup anyway.
        preferences->SetBool("PreventNonIntrusiveNotificationsWhenWindowNotActive", false);
        // Popups of this test close on their own after one second.
        notificationTime = preferences->GetInt("NotificationTime", 20);
        preferences->SetInt("NotificationTime", 1);
        // A popup left visible would be reused without a new Show event. One
        // shown before this test may last the default 20 seconds.
        ASSERT_TRUE(waitForNotificationPopupsToClose(25000));
    }

    void TearDown() override
    {
        EXPECT_TRUE(waitForNotificationPopupsToClose(5000))
            << "a notification popup outlived the test";
        preferences->SetInt("NotificationTime", notificationTime);
        preferences->SetBool("PreventNonIntrusiveNotificationsWhenWindowNotActive", preventWasEnabled);
        preferences->SetBool("NonIntrusiveNotificationsEnabled", nonIntrusiveWasEnabled);
    }

    ParameterGrp::handle preferences;
    bool nonIntrusiveWasEnabled = true;
    bool preventWasEnabled = true;
    long notificationTime = 20;
};

}  // namespace

TEST_F(NotificationAreaReentrancyTest, warningRaisedWhileNotificationShowsDoesNotDeadlock)
{
    auto* mainWindow = Gui::MainWindow::getInstance();
    ASSERT_NE(mainWindow, nullptr);
    ASSERT_TRUE(hasNotificationArea(mainWindow));

    WarnWhileNotificationShows filter;
    qApp->installEventFilter(&filter);
    const auto removeFilter = qScopeGuard([&] { qApp->removeEventFilter(&filter); });

    {
        DeadlockWatchdog watchdog(std::chrono::seconds(60));
        Base::Console().warning("NotificationAreaReentrancy: first warning\n");
        // The area collects warnings for 250 ms before it shows them.
        processEventsFor(10000, &filter.warned);
        // Let the area also show the warning raised from the popup.
        processEventsFor(1000);
    }

    EXPECT_TRUE(filter.warned) << "the notification popup was never shown";
}
