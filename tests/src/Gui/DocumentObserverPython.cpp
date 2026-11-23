// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QEvent>
#include <QMetaObject>
#include <QScopeGuard>
#include <QThread>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/MainThreadSignal.h>
#include <Base/Interpreter.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentObserverPython.h>
#include <Gui/GuiPythonGate.h>
#include <Gui/ViewProvider.h>
#include <src/App/InitApplication.h>

#include <Python.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace
{

// GuiPythonGateTest clears the invocation hooks even when Gui::Application
// still exists. Own a fallback only in that case; never replace existing hooks
// or the application-owned service hook. This fixture pumps Qt for worker work.
class ScopedObserverTestHooks
{
public:
    ScopedObserverTestHooks()
    {
        if (!App::MainThreadSignalConfig::hasHooks()) {
            App::MainThreadSignalConfig::setHooks(&isMainThread, &invoke);
            _installed = true;
        }
    }

    ~ScopedObserverTestHooks()
    {
        if (_installed) {
            App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        }
    }

    ScopedObserverTestHooks(const ScopedObserverTestHooks&) = delete;
    ScopedObserverTestHooks& operator=(const ScopedObserverTestHooks&) = delete;

private:
    static bool isMainThread()
    {
        return QThread::currentThread() == qApp->thread();
    }

    static void invoke(std::function<void()>&& fn, bool blocking)
    {
        if (isMainThread()) {
            fn();
            return;
        }

        // Match the production dispatcher: queued Qt delivery, a promise for
        // completion/exception propagation, and no GIL held while waiting.
        auto completion = blocking ? std::make_shared<std::promise<void>>() : nullptr;
        std::future<void> result;
        if (completion) {
            result = completion->get_future();
        }
        const bool queued = QMetaObject::invokeMethod(
            qApp,
            [fn = std::move(fn), completion]() mutable {
                try {
                    if (completion) {
                        App::MainThreadSignalConfig::BlockingInvokeScope scope;
                        fn();
                        completion->set_value();
                    }
                    else {
                        fn();
                    }
                }
                catch (...) {
                    if (completion) {
                        completion->set_exception(std::current_exception());
                    }
                    else {
                        ADD_FAILURE() << "Asynchronous observer fixture dispatch threw";
                    }
                }
            },
            Qt::QueuedConnection);
        if (!queued) {
            // Qt has not retained fn, so failing admission cannot leave queued
            // references to the sender's stack behind.
            throw std::runtime_error("Cannot queue observer fixture dispatch");
        }
        if (blocking) {
            std::optional<Base::PyGILStateRelease> unlocked;
            if (Py_IsInitialized() && PyGILState_Check()) {
                unlocked.emplace();
            }
            // MainThreadSignal captures self, reference arguments and a stack
            // result. Never return on a timeout with those captures still queued.
            result.get();
        }
    }

    bool _installed {false};
};

// Run for every fixture setup, before any document or Python observers exist.
// This covers both first initialization (production hooks) and repeated runs
// after GuiPythonGateTest (fallback hooks with Gui::Application already alive).
::testing::AssertionResult verifyObserverTestDispatch()
{
    if (QThread::currentThread() != qApp->thread()) {
        return ::testing::AssertionFailure() << "Fixture must run on the Qt GUI thread";
    }
    const auto guiThread = std::this_thread::get_id();
    std::thread::id callbackThread;
    std::thread::id exceptionThread;
    std::thread::id asyncThread;
    bool blockingScope = false;
    bool senderRecognizedAsWorker = false;
    bool completedBeforeReturn = false;
    bool exceptionReturnedToSender = false;
    std::exception_ptr failure;
    std::atomic<bool> finished {false};

    App::MainThreadSignal<int(int&)> signal;
    auto connection = signal.connect([&](int& value) {
        callbackThread = std::this_thread::get_id();
        blockingScope = App::MainThreadSignalConfig::insideBlockingInvoke();
        value = 42;
        return 43;
    });
    App::MainThreadSignal<void()> throwingSignal;
    auto throwingConnection = throwingSignal.connect([&] {
        exceptionThread = std::this_thread::get_id();
        throw std::runtime_error("observer fixture dispatch probe");
    });
    {
        std::thread sender([&] {
            try {
                senderRecognizedAsWorker = !App::MainThreadSignalConfig::isMainThread();
                // Exercise MainThreadSignal's actual self/reference/stack-result
                // captures, including its GIL release around blocking dispatch.
                Base::PyGILStateLocker gil;
                int value = 0;
                const auto result = signal(value);
                completedBeforeReturn = value == 42 && result && *result == 43;
                try {
                    throwingSignal();
                }
                catch (const std::runtime_error& error) {
                    exceptionReturnedToSender =
                        std::string(error.what()) == "observer fixture dispatch probe";
                }
                App::MainThreadSignalConfig::invoke(
                    [&] { asyncThread = std::this_thread::get_id(); }, false);
            }
            catch (...) {
                failure = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });
        auto joinAndDrain = qScopeGuard([&] {
            // No assertions/timeout can abandon sender or queued stack captures.
            // Only dispatch Qt meta-calls; no unrelated timer/input re-entry.
            while (!finished.load(std::memory_order_acquire)) {
                QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
                std::this_thread::yield();
            }
            sender.join();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        });
    }
    if (failure || !senderRecognizedAsWorker || !completedBeforeReturn || !blockingScope
        || !exceptionReturnedToSender || callbackThread != guiThread
        || exceptionThread != guiThread || asyncThread != guiThread) {
        return ::testing::AssertionFailure()
            << "GUI dispatch must complete on the GUI thread, propagate exceptions, "
               "and preserve MainThreadSignal reference/result captures";
    }
    return ::testing::AssertionSuccess();
}

PyObject* pyPumpOneObserverEvent(PyObject* /*self*/, PyObject* /*args*/)
{
    return PyLong_FromSize_t(Gui::GuiPythonGate::pumpQueuedCallbacks(1));
}

PyMethodDef g_pumpOneMethodDef {
    "pump_one",
    &pyPumpOneObserverEvent,
    METH_NOARGS,
    "Pump one deferred GuiPythonGate callback recursively."
};

std::string fetchPythonError()
{
    PyObject* type = nullptr;
    PyObject* value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
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
    return message.empty() ? "Python execution failed" : message;
}

Gui::GuiPythonObserverValueEvent makeCreatedObjectEvent(
    const std::string& documentName,
    const std::string& objectName)
{
    Gui::GuiPythonObserverValueEvent event;
    event.eventKind = "CreatedObject";
    event.payloadJson = std::string(R"({"document":")") + documentName + R"(","object":")"
        + objectName + R"("})";
    return event;
}

