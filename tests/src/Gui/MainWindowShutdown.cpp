// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDockWidget>
#include <QFileOpenEvent>
#include <QFile>
#include <QLocalSocket>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QScopeGuard>
#include <QTimer>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/MainThreadSignal.h>
#include <App/Property.h>
#include <App/PropertyCleanup.h>
#include <App/PropertyStandard.h>
#include <Gui/Application.h>
#include <Gui/DockWindowManager.h>
#include <Gui/MainWindow.h>
#include <Gui/GuiApplication.h>
#include <Gui/Document.h>
#include <Gui/View.h>
#include <Gui/EditorView.h>
#include <Gui/TextEdit.h>
#include <Gui/Selection/Selection.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;

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

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace {
struct ShutdownGate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered {false};
    bool release {false};
};
class BlockingExitProperty final : public App::PropertyInteger
{
public:
    BlockingExitProperty(ShutdownGate& gate, App::Document* document, std::atomic<int>& observed)
        : gate(gate), document(document), observed(observed) {}
    ~BlockingExitProperty() override
    {
        {
            std::unique_lock lock(gate.mutex);
            gate.entered = true;
            gate.changed.notify_all();
            gate.changed.wait(lock, [&] { return gate.release; });
        }
        observed.store(App::GetApplication().getDocument("GuiCleanerBlocked") == document ? 1 : -1);
    }
private:
    ShutdownGate& gate;
    App::Document* document;
    std::atomic<int>& observed;
};
class MarshalledFileOpenOnExit final : public App::PropertyInteger
{
public:
    MarshalledFileOpenOnExit(ShutdownGate& gate, const QString& path,
                             std::atomic<int>& observed)
        : gate(gate), path(path), observed(observed) {}
    ~MarshalledFileOpenOnExit() override
    {
        {
            std::lock_guard lock(gate.mutex);
            gate.entered = true;
            gate.changed.notify_all();
        }
        try {
            App::MainThreadSignalConfig::invoke([this] {
                QFileOpenEvent incoming(path);
                QApplication::sendEvent(qApp, &incoming);
                // Exercise a nested Qt loop while the cleaner preflight is
                // waiting for this actual blocking marshalled callback.
                QEventLoop nested;
                QTimer::singleShot(0, &nested, &QEventLoop::quit);
                nested.exec();
                const auto& app = App::GetApplication();
                observed.store(
                    app.shutdownState() == App::Application::ShutdownState::Running
                        && !app.getDocument("CleanerIncoming")
                        && Gui::Internal::pendingFileOpenCountForTesting(true) == 1
                    ? 1 : -1);
            }, true);
        }
        catch (...) { observed.store(-2); }
    }
private:
    ShutdownGate& gate;
    QString path;
    std::atomic<int>& observed;
};
ShutdownGate* ownerGate = nullptr;
App::Document* expectedGuiDocument = nullptr;
std::atomic<int>* initialCloseObserved = nullptr;
void releaseOwnerAfterFirstClose()
{
    const bool live = App::GetApplication().getDocument("GuiCleanerActive") == expectedGuiDocument
        && Gui::Application::Instance
        && Gui::Application::Instance->getDocument(expectedGuiDocument);
    initialCloseObserved->store(live ? 1 : -1);
    {
        std::lock_guard lock(ownerGate->mutex);
        ownerGate->release = true;
    }
    ownerGate->changed.notify_all();
}
}

