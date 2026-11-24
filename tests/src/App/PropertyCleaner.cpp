// SPDX-License-Identifier: LGPL-2.1-or-later
#include <gtest/gtest.h>

#include <App/PropertyCleanup.h>
#include <App/PropertyContainer.h>
#include <App/PropertyStandard.h>
#include <App/PropertyPythonObject.h>
#include <App/PreparedEditExecutor.h>
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentExecutionLane.h>
#include <App/GeometryJobManager.h>
#include <App/CollaborationRegistry.h>
#include <App/DocumentCollaborationService.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <App/MainThreadSignal.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace {
struct Observed {
    std::atomic<int> destructions {0};
    std::thread::id destroyedOn;
};
class TrackedProperty final: public App::PropertyInteger
{
public:
    explicit TrackedProperty(Observed& observed): observed(observed) {}
    ~TrackedProperty() override {
        observed.destroyedOn = std::this_thread::get_id();
        observed.destructions.fetch_add(1, std::memory_order_release);
    }
private:
    Observed& observed;
};

class ReentrantProperty final: public App::PropertyInteger
{
public:
    explicit ReentrantProperty(Observed& other): other(other) {}
    ~ReentrantProperty() override { App::Property::destroy(new TrackedProperty(other)); }
private:
    Observed& other;
};

class ShutdownOnExitProperty final: public App::PropertyInteger
{
public:
    explicit ShutdownOnExitProperty(std::atomic<int>& result): result(result) {}
    ~ShutdownOnExitProperty() override {
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { result.store(1); return; }
        result.store(-1);
    }
private:
    std::atomic<int>& result;
};

struct RetireOnRelease {
    explicit RetireOnRelease(App::Property* value): value(value) {}
    ~RetireOnRelease() {
        // Slot capture destruction must not hold this signal's mutex.
        App::GetApplication().signalAppendDynamicProperty.connect([](const App::Property&) {});
        App::Property::destroy(value);
    }
    App::Property* value;
};

struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered {false};
    bool release {false};
};

// This object is intentionally initialized before the cleaner holder on its
// owner thread, so its destructor runs after the holder's destructor.
struct EarlierTlsRetirement {
    App::Property* property {nullptr};
    Gate* gate {nullptr};
    ~EarlierTlsRetirement() {
        if (!property) return;
        if (gate) {
            std::unique_lock lock(gate->mutex);
            gate->entered = true;
            gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->release; });
        }
        App::Property::destroy(property);
    }
};
thread_local EarlierTlsRetirement earlierTlsRetirement;

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
Gate* initialCloseGate = nullptr;
Gate* delayedExitGate = nullptr;
Gate* delayedDestructorGate = nullptr;
Gate* beforeChangeGate = nullptr;
std::thread* beforeChangeThread = nullptr;
App::Document* expectedInitialCloseDocument = nullptr;
std::atomic<int>* initialCloseObserved = nullptr;
App::Property* initialCloseRetired = nullptr;
struct CloseFromCapture {
    explicit CloseFromCapture(std::atomic<int>* result): result(result) {}
    std::atomic<int>* result;
    ~CloseFromCapture() {
        result->store(App::GetApplication().closeDocument("CleanerZCapture") ? 1 : -1);
    }
};
fastsignals::signal<void()>* initialCloseSignal = nullptr;
void releaseAfterInitialClose() {
    const bool live = App::GetApplication().getDocument("CleanerLateClose")
        == expectedInitialCloseDocument;
    initialCloseObserved->store(live ? 1 : -1);
    if (initialCloseRetired) {
        App::Property::destroy(initialCloseRetired);
        initialCloseRetired = nullptr;
        App::drainRetiredPropertiesAtQuiescence();
    }
    if (initialCloseSignal) initialCloseSignal->disconnect_all_slots();
    { std::lock_guard lock(initialCloseGate->mutex); initialCloseGate->release = true; }
    initialCloseGate->cv.notify_all();
}
void beginDelayedExitDuringSeal() {
    { std::lock_guard lock(delayedExitGate->mutex); delayedExitGate->release = true; }
    delayedExitGate->cv.notify_all();
    std::unique_lock lock(delayedDestructorGate->mutex);
    if (!delayedDestructorGate->cv.wait_for(lock, 5s,
                                             [] { return delayedDestructorGate->entered; }))
        std::_Exit(90);
}
void resumeBeforeChangeDuringSeal() {
    { std::lock_guard lock(beforeChangeGate->mutex); beforeChangeGate->release = true; }
    beforeChangeGate->cv.notify_all();
    if (beforeChangeThread->joinable()) beforeChangeThread->join();
}
class PendingStatusOperation final: public App::CollaborativeOperation {
public:
    std::string_view typeId() const noexcept override { return "Tests.CleanerPendingStatus"; }
    void apply(App::Document&) const override {}
    App::CollaborativePostconditionResult checkPostcondition(const App::Document&) const override
    { return {true, {}}; }
};
class BlockedExitProperty final: public App::PropertyInteger {
public:
    BlockedExitProperty(Gate& gate, App::Document* document, std::atomic<int>& observed)
        : gate(gate), document(document), observed(observed) {}
    ~BlockedExitProperty() override {
        {
            std::unique_lock lock(gate.mutex);
            gate.entered = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.release; });
        }
        observed.store(App::GetApplication().getDocument("CleanerDrainLive") == document ? 1 : -1);
    }
private:
    Gate& gate;
    App::Document* document;
    std::atomic<int>& observed;
};
#endif

class BlockingChange: public App::PropertyContainer
{
public:
    explicit BlockingChange(Gate& gate): gate(gate) {}
protected:
    void onChanged(const App::Property*) override
    {
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return gate.release; });
    }