std::size_t pumpOneWithinDeadline()
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const std::size_t pumped = Gui::GuiPythonGate::pumpQueuedCallbacks(1);
        if (pumped > 0) {
            return pumped;
        }
        std::this_thread::sleep_for(1ms);
    }
    return 0;
}

// Holds the GIL on a worker thread until released (or until the bounded
// watchdog elapses) so tests can deterministically force ObserverDelivery
// deferral without timing luck or hung worker threads on assertion failure.
class ScopedGilHolderThread
{
public:
    explicit ScopedGilHolderThread(std::function<void()> beforeRelease = {})
        : _beforeRelease(std::move(beforeRelease))
        , _thread([this] {
            Base::PyGILStateLocker gil;
            {
                std::lock_guard lock(_mutex);
                _holding = true;
            }
            _cv.notify_all();
            {
                std::unique_lock lock(_mutex);
                _cv.wait_for(lock, 5s, [this] { return _releaseRequested; });
            }
            if (_beforeRelease) {
                _beforeRelease();
            }
        })
    {
        std::unique_lock lock(_mutex);
        _cv.wait_for(lock, 5s, [this] { return _holding; });
    }

    ~ScopedGilHolderThread()
    {
        release();
    }

    ScopedGilHolderThread(const ScopedGilHolderThread&) = delete;
    ScopedGilHolderThread& operator=(const ScopedGilHolderThread&) = delete;

    bool holding() const
    {
        std::lock_guard lock(_mutex);
        return _holding;
    }

    void release()
    {
        {
            std::lock_guard lock(_mutex);
            _releaseRequested = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _cv;
    bool _holding {false};
    bool _releaseRequested {false};
    std::function<void()> _beforeRelease;
    std::thread _thread;
};

using EventLog = std::vector<std::pair<std::string, std::string>>;

class GuiDocumentObserverPythonTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        if (!QApplication::instance()) {
            static int argc = 1;
            static char name[] = "DocumentObserverPython";
            static char* argv[] = {name, nullptr};
            new QApplication(argc, argv);
        }
        tests::initApplication();
        if (!Gui::Application::Instance) {
            Gui::Application::initApplication();
            Gui::Application::initOpenInventor();
            new Gui::Application(true);
        }
    }