namespace {
void exerciseFileOpenDuringCleanerPreflight(bool throughCloseEvent)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
    std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
        if (!watchdogChanged.wait_for(lock, 25s, [&] { return done; })) std::_Exit(91); });
    App::Application::Config()["ExeName"] =
        "freecad-preflight-open-" + std::to_string(QCoreApplication::applicationPid());
    static int argc = 1; static char name[] = "GuiShutdown_tests_run";
    static char* argv[2]; argv[0] = name; argv[1] = nullptr;
    std::unique_ptr<QApplication> qt(Gui::Internal::createSingleApplicationForTesting(argc, argv));
    tests::initApplication();
    Gui::Application::initApplication();
    Gui::Application::initOpenInventor();
    auto& app = App::GetApplication();
    QTemporaryDir dir;
    if (!dir.isValid()) std::_Exit(92);
    const QString path = dir.filePath(QStringLiteral("CleanerIncoming.FCStd"));
    {
        Gui::Application gui(true);
        Gui::MainWindow window;
        window.setProperty("eventLoop", true);
        auto* source = app.newDocument("CleanerIncoming");
        if (!source || !source->saveAs(path.toUtf8().constData())
            || !app.closeDocument("CleanerIncoming")) std::_Exit(93);
        auto* existing = app.newDocument("CleanerPreflightExisting");
        if (!existing || !existing->executionLane()) std::_Exit(94);
        ShutdownGate gate;
        std::atomic<int> observed {0};
        // This test owns and joins the managed worker. Its thread-exit drain
        // blocks on a real GUI marshal until preflight services the callback.
        std::thread worker([&] {
            App::initializeManagedPropertyCleanup(true);
            App::Property::destroy(new MarshalledFileOpenOnExit(gate, path, observed));
        });
        {
            std::unique_lock lock(gate.mutex);
            if (!gate.changed.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(95);
        }
        if (throughCloseEvent) {
            QCloseEvent close;
            QApplication::sendEvent(&window, &close);
            if (!close.isAccepted()) std::_Exit(96);
        }
        else {
            gui.shutdownForTesting();
        }
        worker.join();
        if (observed.load() != 1 || app.getDocument("CleanerIncoming")
            || app.shutdownState() == App::Application::ShutdownState::Running
            || Gui::Internal::pendingFileOpenCountForTesting(true) != 1) std::_Exit(97);
    }
    App::Application::destruct();
    { std::lock_guard lock(watchdogMutex); done = true; }
    watchdogChanged.notify_all(); watchdog.join();
    std::_Exit(0);
}
}

