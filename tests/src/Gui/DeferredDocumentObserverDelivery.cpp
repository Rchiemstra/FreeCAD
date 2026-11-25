// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentWouldBlock.h>
#include <App/MainThreadSignal.h>
#include <App/private/MainThreadSignalTestHooks.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentObserverPython.h>
#include <Gui/GuiPythonGate.h>
#include <src/App/InitApplication.h>

#include <CXX/Objects.hxx>
#include <Python.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace Gui
{
class DocumentObserverPythonTestAccess
{
public:
    static void setBeforeIdentityCopy(void (*hook)())
    {
        DocumentObserverPython::setBeforeDeferredIdentityCopyForTest(hook);
    }
};
}  // namespace Gui

namespace
{

std::thread::id g_observerTestMainThread;

struct ObserverDispatchTask
{
    std::function<void()> callback;
    std::shared_ptr<std::promise<void>> completion;
};

struct ObserverDispatcher
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ObserverDispatchTask> tasks;
    bool accepting {true};
    std::size_t senders {0};
};

// Published before any sender starts and retained through document destruction.
// Atomic shared ownership also keeps a state alive during a rejected admission.
std::shared_ptr<ObserverDispatcher> g_observerDispatcher;

// The lane's std::thread detaches during GUI close. A notice registered on
// that thread becomes ready only at thread exit, after its TLS destructors.
struct OwnerThreadExitNotice
{
    explicit OwnerThreadExitNotice(std::shared_ptr<std::promise<void>> completion)
        : completion(std::move(completion))
    {}
    ~OwnerThreadExitNotice()
    {
        try {
            completion->set_value_at_thread_exit();
        }
        catch (...) {
            std::fputs("observer test owner exit notification failed\n", stderr);
            std::abort();
        }
    }
    std::shared_ptr<std::promise<void>> completion;
};

thread_local std::unique_ptr<OwnerThreadExitNotice> g_ownerThreadExitNotice;

bool isObserverTestMainThread()
{
    return std::this_thread::get_id() == g_observerTestMainThread;
}

void invokeObserverTestMainThread(std::function<void()>&& fn, bool blocking)
{
    if (isObserverTestMainThread()) {
        fn();
        return;
    }
    const auto dispatcher = std::atomic_load(&g_observerDispatcher);
    if (!dispatcher) {
        throw std::runtime_error("observer test main-thread dispatch unavailable");
    }
    struct Sender
    {
        std::shared_ptr<ObserverDispatcher> state;
        bool admitted {false};
        ~Sender()
        {
            if (admitted) {
                {
                    std::lock_guard lock(state->mutex);
                    --state->senders;
                }
                state->changed.notify_all();
            }
        }
    } sender {dispatcher};
    auto completion = blocking ? std::make_shared<std::promise<void>>() : nullptr;
    std::future<void> result;
    if (completion) {
        result = completion->get_future();
    }
    {
        std::lock_guard lock(dispatcher->mutex);
        if (!dispatcher->accepting) {
            throw std::runtime_error("observer test main-thread dispatch rejected");
        }
        ++dispatcher->senders;
        sender.admitted = true;
        dispatcher->tasks.push_back({std::move(fn), completion});
    }
    dispatcher->changed.notify_one();
    if (completion) {
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        result.get();
    }
}