    void SetUp() override
    {
        _fallbackHooks.emplace();
        ASSERT_TRUE(verifyObserverTestDispatch());
        Gui::DocumentObserverPython::installValueEventHandler();

        App::DocumentInitFlags flags;
        flags.createView = false;
        _docName = App::GetApplication().getUniqueDocumentName("GuiObsPy");
        _document = App::GetApplication().newDocument(_docName.c_str(), "testUser", flags);
        ASSERT_NE(_document, nullptr);
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);

        _firstObject = _document->addObject("App::FeatureTest", "FirstObj");
        _secondObject = _document->addObject("App::FeatureTest", "SecondObj");
        ASSERT_NE(_firstObject, nullptr);
        ASSERT_NE(_secondObject, nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(_firstObject), nullptr);
        ASSERT_NE(_guiDocument->getViewProvider(_secondObject), nullptr);

        drainQueue();
        {
            Base::PyGILStateLocker gil;
            _ns.emplace();
            _ns->setItem("__builtins__", Py::Object(PyEval_GetBuiltins()));
        }
    }

    void TearDown() override
    {
        if (_ns.has_value()) {
            Base::PyGILStateLocker gil;
            auto clearNamespace = qScopeGuard([this] { _ns.reset(); });
            try {
                if (_ns->hasKey("cleanup")) {
                    Py::List observers(_ns->getItem("cleanup"));
                    for (Py::Sequence::size_type i = 0; i < observers.size(); ++i) {
                        Gui::DocumentObserverPython::removeObserver(Py::Object(observers[i]));
                    }
                }
                for (const char* weakKey : {"weak_a", "weak_e", "weak_g", "weak_h"}) {
                    if (_ns->hasKey(weakKey)) {
                        Py::Callable weakRef(_ns->getItem(weakKey));
                        Py::Object target = weakRef.apply(Py::Tuple());
                        if (!target.isNone()) {
                            Gui::DocumentObserverPython::removeObserver(target);
                        }
                    }
                }
            }
            catch (const Py::Exception&) {
                ADD_FAILURE() << "Observer cleanup failed: " << fetchPythonError();
            }
            _ns.reset();
        }
        drainQueue();
        QApplication::processEvents();
        if (!_docName.empty()) {
            if (App::GetApplication().getDocument(_docName.c_str())) {
                App::GetApplication().closeDocument(_docName.c_str());
            }
            _docName.clear();
        }
        QApplication::processEvents();
        drainQueue();

        _fallbackHooks.reset();
    }

    static void drainQueue()
    {
        for (int pass = 0; pass < 32; ++pass) {
            if (Gui::GuiPythonGate::pumpQueuedCallbacks(64) == 0) {
                break;
            }
        }
    }

    // Caller must hold the GIL.
    EventLog readLogAndClear(const char* key = "log")
    {
        EventLog entries;
        if (!_ns.has_value() || !_ns->hasKey(key)) {
            ADD_FAILURE() << "Missing Python test global: " << key;
            return entries;
        }
        Py::List pyLog(_ns->getItem(key));
        for (Py::Sequence::size_type i = 0; i < pyLog.size(); ++i) {
            Py::Tuple pair(pyLog[i]);
            entries.emplace_back(
                static_cast<std::string>(Py::String(pair[0])),
                static_cast<std::string>(Py::String(pair[1])));
        }
        pyLog.setSlice(0, static_cast<Py_ssize_t>(pyLog.size()), Py::List());
        return entries;
    }

    // Caller must hold the GIL.
    ::testing::AssertionResult runScript(const char* script)
    {
        PyObject* result = PyRun_String(script, Py_file_input, _ns->ptr(), _ns->ptr());
        if (!result) {
            return ::testing::AssertionFailure() << fetchPythonError();
        }
        Py_DECREF(result);
        return ::testing::AssertionSuccess();
    }

    // RAII also removes owned hooks if SetUp/TearDown throws or asserts fatally.
    std::optional<ScopedObserverTestHooks> _fallbackHooks;
    std::string _docName;
    App::Document* _document {nullptr};
    Gui::Document* _guiDocument {nullptr};
    App::DocumentObject* _firstObject {nullptr};
    App::DocumentObject* _secondObject {nullptr};
    std::optional<Py::Dict> _ns;
};

}  // namespace