TEST(MainWindowShutdown, closeEventDefersFileOpenDuringCleanerPreflight)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseFileOpenDuringCleanerPreflight(true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, guiShutdownDefersFileOpenDuringCleanerPreflight)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseFileOpenDuringCleanerPreflight(false), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, foreignExitDrainRefusesBeforeWindowAndDocumentsClose)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        QApplication qt(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        {
            Gui::Application gui(true);
            Gui::MainWindow window;
            auto* document = App::GetApplication().newDocument("GuiCleanerBlocked");
            if (!document || !gui.getDocument(document)) std::_Exit(92);
            ShutdownGate gate; std::atomic<int> observed {0};
            std::thread worker([&] { App::Property::destroy(new BlockingExitProperty(gate, document, observed)); });
            { std::unique_lock lock(gate.mutex);
              if (!gate.changed.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
            QCloseEvent rejected;
            QApplication::sendEvent(&window, &rejected);
            if (rejected.isAccepted() || Gui::Application::Instance != &gui
                || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
                || App::GetApplication().getDocument("GuiCleanerBlocked") != document
                || !gui.getDocument(document)) std::_Exit(94);
            { std::lock_guard lock(gate.mutex); gate.release = true; }
            gate.changed.notify_all(); worker.join();
            if (observed.load() != 1) std::_Exit(95);
            QCloseEvent accepted;
            QApplication::sendEvent(&window, &accepted);
            if (!accepted.isAccepted() || App::GetApplication().getDocument("GuiCleanerBlocked"))
                std::_Exit(96);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, activeOwnerClosesOnRetryBeforeGuiTargetsAreRetired)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        QApplication qt(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        auto& app = App::GetApplication();
        {
            auto gui = std::make_unique<Gui::Application>(true);
            auto window = std::make_unique<Gui::MainWindow>();
            auto* document = app.newDocument("GuiCleanerActive");
            if (!document || !document->executionLane() || !gui->getDocument(document)) std::_Exit(92);
            ShutdownGate gate; std::atomic<int> observed {0};
            int guiDeletes = 0;
            gui->signalDeleteDocument.connect([&](const Gui::Document&) {
                if (Gui::Application::Instance == gui.get()) ++guiDeletes;
            });
            auto future = document->executionLane()->postToOwner([&] {
                std::unique_lock lock(gate.mutex);
                gate.entered = true; gate.changed.notify_all();
                gate.changed.wait(lock, [&] { return gate.release; });
            });
            { std::unique_lock lock(gate.mutex);
              if (!gate.changed.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
            ownerGate = &gate; expectedGuiDocument = document; initialCloseObserved = &observed;
            App::Internal::setPostInitialFinalizationCloseHookForTesting(&releaseOwnerAfterFirstClose);
            QCloseEvent close;
            QApplication::sendEvent(window.get(), &close);
            App::Internal::setPostInitialFinalizationCloseHookForTesting(nullptr);
            if (!close.isAccepted() || observed.load() != 1 || guiDeletes != 1
                || app.getDocument("GuiCleanerActive")) std::_Exit(94);
            future.get();
            window.reset();
            gui.reset();
            if (app.signalNewDocument.num_slots() || app.signalDeleteDocument.num_slots()
                || app.signalRenameDocument.num_slots() || app.signalActiveDocument.num_slots()
                || app.signalRelabelDocument.num_slots() || app.signalShowHidden.num_slots()
                || app.signalDeletedObject.num_slots()) std::_Exit(95);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, canceledCloseReleasesThePreflightReservation)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        QApplication qt(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        {
            Gui::Application gui(true);
            Gui::MainWindow window;
            auto* document = App::GetApplication().newDocument("GuiCleanerCancel");
            if (!document || !document->executionLane()) std::_Exit(92);
            document->setClosable(false);
            QTimer::singleShot(0, [] {
                if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    dialog->accept();
            });
            QCloseEvent rejected;
            QApplication::sendEvent(&window, &rejected);
            if (rejected.isAccepted()
                || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
                || App::GetApplication().getDocument("GuiCleanerCancel") != document) std::_Exit(93);
            document->setClosable(true);
            auto* second = App::GetApplication().newDocument("GuiCleanerAfterCancel");
            if (!second || !second->executionLane()) std::_Exit(94);
            auto alive = second->executionLane()->postToOwner([] { return 42; });
            if (alive.get() != 42) std::_Exit(95);
            QCloseEvent accepted;
            QApplication::sendEvent(&window, &accepted);
            if (!accepted.isAccepted()) std::_Exit(96);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}


// Run in a death-test subprocess because these cases tear down the sole Gui and
// App instances. The socket path exercises the same QLocalServer queue and
// timer that the normal single-instance startup uses.
namespace {
void exerciseDeferredFileOpen(bool useFileEvent, bool cancelClose, bool failAfterSeal = false)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
    std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
        if (!watchdogChanged.wait_for(lock, 25s, [&] { return done; })) std::_Exit(91); });
    const QString server = QStringLiteral("freecad-shutdown-test-%1")
                               .arg(QCoreApplication::applicationPid());
    App::Application::Config()["ExeName"] = server.toStdString();
    static int argc = 1; static char name[] = "GuiShutdown_tests_run";
    static char* argv[2]; argv[0] = name; argv[1] = nullptr;
    std::unique_ptr<QApplication> qt(Gui::Internal::createSingleApplicationForTesting(argc, argv));
    tests::initApplication();
    Gui::Application::initApplication();
    Gui::Application::initOpenInventor();
    auto& app = App::GetApplication();
    QTemporaryDir dir;
    if (!dir.isValid()) std::_Exit(92);
    const QString path = dir.filePath(QStringLiteral("Incoming.FCStd"));
    {
        Gui::Application gui(true);
        Gui::MainWindow window;
        window.setProperty("eventLoop", true);
        if (!QObject::connect(qt.get(), SIGNAL(messageReceived(const QList<QString>&)),
                              &window, SLOT(processMessages(const QList<QString>&)))) std::_Exit(102);
        QSignalSpy delivered(qt.get(), SIGNAL(messageReceived(const QList<QString>&)));
        if (!delivered.isValid()) std::_Exit(103);
        auto* source = app.newDocument("Incoming");
        if (!source || !source->saveAs(path.toUtf8().constData())) std::_Exit(93);
        app.closeDocument("Incoming");
        auto* dirty = app.newDocument("GuiCleanerModal");
        if (!dirty || !dirty->addObject("App::Feature", "Dirty")
            || !gui.getDocument(dirty)->isModified()) std::_Exit(94);
        int opened = 0;
        auto observer = app.signalNewDocument.connect([&](const App::Document&, bool) { ++opened; });
        QTimer::singleShot(0, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box || box->objectName() != QStringLiteral("confirmSave")) std::_Exit(95);
            if (useFileEvent) {
                QFileOpenEvent incoming(path);
                QApplication::sendEvent(qt.get(), &incoming);
            }
            else {
                auto* socket = new QLocalSocket(qt.get());
                socket->connectToServer(server);
                if (!socket->waitForConnected(3000)) std::_Exit(96);
                socket->write((QStringLiteral("OpenFile:") + path + QLatin1Char('\n')).toUtf8());
                socket->flush();
            }
            // The server's real batching timer fires while this modal is open.
            QTimer::singleShot(1700, [&, box] {
                if (opened || app.getDocument("Incoming") || delivered.count()
                    || Gui::Internal::pendingFileOpenCountForTesting(useFileEvent) != 1) std::_Exit(97);
                box->done(cancelClose ? QMessageBox::Cancel : QMessageBox::Discard);
            });
        });
        if (failAfterSeal) App::Internal::failNextFinalizationAfterSealForTesting();
        QCloseEvent close;
        QApplication::sendEvent(&window, &close);
        if (close.isAccepted() == (cancelClose || failAfterSeal) || opened || delivered.count())
            std::_Exit(98);
        QEventLoop resumed;
        QTimer::singleShot(1000, &resumed, &QEventLoop::quit);
        resumed.exec();
        if (cancelClose) {
            if (opened != 1 || !app.getDocument("Incoming")
                || delivered.count() != (useFileEvent ? 0 : 1)
                || Gui::Internal::pendingFileOpenCountForTesting(useFileEvent) != 0
                || app.shutdownState() != App::Application::ShutdownState::Running) std::_Exit(99);
            // Close without another save prompt after validating the request.
            dirty->setStatus(App::Document::TempDoc, true);
            if (auto* incoming = app.getDocument("Incoming"))
                incoming->setStatus(App::Document::TempDoc, true);
            QCloseEvent finalClose;
            QApplication::sendEvent(&window, &finalClose);
            if (!finalClose.isAccepted()) std::_Exit(100);
        }
        else {
            if (opened || app.getDocument("Incoming") || delivered.count()
                || Gui::Internal::pendingFileOpenCountForTesting(useFileEvent) != 1) std::_Exit(101);
            if (failAfterSeal) {
                if (app.shutdownState() != App::Application::ShutdownState::Incomplete) std::_Exit(104);
                QCloseEvent retry;
                QApplication::sendEvent(&window, &retry);
                if (!retry.isAccepted() || opened || delivered.count()
                    || Gui::Internal::pendingFileOpenCountForTesting(useFileEvent) != 1) std::_Exit(105);
            }
        }
        observer.disconnect();
    }
    App::Application::destruct();
    { std::lock_guard lock(watchdogMutex); done = true; }
    watchdogChanged.notify_all(); watchdog.join();
    std::_Exit(0);
}
}

TEST(MainWindowShutdown, socketOpenDuringModalIsRetainedWhenCloseIsCanceled)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(false, true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, socketOpenDuringModalIsNotConsumedAfterAcceptedShutdown)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(false, false), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, socketOpenIsNotConsumedAfterIncompleteShutdown)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(false, false, true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, fileOpenEventDuringModalIsRetainedWhenCloseIsCanceled)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(true, true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, fileOpenEventIsNotConsumedAfterAcceptedShutdown)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(true, false), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, fileOpenEventIsNotConsumedAfterIncompleteShutdown)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseDeferredFileOpen(true, false, true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, noInteractionSessionUsesTheCleanerBoundaryBeforeClosingDocuments)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
        App::Application::Config()["ExeName"] =
            "freecad-session-test-" + std::to_string(QCoreApplication::applicationPid());
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        std::unique_ptr<QApplication> qt(Gui::Internal::createSingleApplicationForTesting(argc, argv));
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        auto& app = App::GetApplication();
        {
            Gui::Application gui(true);
            Gui::MainWindow window;
            auto* document = app.newDocument("GuiCleanerBlocked");
            if (!document || !document->executionLane() || !gui.getDocument(document)
                || !document->addObject("App::Feature", "Dirty")
                || !gui.getDocument(document)->isModified()) std::_Exit(92);
            ShutdownGate gate; std::atomic<int> observed {0};
            std::thread worker([&] { App::Property::destroy(new BlockingExitProperty(gate, document, observed)); });
            { std::unique_lock lock(gate.mutex);
              if (!gate.changed.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
            if (Gui::Internal::closeWithoutSessionInteractionForTesting()
                || app.shutdownState() != App::Application::ShutdownState::Running
                || app.getDocument("GuiCleanerBlocked") != document || !gui.getDocument(document)) std::_Exit(94);
            auto stillRunning = document->executionLane()->postToOwner([] { return 42; });
            if (stillRunning.get() != 42) std::_Exit(95);
            { std::lock_guard lock(gate.mutex); gate.release = true; }
            gate.changed.notify_all(); worker.join();
            if (observed.load() != 1 || !Gui::Internal::closeWithoutSessionInteractionForTesting()
                || app.getDocument("GuiCleanerBlocked")) std::_Exit(96);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}


namespace {
class ShutdownPassiveView final : public Gui::BaseView
{
public:
    bool allow {false};
    bool sawDocument {false};
    int closeChecks {0};
    bool onMsg(const char*) override { return false; }
    bool onHasMsg(const char*) const override { return false; }
    bool canClose() override
    {
        ++closeChecks;
        sawDocument = getAppDocument() != nullptr;
        return allow;
    }
};
}

TEST(MainWindowShutdown, passiveViewRefusalAndRetryDetachFromTheDeletedDocument)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        QApplication qt(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        auto& app = App::GetApplication();
        {
            Gui::Application gui(true);
            Gui::MainWindow window;
            auto* doc = app.newDocument("GuiCleanerPassive");
            if (!doc || !gui.getDocument(doc)) std::_Exit(92);
            gui.setActiveDocument(gui.getDocument(doc));
            ShutdownPassiveView view;
            view.setDocument(gui.getDocument(doc));
            QCloseEvent rejected;
            QApplication::sendEvent(&window, &rejected);
            if (rejected.isAccepted() || !view.sawDocument || view.closeChecks != 1
                || view.getAppDocument() != doc || app.getDocument("GuiCleanerPassive") != doc)
                std::_Exit(93);
            view.allow = true;
            QCloseEvent accepted;
            QApplication::sendEvent(&window, &accepted);
            if (!accepted.isAccepted() || !view.sawDocument || view.closeChecks != 2
                || view.getAppDocument() || app.getDocument("GuiCleanerPassive")) std::_Exit(94);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}


namespace {
void writeTestFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(data) != data.size()) std::_Exit(106);
}
QByteArray readTestFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) std::_Exit(107);
    return file.readAll();
}