void serviceObserverTestMainThread(std::chrono::milliseconds maxWait)
{
    if (!isObserverTestMainThread()) {
        throw std::runtime_error("observer test service called off the GUI thread");
    }
    const auto dispatcher = std::atomic_load(&g_observerDispatcher);
    if (!dispatcher) {
        throw std::runtime_error("observer test service unavailable");
    }
    {
        std::unique_lock lock(dispatcher->mutex);
        if (dispatcher->tasks.empty()) {
            dispatcher->changed.wait_for(lock, maxWait, [&] { return !dispatcher->tasks.empty(); });
        }
    }
    while (true) {
        ObserverDispatchTask task;
        {
            std::lock_guard lock(dispatcher->mutex);
            if (dispatcher->tasks.empty()) {
                break;
            }
            task = std::move(dispatcher->tasks.front());
            dispatcher->tasks.pop_front();
        }
        if (task.completion) {
            App::MainThreadSignalConfig::BlockingInvokeScope scope;
            std::exception_ptr failure;
            try {
                task.callback();
            }
            catch (...) {
                failure = std::current_exception();
            }
            // Release all captured arguments on the GUI thread before waking
            // the blocked sender, including on an exception.
            task.callback = {};
            if (failure) {
                task.completion->set_exception(failure);
            }
            else {
                task.completion->set_value();
            }
        }
        else {
            try {
                task.callback();
            }
            catch (...) {
                ADD_FAILURE() << "asynchronous observer test dispatch threw";
            }
        }
    }
}

void initializeGuiForObserverTests()
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
    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
}

std::size_t pumpDeferredObserverEvents(const std::size_t expectedMinimum = 0)
{
    // Try the production no-GIL path, then drain deterministically under the
    // GIL. No callback delivery sleep or successful probe timing is required.
    static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(0ms));
    std::size_t pumped = Gui::GuiPythonGate::pumpQueuedCallbacks(64);
    Base::PyGILStateLocker gil;
    while (const std::size_t batch = Gui::GuiPythonGate::pumpQueuedCallbacks(64)) {
        pumped += batch;
    }
    QApplication::processEvents();
    while (const std::size_t batch = Gui::GuiPythonGate::pumpQueuedCallbacks(64)) {
        pumped += batch;
    }
    EXPECT_GE(pumped, expectedMinimum);
    return pumped;
}

class PausedObserverEmission
{
public:
    explicit PausedObserverEmission(const Gui::Document& document)
        : _thread([this, &document] {
            _current = this;
            Gui::DocumentObserverPythonTestAccess::setBeforeIdentityCopy(&pause);
            try {
                Gui::Application::Instance->signalRelabelDocument(document);
            }
            catch (...) {
                std::lock_guard lock(_mutex);
                _failure = std::current_exception();
            }
            Gui::DocumentObserverPythonTestAccess::setBeforeIdentityCopy(nullptr);
            _current = nullptr;
            {
                std::lock_guard lock(_mutex);
                _finished = true;
            }
            _changed.notify_all();
        })
    {}

    ~PausedObserverEmission()
    {
        resumeAndJoin();
    }

    bool waitUntilPaused()
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, 5s, [this] { return _paused || _finished; }) && _paused;
    }

    void checkWorker() const
    {
        if (_failure) {
            try {
                std::rethrow_exception(_failure);
            }
            catch (const std::exception& e) {
                ADD_FAILURE() << "Observer emission failed: " << e.what();
            }
            catch (...) {
                ADD_FAILURE() << "Observer emission failed with an unknown exception";
            }
        }
    }

    void resumeAndJoin()
    {
        {
            std::lock_guard lock(_mutex);
            _resume = true;
        }
        _changed.notify_all();
        if (_thread.joinable()) {
            std::unique_lock lock(_mutex);
            if (!_changed.wait_for(lock, 5s, [this] { return _finished; })) {
                std::fputs("observer emission did not finish during cleanup\n", stderr);
                std::abort();
            }
            lock.unlock();
            _thread.join();
        }
    }

private:
    static void pause()
    {
        auto& state = *_current;
        std::unique_lock lock(state._mutex);
        // Only the first copied invocation pauses. Other subscribers continue
        // normally after the registration has been retired.
        if (state._paused) {
            return;
        }
        state._paused = true;
        state._changed.notify_all();
        state._changed.wait(lock, [&state] { return state._resume; });
    }

    static thread_local PausedObserverEmission* _current;
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _paused {false};
    bool _resume {false};
    bool _finished {false};
    std::exception_ptr _failure;
    std::thread _thread;
};

thread_local PausedObserverEmission* PausedObserverEmission::_current = nullptr;