private:
    Gate& gate;
};

class BlockingBeforeChange: public App::PropertyContainer
{
public:
    explicit BlockingBeforeChange(Gate& gate): gate(gate) {}
    std::atomic<int> changes {0};
protected:
    void onBeforeChange(const App::Property*) override
    {
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.cv.notify_all();
        gate.cv.wait(lock, [&] { return gate.release; });
    }
    void onChanged(const App::Property*) override { ++changes; }
private:
    Gate& gate;
};

class ShutdownBeforeChange: public App::PropertyContainer
{
public:
    bool refused {false};
    int changes {0};
protected:
    void onBeforeChange(const App::Property*) override
    {
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
    }
    void onChanged(const App::Property*) override { ++changes; }
};

class RetireOnChange: public App::PropertyContainer
{
protected:
    void onChanged(const App::Property* property) override
    {
        App::PropertyCleanupBoundary nestedJob;
        App::Property::destroy(const_cast<App::Property*>(property));
    }
};

std::thread::id fakeGuiThread;
bool isFakeGui() { return std::this_thread::get_id() == fakeGuiThread; }
void invokeFakeGui(std::function<void()>&& fn, bool) { fn(); }

PyObject* notifyFinalized(PyObject* capsule, PyObject*)
{
    auto* count = static_cast<std::atomic<int>*>(PyCapsule_GetPointer(capsule, "cleaner-count"));
    if (!count) return nullptr;
    ++*count;
    Py_RETURN_NONE;
}
PyMethodDef notifyDefinition {"notify", notifyFinalized, METH_NOARGS, nullptr};

PyObject* tryReentrantPool(PyObject* capsule, PyObject*)
{
    auto* result = static_cast<std::atomic<int>*>(PyCapsule_GetPointer(capsule, "cleaner-count"));
    if (!result) return nullptr;
    try {
        App::PreparedEditExecutor pool(8, 8);
        result->store(-1);
    }
    catch (const Base::Exception&) { result->store(1); }
    catch (...) { result->store(-2); }
    Py_RETURN_NONE;
}
PyMethodDef tryPoolDefinition {"try_pool", tryReentrantPool, METH_NOARGS, nullptr};

App::PropertyPythonObject* makeCancelProbe(std::atomic<int>& result)
{
    PyObject* globals = PyDict_New();
    PyObject* capsule = PyCapsule_New(&result, "cleaner-count", nullptr);
    PyObject* notify = PyCFunction_New(&notifyDefinition, capsule);
    if (!globals || !capsule || !notify) return nullptr;
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyDict_SetItemString(globals, "notify", notify);
    Py_DECREF(notify); Py_DECREF(capsule);
    PyObject* created = PyRun_String(
        "import FreeCAD\nclass Probe:\n"
        "    def __del__(self):\n"
        "        FreeCAD.getDocument('CleanerCancel')\n"
        "        notify()\nprobe = Probe()\n", Py_file_input, globals, globals);
    if (!created) return nullptr;
    Py_DECREF(created);
    auto* property = new App::PropertyPythonObject;
    property->setPyObject(PyDict_GetItemString(globals, "probe"));
    if (PyDict_DelItemString(globals, "probe") != 0) return nullptr;
    Py_DECREF(globals);
    return property;
}
// The owner may be drained after successful shutdown closes the document.
// Its probe measures finalization without requiring the document to remain live.
App::PropertyPythonObject* makeFinalizationProbe(std::atomic<int>& result)
{
    PyObject* globals = PyDict_New();
    PyObject* capsule = PyCapsule_New(&result, "cleaner-count", nullptr);
    PyObject* notify = capsule ? PyCFunction_New(&notifyDefinition, capsule) : nullptr;
    if (!globals || !capsule || !notify
        || PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) != 0
        || PyDict_SetItemString(globals, "notify", notify) != 0) {
        Py_XDECREF(notify); Py_XDECREF(capsule); Py_XDECREF(globals);
        return nullptr;
    }
    Py_DECREF(notify); Py_DECREF(capsule);
    PyObject* created = PyRun_String(
        "class Probe:\n    def __del__(self): notify()\nprobe = Probe()\n",
        Py_file_input, globals, globals);
    if (!created) { Py_DECREF(globals); return nullptr; }
    Py_DECREF(created);
    PyObject* probe = PyDict_GetItemString(globals, "probe");
    if (!probe) { Py_DECREF(globals); return nullptr; }
    auto* property = new App::PropertyPythonObject;
    property->setPyObject(probe);
    if (PyDict_DelItemString(globals, "probe") != 0) {
        delete property; Py_DECREF(globals); return nullptr;
    }
    Py_DECREF(globals);
    return property;
}
}  // namespace

TEST(PropertyCleaner, IndependentManagedJobsDrainOnTheirOwnThread)
{
    tests::initApplication();
    Gate gate;
    Observed observed;
    std::thread::id workerId;
    std::thread other([&] {
        BlockingChange container(gate);
        App::PropertyInteger property;
        property.setContainer(&container);
        property.setValue(1); // Hold an actual BASE PropertyCleaner guard.
        property.setContainer(nullptr);
    });
    struct Join {
        Gate& gate;
        std::thread& thread;
        ~Join() {
            { std::lock_guard lock(gate.mutex); gate.release = true; }
            gate.cv.notify_all();
            thread.join();
        }
    } join {gate, other};
    {
        std::unique_lock lock(gate.mutex);
        ASSERT_TRUE(gate.cv.wait_for(lock, 5s, [&] { return gate.entered; }));
    }
    std::thread worker([&] {
        workerId = std::this_thread::get_id();
        App::PropertyCleanupBoundary boundary;
        App::Property::destroy(new TrackedProperty(observed));
    });
    worker.join();
    EXPECT_EQ(observed.destructions.load(std::memory_order_acquire), 1);
    EXPECT_EQ(observed.destroyedOn, workerId);
    // B is still held here; the old global guard counter would defer A's
    // destruction in an otherwise ordered, race-free schedule.
}

