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
#include <Gui/Selection/SelectionObjectPy.h>
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
    using SelectionSingleton::sUpdateSelection;
    using SelectionSingleton::sSetPreselection;
    using SelectionSingleton::sClearPreselection;
    using SelectionSingleton::sEnablePickedList;
    using SelectionSingleton::sSetSelectionStyle;
    using SelectionSingleton::sAddObserver;
    using SelectionSingleton::sRemoveObserver;
    using SelectionSingleton::sAddSelectionGate;
    using SelectionSingleton::sRemoveSelectionGate;
    using SelectionSingleton::sPushSelStack;

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
        PyObject* args;
        {
            Base::PyGILStateLocker gil;
            args = withObject ? Py_BuildValue("(ss)", doc->getName(), "Rectangle")
                              : PyTuple_New(0);
        }
        expectWorkerRejection([&](PyObject* a) { return function(nullptr, a); }, api, args);
        Base::PyGILStateLocker gil;
        Py_DECREF(args);
    }

    void expectWorkerRejection(const std::function<PyObject*(PyObject*)>& function,
                               const char* api, PyObject* args)
    {
        std::string escapedException;
        std::string pythonError;
        bool runtimeError = false;
        bool returnedNull = false;
        std::thread worker([&] {
            Base::PyGILStateLocker gil;
            try {
                PyObject* result = function(args);
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
            PyErr_Clear();
        });
        worker.join();
        EXPECT_TRUE(escapedException.empty()) << escapedException;
        EXPECT_TRUE(returnedNull);
        EXPECT_TRUE(runtimeError);
        EXPECT_EQ(pythonError, std::string("GUI API '") + api
                                  + "' may only be used from the main thread");
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

namespace
{
enum class Mutator
{
    Update, SetPreselection, ClearPreselection, PickedList, Style,
    AddObserver, RemoveObserver, AddGate, RemoveGate, History, ObjectRemove
};

class RemainingSelectionMutatorTest: public SelectionPythonThreadTest,
                                    public ::testing::WithParamInterface<Mutator>
{
protected:
    void check(bool malformed)
    {
        auto& selection = Gui::Selection();
        ASSERT_TRUE(selection.addSelection(doc->getName(), object->getNameInDocument()));
        selection.setPreselect(doc->getName(), object->getNameInDocument(), "", 1, 2, 3,
                               Gui::SelectionChanges::MsgSource::Any);
        selection.enablePickedList(false);
        selection.setSelectionStyle(Gui::SelectionSingleton::SelectionStyle::NormalSelection);

        PyObject *probe, *args, *selectionObject, *globals, *pyObject;
        std::function<PyObject*(PyObject*)> call;
        const char* api = nullptr;
        {
            Base::PyGILStateLocker gil;
            globals = PyDict_New();
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyObject* result = PyRun_String(
                "class Probe:\n"
                "    reads = 0\n"
                "    events = 0\n"
                "    def __getattribute__(self, name):\n"
                "        type(self).reads += 1\n"
                "        return object.__getattribute__(self, name)\n"
                "    def addSelection(self, *args):\n"
                "        type(self).events += 1\n"
                "    def allow(self, *args):\n"
                "        return True\n"
                "probe = Probe()\n", Py_file_input, globals, globals);
            ASSERT_NE(result, nullptr);
            Py_DECREF(result);
            probe = PyDict_GetItemString(globals, "probe");
            selectionObject = Gui::SelectionObject(object).getPyObject();
            pyObject = object->getPyObject();
            args = PyTuple_New(0);
            auto configure = [&](PyCFunction fn, const char* name, PyObject* a) {
                call = [fn](PyObject* a) { return fn(nullptr, a); };
                api = name;
                Py_DECREF(args);
                args = a;
            };
            switch (GetParam()) {
                case Mutator::Update:
                    configure(&SelectionPythonAccess::sUpdateSelection,
                              "FreeCADGui.Selection.updateSelection",
                              Py_BuildValue("(OO)", Py_False, pyObject));
                    break;
                case Mutator::SetPreselection:
                    call = [](PyObject* a) {
                        return SelectionPythonAccess::sSetPreselection(nullptr, a, nullptr);
                    };
                    api = "FreeCADGui.Selection.setPreselection";
                    Py_DECREF(args);
                    args = Py_BuildValue("(Os)", pyObject, "Face1");
                    break;
                case Mutator::ClearPreselection:
                    configure(&SelectionPythonAccess::sClearPreselection,
                              "FreeCADGui.Selection.clearPreselection", PyTuple_New(0));
                    break;
                case Mutator::PickedList:
                    configure(&SelectionPythonAccess::sEnablePickedList,
                              "FreeCADGui.Selection.enablePickedList", PyTuple_New(0));
                    break;
                case Mutator::Style:
                    configure(&SelectionPythonAccess::sSetSelectionStyle,
                              "FreeCADGui.Selection.setSelectionStyle", Py_BuildValue("(i)", 1));
                    break;
                case Mutator::RemoveObserver: {
                    PyObject* a = PyTuple_Pack(1, probe);
                    result = SelectionPythonAccess::sAddObserver(nullptr, a);
                    ASSERT_NE(result, nullptr);
                    Py_DECREF(result);
                    Py_DECREF(a);
                    [[fallthrough]];
                }
                case Mutator::AddObserver:
                    configure(GetParam() == Mutator::AddObserver
                                  ? &SelectionPythonAccess::sAddObserver
                                  : &SelectionPythonAccess::sRemoveObserver,
                              GetParam() == Mutator::AddObserver
                                  ? "FreeCADGui.Selection.addObserver"
                                  : "FreeCADGui.Selection.removeObserver", PyTuple_Pack(1, probe));
                    break;
                case Mutator::RemoveGate: {
                    PyObject* a = PyTuple_Pack(1, probe);
                    result = SelectionPythonAccess::sAddSelectionGate(nullptr, a);
                    ASSERT_NE(result, nullptr);
                    Py_DECREF(result);
                    Py_DECREF(a);
                    [[fallthrough]];
                }
                case Mutator::AddGate:
                    configure(GetParam() == Mutator::AddGate
                                  ? &SelectionPythonAccess::sAddSelectionGate
                                  : &SelectionPythonAccess::sRemoveSelectionGate,
                              GetParam() == Mutator::AddGate
                                  ? "FreeCADGui.Selection.addSelectionGate"
                                  : "FreeCADGui.Selection.removeSelectionGate",
                              GetParam() == Mutator::AddGate ? PyTuple_Pack(1, probe)
                                                            : PyTuple_New(0));
                    break;
                case Mutator::History:
                    configure(&SelectionPythonAccess::sPushSelStack,
                              "FreeCADGui.Selection.pushSelStack", PyTuple_New(0));
                    break;
                case Mutator::ObjectRemove:
                    call = [selectionObject](PyObject* a) {
                        return static_cast<Gui::SelectionObjectPy*>(selectionObject)->remove(a);
                    };
                    api = "FreeCADGui.SelectionObject.remove";
                    break;
            }
            if (malformed) {
                Py_DECREF(args);
                args = PyTuple_Pack(5, Py_None, Py_None, Py_None, Py_None, Py_None);
            }
        }
        const auto preselection = selection.getPreselection();
        const auto gate = selection.getSelectionGate(doc);
        const auto back = selection.selStackBackSize();
        const auto forward = selection.selStackForwardSize();
        Py_ssize_t refs;
        long reads;
        auto probeCount = [&](const char* name) {
            PyObject* type = PyDict_GetItemString(globals, "Probe");
            PyObject* value = PyObject_GetAttrString(type, name);
            const long count = PyLong_AsLong(value);
            Py_DECREF(value);
            return count;
        };
        {
            Base::PyGILStateLocker gil;
            refs = Py_REFCNT(probe);
            reads = probeCount("reads");
        }
        int notifications = 0;
        auto record = [&](const Gui::SelectionChanges&) { ++notifications; };
        fastsignals::scoped_connection c1 = selection.signalSelectionChanged.connect(record);
        fastsignals::scoped_connection c2 = selection.signalSelectionChanged2.connect(record);
        fastsignals::scoped_connection c3 = selection.signalSelectionChanged3.connect(record);
        expectWorkerRejection(call, api, args);
        QApplication::processEvents();
        EXPECT_TRUE(selection.isSelected(object));
        EXPECT_EQ(selection.getPreselection().Object, preselection.Object);
        EXPECT_EQ(selection.getPreselection().Object.getSubName(), preselection.Object.getSubName());
        EXPECT_EQ(selection.getPreselection().x, preselection.x);
        EXPECT_EQ(selection.getSelectionGate(doc), gate);
        EXPECT_FALSE(selection.needPickedList());
        EXPECT_EQ(selection.getSelectionStyle(),
                  Gui::SelectionSingleton::SelectionStyle::NormalSelection);
        EXPECT_EQ(selection.selStackBackSize(), back);
        EXPECT_EQ(selection.selStackForwardSize(), forward);
        EXPECT_EQ(notifications, 0);
        {
            Base::PyGILStateLocker gil;
            EXPECT_EQ(Py_REFCNT(probe), refs);
            EXPECT_EQ(probeCount("reads"), reads);
            EXPECT_EQ(probeCount("events"), 0);
            selection.rmvSelectionGate();
            selection.rmvPreselect();
            // A later GUI notification proves registration/disconnection did not change.
            selection.clearSelection();
            selection.addSelection(doc->getName(), object->getNameInDocument());
            EXPECT_EQ(probeCount("events"), GetParam() == Mutator::RemoveObserver ? 1 : 0);
            PyObject* a = PyTuple_Pack(1, probe);
            PyObject* result = SelectionPythonAccess::sRemoveObserver(nullptr, a);
            Py_XDECREF(result);
            Py_DECREF(a);
            selection.rmvSelectionGate();
            selection.rmvPreselect();
            selection.enablePickedList(false);
            selection.setSelectionStyle(Gui::SelectionSingleton::SelectionStyle::NormalSelection);
            Py_DECREF(args);
            Py_DECREF(selectionObject);
            Py_DECREF(pyObject);
            Py_DECREF(globals);
        }
    }
};
}

TEST_P(RemainingSelectionMutatorTest, validCallPreservesStateAndNotifications)
{
    check(false);
}

TEST_P(RemainingSelectionMutatorTest, malformedCallRejectsBeforeParsing)
{
    check(true);
}

INSTANTIATE_TEST_SUITE_P(
    PythonSelection, RemainingSelectionMutatorTest,
    ::testing::Values(Mutator::Update, Mutator::SetPreselection, Mutator::ClearPreselection,
                      Mutator::PickedList, Mutator::Style, Mutator::AddObserver,
                      Mutator::RemoveObserver, Mutator::AddGate, Mutator::RemoveGate,
                      Mutator::History, Mutator::ObjectRemove),
    ([](const ::testing::TestParamInfo<Mutator>& info) {
        static const char* names[] = {"Update", "SetPreselection", "ClearPreselection", "PickedList",
                                     "Style", "AddObserver", "RemoveObserver", "AddGate",
                                     "RemoveGate", "History", "ObjectRemove"};
        return names[static_cast<int>(info.param)];
    }));