class ScopedPythonGuiObserver
{
public:
    explicit ScopedPythonGuiObserver(PyObject* observerClass, bool registerImmediately = true)
    {
        Base::PyGILStateLocker gil;
        _observer = PyObject_CallObject(observerClass, nullptr);
        EXPECT_NE(_observer, nullptr);
        if (_observer && registerImmediately) {
            Gui::DocumentObserverPython::addObserver(Py::Object(_observer, false));
            _registeredCount = 1;
        }
    }

    ~ScopedPythonGuiObserver()
    {
        unregisterAll();
        if (_observer) {
            Base::PyGILStateLocker gil;
            Py_DECREF(_observer);
            _observer = nullptr;
        }
    }

    ScopedPythonGuiObserver(const ScopedPythonGuiObserver&) = delete;
    ScopedPythonGuiObserver& operator=(const ScopedPythonGuiObserver&) = delete;

    void registerAgain()
    {
        ASSERT_NE(_observer, nullptr);
        Base::PyGILStateLocker gil;
        Gui::DocumentObserverPython::addObserver(Py::Object(_observer, false));
        ++_registeredCount;
    }

    void unregisterOne()
    {
        if (!_observer || _registeredCount == 0) {
            return;
        }
        Base::PyGILStateLocker gil;
        Gui::DocumentObserverPython::removeObserver(Py::Object(_observer, false));
        --_registeredCount;
    }

    void unregisterAll()
    {
        while (_registeredCount > 0) {
            unregisterOne();
        }
    }

    std::vector<std::string> stringListAttr(const char* attrName) const
    {
        std::vector<std::string> values;
        if (!_observer) {
            return values;
        }
        Base::PyGILStateLocker gil;
        PyObject* list = PyObject_GetAttrString(_observer, attrName);
        if (!list) {
            PyErr_Clear();
            return values;
        }
        if (PyList_Check(list)) {
            const Py_ssize_t count = PyList_Size(list);
            values.reserve(static_cast<std::size_t>(count));
            for (Py_ssize_t i = 0; i < count; ++i) {
                PyObject* item = PyList_GetItem(list, i);
                if (item && PyUnicode_Check(item)) {
                    const char* utf8 = PyUnicode_AsUTF8(item);
                    values.emplace_back(utf8 ? utf8 : "");
                }
            }
        }
        Py_DECREF(list);
        return values;
    }

private:
    PyObject* _observer {nullptr};
    std::size_t _registeredCount {0};
};

class DeferredDocumentObserverDeliveryTest: public ::testing::Test
{
protected:
    struct OwnedHooks
    {
        std::shared_ptr<ObserverDispatcher> dispatcher;
        App::Test::MainThreadSignalHooks previous {};
        bool installed {false};

        void install()
        {
            dispatcher = std::make_shared<ObserverDispatcher>();
            std::atomic_store(&g_observerDispatcher, dispatcher);
            previous = App::Test::exchangeMainThreadSignalHooks(
                {&isObserverTestMainThread, &invokeObserverTestMainThread,
                 &serviceObserverTestMainThread});
            installed = true;
        }

        ~OwnedHooks()
        {
            if (installed) {
                {
                    std::lock_guard lock(dispatcher->mutex);
                    if (!dispatcher->tasks.empty() || dispatcher->senders != 0) {
                        std::fputs("observer test dispatch still active during cleanup\n", stderr);
                        std::abort();
                    }
                    dispatcher->accepting = false;
                }
                static_cast<void>(App::Test::exchangeMainThreadSignalHooks(previous));
                std::atomic_store(&g_observerDispatcher, std::shared_ptr<ObserverDispatcher> {});
            }
        }
    };
    static void SetUpTestSuite()
    {
        initializeGuiForObserverTests();
    }