TEST(PropertyCleaner, BoundaryUnwindsOnExceptionAndLeavesOwnedValueAlive)
{
    tests::initApplication();
    Observed retired;
    Observed owned;
    auto live = std::make_unique<TrackedProperty>(owned);
    try {
        App::PropertyCleanupBoundary boundary;
        App::Property::destroy(new TrackedProperty(retired));
        throw std::runtime_error("job failed");
    }
    catch (const std::runtime_error&) {}
    EXPECT_EQ(retired.destructions.load(), 1);
    EXPECT_EQ(owned.destructions.load(), 0);
    live.reset();
    EXPECT_EQ(owned.destructions.load(), 1);
}

TEST(PropertyCleaner, SelfRemovalRemainsQueuedUntilAnotherGuard)
{
    tests::initApplication();
    Observed observed;
    RetireOnChange container;
    auto* property = new TrackedProperty(observed);
    property->setContainer(&container);
    property->setValue(7);
    EXPECT_EQ(observed.destructions.load(), 0);
    App::PropertyInteger later;
    later.touch();
    EXPECT_EQ(observed.destructions.load(), 1);
}

TEST(PropertyCleaner, JobBoundaryDoesNotDrainOlderUnrelatedRetirement)
{
    tests::initApplication();
    Observed older;
    Observed job;
    App::Property::destroy(new TrackedProperty(older));
    {
        App::PropertyCleanupBoundary boundary;
        App::Property::destroy(new TrackedProperty(job));
    }
    EXPECT_EQ(job.destructions.load(), 1);
    EXPECT_EQ(older.destructions.load(), 0);
    App::drainRetiredPropertiesAtQuiescence();
    EXPECT_EQ(older.destructions.load(), 1);
}