TEST_F(GuiDocumentObserverPythonTest, DeferredDeliveryHandlesRemoveOtherSelfRemoveAndAdd)
{
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "import FreeCADGui as Gui\n"
            "log = []\n"
            "cleanup = []\n"
            "\n"
            "class RecordingObserver:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def slotCreatedObject(self, vp):\n"
            "        log.append((self.tag, vp.Object.Name))\n"
            "\n"
            "class RelabelOnlyObserver:\n"
            "    def slotRelabelDocument(self, doc):\n"
            "        pass\n"
            "\n"
            "class MutatingObserver:\n"
            "    def __init__(self, obs_b, obs_c):\n"
            "        self.obs_b = obs_b\n"
            "        self.obs_c = obs_c\n"
            "        self.mutated = False\n"
            "    def slotCreatedObject(self, vp):\n"
            "        name = vp.Object.Name\n"
            "        log.append(('A', name))\n"
            "        if not self.mutated:\n"
            "            self.mutated = True\n"
            "            Gui.removeDocumentObserver(self.obs_b)\n"
            "            Gui.removeDocumentObserver(self)\n"
            "            Gui.addDocumentObserver(self.obs_c)\n"
            "            for _ in range(64):\n"
            "                pad = RelabelOnlyObserver()\n"
            "                cleanup.append(pad)\n"
            "                Gui.addDocumentObserver(pad)\n"
            "        log.append(('A-done', name))\n"
            "\n"
            "obs_b = RecordingObserver('B')\n"
            "obs_c = RecordingObserver('C')\n"
            "obs_d = RecordingObserver('D')\n"
            "obs_a = MutatingObserver(obs_b, obs_c)\n"
            "cleanup.extend([obs_a, obs_b, obs_c, obs_d])\n"
            "Gui.addDocumentObserver(obs_a)\n"
            "Gui.addDocumentObserver(obs_b)\n"
            "Gui.addDocumentObserver(obs_d)\n"));
    }

    // Event 1: A runs, removes B, removes itself, adds C + 64 padding observers
    // (which do not implement slotCreatedObject). Eligibility for Event 1: A and
    // D receive Event 1; removed B and newly added C do not.
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "FirstObj"));
    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog firstEventLog;
    {
        Base::PyGILStateLocker gil;
        firstEventLog = readLogAndClear();
    }
    const EventLog expectedFirst {
        {"A", "FirstObj"},
        {"A-done", "FirstObj"},
        {"D", "FirstObj"},
    };
    EXPECT_EQ(firstEventLog, expectedFirst);

    // Event 2: A and B remain ineligible; D and C are eligible and receive Event 2.
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "SecondObj"));
    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog secondEventLog;
    {
        Base::PyGILStateLocker gil;
        secondEventLog = readLogAndClear();
    }
    const EventLog expectedSecond {
        {"D", "SecondObj"},
        {"C", "SecondObj"},
    };
    EXPECT_EQ(secondEventLog, expectedSecond);
}

