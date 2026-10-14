// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <QApplication>
#include <QEvent>
#include <QListWidget>
#include <QMetaObject>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/FeatureTest.h>
#include <App/MainThreadSignal.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <Gui/Selection/Selection.h>
#include <Gui/Selection/SelectionView.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <stdexcept>
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
    if (!blocking) {
        QMetaObject::invokeMethod(QApplication::instance(), std::move(fn), Qt::QueuedConnection);
        return;
    }
    // Match the production hook: propagate a blocking subscriber failure back
    // to its sender, without letting it escape through Qt's event loop.
    std::exception_ptr failure;
    QMetaObject::invokeMethod(QApplication::instance(), [&] {
        App::MainThreadSignalConfig::BlockingInvokeScope scope;
        try {
            fn();
        }
        catch (...) {
            failure = std::current_exception();
        }
    }, Qt::BlockingQueuedConnection);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void serviceDeletionMainThread(std::chrono::milliseconds)
{
    // Service only the functors sent by this fixture's invoke hook while a
    // main-thread recompute or undo synchronously waits for the owner lane.
    QCoreApplication::sendPostedEvents(QApplication::instance(), QEvent::MetaCall);
}

struct RetirementProbe
{
    std::atomic<bool> destroyed {false};
    std::atomic<bool> selectedAtDestruction {false};
    std::atomic<bool> guiDeletionDelivered {false};
    bool requireGuiDeletion = false;
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
        if (probe.requireGuiDeletion) {
            EXPECT_TRUE(probe.guiDeletionDelivered.load())
                << "GUI deletion cleanup was skipped before object destruction";
        }
    }
private:
    RetirementProbe& probe;
};

// Exercise the production owner-lane recompute and deferred deletion replay.
class PersonalSelectionReplacementFeature: public App::FeatureTest
{
public:
    std::function<void()> replace;
    bool canRecomputeOnWorker() const override { return false; }
    App::DocumentObjectExecReturn* execute() override
    {
        replace();
        return App::DocumentObject::StdReturn;
    }
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
        App::MainThreadSignalConfig::setServiceHook(&serviceDeletionMainThread);
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
        App::MainThreadSignalConfig::setServiceHook(nullptr);
    }

    void checkDeletionReplay(bool openTransaction = false, bool reuseName = false,
                             bool throwingObservers = false, bool workerRecompute = true);

    App::Document* doc = nullptr;
    RetirementProbe probe;
};
}