TEST(PropertyCleaner, JobDrainAlsoHandlesRetirementFromAPropertyDestructor)
{
    tests::initApplication();
    Observed nested;
    {
        App::PropertyCleanupBoundary boundary;
        App::Property::destroy(new ReentrantProperty(nested));
    }
    EXPECT_EQ(nested.destructions.load(), 1);
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST(PropertyCleaner, FailedOwnerStartupDoesNotPublishDocument)
{
    tests::initApplication();
    const auto name = App::GetApplication().getUniqueDocumentName("CleanerStartFailure");
    App::Internal::failNextLaneStartupForTesting();
    EXPECT_THROW(static_cast<void>(App::GetApplication().newDocument(name.c_str())), std::bad_alloc);
    EXPECT_EQ(App::GetApplication().getDocument(name.c_str()), nullptr);
    EXPECT_EQ(App::GetApplication().getUniqueDocumentName(name.c_str()), name);
}
#endif

TEST(PropertyCleaner, UnmanagedPythonThreadMustNotStrandRetiredProperty)
{
    tests::initApplication();
    Base::PyGILStateLocker gil;
    PyObject* globals = PyDict_New();
    ASSERT_NE(globals, nullptr);
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyObject* evaluated = PyRun_String(
        "released = []\n"
        "class Probe:\n"
        "    def __del__(self): released.append('finalized')\n"
        "probe = Probe()\n", Py_file_input, globals, globals);
    ASSERT_NE(evaluated, nullptr);
    Py_DECREF(evaluated);
    auto* property = new App::PropertyPythonObject;
    property->setPyObject(PyDict_GetItemString(globals, "probe"));
    ASSERT_EQ(PyDict_DelItemString(globals, "probe"), 0);
    {
        Base::PyGILStateRelease release;
        std::thread worker([property] {
            Base::PyGILStateLocker lock;
            App::Property::destroy(property);
        });
        worker.join();
    }
    const auto released = PyList_Size(PyDict_GetItemString(globals, "released"));
    EXPECT_EQ(released, 1);
    if (released == 0) delete property; // This detached property has no other owner.
    Py_DECREF(globals);
}

#if GTEST_HAS_DEATH_TEST
#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST(PropertyCleaner, EarlierTlsDestructorCanRetireAfterCleanerHolder)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        Observed original; Observed late;
        std::thread worker([&] {
            earlierTlsRetirement.property = new TrackedProperty(late);
            App::Property::destroy(new TrackedProperty(original));
        });
        worker.join();
        if (original.destructions.load() != 1) std::_Exit(92);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (late.destructions.load() != 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (late.destructions.load() != 1) std::_Exit(93);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, LiveForeignThreadRefusesUntilEarlierTlsDestructorCompletes)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerCancel");
        if (!document) std::_Exit(92);
        document->lockTransaction();
        std::atomic<int> first {0}; std::atomic<int> late {0};
        App::PropertyPythonObject* firstProperty;
        App::PropertyPythonObject* lateProperty;
        {
            Base::PyGILStateLocker gil;
            firstProperty = makeCancelProbe(first);
            lateProperty = makeCancelProbe(late);
            if (!firstProperty || !lateProperty) std::_Exit(93);
        }
        Gate workerGate; Gate destructorGate;
        std::thread worker([&] {
            earlierTlsRetirement.property = lateProperty;
            earlierTlsRetirement.gate = &destructorGate;
            App::Property::destroy(firstProperty);
            std::unique_lock lock(workerGate.mutex);
            workerGate.entered = true; workerGate.cv.notify_all();
            workerGate.cv.wait(lock, [&] { return workerGate.release; });
        });
        { std::unique_lock lock(workerGate.mutex);
          if (!workerGate.cv.wait_for(lock, 5s, [&] { return workerGate.entered; })) std::_Exit(94); }
        delayedExitGate = &workerGate; delayedDestructorGate = &destructorGate;
        App::Internal::setPostInitialFinalizationCloseHookForTesting(&beginDelayedExitDuringSeal);
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || first.load() != 0 || late.load() != 0
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerCancel") != document) std::_Exit(95);
        App::Internal::setPostInitialFinalizationCloseHookForTesting(nullptr);
        { std::lock_guard lock(workerGate.mutex); workerGate.release = true; }
        workerGate.cv.notify_all();
        { Base::PyGILStateLocker gil; Base::PyGILStateRelease release;
          std::unique_lock lock(destructorGate.mutex);
          if (!destructorGate.cv.wait_for(lock, 5s, [&] { return destructorGate.entered; })) std::_Exit(97); }
        if (first.load() != 1 || late.load() != 0) std::_Exit(98);
        { std::lock_guard lock(destructorGate.mutex); destructorGate.release = true; }
        destructorGate.cv.notify_all();
        {
            Base::PyGILStateLocker gil; Base::PyGILStateRelease release;
            worker.join();
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while ((first.load() != 1 || late.load() != 1)
                   && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        }
        if (first.load() != 1 || late.load() != 1) std::_Exit(96);
        App::Internal::setPostInitialFinalizationCloseHookForTesting(nullptr);
        document->unlockTransaction();
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, PythonFinalizerCannotRegisterNewPoolWhileRegistryIsSealed)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        std::atomic<int> result {0};
        App::PropertyPythonObject* property = nullptr;
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            PyObject* capsule = PyCapsule_New(&result, "cleaner-count", nullptr);
            PyObject* attempt = PyCFunction_New(&tryPoolDefinition, capsule);
            if (!globals || !capsule || !attempt) std::_Exit(92);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyDict_SetItemString(globals, "try_pool", attempt);
            Py_DECREF(attempt); Py_DECREF(capsule);
            PyObject* created = PyRun_String(
                "class Probe:\n    def __del__(self): try_pool()\nprobe = Probe()\n",
                Py_file_input, globals, globals);
            if (!created) std::_Exit(93);
            Py_DECREF(created);
            property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(94);
            Py_DECREF(globals);
        }
        App::Property::destroy(property);
        App::Application::destruct();
        if (result.load() != 1) std::_Exit(96);
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, LiveForeignOwnersRefuseAndDrainOnTheirOwnExit)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerCancel");
        if (!document || !document->executionLane()) std::_Exit(92);
        document->lockTransaction();
        std::atomic<int> ownerFinalized {0}; std::atomic<int> externalFinalized {0};
        std::atomic<int> idleFinalized {0};
        App::PropertyPythonObject* ownerProperty;
        App::PropertyPythonObject* externalProperty;
        App::PropertyPythonObject* idleProperty;
        {
            Base::PyGILStateLocker gil;
            ownerProperty = makeFinalizationProbe(ownerFinalized);
            externalProperty = makeCancelProbe(externalFinalized);
            idleProperty = makeCancelProbe(idleFinalized);
            if (!ownerProperty || !externalProperty || !idleProperty) std::_Exit(93);
        }
        auto posted = document->executionLane()->postToOwner([ownerProperty] {
            App::Property::destroy(ownerProperty);
        });
        posted.get();
        Gate exiting; Gate idle;
        std::thread external([&] {
            App::Property::destroy(externalProperty);
            std::unique_lock lock(exiting.mutex); exiting.entered = true; exiting.cv.notify_all();
            exiting.cv.wait(lock, [&] { return exiting.release; });
        });
        std::thread alive([&] {
            App::Property::destroy(idleProperty);
            std::unique_lock lock(idle.mutex); idle.entered = true; idle.cv.notify_all();
            idle.cv.wait(lock, [&] { return idle.release; });
        });
        { std::unique_lock lock(exiting.mutex);
          if (!exiting.cv.wait_for(lock, 5s, [&] { return exiting.entered; })) std::_Exit(94); }
        { std::unique_lock lock(idle.mutex);
          if (!idle.cv.wait_for(lock, 5s, [&] { return idle.entered; })) std::_Exit(95); }
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerCancel") != document
            || ownerFinalized.load() != 0 || externalFinalized.load() != 0
            || idleFinalized.load() != 0) std::_Exit(96);
        { std::lock_guard lock(exiting.mutex); exiting.release = true; }
        exiting.cv.notify_all();
        { Base::PyGILStateLocker gil; Base::PyGILStateRelease release; external.join(); }
        if (externalFinalized.load() != 1 || idleFinalized.load() != 0) std::_Exit(97);
        { std::lock_guard lock(idle.mutex); idle.release = true; }
        idle.cv.notify_all();
        { Base::PyGILStateLocker gil; Base::PyGILStateRelease release; alive.join(); }
        if (idleFinalized.load() != 1) std::_Exit(98);
        document->unlockTransaction();
        App::Application::destruct();
        if (ownerFinalized.load() != 1) std::_Exit(99);
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, FailureAfterSealKeepsManagersAndAllowsFinalizationRetry)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        Gate gate; Observed first; Observed second;
        std::thread idle([&] {
            App::Property::destroy(new TrackedProperty(first));
            { std::unique_lock lock(gate.mutex); gate.entered = true; gate.cv.notify_all();
              gate.cv.wait(lock, [&] { return gate.release; }); }
            App::Property::destroy(new TrackedProperty(second));
        });
        { std::unique_lock lock(gate.mutex);
          if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(92); }
        bool preflightRefused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { preflightRefused = true; }
        if (!preflightRefused || first.destructions.load() != 0
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running)
            std::_Exit(93);
        { std::lock_guard lock(gate.mutex); gate.release = true; }
        gate.cv.notify_all(); idle.join();
        if (first.destructions.load() != 1 || second.destructions.load() != 1) std::_Exit(95);
        App::Internal::failNextFinalizationAfterSealForTesting();
        bool failedAfterSeal = false;
        try { App::Application::destruct(); }
        catch (const std::bad_alloc&) { failedAfterSeal = true; }
        if (!failedAfterSeal
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Incomplete
            || App::GetApplication().geometryJobManager().status(0).has_value()) std::_Exit(94);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, ActiveCommandDocumentClosesAfterOwnerStops)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerLateClose");
        if (!document || !document->executionLane()) std::_Exit(92);
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            "Tests.CleanerPendingStatus", [](const App::Document&,
                                             const App::CollaborativeOperationIntent&) {
                App::CollaborativeOperationPreparation::DetachedTask task = [](std::stop_token) {
                    return std::make_unique<const PendingStatusOperation>();
                };
                return (App::CollaborativeOperationPreparation {{}, {}, {}, std::move(task)});
            }));
        auto session = document->collaborationService().beginEditSession("cleaner-test");
        App::CollaborativeOperationIntent intent; intent.operationType = "Tests.CleanerPendingStatus";
        const auto pending = document->collaborationService().prepareEditAsync(
            session.sessionId(), "pending", intent, "cleaner-test");
        if (!document->collaborationService().preparedEditStatus(pending)) std::_Exit(96);
        auto* other = App::GetApplication().newDocument("CleanerZCapture");
        if (!other) std::_Exit(100);
        auto otherSession = other->collaborationService().beginEditSession("cleaner-test");
        const auto otherPending = other->collaborationService().prepareEditAsync(
            otherSession.sessionId(), "pending-other", intent, "cleaner-test");
        if (!other->collaborationService().preparedEditStatus(otherPending)) std::_Exit(101);
        Gate gate; std::atomic<int> observed {0}; std::atomic<int> closed {0};
        std::atomic<int> finalized {0}; std::atomic<int> lateFinalized {0};
        std::atomic<int> captureClosed {0};
        fastsignals::signal<void()> closeSignal;
        auto closeCapture = std::make_shared<CloseFromCapture>(&captureClosed);
        closeSignal.connect([closeCapture] {});
        closeCapture.reset();
        initialCloseSignal = &closeSignal;
        App::PropertyPythonObject* lateProperty = nullptr;
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            PyObject* capsule = PyCapsule_New(&finalized, "cleaner-count", nullptr);
            PyObject* notify = PyCFunction_New(&notifyDefinition, capsule);
            PyObject* lateCapsule = PyCapsule_New(&lateFinalized, "cleaner-count", nullptr);
            PyObject* lateNotify = PyCFunction_New(&notifyDefinition, lateCapsule);
            PyObject* executionId = PyLong_FromUnsignedLongLong(pending);
            if (!globals || !capsule || !notify || !lateCapsule || !lateNotify || !executionId)
                std::_Exit(97);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyDict_SetItemString(globals, "notify", notify);
            PyDict_SetItemString(globals, "late_notify", lateNotify);
            PyDict_SetItemString(globals, "execution_id", executionId);
            Py_DECREF(executionId); Py_DECREF(notify); Py_DECREF(capsule);
            Py_DECREF(lateNotify); Py_DECREF(lateCapsule);
            PyObject* result = PyRun_String(
                "import FreeCAD\nclass Probe:\n"
                "    def __del__(self):\n"
                "        if FreeCAD.getDocument('CleanerLateClose').preparedEditStatus(execution_id) is not None: notify()\n"
                "class LateProbe:\n"
                "    def __del__(self):\n"
                "        try: document = FreeCAD.getDocument('CleanerLateClose')\n"
                "        except NameError: late_notify()\n"
                "        else: document.preparedEditStatus(execution_id)\n"
                "probe = Probe()\nlate_probe = LateProbe()\n", Py_file_input, globals, globals);
            if (!result) std::_Exit(98);
            Py_DECREF(result);
            auto* property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            lateProperty = new App::PropertyPythonObject;
            lateProperty->setPyObject(PyDict_GetItemString(globals, "late_probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(99);
            if (PyDict_DelItemString(globals, "late_probe") != 0) std::_Exit(102);
            initialCloseRetired = property;
            Py_DECREF(globals);
        }
        auto lateCapture = std::make_shared<RetireOnRelease>(lateProperty);
        App::GetApplication().signalAppendDynamicProperty.connect(
            [lateCapture](const App::Property&) {});
        lateCapture.reset();
        initialCloseGate = &gate; expectedInitialCloseDocument = document; initialCloseObserved = &observed;
        App::Internal::setPostInitialFinalizationCloseHookForTesting(&releaseAfterInitialClose);
        App::GetApplication().signalDeleteDocument.connect([&](const App::Document&) { ++closed; });
        auto blocker = document->executionLane()->postToOwner([&] {
            std::unique_lock lock(gate.mutex); gate.entered = true; gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.release; });
        });
        { std::unique_lock lock(gate.mutex);
          if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
        auto handle = document->executionHandle();
        App::DocumentCommand command; command.kind = App::DocumentCommandKind::Recompute;
        command.document = handle.identity(); command.recompute = App::DocumentCommandRecomputePayload {};
        command.recompute->coalescingKey = "shutdown-close-regression";
        if (!handle.trySubmit(std::move(command)).accepted()) std::_Exit(94);
        App::Application::destruct();
        blocker.get();
        if (observed.load() != 1 || closed.load() != 2 || finalized.load() != 1
            || lateFinalized.load() != 1
            || captureClosed.load() != 1) std::_Exit(95);
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, PersistentCloseRefusalPreservesDocumentAndServicesForRetry)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerPersistentRefusal");
        if (!document) std::_Exit(92);
        const auto identity = document->collaborationIdentity();
        document->lockTransaction();
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || App::GetApplication().getDocument("CleanerPersistentRefusal") != document
            || document->collaborationIdentity() != identity
            || App::GetApplication().geometryJobManager().status(0).has_value()) std::_Exit(93);
        if (App::GetApplication().shutdownState() != App::Application::ShutdownState::Incomplete)
            std::_Exit(94);
        auto handle = document->executionHandle();
        App::DocumentCommand command;
        command.kind = App::DocumentCommandKind::Recompute;
        command.document = handle.identity();
        command.recompute = App::DocumentCommandRecomputePayload {};
        if (handle.trySubmit(std::move(command)).result != App::DocumentCommandSubmitResult::Closed)
            std::_Exit(95);
        bool rejectedNewDocument = false;
        try { (void)App::GetApplication().newDocument("CleanerRejectedAfterShutdown"); }
        catch (const Base::RuntimeError&) { rejectedNewDocument = true; }
        if (!rejectedNewDocument
            || App::GetApplication().getDocument("CleanerRejectedAfterShutdown")) std::_Exit(96);
        document->unlockTransaction();
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