void exerciseBatchedMacroClose(bool cancelClose, bool failAfterSeal)
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
    std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
    std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
        if (!watchdogChanged.wait_for(lock, 25s, [&] { return done; })) std::_Exit(91); });
    const QString server = QStringLiteral("freecad-batch-test-%1")
                               .arg(QCoreApplication::applicationPid());
    App::Application::Config()["ExeName"] = server.toStdString();
    static int argc = 1; static char name[] = "GuiShutdown_tests_run";
    static char* argv[2]; argv[0] = name; argv[1] = nullptr;
    std::unique_ptr<QApplication> qt(Gui::Internal::createSingleApplicationForTesting(argc, argv));
    tests::initApplication();
    Gui::Application::initApplication();
    Gui::Application::initOpenInventor();
    auto& app = App::GetApplication();
    QTemporaryDir dir;
    if (!dir.isValid()) std::_Exit(92);
    const QString first = dir.filePath(QStringLiteral("first.FCMacro"));
    const QString second = dir.filePath(QStringLiteral("second.FCMacro"));
    const QString marker = dir.filePath(QStringLiteral("marker"));
    QString escaped = marker;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    const QByteArray prefix = "with open('" + escaped.toUtf8() + "', 'a') as out:\n    out.write('";
    writeTestFile(first, prefix + "a')\nimport FreeCADGui\nFreeCADGui.getMainWindow().close()\n"
                  + "FreeCADGui.updateGui()\n" + prefix + "c')\n");
    writeTestFile(second, prefix + "b')\n");
    {
        Gui::Application gui(true);
        Gui::MainWindow window;
        window.setProperty("eventLoop", true);
        if (!QObject::connect(qt.get(), SIGNAL(messageReceived(const QList<QString>&)),
                              &window, SLOT(processMessages(const QList<QString>&)))) std::_Exit(93);
        QSignalSpy delivered(qt.get(), SIGNAL(messageReceived(const QList<QString>&)));
        if (!delivered.isValid()) std::_Exit(94);
        auto* dirty = app.newDocument("BatchDirty");
        if (!dirty || !dirty->addObject("App::Feature", "Dirty")
            || !gui.getDocument(dirty)->isModified()) std::_Exit(95);
        bool sawDialog = false;
        QTimer responder;
        responder.setInterval(5);
        QObject::connect(&responder, &QTimer::timeout, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!box || box->objectName() != QStringLiteral("confirmSave")) return;
            sawDialog = true;
            responder.stop();
            // The second request must still be owned by the single-instance queue.
            if (Gui::Internal::pendingFileOpenCountForTesting(false) != 1
                || readTestFile(marker) != "a") std::_Exit(96);
            box->done(cancelClose ? QMessageBox::Cancel : QMessageBox::Discard);
        });
        responder.start();
        if (failAfterSeal) App::Internal::failNextFinalizationAfterSealForTesting();
        QLocalSocket sender;
        sender.connectToServer(server);
        if (!sender.waitForConnected(3000)) std::_Exit(97);
        const QByteArray batch = (QStringLiteral("OpenFile:") + first + QLatin1Char('\n')
                                  + QStringLiteral("OpenFile:") + second + QLatin1Char('\n')).toUtf8();
        if (sender.write(batch) != batch.size()) std::_Exit(98);
        sender.flush();
        QEventLoop run;
        QTimer::singleShot(3500, &run, &QEventLoop::quit);
        run.exec();
        responder.stop();
        if (!sawDialog) std::_Exit(99);
        if (cancelClose) {
            if (readTestFile(marker) != "acb" || delivered.count() != 2
                || Gui::Internal::pendingFileOpenCountForTesting(false) != 0
                || app.shutdownState() != App::Application::ShutdownState::Running) std::_Exit(100);
            dirty->setStatus(App::Document::TempDoc, true);
            QCloseEvent retry;
            QApplication::sendEvent(&window, &retry);
            if (!retry.isAccepted()) std::_Exit(101);
        }
        else {
            if (readTestFile(marker) != "ac" || delivered.count() != 1
                || Gui::Internal::pendingFileOpenCountForTesting(false) != 1) std::_Exit(102);
            if (failAfterSeal) {
                if (app.shutdownState() != App::Application::ShutdownState::Incomplete) std::_Exit(103);
                QCloseEvent retry;
                QApplication::sendEvent(&window, &retry);
                if (!retry.isAccepted() || Gui::Internal::pendingFileOpenCountForTesting(false) != 1
                    || readTestFile(marker) != "ac") std::_Exit(104);
            }
            else if (app.shutdownState() == App::Application::ShutdownState::Running) std::_Exit(105);
        }
    }
    App::Application::destruct();
    { std::lock_guard lock(watchdogMutex); done = true; }
    watchdogChanged.notify_all(); watchdog.join();
    std::_Exit(0);
}
}