TEST_F(
    GuiDocumentObserverPythonTest,
    DeferredSelfRemovalPreservesLifetimeThroughExceptionFinalizerAndReRegistration)
{
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "import gc, weakref\n"
            "import FreeCADGui as Gui\n"
            "log = []\n"
            "cleanup = []\n"
            "\n"
            "class RecordingObserver:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def slotCreatedObject(self, vp):\n"
            "        log.append((self.tag, vp.Object.Name))\n"
            "\n"
            "class WrapperLifetimeProbe:\n"
            "    def __init__(self, tag, obs_to_remove=None, obs_to_add=None):\n"
            "        self.tag = tag\n"
            "        self.obs_to_remove = obs_to_remove\n"
            "        self.obs_to_add = obs_to_add\n"
            "    def __call__(self, doc):\n"
            "        pass\n"
            "    def __del__(self):\n"
            "        log.append((self.tag, ''))\n"
            "        if self.obs_to_remove is not None:\n"
            "            Gui.removeDocumentObserver(self.obs_to_remove)\n"
            "        if self.obs_to_add is not None:\n"
            "            Gui.addDocumentObserver(self.obs_to_add)\n"
            "\n"
            "class ThrowingSelfRemovingObserver:\n"
            "    def __init__(self, obs_b):\n"
            "        self.obs_b = obs_b\n"
            "    def slotCreatedObject(self, vp):\n"
            "        name = vp.Object.Name\n"
            "        log.append(('A-enter', name))\n"
            "        Gui.removeDocumentObserver(self)\n"
            "        Gui.removeDocumentObserver(self.obs_b)\n"
            "        Gui.addDocumentObserver(self.obs_b)\n"
            "        log.append(('A-after-self-remove', name))\n"
            "        raise RuntimeError('intentional deferred observer failure')\n"
            "\n"
            "class ReentrantEqObserver:\n"
            "    def __init__(self, side_obs):\n"
            "        self.side_obs = side_obs\n"
            "        self.mutated = False\n"
            "    def __eq__(self, other):\n"
            "        if not self.mutated and other is not self:\n"
            "            self.mutated = True\n"
            "            Gui.addDocumentObserver(self.side_obs)\n"
            "            Gui.removeDocumentObserver(self.side_obs)\n"
            "        return self is other\n"
            "\n"
            "obs_b = RecordingObserver('B')\n"
            "obs_d = RecordingObserver('D')\n"
            "obs_e = RecordingObserver('E')\n"
            "obs_f = RecordingObserver('F')\n"
            "obs_a = ThrowingSelfRemovingObserver(obs_b)\n"
            "# Attach a callable whose sole remaining reference after registration is\n"
            "# DocumentObserverPython::pyDeletedDocument.py so __del__ observes the\n"
            "# exact moment ~DocumentObserverPython() runs.\n"
            "probe = WrapperLifetimeProbe('A-wrapper-destroyed', obs_e, obs_f)\n"
            "obs_a.slotDeletedDocument = probe\n"
            "weak_a = weakref.ref(obs_a)\n"
            "cleanup.extend([obs_b, obs_d, obs_e, obs_f])\n"
            "Gui.addDocumentObserver(obs_a)\n"
            "del obs_a.slotDeletedDocument\n"
            "del probe\n"
            "Gui.addDocumentObserver(obs_b)\n"
            "Gui.addDocumentObserver(obs_d)\n"
            "Gui.addDocumentObserver(obs_e)\n"
            "del obs_a\n"));
    }

    // Event 1: A removes itself, removes and re-registers B (new registration ID),
    // and raises. The C++ wrapper must stay alive past A-after-self-remove and
    // exception reporting, then destroy before D runs; its __del__ finalizer
    // safely removes E and adds F.
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "FirstObj"));
    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog firstEventLog;
    bool tracebackRetainsA = false;
    {
        Base::PyGILStateLocker gil;
        firstEventLog = readLogAndClear();
        ASSERT_TRUE(runScript(
            "import sys\n"
            "def callback_frames_holding_a(tb):\n"
            "    frames = []\n"
            "    while tb is not None:\n"
            "        frame = tb.tb_frame\n"
            "        if (frame.f_code is ThrowingSelfRemovingObserver.slotCreatedObject.__code__ and\n"
            "                frame.f_locals.get('self') is weak_a() and weak_a() is not None):\n"
            "            frames.append(frame)\n"
            "        tb = tb.tb_next\n"
            "    return frames\n"
            "gc.collect()\n"
            "reported_traceback = getattr(sys, 'last_traceback', None)\n"
            "callback_frames = callback_frames_holding_a(reported_traceback)\n"
            "a_retained_by_traceback = bool(callback_frames)\n"));
        tracebackRetainsA = static_cast<bool>(Py::Boolean(_ns->getItem("a_retained_by_traceback")));
    }
    const EventLog expectedFirst {
        {"A-enter", "FirstObj"},
        {"A-after-self-remove", "FirstObj"},
        {"A-wrapper-destroyed", ""},
        {"D", "FirstObj"},
    };
    EXPECT_EQ(firstEventLog, expectedFirst);
    EXPECT_TRUE(tracebackRetainsA);

    // Exception reporting intentionally retains the traceback for debugging.
    // Its completed callback frame owns `self` independently of the C++
    // wrapper. Clear only the identified, completed callback frames of this
    // test's exception, then verify no observer or callable reference remains.
    // The traceback object and its other frames remain available for debugging.
    bool aReleased = false;
    bool tracebackPreserved = false;
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "for callback_frame in callback_frames:\n"
            "    callback_frame.clear()\n"
            "gc.collect()\n"
            "a_released = (weak_a() is None)\n"
            "traceback_preserved = (getattr(sys, 'last_traceback', None) is reported_traceback)\n"));
        aReleased = static_cast<bool>(Py::Boolean(_ns->getItem("a_released")));
        tracebackPreserved = static_cast<bool>(Py::Boolean(_ns->getItem("traceback_preserved")));
    }
    EXPECT_TRUE(aReleased);
    EXPECT_TRUE(tracebackPreserved);

    // Event 2: D, re-registered B, and finalizer-added F receive Event 2; removed
    // A and finalizer-removed E do not.
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "SecondObj"));
    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog secondEventLog;
    {
        Base::PyGILStateLocker gil;
        secondEventLog = readLogAndClear();
    }
    const EventLog expectedSecond {
        {"D", "SecondObj"},
        {"B", "SecondObj"},
        {"F", "SecondObj"},
    };
    EXPECT_EQ(secondEventLog, expectedSecond);

    // Verify removeObserver() tolerates re-entrant registry mutation inside __eq__.
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "side_obs = RecordingObserver('Side')\n"
            "eq_obs = ReentrantEqObserver(side_obs)\n"
            "target_obs = RecordingObserver('Target')\n"
            "cleanup.extend([side_obs, eq_obs, target_obs])\n"
            "Gui.addDocumentObserver(eq_obs)\n"
            "Gui.addDocumentObserver(target_obs)\n"
            "Gui.removeDocumentObserver(target_obs)\n"
            "Gui.removeDocumentObserver(eq_obs)\n"
            "eq_mutated = eq_obs.mutated\n"));
        EXPECT_TRUE(static_cast<bool>(Py::Boolean(_ns->getItem("eq_mutated"))));
    }
}