// Shutdown must refuse before sealing or stopping anything while a foreign
// setter is between before-change and its value store.
TEST(PropertyCleaner, BeforeChangeSetterRequiresPreEffectShutdownRefusal)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerBeforeChange");
        if (!document) std::_Exit(92);
        document->lockTransaction();
        Gate gate;
        BlockingBeforeChange container(gate);
        App::PropertyInteger property;
        property.setContainer(&container);
        std::atomic<bool> setterFailed {false};
        std::thread setter([&] {
            try { property.setValue(42); }
            catch (...) { setterFailed.store(true); }
        });
        { std::unique_lock lock(gate.mutex);
          if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
        beforeChangeGate = &gate; beforeChangeThread = &setter;
        App::Internal::setPostInitialFinalizationCloseHookForTesting(&resumeBeforeChangeDuringSeal);
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        // A correct preflight returns before closing the document or invoking
        // the hook; only then may this caller let the admitted setter finish.
        if (!refused || !setter.joinable()
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerBeforeChange") != document)
            std::_Exit(94);
        App::Internal::setPostInitialFinalizationCloseHookForTesting(nullptr);
        { std::lock_guard lock(gate.mutex); gate.release = true; }
        gate.cv.notify_all(); setter.join();
        if (setterFailed.load() || property.getValue() != 42 || container.changes.load() != 1)
            std::_Exit(95);
        property.setContainer(nullptr);
        document->unlockTransaction();
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, BeforeChangeReentrantShutdownIsRefused)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(90); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerBeforeReentry");
        if (!document) std::_Exit(91);
        ShutdownBeforeChange container;
        App::PropertyInteger property;
        property.setContainer(&container);
        property.setValue(42);
        if (!container.refused || container.changes != 1 || property.getValue() != 42
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerBeforeReentry") != document)
            std::_Exit(92);
        property.setContainer(nullptr);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, IndependentPreparedPoolMustExitBeforeShutdown)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(90); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerExternalPool");
        if (!document) std::_Exit(91);
        auto pool = std::make_unique<App::PreparedEditExecutor>(1, 2);
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerExternalPool") != document)
            std::_Exit(92);
        pool.reset();
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}
#endif