void SelectionRecomputeDeletionTest::checkDeletionReplay(
    bool openTransaction, bool reuseName, bool throwingObservers, bool workerRecompute)
{
    if (openTransaction) {
        doc->openTransaction("Add retiring object");
    }
    auto* retiring = new RetiringSelectionFeature(probe);
    doc->addObject(retiring, "Retiring");
    retiring->Label.setValue("Original label");
    // The victim is already recomputed; only the remover is scheduled below.
    retiring->purgeTouched();
    auto* remover = doc->addObject("App::FeaturePython", "Remover");
    {
        Base::PyGILStateLocker gil;
        PyObject* globals = PyDict_New();
        PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
        PyDict_SetItemString(globals, "reuse_name", reuseName ? Py_True : Py_False);
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
            "        if reuse_name:\n"
            "            replacement = obj.Document.addObject('App::FeatureTest', 'Retiring')\n"
            "            replacement.Label = 'Replacement label'\n"
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
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    auto* retiringProvider = guiDoc->getViewProvider(retiring);
    ASSERT_NE(retiringProvider, nullptr);
    fastsignals::scoped_connection guiDeletion = guiDoc->signalDeletedObject.connect(
        [&](const Gui::ViewProviderDocumentObject& provider) {
            if (&provider == retiringProvider) {
                probe.guiDeletionDelivered = !probe.destroyed && isDeletionMainThread();
            }
        });
    probe.requireGuiDeletion = true;
    int appThrowCalls = 0;
    int appTailCalls = 0;
    int docThrowCalls = 0;
    bool appTailAlive = false;
    fastsignals::scoped_connection appThrow;
    fastsignals::scoped_connection appTail;
    fastsignals::scoped_connection docThrow;
    if (throwingObservers) {
        appThrow = App::GetApplication().signalDeletedObject.connect(
            [&](const App::DocumentObject& object) {
                if (&object == retiring) {
                    ++appThrowCalls;
                    throw std::runtime_error("application deletion observer");
                }
            });
        appTail = App::GetApplication().signalDeletedObject.connect(
            [&](const App::DocumentObject& object) {
                if (&object == retiring) {
                    ++appTailCalls;
                    appTailAlive = !probe.destroyed && isDeletionMainThread();
                }
            });
        docThrow = doc->signalDeletedObject.connect(
            [&](const App::DocumentObject& object) {
                if (&object == retiring) {
                    ++docThrowCalls;
                    // Exercise the non-std::exception path too.
                    throw 42;
                }
            });
    }
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
    auto recompute = [&] {
        try {
            static_cast<void>(doc->recompute({}, false, &recomputeError));
        }
        catch (const std::exception& exception) {
            error = exception.what();
        }
        done = true;
    };
    std::thread worker;
    if (workerRecompute) {
        worker = std::thread(recompute);
    }
    else {
        recompute();
    }
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
    if (worker.joinable()) {
        worker.join();
    }
    QApplication::processEvents();
    {
        Base::PyGILStateLocker gil;
        PyObject* pyRemover = remover->getPyObject();
        PyObject* proxy = PyObject_GetAttrString(pyRemover, "Proxy");
        PyObject* executedOnWorker = proxy ? PyObject_GetAttrString(proxy, "worker") : nullptr;
        EXPECT_NE(executedOnWorker, nullptr);
        if (executedOnWorker) {
            // Both calling venues execute features on the document owner lane.
            EXPECT_EQ(PyObject_IsTrue(executedOnWorker), 1);
        }
        Py_XDECREF(executedOnWorker);
        Py_XDECREF(proxy);
        Py_DECREF(pyRemover);
        PyErr_Clear();
    }
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_FALSE(recomputeError);
    if (reuseName) {
        auto* replacement = doc->getObject("Retiring");
        ASSERT_NE(replacement, nullptr);
        EXPECT_NE(replacement, retiring);
        EXPECT_STREQ(replacement->Label.getValue(), "Replacement label");
        EXPECT_NE(guiDoc->getViewProvider(replacement), nullptr);
    }
    else {
        EXPECT_EQ(doc->getObject("Retiring"), nullptr);
    }
    if (throwingObservers) {
        EXPECT_EQ(appThrowCalls, 1);
        EXPECT_EQ(appTailCalls, 1);
        EXPECT_TRUE(appTailAlive);
        EXPECT_EQ(docThrowCalls, 1);
    }
    EXPECT_EQ(deleted, 1);
    EXPECT_TRUE(aliveAtReplay);
    EXPECT_TRUE(selectionClearedAtReplay);
    EXPECT_TRUE(replayOnGui);
    EXPECT_TRUE(probe.guiDeletionDelivered.load());
    EXPECT_EQ(guiDoc->getViewProvider(retiring), nullptr);
    EXPECT_TRUE(probe.destroyed.load());
    EXPECT_FALSE(probe.selectedAtDestruction.load())
        << "Selection retained a dangling pointer when the recompute transaction destroyed its object";
    EXPECT_FALSE(Gui::Selection().hasSelection());
    EXPECT_EQ(selectionList->count(), 0);
    EXPECT_EQ(doc->getAvailableUndos(), openTransaction ? 1 : 0);
    if (openTransaction) {
        EXPECT_TRUE(doc->hasPendingTransaction());
        doc->commitTransaction();
        EXPECT_EQ(doc->getAvailableUndos(), 1);
        doc->undo();
        EXPECT_EQ(doc->getObject("Retiring"), nullptr);
        EXPECT_EQ(doc->getObject("Remover"), nullptr);
        doc->redo();
        EXPECT_EQ(doc->getObject("Retiring"), nullptr);
        EXPECT_NE(doc->getObject("Remover"), nullptr);
    }
}

TEST_F(SelectionRecomputeDeletionTest, selectedObjectSurvivesUntilWorkerRecomputeDeletionReplay)
{
    checkDeletionReplay();
}

TEST_F(SelectionRecomputeDeletionTest, openTransactionCancellationSurvivesDeletionReplay)
{
    checkDeletionReplay(true);
}

TEST_F(SelectionRecomputeDeletionTest, sameNameReplacementDoesNotKeepRetiredSelectionRow)
{
    checkDeletionReplay(false, true);
}

TEST_F(SelectionRecomputeDeletionTest, throwingDeletionObserversDoNotSkipCleanup)
{
    checkDeletionReplay(false, false, true);
}

TEST_F(SelectionRecomputeDeletionTest, mainThreadRecomputeStillCleansSelectionBeforeDestruction)
{
    checkDeletionReplay(false, false, false, false);
}

TEST_F(SelectionRecomputeDeletionTest, removingOneSubelementPreservesOtherRows)
{
    auto* object = doc->addObject("App::FeatureTest", "Elements");
    auto* view = Gui::MainWindow::getInstance()->findChild<Gui::DockWnd::SelectionView*>();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Elements", "Face1"));
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Elements", "Face10"));
    ASSERT_EQ(view->selectionView->count(), 2);
    Gui::Selection().rmvSelection(doc->getName(), "Elements", "Face1");
    ASSERT_EQ(view->selectionView->count(), 1);
    EXPECT_TRUE(view->selectionView->item(0)->text().contains("Face10"));
    Gui::Selection().rmvSelection(doc->getName(), "Elements");
    EXPECT_EQ(view->selectionView->count(), 0);
    // Rows populated through SetSelection use the same stored identity.
    Gui::Selection().setSelection(doc->getName(), {object});
    ASSERT_EQ(view->selectionView->count(), 1);
    Gui::Selection().rmvSelection(doc->getName(), "Elements");
    EXPECT_EQ(view->selectionView->count(), 0);
}

TEST_F(SelectionRecomputeDeletionTest, personalSelectionSurvivesUnrelatedDeletion)
{
    doc->addObject("App::FeatureTest", "Target");
    doc->addObject("App::FeatureTest", "Other");
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    Gui::PersonalViewContext context;
    context.selectionPaths = {"Target", "Target.Face1"};
    guiDoc->storePersonalViewContext("actor", context);
    doc->removeObject("Other");
    EXPECT_EQ(guiDoc->personalViewContext("actor"), context);
}

TEST_F(SelectionRecomputeDeletionTest, personalSelectionRetiredByDeletionAndHistory)
{
    doc->addObject("App::FeatureTest", "Target");
    doc->addObject("App::FeatureTest", "TargetSibling");
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    Gui::PersonalViewContext context;
    context.selectionPaths = {"Target", "Target.Face1", "TargetSibling"};
    context.preselectionPath = "Target.Edge1";
    guiDoc->storePersonalViewContext("actor-a", context);
    guiDoc->storePersonalViewContext("actor-b", context);
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Target"));
    const auto expectRetired = [&] {
        EXPECT_FALSE(Gui::Selection().hasSelection());
        for (const auto* actor : {"actor-a", "actor-b"}) {
            const auto stored = guiDoc->personalViewContext(actor);
            ASSERT_TRUE(stored);
            EXPECT_EQ(stored->selectionPaths, std::vector<std::string> {"TargetSibling"});
            EXPECT_FALSE(stored->preselectionPath);
        }
    };
    doc->openTransaction("delete selected");
    doc->removeObject("Target");
    doc->commitTransaction();
    expectRetired();
    ASSERT_TRUE(doc->undo());
    expectRetired();
    ASSERT_TRUE(doc->redo());
    expectRetired();
    doc->addObject("App::FeatureTest", "Target");
    expectRetired();
}

TEST_F(SelectionRecomputeDeletionTest, personalQualifiedSelectionRetiredByDeletion)
{
    doc->addObject("App::FeatureTest", "Target");
    doc->addObject("App::FeatureTest", "Other");
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    const std::string qualified = std::string(doc->getName()) + "#Target";
    Gui::PersonalViewContext context;
    context.camera = "retain camera";
    context.selectionPaths = {qualified, qualified + ".Face1", "Other", "Foreign#Target.Face1"};
    context.preselectionPath = qualified + ".Edge1";
    guiDoc->storePersonalViewContext("actor-a", context);
    Gui::PersonalViewContext other;
    other.selectionPaths = {"Other"};
    other.preselectionPath = "Other.Face1";
    guiDoc->storePersonalViewContext("actor-b", other);
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Other"));
    doc->removeObject("Target");
    context.selectionPaths = {"Other", "Foreign#Target.Face1"};
    context.preselectionPath.reset();
    EXPECT_EQ(guiDoc->personalViewContext("actor-a"), context);
    EXPECT_EQ(guiDoc->personalViewContext("actor-b"), other);
    EXPECT_TRUE(Gui::Selection().isSelected(doc->getName(), "Other"));
}

TEST_F(SelectionRecomputeDeletionTest, personalReplacementSelectionSurvivesDeferredDeletion)
{
    auto* original = doc->addObject("App::FeatureTest", "Target");
    original->purgeTouched();
    doc->addObject("App::FeatureTest", "Other")->purgeTouched();
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    Gui::PersonalViewContext stale;
    stale.selectionPaths = {"Target", "Target.Face1", "Other"};
    stale.preselectionPath = "Target.Edge1";
    guiDoc->storePersonalViewContext("actor-old", stale);
    Gui::PersonalViewContext replacement;
    replacement.selectionPaths = {std::string(doc->getName()) + "#Target.Face1", "Target"};
    replacement.preselectionPath = "Target.Edge1";
    Gui::PersonalViewContext other;
    other.selectionPaths = {"Other"};
    guiDoc->storePersonalViewContext("actor-other", other);
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Other"));
    std::atomic<bool> replaced {false};
    bool replayed = false;
    fastsignals::scoped_connection deletion = doc->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == original) {
                EXPECT_TRUE(isDeletionMainThread());
                EXPECT_TRUE(replaced.load());
                EXPECT_NE(doc->getObject("Target"), original);
                replayed = true;
            }
        });
    auto* remover = new PersonalSelectionReplacementFeature;
    remover->replace = [&] {
        EXPECT_FALSE(isDeletionMainThread());
        doc->removeObject("Target");
        auto* target = doc->addObject("App::FeatureTest", "Target");
        EXPECT_NE(target, original);
        target->purgeTouched();
        // Store before the deleted original's GUI notification is replayed.
        guiDoc->storePersonalViewContext("actor-new", replacement);
        replaced = true;
    };
    doc->addObject(remover, "Remover");
    remover->touch();
    bool error = false;
    static_cast<void>(doc->recompute({remover}, true, &error));
    ASSERT_FALSE(error);
    ASSERT_TRUE(replayed);
    EXPECT_EQ(guiDoc->personalViewContext("actor-new"), replacement);
    stale.selectionPaths = {"Other"};
    stale.preselectionPath.reset();
    EXPECT_EQ(guiDoc->personalViewContext("actor-old"), stale);
    EXPECT_EQ(guiDoc->personalViewContext("actor-other"), other);
    EXPECT_TRUE(Gui::Selection().isSelected(doc->getName(), "Other"));
}