TEST_F(GuiDocumentObserverPythonTest, DeferredRecursiveDeliveryHonorsImmediateRemovalAndNewAdditions)
{
    {
        Base::PyGILStateLocker gil;
        _ns->setItem("pump_one", Py::Object(PyCFunction_New(&g_pumpOneMethodDef, nullptr), true));
        ASSERT_TRUE(runScript(
            "import FreeCADGui as Gui\n"
            "log = []\n"
            "cleanup = []\n"
            "recursive_pumped = 0\n"
            "\n"
            "class RecordingObserver:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def slotCreatedObject(self, vp):\n"
            "        log.append((self.tag, vp.Object.Name))\n"
            "\n"
            "class RecursiveObserver:\n"
            "    def __init__(self, obs_b, obs_c):\n"
            "        self.obs_b = obs_b\n"
            "        self.obs_c = obs_c\n"
            "    def slotCreatedObject(self, vp):\n"
            "        global recursive_pumped\n"
            "        name = vp.Object.Name\n"
            "        log.append(('A-enter', name))\n"
            "        Gui.removeDocumentObserver(self.obs_b)\n"
            "        Gui.removeDocumentObserver(self)\n"
            "        Gui.addDocumentObserver(self.obs_c)\n"
            "        recursive_pumped = pump_one()\n"
            "        log.append(('A-exit', name))\n"
            "\n"
            "obs_b = RecordingObserver('B')\n"
            "obs_c = RecordingObserver('C')\n"
            "obs_d = RecordingObserver('D')\n"
            "obs_a = RecursiveObserver(obs_b, obs_c)\n"
            "cleanup.extend([obs_a, obs_b, obs_c, obs_d])\n"
            "Gui.addDocumentObserver(obs_a)\n"
            "Gui.addDocumentObserver(obs_b)\n"
            "Gui.addDocumentObserver(obs_d)\n"));
    }

    // Queue Event 1 followed by Event 2. During Event 1, A removes B, removes itself,
    // adds C, and recursively pumps Event 2. Event 2 must deliver to D and C only
    // (never re-entering removed A or B), while outer Event 1 keeps A alive through
    // A-exit, skips removed B, delivers to D, and skips C.
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "FirstObj"));
    Gui::GuiPythonGate::enqueueObserverValueEvent(makeCreatedObjectEvent(_docName, "SecondObj"));
    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog combinedLog;
    long recursivePumped = 0;
    {
        Base::PyGILStateLocker gil;
        combinedLog = readLogAndClear();
        recursivePumped = static_cast<long>(Py::Long(_ns->getItem("recursive_pumped")));
    }
    EXPECT_EQ(recursivePumped, 1L);
    const EventLog expected {
        {"A-enter", "FirstObj"},
        {"D", "SecondObj"},
        {"C", "SecondObj"},
        {"A-exit", "FirstObj"},
        {"D", "FirstObj"},
    };
    EXPECT_EQ(combinedLog, expected);
}