TEST(MainWindowShutdown, batchReentryCancellationRunsSecondMacroOnce)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseBatchedMacroClose(true, false), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, batchReentryAcceptedCloseRetainsSecondMacro)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseBatchedMacroClose(false, false), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, batchReentryIncompleteCloseRetainsSecondMacro)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(exerciseBatchedMacroClose(false, true), ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, noInteractionSessionRefusesModifiedPassiveEditorWithoutPromptOrSave)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 20s, [&] { return done; })) std::_Exit(91); });
        App::Application::Config()["ExeName"] =
            "freecad-editor-session-" + std::to_string(QCoreApplication::applicationPid());
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        std::unique_ptr<QApplication> qt(Gui::Internal::createSingleApplicationForTesting(argc, argv));
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        auto& app = App::GetApplication();
        QTemporaryDir dir;
        if (!dir.isValid()) std::_Exit(92);
        const auto path = dir.filePath(QStringLiteral("draft.txt"));
        writeTestFile(path, "original");
        {
            Gui::Application gui(true);
            Gui::MainWindow window;
            auto* doc = app.newDocument("SessionEditorDoc");
            if (!doc || !doc->executionLane()) std::_Exit(93);
            Gui::EditorView editor(new Gui::TextEdit(), &window);
            if (!editor.isPassive() || !editor.open(path)) std::_Exit(94);
            editor.getEditor()->setPlainText(QStringLiteral("unsaved edit"));
            editor.getEditor()->document()->setModified(true);
            bool dialogSeen = false;
            QTimer responder;
            responder.setInterval(1);
            QObject::connect(&responder, &QTimer::timeout, [&] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    dialogSeen = true;
                    box->done(QMessageBox::Cancel);
                }
            });
            responder.start();
            const bool closed = Gui::Internal::closeWithoutSessionInteractionForTesting();
            responder.stop();
            if (closed || dialogSeen || readTestFile(path) != "original"
                || !editor.getEditor()->document()->isModified()
                || app.shutdownState() != App::Application::ShutdownState::Running
                || app.getDocument("SessionEditorDoc") != doc) std::_Exit(95);
            auto stillRunning = doc->executionLane()->postToOwner([] { return 42; });
            if (stillRunning.get() != 42) std::_Exit(96);
            // The ordinary interactive close still asks and honors Cancel.
            responder.start();
            QCloseEvent interactive;
            QApplication::sendEvent(&window, &interactive);
            responder.stop();
            if (interactive.isAccepted() || !dialogSeen || readTestFile(path) != "original"
                || app.getDocument("SessionEditorDoc") != doc) std::_Exit(97);
            editor.getEditor()->document()->setModified(false);
            if (!Gui::Internal::closeWithoutSessionInteractionForTesting()
                || app.getDocument("SessionEditorDoc")) std::_Exit(98);
        }
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}