TEST(PropertyCleaner, OwnerDispatchedDestructRefusesBeforeStoppingLane)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerOwnerRefusal");
        if (!document || !document->executionLane()) std::_Exit(92);
        auto* lane = document->executionLane();
        const auto identity = document->collaborationIdentity();
        auto result = lane->postToOwner([] {
            try { App::Application::destruct(); }
            catch (const Base::Exception&) { return true; }
            return false;
        });
        if (!result.get() || App::GetApplication().getDocument("CleanerOwnerRefusal") != document
            || document->executionLane() != lane || document->collaborationIdentity() != identity)
            std::_Exit(93);
        auto alive = lane->postToOwner([] { return 42; });
        if (alive.get() != 42) std::_Exit(94);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, TlsDrainReentrantDestructRefusesBeforeChanges)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerDrainRefusal");
        if (!document || !document->executionLane()) std::_Exit(92);
        auto* lane = document->executionLane();
        const auto identity = document->collaborationIdentity();
        std::atomic<int> refused {0};
        std::thread worker([&] { App::Property::destroy(new ShutdownOnExitProperty(refused)); });
        worker.join();
        if (refused.load() != 1 || App::GetApplication().getDocument("CleanerDrainRefusal") != document
            || document->executionLane() != lane || document->collaborationIdentity() != identity)
            std::_Exit(93);
        auto alive = lane->postToOwner([] { return 42; });
        if (alive.get() != 42) std::_Exit(94);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
TEST(PropertyCleaner, ActiveForeignExitDrainRefusesBeforeClosingDocument)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerDrainLive");
        if (!document) std::_Exit(92);
        Gate gate; std::atomic<int> observed {0};
        std::thread worker([&] { App::Property::destroy(new BlockedExitProperty(gate, document, observed)); });
        { std::unique_lock lock(gate.mutex);
          if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93); }
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running
            || App::GetApplication().getDocument("CleanerDrainLive") != document) std::_Exit(94);
        { std::lock_guard lock(gate.mutex); gate.release = true; }
        gate.cv.notify_all(); worker.join();
        if (observed.load() != 1) std::_Exit(95);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}
#endif

