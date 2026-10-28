// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/MainThreadSignal.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/Sequencer.h>
#include <Base/Writer.h>
#include <Gui/ApplicationPy.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/ProgressBar.h>
#include <src/App/InitApplication.h>

#include <QApplication>
#include <QTimer>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace
{

std::thread::id guiTestMainThread;

bool updateGuiTestIsMainThread()
{
    return std::this_thread::get_id() == guiTestMainThread;
}

void updateGuiTestInvokeOnMain(std::function<void()>&& fn, bool /*blocking*/)
{
    fn();
}

class ErrorLogCapture: public Base::ILogger
{
public:
    void sendLog(
        const std::string& /*notifiername*/,
        const std::string& msg,
        Base::LogStyle level,
        Base::IntendedRecipient /*recipient*/,
        Base::ContentType /*content*/) override
    {
        logs += msg;
        if (level == Base::LogStyle::Error || level == Base::LogStyle::Critical) {
            errors += msg;
        }
    }

    std::string errors;
    std::string logs;
};

class UpdateGuiThreadTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "UpdateGuiThread";
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
        Gui::SequencerBar::instance()->getProgressBar();
    }

    void SetUp() override
    {
        guiTestMainThread = std::this_thread::get_id();
        App::MainThreadSignalConfig::setHooks(
            &updateGuiTestIsMainThread, &updateGuiTestInvokeOnMain);
        Base::Console().attachObserver(&errors);
    }

    void TearDown() override
    {
        Base::Console().detachObserver(&errors);
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
    }

    std::string callUpdateGui()
    {
        Base::PyGILStateLocker gil;
        PyObject* args = PyTuple_New(0);
        PyObject* result = Gui::ApplicationPy::sUpdateGui(nullptr, args);
        Py_DECREF(args);
        if (result) {
            const bool isNone = result == Py_None;
            Py_DECREF(result);
            return isNone ? "" : "updateGui did not return None";
        }
        PyObject *type = nullptr;
        PyObject *value = nullptr;
        PyObject *traceback = nullptr;
        PyErr_Fetch(&type, &value, &traceback);
        std::string message;
        if (value) {
            PyObject* text = PyObject_Str(value);
            if (text && PyUnicode_Check(text)) {
                message = PyUnicode_AsUTF8(text);
            }
            Py_XDECREF(text);
        }
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return message.empty() ? "updateGui failed" : message;
    }

    ErrorLogCapture errors;
};

}  // namespace

TEST_F(UpdateGuiThreadTest, updateGuiOnMainThreadPumpsWithoutError)
{
    bool eventProcessed = false;
    QTimer::singleShot(0, [&] { eventProcessed = true; });
    const std::string message = callUpdateGui();
    EXPECT_TRUE(eventProcessed);
    EXPECT_TRUE(message.empty()) << message;
    EXPECT_TRUE(errors.errors.empty()) << errors.errors;
}

TEST_F(UpdateGuiThreadTest, updateGuiOffMainThreadIsSilent)
{
    std::atomic<bool> eventProcessed {false};
    QTimer::singleShot(0, [&] { eventProcessed = true; });
    std::string message;
    std::thread worker([this, &message] {
        message = callUpdateGui();
    });
    worker.join();
    EXPECT_TRUE(message.empty()) << message;
    EXPECT_EQ(errors.errors.find("main thread"), std::string::npos) << errors.errors;
    EXPECT_TRUE(errors.logs.empty()) << errors.logs;
    EXPECT_FALSE(eventProcessed.load());
    QApplication::processEvents();
    EXPECT_TRUE(eventProcessed.load());
}

TEST_F(UpdateGuiThreadTest, progressIndicatorOffMainThreadDoesNotLogOrThrow)
{
    std::string message;
    std::thread worker([&message] {
        Base::PyGILStateLocker gil;
        PyObject* module = PyImport_AddModule("__main__");
        PyObject* globals = PyModule_GetDict(module);
        PyObject* result = PyRun_String(
            "import FreeCAD\n"
            "indicator = FreeCAD.Base.ProgressIndicator()\n"
            "indicator.start('owner progress', 3)\n"
            "indicator.next()\n"
            "indicator.next()\n"
            "indicator.stop()\n",
            Py_file_input,
            globals,
            globals);
        if (!result) {
            PyObject *type = nullptr;
            PyObject *value = nullptr;
            PyObject *traceback = nullptr;
            PyErr_Fetch(&type, &value, &traceback);
            if (value) {
                PyObject* text = PyObject_Str(value);
                if (text && PyUnicode_Check(text)) {
                    message = PyUnicode_AsUTF8(text);
                }
                Py_XDECREF(text);
            }
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
            if (message.empty()) {
                message = "progress indicator failed";
            }
        }
        else {
            Py_DECREF(result);
        }
    });
    worker.join();
    EXPECT_TRUE(message.empty()) << message;
    EXPECT_EQ(errors.errors.find("main thread"), std::string::npos) << errors.errors;

    std::string sequencerError;
    std::thread sequencer([&sequencerError] {
        try {
            Base::SequencerLauncher launcher("owner sequencer", 2);
            launcher.next();
            launcher.next();
        }
        catch (const Base::Exception& exception) {
            sequencerError = exception.what();
        }
    });
    sequencer.join();
    EXPECT_TRUE(sequencerError.empty()) << sequencerError;
    EXPECT_TRUE(errors.errors.empty()) << errors.errors;
}

TEST_F(UpdateGuiThreadTest, guiDocumentSaveReleasesGilBeforeBlockingInvoke)
{
    App::DocumentInitFlags flags;
    flags.createView = false;
    auto* document = App::GetApplication().newDocument("GuiSaveGil", "", flags);
    ASSERT_NE(document, nullptr);
    auto* guiDocument = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(guiDocument, nullptr);

    struct SaveInvokeProbe {};
    App::MainThreadSignalConfig::setHooks(
        &updateGuiTestIsMainThread,
        [](std::function<void()>&&, bool blocking) {
            EXPECT_TRUE(blocking);
            EXPECT_FALSE(PyGILState_Check());
            // Inspect the caller before any invoke hook can release the GIL.
            // Stop here so a regression fails without deadlocking the test.
            throw SaveInvokeProbe {};
        });
    for (const bool withGil : {false, true}) {
        std::thread worker([&] {
            std::optional<Base::PyGILStateLocker> gil;
            if (withGil) {
                gil.emplace();
            }
            Base::StringWriter writer;
            EXPECT_THROW(guiDocument->SaveDocFile(writer), SaveInvokeProbe);
            EXPECT_EQ(PyGILState_Check() != 0, withGil);
        });
        worker.join();
    }
    App::MainThreadSignalConfig::setHooks(
        &updateGuiTestIsMainThread, &updateGuiTestInvokeOnMain);
    App::GetApplication().closeDocument("GuiSaveGil");
}