TEST(MainWindowShutdown, guiTargetsDisconnectAppSignalsOnDestruction)
{
    const auto oldStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        static int argc = 1; static char name[] = "GuiShutdown_tests_run";
        static char* argv[2]; argv[0] = name; argv[1] = nullptr;
        QApplication qt(argc, argv);
        tests::initApplication();
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        auto& app = App::GetApplication();
        auto gui = std::make_unique<Gui::Application>(true);
        (void)Gui::Selection();
        const auto newSlots = app.signalNewDocument.num_slots();
        const auto deleteSlots = app.signalDeleteDocument.num_slots();
        const auto renameSlots = app.signalRenameDocument.num_slots();
        const auto activeSlots = app.signalActiveDocument.num_slots();
        const auto relabelSlots = app.signalRelabelDocument.num_slots();
        const auto hiddenSlots = app.signalShowHidden.num_slots();
        const auto objectSlots = app.signalDeletedObject.num_slots();
        gui.reset();
        if (app.signalNewDocument.num_slots() >= newSlots
            || app.signalDeleteDocument.num_slots() >= deleteSlots
            || app.signalRenameDocument.num_slots() >= renameSlots
            || app.signalActiveDocument.num_slots() >= activeSlots
            || app.signalRelabelDocument.num_slots() >= relabelSlots
            || app.signalShowHidden.num_slots() >= hiddenSlots
            || app.signalDeletedObject.num_slots() >= objectSlots) std::_Exit(95);
        App::Application::destruct();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, oldStyle);
}
#endif