TEST(PropertyCleaner, SignalCaptureRetirementFinalizesBeforeApplicationMembersDie)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex; std::condition_variable watchdogChanged; bool done = false;
        std::thread watchdog([&] { std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91); });
        tests::initApplication();
        if (!App::GetApplication().newDocument("CleanerCapture")) std::_Exit(92);
        std::atomic<int> finalized {0};
        App::PropertyPythonObject* property;
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            PyObject* capsule = PyCapsule_New(&finalized, "cleaner-count", nullptr);
            PyObject* notify = PyCFunction_New(&notifyDefinition, capsule);
            if (!globals || !capsule || !notify) std::_Exit(93);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyDict_SetItemString(globals, "notify", notify);
            Py_DECREF(notify); Py_DECREF(capsule);
            PyObject* result = PyRun_String(
                "import FreeCAD\nclass Probe:\n"
                "    def __del__(self):\n"
                "        try: FreeCAD.getDocument('CleanerCapture')\n"
                "        except NameError: pass\n"
                "        notify()\nprobe = Probe()\n", Py_file_input, globals, globals);
            if (!result) std::_Exit(94);
            Py_DECREF(result);
            property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(95);
            Py_DECREF(globals);
        }
        auto capture = std::make_shared<RetireOnRelease>(property);
        App::GetApplication().signalAppendDynamicProperty.connect([capture](const App::Property&) {});
        capture.reset();
        App::Application::destruct();
        if (finalized.load() != 1) std::_Exit(96);
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all(); watchdog.join(); std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, ActiveExternalGuardRefusesDestructBeforeChangesAndAllowsRetry)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex;
        std::condition_variable watchdogChanged;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91);
        });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerRefusal");
        if (!document || !document->executionLane()) std::_Exit(92);
        auto* lane = document->executionLane();
        const auto id = document->collaborationIdentity();
        Gate gate;
        std::thread worker([&] {
            BlockingChange container(gate);
            App::PropertyInteger property;
            property.setContainer(&container);
            property.setValue(1);
            property.setContainer(nullptr);
        });
        {
            std::unique_lock lock(gate.mutex);
            if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(93);
        }
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || App::GetApplication().getDocument("CleanerRefusal") != document
            || document->executionLane() != lane
            || document->collaborationIdentity() != id
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running)
            std::_Exit(94);
        auto task = lane->postToOwner([] { return 42; });
        if (task.get() != 42) std::_Exit(95);
        { std::lock_guard lock(gate.mutex); gate.release = true; }
        gate.cv.notify_all();
        worker.join();
        if (!App::GetApplication().closeDocument("CleanerRefusal")) std::_Exit(96);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all();
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, ShutdownRefusesWhileIdlePythonOwnerIsAlive)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex;
        std::condition_variable watchdogChanged;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91);
        });
        tests::initApplication();
        std::atomic<int> finalized {0};
        App::PropertyPythonObject* property;
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            PyObject* capsule = PyCapsule_New(&finalized, "cleaner-count", nullptr);
            PyObject* notify = PyCFunction_New(&notifyDefinition, capsule);
            if (!globals || !capsule || !notify) std::_Exit(92);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyDict_SetItemString(globals, "notify", notify);
            Py_DECREF(notify);
            Py_DECREF(capsule);
            PyObject* result = PyRun_String(
                "class Probe:\n    def __del__(self): notify()\nprobe = Probe()\n",
                Py_file_input, globals, globals);
            if (!result) std::_Exit(93);
            Py_DECREF(result);
            property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(94);
            Py_DECREF(globals);
        }
        Gate gate;
        std::thread idle([&] {
            { Base::PyGILStateLocker gil; App::Property::destroy(property); }
            std::unique_lock lock(gate.mutex);
            gate.entered = true;
            gate.cv.notify_all();
            gate.cv.wait(lock, [&] { return gate.release; });
        });
        {
            std::unique_lock lock(gate.mutex);
            if (!gate.cv.wait_for(lock, 5s, [&] { return gate.entered; })) std::_Exit(95);
        }
        bool refused = false;
        try { App::Application::destruct(); }
        catch (const Base::Exception&) { refused = true; }
        if (!refused || finalized.load() != 0
            || App::GetApplication().shutdownState() != App::Application::ShutdownState::Running)
            std::_Exit(96);
        { std::lock_guard lock(gate.mutex); gate.release = true; }
        gate.cv.notify_all();
        { Base::PyGILStateLocker gil; Base::PyGILStateRelease release; idle.join(); }
        if (finalized.load() != 1) std::_Exit(97);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all();
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, PythonThreadRemovePropertyMustNotStrandRetiredPayload)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex;
        std::condition_variable watchdogChanged;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91);
        });
        tests::initApplication();
        auto* document = App::GetApplication().newDocument("CleanerPythonThread");
        if (!document) std::_Exit(92);
        auto* object = document->addObject("App::FeatureTest", "Target");
        if (!object) std::_Exit(93);
        auto* property = dynamic_cast<App::PropertyPythonObject*>(
            object->addDynamicProperty("App::PropertyPythonObject", "Payload"));
        if (!property) std::_Exit(94);
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            if (!globals) std::_Exit(95);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyObject* pyObject = object->getPyObject();
            PyDict_SetItemString(globals, "obj", pyObject);
            Py_DECREF(pyObject);
            PyObject* created = PyRun_String(
                "import threading\n"
                "released = []\n"
                "finalized = threading.Event()\n"
                "class Probe:\n"
                "    def __del__(self):\n"
                "        released.append('finalized')\n"
                "        finalized.set()\n"
                "probe = Probe()\n", Py_file_input, globals, globals);
            if (!created) std::_Exit(96);
            Py_DECREF(created);
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(97);
            PyObject* completed = PyRun_String(
                "removed = []\n"
                "errors = []\n"
                "import threading\n"
                "def remove_in_thread():\n"
                "    try: removed.append(obj.removeProperty('Payload'))\n"
                "    except BaseException as error: errors.append(str(error))\n"
                "worker = threading.Thread(target=remove_in_thread)\n"
                "worker.start()\n"
                "worker.join()\n", Py_file_input, globals, globals);
            if (!completed) std::_Exit(98);
            Py_DECREF(completed);
            if (PyList_Size(PyDict_GetItemString(globals, "errors")) != 0
                || PyList_Size(PyDict_GetItemString(globals, "removed")) != 1
                || PyList_GetItem(PyDict_GetItemString(globals, "removed"), 0) != Py_True
                || object->getDynamicPropertyByName("Payload")) std::_Exit(99);
            // Python join can precede C++ TLS completion and the registry reaper.
            // Event.wait releases the GIL so either cleanup path can finalize.
            PyObject* waited = PyObject_CallMethod(
                PyDict_GetItemString(globals, "finalized"), "wait", "d", 5.0);
            if (!waited) std::_Exit(101);
            const bool finalized = waited == Py_True;
            Py_DECREF(waited);
            if (!finalized || PyList_Size(PyDict_GetItemString(globals, "released")) != 1)
                std::_Exit(102);
            Py_DECREF(globals);
        }
        if (!App::GetApplication().closeDocument("CleanerPythonThread")) std::_Exit(100);
        // The finalizer can run in TLS cleanup before the native thread exits.
        // Shutdown requires foreign cleaner owners to have physically exited.
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!App::preparePropertyCleanupFinalization()) {
            if (std::chrono::steady_clock::now() >= deadline) std::_Exit(103);
            std::this_thread::yield();
        }
        App::cancelPropertyCleanupFinalization();
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all();
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