TEST_F(GuiDocumentObserverPythonTest, SynchronousAndDeferredNativeSignalLifecycleIsGilSafe)
{
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "import gc, weakref\n"
            "import FreeCADGui as Gui\n"
            "log = []\n"
            "cleanup = []\n"
            "\n"
            "class RecordingObserver:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def slotRelabelDocument(self, doc):\n"
            "        log.append((self.tag, doc.Document.Name))\n"
            "\n"
            "class WrapperLifetimeProbe:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def __call__(self, doc):\n"
            "        pass\n"
            "    def __del__(self):\n"
            "        log.append((self.tag, ''))\n"
            "\n"
            "class SyncMutatingObserver:\n"
            "    def __init__(self, obs_b, obs_c):\n"
            "        self.obs_b = obs_b\n"
            "        self.obs_c = obs_c\n"
            "    def slotRelabelDocument(self, doc):\n"
            "        name = doc.Document.Name\n"
            "        log.append(('A-enter', name))\n"
            "        Gui.removeDocumentObserver(self.obs_b)\n"
            "        Gui.removeDocumentObserver(self)\n"
            "        Gui.addDocumentObserver(self.obs_c)\n"
            "        log.append(('A-exit', name))\n"
            "\n"
            "class OneShotObserver:\n"
            "    def __init__(self, tag):\n"
            "        self.tag = tag\n"
            "    def slotRelabelDocument(self, doc):\n"
            "        name = doc.Document.Name\n"
            "        log.append((self.tag + '-enter', name))\n"
            "        Gui.removeDocumentObserver(self)\n"
            "        log.append((self.tag + '-exit', name))\n"
            "\n"
            "obs_b = RecordingObserver('B')\n"
            "obs_c = RecordingObserver('C')\n"
            "obs_d = RecordingObserver('D')\n"
            "obs_a = SyncMutatingObserver(obs_b, obs_c)\n"
            "probe_a = WrapperLifetimeProbe('A-wrapper-destroyed')\n"
            "obs_a.slotDeletedDocument = probe_a\n"
            "weak_a = weakref.ref(obs_a)\n"
            "cleanup.extend([obs_b, obs_c, obs_d])\n"
            "Gui.addDocumentObserver(obs_a)\n"
            "del obs_a.slotDeletedDocument\n"
            "del probe_a\n"
            "Gui.addDocumentObserver(obs_b)\n"
            "Gui.addDocumentObserver(obs_d)\n"
            "del obs_a\n"));
    }

    // 1. Deterministic immediate synchronous path (caller holds the GIL).
    // A must remain alive through A-exit and destroy its wrapper inside the
    // admitted GIL scope before D and C run, with zero pump calls needed.
    {
        Base::PyGILStateLocker gil;
        Gui::Application::Instance->signalRelabelDocument(*_guiDocument);
    }

    EventLog firstSyncLog;
    bool aReleased = false;
    {
        Base::PyGILStateLocker gil;
        firstSyncLog = readLogAndClear();
        ASSERT_TRUE(runScript("gc.collect()\na_released = (weak_a() is None)\n"));
        aReleased = static_cast<bool>(Py::Boolean(_ns->getItem("a_released")));
    }
    const EventLog expectedFirst {
        {"A-enter", _docName},
        {"A-exit", _docName},
        {"A-wrapper-destroyed", ""},
        {"D", _docName},
        {"C", _docName},
    };
    EXPECT_EQ(firstSyncLog, expectedFirst);
    EXPECT_TRUE(aReleased);

    // Subsequent synchronous emission delivers only to D and C.
    {
        Base::PyGILStateLocker gil;
        Gui::Application::Instance->signalRelabelDocument(*_guiDocument);
    }

    EventLog secondSyncLog;
    {
        Base::PyGILStateLocker gil;
        secondSyncLog = readLogAndClear();
    }
    const EventLog expectedSecond {
        {"D", _docName},
        {"C", _docName},
    };
    EXPECT_EQ(secondSyncLog, expectedSecond);

    // 2. Deterministic deferred native signal via worker holding the GIL:
    // the real connected slot must defer without blocking or touching Python,
    // and pumpQueuedCallbacks must deliver E and release its wrapper under the
    // pump's GIL.
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "Gui.removeDocumentObserver(obs_c)\n"
            "Gui.removeDocumentObserver(obs_d)\n"
            "obs_e = OneShotObserver('E')\n"
            "probe_e = WrapperLifetimeProbe('E-wrapper-destroyed')\n"
            "obs_e.slotDeletedDocument = probe_e\n"
            "weak_e = weakref.ref(obs_e)\n"
            "Gui.addDocumentObserver(obs_e)\n"
            "del obs_e.slotDeletedDocument\n"
            "del probe_e\n"
            "del obs_e\n"));
    }

    {
        ScopedGilHolderThread holder;
        ASSERT_TRUE(holder.holding());
        Gui::Application::Instance->signalRelabelDocument(*_guiDocument);
        holder.release();
    }

    EventLog beforePumpLog;
    bool eReleasedBeforePump = false;
    {
        Base::PyGILStateLocker gil;
        beforePumpLog = readLogAndClear();
        ASSERT_TRUE(runScript("gc.collect()\ne_released = (weak_e() is None)\n"));
        eReleasedBeforePump = static_cast<bool>(Py::Boolean(_ns->getItem("e_released")));
    }
    EXPECT_TRUE(beforePumpLog.empty());
    EXPECT_FALSE(eReleasedBeforePump);

    EXPECT_EQ(pumpOneWithinDeadline(), 1U);

    EventLog afterPumpLog;
    bool eReleasedAfterPump = false;
    {
        Base::PyGILStateLocker gil;
        afterPumpLog = readLogAndClear();
        ASSERT_TRUE(runScript("gc.collect()\ne_released = (weak_e() is None)\n"));
        eReleasedAfterPump = static_cast<bool>(Py::Boolean(_ns->getItem("e_released")));
    }
    const EventLog expectedDeferred {
        {"E-enter", _docName},
        {"E-exit", _docName},
        {"E-wrapper-destroyed", ""},
    };
    EXPECT_EQ(afterPumpLog, expectedDeferred);
    EXPECT_TRUE(eReleasedAfterPump);

    // 3. Deterministic off-thread removal while a native signal is deferred:
    // worker holds the GIL while the GUI emits signalRelabelDocument (which
    // queues without blocking), then removes G under the worker's GIL before
    // releasing. Subsequent pump must skip removed G without UAF.
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "obs_g = RecordingObserver('G')\n"
            "weak_g = weakref.ref(obs_g)\n"
            "Gui.addDocumentObserver(obs_g)\n"));
    }

    std::string workerRemoveError;
    {
        ScopedGilHolderThread holder([this, &workerRemoveError] {
            PyObject* result = PyRun_String(
                "Gui.removeDocumentObserver(obs_g)\n"
                "del obs_g\n",
                Py_file_input,
                _ns->ptr(),
                _ns->ptr());
            if (!result) {
                workerRemoveError = fetchPythonError();
            }
            else {
                Py_DECREF(result);
            }
        });
        ASSERT_TRUE(holder.holding());
        Gui::Application::Instance->signalRelabelDocument(*_guiDocument);
        holder.release();
    }
    ASSERT_TRUE(workerRemoveError.empty()) << workerRemoveError;

    bool gReleased = false;
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript("gc.collect()\ng_released = (weak_g() is None)\n"));
        gReleased = static_cast<bool>(Py::Boolean(_ns->getItem("g_released")));
    }
    EXPECT_TRUE(gReleased);

    EXPECT_EQ(pumpOneWithinDeadline(), 1U);
    EventLog afterRemovedPumpLog;
    {
        Base::PyGILStateLocker gil;
        afterRemovedPumpLog = readLogAndClear();
    }
    EXPECT_TRUE(afterRemovedPumpLog.empty());

    // 4. Native signal emitted without the caller holding the GIL when the GIL
    // is free: distinguish immediate tryAdmit execution from a 2ms-probe
    // deferral explicitly. In the immediate schedule, H's wrapper and Python
    // object must already be released inside tryAdmit's GilTryLock scope before
    // any pumpQueuedCallbacks call.
    {
        Base::PyGILStateLocker gil;
        ASSERT_TRUE(runScript(
            "obs_h = OneShotObserver('H')\n"
            "probe_h = WrapperLifetimeProbe('H-wrapper-destroyed')\n"
            "obs_h.slotDeletedDocument = probe_h\n"
            "weak_h = weakref.ref(obs_h)\n"
            "Gui.addDocumentObserver(obs_h)\n"
            "del obs_h.slotDeletedDocument\n"
            "del probe_h\n"
            "del obs_h\n"));
    }

    Gui::Application::Instance->signalRelabelDocument(*_guiDocument);

    EventLog immediateLog;
    bool hReleasedBeforePump = false;
    {
        Base::PyGILStateLocker gil;
        immediateLog = readLogAndClear();
        ASSERT_TRUE(runScript("gc.collect()\nh_released = (weak_h() is None)\n"));
        hReleasedBeforePump = static_cast<bool>(Py::Boolean(_ns->getItem("h_released")));
    }

    const EventLog expectedH {
        {"H-enter", _docName},
        {"H-exit", _docName},
        {"H-wrapper-destroyed", ""},
    };
    if (!immediateLog.empty()) {
        EXPECT_EQ(immediateLog, expectedH);
        EXPECT_TRUE(hReleasedBeforePump);
    }
    else {
        EXPECT_FALSE(hReleasedBeforePump);
        EXPECT_EQ(pumpOneWithinDeadline(), 1U);
        EventLog deferredLog;
        bool hReleasedAfterPump = false;
        {
            Base::PyGILStateLocker gil;
            deferredLog = readLogAndClear();
            ASSERT_TRUE(runScript("gc.collect()\nh_released = (weak_h() is None)\n"));
            hReleasedAfterPump = static_cast<bool>(Py::Boolean(_ns->getItem("h_released")));
        }
        EXPECT_EQ(deferredLog, expectedH);
        EXPECT_TRUE(hReleasedAfterPump);
    }
}
