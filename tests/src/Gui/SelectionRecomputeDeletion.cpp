// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QListWidget>
#include <QMetaObject>

#include <App/Application.h>
#include <App/Document.h>
#include <App/FeatureTest.h>
#include <App/MainThreadSignal.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/MainWindow.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionView.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace
{
std::thread::id deletionMainThread;
bool isDeletionMainThread()
{
    return std::this_thread::get_id() == deletionMainThread;
}
void invokeDeletionMainThread(std::function<void()>&& fn, bool blocking)
{
    if (isDeletionMainThread()) {
        fn();
        return;
    }
    QMetaObject::invokeMethod(QApplication::instance(), std::move(fn),
                              blocking ? Qt::BlockingQueuedConnection : Qt::QueuedConnection);
}

struct RetirementProbe
{
    std::atomic<bool> destroyed {false};
    std::atomic<bool> selectedAtDestruction {false};
};

class RetiringSelectionFeature: public App::FeatureTest
{
public:
    explicit RetiringSelectionFeature(RetirementProbe& probe): probe(probe) {}
    ~RetiringSelectionFeature() override
    {
        // Only this object is selected. hasSelection() does not dereference the
        // retiring pointer, so the regression reports the dangling entry even
        // when the allocator happens to leave readable memory behind.
        probe.selectedAtDestruction = Gui::Selection().hasSelection();
        probe.destroyed = true;
        EXPECT_FALSE(probe.selectedAtDestruction.load())
            << "Selection still contains the retiring object at destruction";
    }
private:
    RetirementProbe& probe;
};

class SelectionRecomputeDeletionTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "SelectionRecomputeDeletion";
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
        deletionMainThread = std::this_thread::get_id();
        App::MainThreadSignalConfig::setHooks(&isDeletionMainThread, &invokeDeletionMainThread);
        App::DocumentInitFlags flags;
        flags.createView = false;
        doc = App::GetApplication().newDocument("SelectionRecomputeDeletion", "testUser", flags);
        Gui::Selection().clearSelection();
    }

    void TearDown() override
    {
        Gui::Selection().clearSelection();
        App::GetApplication().closeDocument(doc->getName());
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
    }

    App::Document* doc = nullptr;
    RetirementProbe probe;
};
}

TEST_F(SelectionRecomputeDeletionTest, selectedObjectSurvivesUntilWorkerRecomputeDeletionReplay)
{
    auto* retiring = new RetiringSelectionFeature(probe);
    doc->addObject(retiring, "Retiring");
    // The victim is already recomputed; only the remover is scheduled below.
    retiring->purgeTouched();
    auto* remover = doc->addObject("App::FeaturePython", "Remover");
    {
        Base::PyGILStateLocker gil;
        PyObject* globals = PyDict_New();
        PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
        PyObject* pyRemover = remover->getPyObject();
        PyDict_SetItemString(globals, "remover", pyRemover);
        Py_DECREF(pyRemover);
        PyObject* result = PyRun_String(
            "import threading\n"
            "main_thread = threading.get_ident()\n"
            "class RemoveSelected:\n"
            "    def execute(self, obj):\n"
            "        self.worker = threading.get_ident() != main_thread\n"
            "        obj.Document.removeObject('Retiring')\n"
            "remover.Proxy = RemoveSelected()\n", Py_file_input, globals, globals);
        if (!result) {
            PyErr_Print();
        }
        ASSERT_NE(result, nullptr);
        Py_DECREF(result);
        Py_DECREF(globals);
    }
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Retiring"));
    auto* view = Gui::MainWindow::getInstance()->findChild<Gui::DockWnd::SelectionView*>();
    ASSERT_NE(view, nullptr);
    const auto lists = view->findChildren<QListWidget*>(QString(), Qt::FindDirectChildrenOnly);
    ASSERT_FALSE(lists.empty());
    auto* selectionList = lists.front();
    ASSERT_EQ(selectionList->count(), 1);
    int deleted = 0;
    bool aliveAtReplay = false;
    bool selectionClearedAtReplay = false;
    bool replayOnGui = false;
    fastsignals::scoped_connection connection = doc->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == retiring) {
                ++deleted;
                aliveAtReplay = !probe.destroyed;
                selectionClearedAtReplay = !Gui::Selection().hasSelection();
                replayOnGui = isDeletionMainThread();
            }
        });
    std::atomic<bool> done {false};
    bool recomputeError = false;
    std::string error;
    std::thread worker([&] {
        try {
            static_cast<void>(doc->recompute({}, false, &recomputeError));
        }
        catch (const std::exception& exception) {
            error = exception.what();
        }
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!done) {
        // A stalled worker still owns document pointers and may be waiting for
        // GUI replay. Fail this test process instead of hanging in join() or
        // tearing down its document under a live worker.
        ADD_FAILURE() << "worker recompute exceeded 30 seconds";
        std::abort();
    }
    worker.join();
    QApplication::processEvents();
    {
        Base::PyGILStateLocker gil;
        PyObject* pyRemover = remover->getPyObject();
        PyObject* proxy = PyObject_GetAttrString(pyRemover, "Proxy");
        PyObject* executedOnWorker = proxy ? PyObject_GetAttrString(proxy, "worker") : nullptr;
        EXPECT_NE(executedOnWorker, nullptr);
        if (executedOnWorker) {
            EXPECT_EQ(PyObject_IsTrue(executedOnWorker), 1);
        }
        Py_XDECREF(executedOnWorker);
        Py_XDECREF(proxy);
        Py_DECREF(pyRemover);
        PyErr_Clear();
    }
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(recomputeError);
    EXPECT_EQ(doc->getObject("Retiring"), nullptr);
    EXPECT_EQ(deleted, 1);
    EXPECT_TRUE(aliveAtReplay);
    EXPECT_TRUE(selectionClearedAtReplay);
    EXPECT_TRUE(replayOnGui);
    EXPECT_TRUE(probe.destroyed.load());
    EXPECT_FALSE(probe.selectedAtDestruction.load())
        << "Selection retained a dangling pointer when the recompute transaction destroyed its object";
    EXPECT_FALSE(Gui::Selection().hasSelection());
    EXPECT_EQ(selectionList->count(), 0);
    EXPECT_EQ(doc->getAvailableUndos(), 0);
}