// A separate process is essential: application shutdown seals lane creation,
// and a watchdog must be able to terminate a broken GIL-held join safely.
TEST(PropertyCleaner, DetachedGuiOwnerDrainsPythonBeforeApplicationShutdown)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex;
        std::condition_variable watchdogChanged;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91);
        });
        tests::initApplication();
        fakeGuiThread = std::this_thread::get_id();
        App::MainThreadSignalConfig::setHooks(&isFakeGui, &invokeFakeGui);
        auto* document = App::GetApplication().newDocument("CleanerShutdown");
        if (!document || !document->executionLane()) std::_Exit(92);
        const std::string name = document->getName();
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            if (!globals) std::_Exit(93);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyObject* evaluated = PyRun_String(
                "released = []\n"
                "class Probe:\n"
                "    def __del__(self): released.append('finalized')\n"
                "probe = Probe()\n", Py_file_input, globals, globals);
            if (!evaluated) std::_Exit(94);
            Py_DECREF(evaluated);
            auto* property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(95);
            auto queued = document->executionLane()->postToOwner([property] {
                App::Property::destroy(property);
            });
            queued.get();
            // This follows the real GUI-detach path. The owner cannot run its
            // Python finalizer until the shutdown wait releases this GIL.
            document->shutdownExecutionLane();
            App::DocumentExecutionLane::shutdownAllBeforeInterpreterFinalization();
            if (PyList_Size(PyDict_GetItemString(globals, "released")) != 1) std::_Exit(96);
            Py_DECREF(globals);
        }
        if (!App::GetApplication().closeDocument(name.c_str())) std::_Exit(97);
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all();
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}

TEST(PropertyCleaner, NonGuiOwnerJoinReleasesGilForPythonFinalizer)
{
    const auto previousStyle = GTEST_FLAG_GET(death_test_style);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT({
        std::mutex watchdogMutex;
        std::condition_variable watchdogChanged;
        bool done = false;
        std::thread watchdog([&] {
            std::unique_lock lock(watchdogMutex);
            if (!watchdogChanged.wait_for(lock, 10s, [&] { return done; })) std::_Exit(91);
        });
        tests::initApplication();
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        auto* document = App::GetApplication().newDocument("CleanerJoin");
        if (!document || !document->executionLane()) std::_Exit(92);
        const std::string name = document->getName();
        {
            Base::PyGILStateLocker gil;
            PyObject* globals = PyDict_New();
            if (!globals) std::_Exit(93);
            PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
            PyObject* evaluated = PyRun_String(
                "released = []\n"
                "class Probe:\n"
                "    def __del__(self): released.append('finalized')\n"
                "probe = Probe()\n", Py_file_input, globals, globals);
            if (!evaluated) std::_Exit(94);
            Py_DECREF(evaluated);
            auto* property = new App::PropertyPythonObject;
            property->setPyObject(PyDict_GetItemString(globals, "probe"));
            if (PyDict_DelItemString(globals, "probe") != 0) std::_Exit(95);
            auto queued = document->executionLane()->postToOwner([property] {
                App::Property::destroy(property);
            });
            queued.get();
            // No GUI hooks: this path joins the owner, while this thread holds
            // the GIL needed by its retired Python property's destructor.
            document->shutdownExecutionLane();
            if (PyList_Size(PyDict_GetItemString(globals, "released")) != 1) std::_Exit(96);
            Py_DECREF(globals);
        }
        if (!App::GetApplication().closeDocument(name.c_str())) std::_Exit(97);
        App::Application::destruct();
        { std::lock_guard lock(watchdogMutex); done = true; }
        watchdogChanged.notify_all();
        watchdog.join();
        std::_Exit(0);
    }, ::testing::ExitedWithCode(0), "");
    GTEST_FLAG_SET(death_test_style, previousStyle);
}
#endif