    void SetUp() override
    {
        g_observerTestMainThread = std::this_thread::get_id();
        _hooks.install();
        ASSERT_TRUE(App::DocumentWouldBlock::isGuiThread());
        ASSERT_FALSE(PyGILState_Check());

        Gui::DocumentObserverPython::installValueEventHandler();
        static_cast<void>(pumpDeferredObserverEvents());

        {
            Base::PyGILStateLocker gil;
            _globals = PyDict_New();
            ASSERT_NE(_globals, nullptr);
            ASSERT_EQ(PyDict_SetItemString(_globals, "__builtins__", PyEval_GetBuiltins()), 0);
            PyObject* executed = PyRun_String(
                "class FullObserver:\n"
                "    def __init__(self):\n"
                "        self.created = []\n"
                "        self.relabeled = []\n"
                "    def slotCreatedObject(self, vp):\n"
                "        obj = getattr(vp, 'Object', None)\n"
                "        self.created.append(obj.Name if obj is not None else '')\n"
                "    def slotRelabelDocument(self, gdoc):\n"
                "        doc = getattr(gdoc, 'Document', None)\n"
                "        self.relabeled.append(doc.Name if doc is not None else '')\n"
                "\n"
                "class DeleteOnlyObserver:\n"
                "    def __init__(self):\n"
                "        self.deleted = []\n"
                "    def slotDeletedObject(self, vp):\n"
                "        obj = getattr(vp, 'Object', None)\n"
                "        self.deleted.append(obj.Name if obj is not None else '')\n",
                Py_file_input,
                _globals,
                _globals);
            ASSERT_NE(executed, nullptr);
            Py_DECREF(executed);
            _fullObserverClass = PyDict_GetItemString(_globals, "FullObserver");
            _deleteOnlyObserverClass = PyDict_GetItemString(_globals, "DeleteOnlyObserver");
            ASSERT_NE(_fullObserverClass, nullptr);
            ASSERT_NE(_deleteOnlyObserverClass, nullptr);
        }

        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName = App::GetApplication().getUniqueDocumentName("DeferredGuiObserver");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "deferred gui observer", flags);
        ASSERT_NE(_document, nullptr);
        ASSERT_NE(_document->executionLane(), nullptr);
        _ownerLane = _document->executionLane()->shared_from_this();
        auto completion = std::make_shared<std::promise<void>>();
        _ownerExited = completion->get_future();
        _ownerLane->dispatchToOwner([completion] {
            if (g_ownerThreadExitNotice) {
                throw std::runtime_error("owner exit notice already installed");
            }
            g_ownerThreadExitNotice = std::make_unique<OwnerThreadExitNotice>(completion);
        });
        _exitNoticeArmed = true;
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);
    }

    void TearDown() override
    {
        try {
            static_cast<void>(pumpDeferredObserverEvents());
            auto* liveDocument = _documentName.empty()
                ? nullptr : App::GetApplication().getDocument(_documentName.c_str());
            if (liveDocument) {
                if (liveDocument != _document || !_ownerLane || !_exitNoticeArmed) {
                    std::fputs("observer test owner exit notice was not established\n", stderr);
                    std::abort();
                }
                if (!App::GetApplication().closeDocument(_documentName.c_str())
                    || App::GetApplication().getDocument(_documentName.c_str())) {
                    std::fputs("observer test document did not close during cleanup\n", stderr);
                    std::abort();
                }
                QApplication::processEvents();
            }
            _document = nullptr;
            _guiDocument = nullptr;
            if (_ownerLane) {
                waitForOwnerExit();
            }
            if (_globals) {
                Base::PyGILStateLocker gil;
                Py_DECREF(_globals);
                _globals = nullptr;
                _fullObserverClass = nullptr;
                _deleteOnlyObserverClass = nullptr;
            }
            // The owner and its TLS have exited and the retained lane has
            // been destroyed. Explicit test senders joined before TearDown.
            static_cast<void>(pumpDeferredObserverEvents());
        }
        catch (...) {
            std::fputs("observer test cleanup failed with live dependencies\n", stderr);
            std::abort();
        }
    }

    void waitForOwnerExit()
    {
        if (!_exitNoticeArmed || !_ownerExited.valid()) {
            std::fputs("observer test cannot verify owner exit\n", stderr);
            std::abort();
        }
        struct ExitWatchdog
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool done {false};
            std::thread thread {[this] {
                std::unique_lock lock(mutex);
                if (!changed.wait_for(lock, 5s, [this] { return done; })) {
                    std::fputs("observer test owner did not exit during cleanup\n", stderr);
                    std::abort();
                }
            }};
            ~ExitWatchdog()
            {
                {
                    std::lock_guard lock(mutex);
                    done = true;
                }
                changed.notify_all();
                thread.join();
            }
        } watchdog;
        std::optional<Base::PyGILStateRelease> release;
        if (Py_IsInitialized() && PyGILState_Check()) {
            release.emplace();
        }
        while (_ownerExited.wait_for(0ms) != std::future_status::ready) {
            static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
        }
        _ownerExited.get();
        // Retain the lane until thread exit and all other holders have gone.
        // Resetting our last strong reference runs its complete destructor
        // synchronously before the hook triple can be restored.
        while (_ownerLane.use_count() != 1) {
            static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
        }
        _ownerLane.reset();
    }

    // TearDown runs even after a fatal assertion in SetUp or the test body.
    OwnedHooks _hooks;
    std::shared_ptr<App::DocumentExecutionLane> _ownerLane;
    std::future<void> _ownerExited;
    bool _exitNoticeArmed {false};
    PyObject* _globals {nullptr};
    PyObject* _fullObserverClass {nullptr};
    PyObject* _deleteOnlyObserverClass {nullptr};
    std::string _documentName;
    App::Document* _document {nullptr};
    Gui::Document* _guiDocument {nullptr};
};

}  // namespace

