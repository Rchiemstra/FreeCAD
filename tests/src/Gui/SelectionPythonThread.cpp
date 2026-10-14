// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/MainThreadSignal.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <src/App/InitApplication.h>

#include <thread>

namespace
{
std::thread::id selectionMainThread;
bool isSelectionMainThread()
{
    return std::this_thread::get_id() == selectionMainThread;
}
void invokeSelectionMainThread(std::function<void()>&& fn, bool)
{
    fn();
}

// Exercise the Python C boundary directly so the pre-fix C++ exception can be
// caught without unwinding through CPython frames and ending the test process.
class SelectionPythonAccess: public Gui::SelectionSingleton
{
public:
    using SelectionSingleton::sAddSelection;
    using SelectionSingleton::sClearSelection;
    using SelectionSingleton::sRemoveSelection;
};

class SelectionPythonThreadTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "SelectionPythonThread";
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
        selectionMainThread = std::this_thread::get_id();
        App::MainThreadSignalConfig::setHooks(
            &isSelectionMainThread, &invokeSelectionMainThread);
        App::DocumentInitFlags flags;
        flags.createView = false;
        doc = App::GetApplication().newDocument("SelectionPythonThread", "testUser", flags);
        object = doc->addObject("App::FeatureTest", "Rectangle");
        Gui::Selection().clearSelection();
    }

    void TearDown() override
    {
        Gui::Selection().clearSelection();
        App::GetApplication().closeDocument(doc->getName());
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
    }

    void expectWorkerRejection(PyCFunction function, const char* api, bool withObject)
    {
        std::string escapedException;
        std::string pythonError;
        bool runtimeError = false;
        bool returnedNull = false;
        std::thread worker([&] {
            Base::PyGILStateLocker gil;
            PyObject* args = withObject ? Py_BuildValue("(ss)", doc->getName(), "Rectangle")
                                        : PyTuple_New(0);
            try {
                PyObject* result = function(nullptr, args);
                returnedNull = result == nullptr;
                Py_XDECREF(result);
                runtimeError = PyErr_ExceptionMatches(PyExc_RuntimeError);
                PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
                PyErr_Fetch(&type, &value, &traceback);
                if (value) {
                    PyObject* text = PyObject_Str(value);
                    if (text) {
                        pythonError = PyUnicode_AsUTF8(text);
                        Py_DECREF(text);
                    }
                }
                Py_XDECREF(type);
                Py_XDECREF(value);
                Py_XDECREF(traceback);
            }
            catch (const Base::Exception& exception) {
                escapedException = exception.what();
                Base::setPythonFrameInspectionUnsafe(false);
            }
            Py_DECREF(args);
            PyErr_Clear();
        });
        worker.join();
        EXPECT_TRUE(escapedException.empty()) << escapedException;
        EXPECT_TRUE(returnedNull);
        EXPECT_TRUE(runtimeError);
        EXPECT_NE(pythonError.find(api), std::string::npos) << pythonError;
        EXPECT_NE(pythonError.find("main thread"), std::string::npos) << pythonError;
    }

    App::Document* doc = nullptr;
    App::DocumentObject* object = nullptr;
};
}

TEST_F(SelectionPythonThreadTest, workerAddRejectsBeforePublishingSelection)
{
    expectWorkerRejection(&SelectionPythonAccess::sAddSelection,
                          "FreeCADGui.Selection.addSelection", true);
    EXPECT_FALSE(Gui::Selection().hasSelection());
}

TEST_F(SelectionPythonThreadTest, workerClearPreservesExistingSelection)
{
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), object->getNameInDocument()));
    expectWorkerRejection(&SelectionPythonAccess::sClearSelection,
                          "FreeCADGui.Selection.clearSelection", false);
    EXPECT_TRUE(Gui::Selection().isSelected(object));
}

TEST_F(SelectionPythonThreadTest, workerRemovePreservesExistingSelection)
{
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), object->getNameInDocument()));
    expectWorkerRejection(&SelectionPythonAccess::sRemoveSelection,
                          "FreeCADGui.Selection.removeSelection", true);
    EXPECT_TRUE(Gui::Selection().isSelected(object));
}

TEST_F(SelectionPythonThreadTest, mainThreadPythonSelectionStillWorks)
{
    Base::PyGILStateLocker gil;
    PyObject* args = Py_BuildValue("(ss)", doc->getName(), object->getNameInDocument());
    PyObject* result = SelectionPythonAccess::sAddSelection(nullptr, args);
    ASSERT_NE(result, nullptr);
    Py_DECREF(result);
    EXPECT_TRUE(Gui::Selection().isSelected(object));
    result = SelectionPythonAccess::sRemoveSelection(nullptr, args);
    ASSERT_NE(result, nullptr);
    Py_DECREF(result);
    Py_DECREF(args);
    EXPECT_FALSE(Gui::Selection().hasSelection());
}