TEST_F(SelectionRecomputeDeletionTest, personalNestedSubshapeRetiredByResolvedObjectDeletion)
{
    {
        Base::PyGILStateLocker gil;
        Base::Interpreter().runString("import Part");
    }
    auto* target = doc->addObject("Part::Box", "Target");
    auto* other = doc->addObject("Part::Box", "Other");
    auto* group = static_cast<App::DocumentObjectGroup*>(
        doc->addObject("App::DocumentObjectGroup", "Group"));
    group->addObject(target);
    group->addObject(other);
    static_cast<void>(doc->recompute());
    ASSERT_EQ(group->getSubObject("Target.Face1"), target);
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    Gui::PersonalViewContext context;
    context.selectionPaths = {"Group.Target.Face1", std::string(doc->getName()) + "#Group.Target.Edge1",
                              "Group.Other.Face1", "Group"};
    context.preselectionPath = "Group.Target.Face2";
    guiDoc->storePersonalViewContext("actor", context);
    Gui::PersonalViewContext otherActor;
    otherActor.selectionPaths = {"Group.Other.Face1"};
    guiDoc->storePersonalViewContext("other-actor", otherActor);
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Group", "Target.Face1"));
    ASSERT_TRUE(Gui::Selection().addSelection(doc->getName(), "Group", "Other.Face1"));
    doc->removeObject("Target");
    context.selectionPaths = {"Group.Other.Face1", "Group"};
    context.preselectionPath.reset();
    EXPECT_EQ(guiDoc->personalViewContext("actor"), context);
    EXPECT_EQ(guiDoc->personalViewContext("other-actor"), otherActor);
    EXPECT_FALSE(Gui::Selection().isSelected(doc->getName(), "Group", "Target.Face1"));
    EXPECT_TRUE(Gui::Selection().isSelected(doc->getName(), "Group", "Other.Face1"));
    // Root identity must also be tracked for paths resolved to a surviving child.
    doc->removeObject("Group");
    context.selectionPaths.clear();
    otherActor.selectionPaths.clear();
    EXPECT_EQ(guiDoc->personalViewContext("actor"), context);
    EXPECT_EQ(guiDoc->personalViewContext("other-actor"), otherActor);
}

TEST_F(SelectionRecomputeDeletionTest, personalSelectionsDiscardedOnDocumentCloseAndReopen)
{
    doc->addObject("App::FeatureTest", "Target");
    auto* guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    Gui::PersonalViewContext context;
    context.selectionPaths = {"Target", std::string(doc->getName()) + "#Target.Face1"};
    context.preselectionPath = "Target.Edge1";
    guiDoc->storePersonalViewContext("actor", context);
    const std::string name = doc->getName();
    App::GetApplication().closeDocument(name.c_str());
    App::DocumentInitFlags flags;
    flags.createView = false;
    doc = App::GetApplication().newDocument(name.c_str(), "testUser", flags);
    doc->addObject("App::FeatureTest", "Target");
    guiDoc = Gui::Application::Instance->getDocument(doc);
    ASSERT_NE(guiDoc, nullptr);
    EXPECT_FALSE(guiDoc->personalViewContext("actor"));
    guiDoc->storePersonalViewContext("actor", context);
    EXPECT_EQ(guiDoc->personalViewContext("actor"), context);
    doc->removeObject("Target");
    context.selectionPaths.clear();
    context.preselectionPath.reset();
    EXPECT_EQ(guiDoc->personalViewContext("actor"), context);
}