TEST_F(DeferredDocumentObserverDeliveryTest,
       DeferredNativeCreationDeliversOncePerIntendedSubscriber)
{
    ScopedPythonGuiObserver first(_fullObserverClass);
    ScopedPythonGuiObserver second(_fullObserverClass);
    ScopedPythonGuiObserver deleteOnly(_deleteOnlyObserverClass);
    ScopedPythonGuiObserver late(_fullObserverClass, /*registerImmediately=*/false);

    {
        // Force native ObserverDelivery deferral deterministically on the GUI
        // thread without holding the GIL, matching active lane-dispatch pumping.
        App::DocumentExecutionLaneGuiDispatchPumpScope pumpScope;
        ASSERT_FALSE(PyGILState_Check());
        ASSERT_FALSE(Gui::GuiPythonGate::gilAvailableForNonBlockingAcquire());

        auto* created = _document->addObject("App::FeatureTest", "DeferredBox");
        ASSERT_NE(created, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(created), nullptr);

        // Delivery must remain deferred while the pump guard is active.
        EXPECT_TRUE(first.stringListAttr("created").empty());
        EXPECT_TRUE(second.stringListAttr("created").empty());

        // A subscriber registered after enqueueing and before pumping must not
        // receive the earlier event.
        late.registerAgain();
    }

    EXPECT_GE(pumpDeferredObserverEvents(/*expectedMinimum=*/2), 2U);

    EXPECT_EQ(first.stringListAttr("created"), (std::vector<std::string> {"DeferredBox"}));
    EXPECT_EQ(second.stringListAttr("created"), (std::vector<std::string> {"DeferredBox"}));
    EXPECT_TRUE(late.stringListAttr("created").empty());
    EXPECT_TRUE(deleteOnly.stringListAttr("deleted").empty());

    // Subsequent deferred events must reach all three creation subscribers once each.
    {
        App::DocumentExecutionLaneGuiDispatchPumpScope pumpScope;
        ASSERT_FALSE(PyGILState_Check());
        auto* secondObject = _document->addObject("App::FeatureTest", "SecondDeferredBox");
        ASSERT_NE(secondObject, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(secondObject), nullptr);
    }

    EXPECT_GE(pumpDeferredObserverEvents(/*expectedMinimum=*/3), 3U);

    const std::vector<std::string> expectedExisting {"DeferredBox", "SecondDeferredBox"};
    EXPECT_EQ(first.stringListAttr("created"), expectedExisting);
    EXPECT_EQ(second.stringListAttr("created"), expectedExisting);
    EXPECT_EQ(late.stringListAttr("created"), (std::vector<std::string> {"SecondDeferredBox"}));
}

TEST_F(DeferredDocumentObserverDeliveryTest,
       RemovedAndReregisteredObserverDoesNotInheritPriorIdentity)
{
    ScopedPythonGuiObserver retained(_fullObserverClass);
    ScopedPythonGuiObserver churned(_fullObserverClass);
    ScopedPythonGuiObserver removedOnly(_fullObserverClass);

    {
        App::DocumentExecutionLaneGuiDispatchPumpScope pumpScope;
        ASSERT_FALSE(PyGILState_Check());
        ASSERT_FALSE(Gui::GuiPythonGate::gilAvailableForNonBlockingAcquire());

        auto* beforeChurn = _document->addObject("App::FeatureTest", "BeforeChurn");
        ASSERT_NE(beforeChurn, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(beforeChurn), nullptr);
        _document->Label.setValue("BeforeChurnLabel");

        EXPECT_TRUE(retained.stringListAttr("created").empty());
        EXPECT_TRUE(retained.stringListAttr("relabeled").empty());
        EXPECT_TRUE(churned.stringListAttr("created").empty());
        EXPECT_TRUE(churned.stringListAttr("relabeled").empty());

        // Remove and immediately re-register the same Python object before the
        // deferred queue drains. Neither Python identity nor allocator address
        // reuse may inherit the removed registration's queued events.
        removedOnly.unregisterOne();
        churned.unregisterOne();
        churned.registerAgain();
    }

    EXPECT_GE(pumpDeferredObserverEvents(/*expectedMinimum=*/6), 6U);

    EXPECT_EQ(retained.stringListAttr("created"), (std::vector<std::string> {"BeforeChurn"}));
    EXPECT_EQ(retained.stringListAttr("relabeled"), (std::vector<std::string> {_documentName}));
    EXPECT_TRUE(churned.stringListAttr("created").empty());
    EXPECT_TRUE(churned.stringListAttr("relabeled").empty());
    EXPECT_TRUE(removedOnly.stringListAttr("created").empty());
    EXPECT_TRUE(removedOnly.stringListAttr("relabeled").empty());

    // The re-registered observer has a fresh identity and receives later events once.
    {
        App::DocumentExecutionLaneGuiDispatchPumpScope pumpScope;
        ASSERT_FALSE(PyGILState_Check());
        auto* afterChurn = _document->addObject("App::FeatureTest", "AfterChurn");
        ASSERT_NE(afterChurn, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(afterChurn), nullptr);
        _document->Label.setValue("AfterChurnLabel");
    }

    EXPECT_GE(pumpDeferredObserverEvents(/*expectedMinimum=*/4), 4U);

    EXPECT_EQ(
        retained.stringListAttr("created"),
        (std::vector<std::string> {"BeforeChurn", "AfterChurn"}));
    EXPECT_EQ(
        retained.stringListAttr("relabeled"),
        (std::vector<std::string> {_documentName, _documentName}));
    EXPECT_EQ(churned.stringListAttr("created"), (std::vector<std::string> {"AfterChurn"}));
    EXPECT_EQ(churned.stringListAttr("relabeled"), (std::vector<std::string> {_documentName}));
    EXPECT_TRUE(removedOnly.stringListAttr("created").empty());
    EXPECT_TRUE(removedOnly.stringListAttr("relabeled").empty());
}

TEST_F(DeferredDocumentObserverDeliveryTest,
       UntargetedEventsDoNotBroadcastAndDuplicateRegistrationsKeepDistinctTokens)
{
    ScopedPythonGuiObserver observer(_fullObserverClass);
    observer.registerAgain();

    {
        App::DocumentExecutionLaneGuiDispatchPumpScope pumpScope;
        ASSERT_FALSE(PyGILState_Check());
        ASSERT_FALSE(Gui::GuiPythonGate::gilAvailableForNonBlockingAcquire());

        auto* created = _document->addObject("App::FeatureTest", "DupTarget");
        ASSERT_NE(created, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(created), nullptr);

        // Remove one of the two registrations for the same Python object; only
        // the surviving registration's queued event should be delivered.
        observer.unregisterOne();

        // Untargeted or stale tokens must never broadcast to live observers.
        Gui::GuiPythonObserverValueEvent untargeted;
        untargeted.eventKind = "RelabelDocument";
        untargeted.payloadJson = std::string(R"({"document":")") + _documentName + R"("})";
        Gui::GuiPythonGate::enqueueObserverValueEvent(std::move(untargeted));

        Gui::GuiPythonObserverValueEvent unknownToken;
        unknownToken.observerToken = "stale-nonexistent-token";
        unknownToken.eventKind = "RelabelDocument";
        unknownToken.payloadJson = std::string(R"({"document":")") + _documentName + R"("})";
        Gui::GuiPythonGate::enqueueObserverValueEvent(std::move(unknownToken));
    }

    EXPECT_GE(pumpDeferredObserverEvents(/*expectedMinimum=*/4), 4U);

    EXPECT_EQ(observer.stringListAttr("created"), (std::vector<std::string> {"DupTarget"}));
    EXPECT_TRUE(observer.stringListAttr("relabeled").empty());
}

TEST_F(DeferredDocumentObserverDeliveryTest, MissingAndUnknownRecipientsAreDropped)
{
    ScopedPythonGuiObserver observer(_fullObserverClass);
    Gui::GuiPythonObserverValueEvent missing;
    missing.eventKind = "RelabelDocument";
    missing.payloadJson = std::string(R"({"document":")") + _documentName + R"("})";
    Gui::GuiPythonGate::enqueueObserverValueEvent(missing);
    missing.observerToken = "not-a-registration";
    Gui::GuiPythonGate::enqueueObserverValueEvent(std::move(missing));
    EXPECT_GE(pumpDeferredObserverEvents(2), 2U);
    EXPECT_TRUE(observer.stringListAttr("relabeled").empty());
}

TEST_F(DeferredDocumentObserverDeliveryTest, CopiedInvocationOwnsIdentityAfterRemoval)
{
    ScopedPythonGuiObserver retired(_fullObserverClass);
    ScopedPythonGuiObserver retained(_fullObserverClass);
    ScopedPythonGuiObserver replacement(_fullObserverClass, false);

    // The real FastSignals invocation is copied and admitted on this worker.
    // It pauses before copying its token to the event, without holding the GIL
    // or the signal registry mutex. The GUI document stays alive until join.
    PausedObserverEmission emission(*_guiDocument);
    ASSERT_TRUE(emission.waitUntilPaused());
    retired.unregisterOne();
    emission.resumeAndJoin();
    emission.checkWorker();
    replacement.registerAgain();

    EXPECT_TRUE(retired.stringListAttr("relabeled").empty());
    EXPECT_TRUE(retained.stringListAttr("relabeled").empty());
    EXPECT_GE(pumpDeferredObserverEvents(2), 2U);
    EXPECT_TRUE(retired.stringListAttr("relabeled").empty());
    EXPECT_EQ(retained.stringListAttr("relabeled"), (std::vector<std::string> {_documentName}));
    EXPECT_TRUE(replacement.stringListAttr("relabeled").empty());
}

TEST_F(DeferredDocumentObserverDeliveryTest, FallbackDispatchRunsAndCompletesOnMainThread)
{
    // The owned service is the same queue used by GUI document-lane waits.
    std::promise<void> completed;
    auto result = completed.get_future();
    std::thread::id executedOn;
    bool blockingScope = false;
    bool propagated = false;
    std::promise<void> allowEnqueue;
    auto enqueueAllowed = allowEnqueue.get_future();
    std::thread worker([&] {
        try {
            enqueueAllowed.get();
            invokeObserverTestMainThread([&] {
                executedOn = std::this_thread::get_id();
                blockingScope = App::MainThreadSignalConfig::insideBlockingInvoke();
            }, true);
            try {
                invokeObserverTestMainThread([] { throw std::runtime_error("test exception"); },
                                             true);
            }
            catch (const std::runtime_error&) {
                propagated = true;
            }
            completed.set_value();
        }
        catch (...) {
            completed.set_exception(std::current_exception());
        }
    });
    // A pump before releasing the sender cannot be treated as quiescence.
    static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(0ms));
    allowEnqueue.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (result.wait_for(0ms) != std::future_status::ready
           && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
    }
    // Preserve the watchdog diagnostic and keep servicing a late enqueue.
    // If the sender still cannot finish, contain the failure before stack
    // references or hook state can be destroyed underneath it.
    EXPECT_EQ(result.wait_for(0ms), std::future_status::ready);
    const auto cleanupDeadline = std::chrono::steady_clock::now() + 5s;
    while (result.wait_for(0ms) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= cleanupDeadline) {
            std::fputs("observer test sender did not finish during cleanup\n", stderr);
            std::abort();
        }
        static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
    }
    worker.join();
    EXPECT_NO_THROW(result.get());
    EXPECT_EQ(executedOn, std::this_thread::get_id());
    EXPECT_TRUE(blockingScope);
    EXPECT_TRUE(propagated);

    // Exercise the real synchronous GUI lane wait: its restricted pump must
    // deliver the owner's blocking main-thread callback without Qt events.
    {
        struct LaneWaitWatchdog
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool done {false};
            std::thread thread {[this] {
                std::unique_lock lock(mutex);
                if (!changed.wait_for(lock, 5s, [this] { return done; })) {
                    std::fputs("observer test lane dispatch did not complete\n", stderr);
                    std::abort();
                }
            }};
            ~LaneWaitWatchdog()
            {
                {
                    std::lock_guard lock(mutex);
                    done = true;
                }
                changed.notify_all();
                thread.join();
            }
        } watchdog;
        std::thread::id laneCallbackThread;
        bool laneBlockingScope = false;
        _document->executionLane()->dispatchToOwner([&] {
            App::MainThreadSignalConfig::invoke([&] {
                laneCallbackThread = std::this_thread::get_id();
                laneBlockingScope = App::MainThreadSignalConfig::insideBlockingInvoke();
            }, true);
        });
        EXPECT_EQ(laneCallbackThread, std::this_thread::get_id());
        EXPECT_TRUE(laneBlockingScope);
    }

    // A rejected enqueue must throw to its sender without queuing a callback.
    {
        std::lock_guard lock(_hooks.dispatcher->mutex);
        _hooks.dispatcher->accepting = false;
    }
    struct RestoreAdmissions
    {
        std::shared_ptr<ObserverDispatcher> dispatcher;
        ~RestoreAdmissions()
        {
            std::lock_guard lock(dispatcher->mutex);
            dispatcher->accepting = true;
        }
    } restoreAdmissions {_hooks.dispatcher};
    std::promise<bool> rejected;
    auto rejection = rejected.get_future();
    std::thread rejecting([&] {
        try {
            invokeObserverTestMainThread([] {}, true);
            rejected.set_value(false);
        }
        catch (const std::runtime_error&) {
            rejected.set_value(true);
        }
        catch (...) {
            rejected.set_exception(std::current_exception());
        }
    });
    const auto rejectionDeadline = std::chrono::steady_clock::now() + 5s;
    while (rejection.wait_for(0ms) != std::future_status::ready
           && std::chrono::steady_clock::now() < rejectionDeadline) {
        static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
    }
    EXPECT_EQ(rejection.wait_for(0ms), std::future_status::ready);
    const auto rejectionCleanupDeadline = std::chrono::steady_clock::now() + 5s;
    while (rejection.wait_for(0ms) != std::future_status::ready) {
        if (std::chrono::steady_clock::now() >= rejectionCleanupDeadline) {
            std::fputs("observer test rejecting sender did not finish\n", stderr);
            std::abort();
        }
        static_cast<void>(App::MainThreadSignalConfig::serviceMarshalledTasks(1ms));
    }
    rejecting.join();
    EXPECT_TRUE(rejection.get());
}
