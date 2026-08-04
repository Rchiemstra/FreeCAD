// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentPy.h"
#include "App/DocumentRevisionIndex.h"
#include "App/FeatureTest.h"
#include "App/MainThreadSignal.h"
#include <src/App/InitApplication.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace
{

class PyObjectRef final
{
public:
    explicit PyObjectRef(PyObject* object = nullptr)
        : _object(object)
    {}

    ~PyObjectRef()
    {
        Py_XDECREF(_object);
    }

    PyObjectRef(const PyObjectRef&) = delete;
    PyObjectRef& operator=(const PyObjectRef&) = delete;

    [[nodiscard]] PyObject* get() const noexcept
    {
        return _object;
    }

private:
    PyObject* _object;
};

struct CallbackProbe
{
    App::FeatureTest* target {nullptr};
    PyObject* document {nullptr};
    int calls {0};
    int nestedCalls {0};
    bool gilHeld {false};
    bool raisePythonError {false};
    bool reenter {false};
    std::thread::id callbackThread;
    std::string nestedStatus;
};

PyObject* runCompatibilityCallback(PyObject* self, PyObject*)
{
    auto* probe = static_cast<CallbackProbe*>(
        PyCapsule_GetPointer(self, "App.DocumentCompatibilityCallbackProbe"));
    if (!probe) {
        return nullptr;
    }
    ++probe->calls;
    probe->gilHeld = PyGILState_Check();
    probe->callbackThread = std::this_thread::get_id();

    if (probe->reenter) {
        CallbackProbe nestedProbe;
        nestedProbe.target = probe->target;
        PyObjectRef nestedCapsule(PyCapsule_New(
            &nestedProbe, "App.DocumentCompatibilityCallbackProbe", nullptr));
        if (!nestedCapsule.get()) {
            return nullptr;
        }
        static PyMethodDef nestedDefinition {
            "nestedCompatibilityCallback",
            runCompatibilityCallback,
            METH_NOARGS,
            nullptr,
        };
        PyObjectRef nestedCallback(
            PyCFunction_NewEx(&nestedDefinition, nestedCapsule.get(), nullptr));
        if (!nestedCallback.get()) {
            return nullptr;
        }
        PyObjectRef nestedResult(PyObject_CallMethod(
            probe->document, "commitCompatibilityMutation", "O", nestedCallback.get()));
        if (!nestedResult.get()) {
            return nullptr;
        }
        PyObject* status = PyDict_GetItemString(nestedResult.get(), "status");
        if (!status) {
            PyErr_SetString(PyExc_RuntimeError, "nested result has no status");
            return nullptr;
        }
        const char* statusText = PyUnicode_AsUTF8(status);
        if (!statusText) {
            return nullptr;
        }
        probe->nestedCalls = nestedProbe.calls;
        probe->nestedStatus = statusText;
    }

    if (probe->target) {
        probe->target->Label.setValue("Compatibility callback");
    }
    if (probe->raisePythonError) {
        PyErr_SetString(PyExc_ValueError, "compatibility callback failed");
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* makeCompatibilityCallback(CallbackProbe& probe)
{
    static PyMethodDef definition {
        "compatibilityCallback",
        runCompatibilityCallback,
        METH_NOARGS,
        nullptr,
    };
    PyObjectRef capsule(
        PyCapsule_New(&probe, "App.DocumentCompatibilityCallbackProbe", nullptr));
    if (!capsule.get()) {
        return nullptr;
    }
    return PyCFunction_NewEx(&definition, capsule.get(), nullptr);
}

class BlockingDocumentDispatcher
{
private:
    struct Task
    {
        std::function<void()> callback;
        std::mutex mutex;
        std::condition_variable changed;
        bool done {false};
    };

public:
    BlockingDocumentDispatcher()
        : _owner(std::this_thread::get_id())
    {
        Active = this;
        App::MainThreadSignalConfig::setHooks(&isOwnerThread, &invoke);
    }

    ~BlockingDocumentDispatcher()
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        Active = nullptr;
    }

    [[nodiscard]] bool waitUntilQueued(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [this] { return !_tasks.empty(); });
    }

    [[nodiscard]] bool runOne(std::chrono::milliseconds timeout = 2s)
    {
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(_mutex);
            if (!_changed.wait_for(lock, timeout, [this] { return !_tasks.empty(); })) {
                return false;
            }
            task = std::move(_tasks.front());
            _tasks.pop_front();
        }
        task->callback();
        {
            std::lock_guard lock(task->mutex);
            task->done = true;
        }
        task->changed.notify_all();
        return true;
    }

    void abortPending()
    {
        std::deque<std::shared_ptr<Task>> tasks;
        {
            std::lock_guard lock(_mutex);
            _aborting = true;
            tasks.swap(_tasks);
        }
        for (const auto& task : tasks) {
            {
                std::lock_guard lock(task->mutex);
                task->done = true;
            }
            task->changed.notify_all();
        }
        _changed.notify_all();
    }

private:

    static bool isOwnerThread()
    {
        return Active && std::this_thread::get_id() == Active->_owner;
    }

    static void invoke(std::function<void()>&& callback, bool blocking)
    {
        auto* dispatcher = Active;
        if (!dispatcher || isOwnerThread()) {
            callback();
            return;
        }
        auto task = std::make_shared<Task>();
        task->callback = std::move(callback);
        bool aborted = false;
        {
            std::lock_guard lock(dispatcher->_mutex);
            aborted = dispatcher->_aborting;
            if (!aborted) {
                dispatcher->_tasks.push_back(task);
            }
        }
        if (aborted) {
            {
                std::lock_guard lock(task->mutex);
                task->done = true;
            }
            task->changed.notify_all();
        }
        else {
            dispatcher->_changed.notify_one();
        }
        if (blocking) {
            std::unique_lock lock(task->mutex);
            task->changed.wait(lock, [&] { return task->done; });
        }
    }

    static inline BlockingDocumentDispatcher* Active {nullptr};
    const std::thread::id _owner;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<Task>> _tasks;
    bool _aborting {false};
};

class DocumentCollaborationPythonCompatibilityTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("pythonCompatibilityMutation");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Python compatibility");
        _target = _document->addObject<App::FeatureTest>("Target");
        ASSERT_NE(_target, nullptr);
        _target->Label.setValue("Before");
        _document->recompute();
    }

    void TearDown() override
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    [[nodiscard]] App::DocumentRevision wildcardRevision() const
    {
        const auto captured = _document->collaborationRevisions().capture(
            {App::DocumentRevisionKey::unknownModelMutation()});
        return captured.front().revision;
    }

    App::Document* _document {nullptr};
    App::FeatureTest* _target {nullptr};
    std::string _documentName;
};

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       surfaceCommitsExactCallbackAndPublishesOnlyUnknownModel)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_NE(document.get(), nullptr);
    EXPECT_EQ(PyObject_HasAttrString(document.get(), "commitCompatibilityMutation"), 1);

    CallbackProbe probe;
    probe.target = _target;
    probe.document = document.get();
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto callbackReferences = Py_REFCNT(callback.get());
    const auto wildcardBefore = wildcardRevision();

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));
    ASSERT_NE(result.get(), nullptr);
    ASSERT_TRUE(PyDict_Check(result.get()));
    PyObject* status = PyDict_GetItemString(result.get(), "status");
    PyObject* committed = PyDict_GetItemString(result.get(), "committed");
    PyObject* published = PyDict_GetItemString(result.get(), "published_revisions");
    ASSERT_NE(status, nullptr);
    ASSERT_TRUE(PyUnicode_Check(status));
    ASSERT_NE(committed, nullptr);
    ASSERT_TRUE(PyBool_Check(committed));
    ASSERT_NE(published, nullptr);
    ASSERT_TRUE(PyList_Check(published));
    ASSERT_EQ(PyList_Size(published), 1);
    PyObject* publication = PyList_GetItem(published, 0);
    ASSERT_NE(publication, nullptr);
    ASSERT_TRUE(PyDict_Check(publication));
    PyObject* publicationKind = PyDict_GetItemString(publication, "kind");
    ASSERT_NE(publicationKind, nullptr);
    ASSERT_TRUE(PyUnicode_Check(publicationKind));
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
    EXPECT_EQ(PyObject_IsTrue(committed), 1);
    EXPECT_STREQ(PyUnicode_AsUTF8(publicationKind), "UnknownModelMutation");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_TRUE(probe.gilHeld);
    EXPECT_EQ(probe.callbackThread, std::this_thread::get_id());
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_EQ(wildcardRevision(), wildcardBefore + 1);
    EXPECT_EQ(Py_REFCNT(callback.get()), callbackReferences);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       pythonFailureRollsBackAndPreservesThePythonException)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.target = _target;
    probe.raisePythonError = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto callbackReferences = Py_REFCNT(callback.get());
    const auto wildcardBefore = wildcardRevision();

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));
    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
    EXPECT_EQ(Py_REFCNT(callback.get()), callbackReferences);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       reentrantCompatibilityMutationIsRejectedWithoutInvokingNestedCallback)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.target = _target;
    probe.document = document.get();
    probe.reenter = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));
    ASSERT_NE(result.get(), nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")), "Committed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.nestedCalls, 0);
    EXPECT_EQ(probe.nestedStatus, "Busy");
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       offOwnerPythonCallDispatchesCallbackWithGilToDocumentOwner)
{
    BlockingDocumentDispatcher dispatcher;
    PyObject* document = nullptr;
    PyObject* callback = nullptr;
    CallbackProbe probe;
    probe.target = _target;
    {
        Base::PyGILStateLocker gil;
        document = _document->getPyObject();
        ASSERT_NE(document, nullptr);
        probe.document = document;
        callback = makeCompatibilityCallback(probe);
        ASSERT_NE(callback, nullptr);
    }

    PyObject* result = nullptr;
    bool callFailed = false;
    std::thread worker([&] {
        Base::PyGILStateLocker gil;
        result = PyObject_CallMethod(document, "commitCompatibilityMutation", "O", callback);
        callFailed = result == nullptr;
    });

    const bool queued = dispatcher.waitUntilQueued();
    const bool dispatched = queued && dispatcher.runOne();
    if (!dispatched) {
        dispatcher.abortPending();
    }
    worker.join();

    ASSERT_TRUE(queued);
    ASSERT_TRUE(dispatched);

    {
        Base::PyGILStateLocker gil;
        ASSERT_FALSE(callFailed);
        ASSERT_NE(result, nullptr);
        EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result, "status")), "Committed");
        EXPECT_EQ(probe.calls, 1);
        EXPECT_TRUE(probe.gilHeld);
        EXPECT_EQ(probe.callbackThread, std::this_thread::get_id());
        Py_DECREF(result);
        Py_DECREF(callback);
        Py_DECREF(document);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       offOwnerPythonFailureRollsBackAndRestoresTheCallingThreadException)
{
    BlockingDocumentDispatcher dispatcher;
    PyObject* document = nullptr;
    PyObject* callback = nullptr;
    Py_ssize_t callbackReferences = 0;
    CallbackProbe probe;
    probe.target = _target;
    probe.raisePythonError = true;
    {
        Base::PyGILStateLocker gil;
        document = _document->getPyObject();
        ASSERT_NE(document, nullptr);
        probe.document = document;
        callback = makeCompatibilityCallback(probe);
        ASSERT_NE(callback, nullptr);
        callbackReferences = Py_REFCNT(callback);
    }
    const auto wildcardBefore = wildcardRevision();

    PyObject* result = nullptr;
    bool exactType = false;
    std::string message;
    std::thread worker([&] {
        Base::PyGILStateLocker gil;
        result = PyObject_CallMethod(document, "commitCompatibilityMutation", "O", callback);
        exactType = result == nullptr && PyErr_ExceptionMatches(PyExc_ValueError);
        if (result == nullptr) {
            PyObject* type = nullptr;
            PyObject* value = nullptr;
            PyObject* traceback = nullptr;
            PyErr_Fetch(&type, &value, &traceback);
            PyObject* text = value ? PyObject_Str(value) : nullptr;
            if (text) {
                if (const char* utf8 = PyUnicode_AsUTF8(text)) {
                    message = utf8;
                }
            }
            Py_XDECREF(text);
            Py_XDECREF(type);
            Py_XDECREF(value);
            Py_XDECREF(traceback);
        }
    });

    const bool queued = dispatcher.waitUntilQueued();
    const bool dispatched = queued && dispatcher.runOne();
    if (!dispatched) {
        dispatcher.abortPending();
    }
    worker.join();

    ASSERT_TRUE(queued);
    ASSERT_TRUE(dispatched);
    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(exactType);
    EXPECT_EQ(message, "compatibility callback failed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_TRUE(probe.gilHeld);
    EXPECT_EQ(probe.callbackThread, std::this_thread::get_id());
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
    {
        Base::PyGILStateLocker gil;
        EXPECT_EQ(Py_REFCNT(callback), callbackReferences);
        Py_DECREF(callback);
        Py_DECREF(document);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       rejectedOffOwnerCallDoesNotInvokeOrRetainTheCallback)
{
    PyObject* document = nullptr;
    PyObject* callback = nullptr;
    Py_ssize_t callbackReferences = 0;
    CallbackProbe probe;
    probe.target = _target;
    {
        Base::PyGILStateLocker gil;
        document = _document->getPyObject();
        ASSERT_NE(document, nullptr);
        callback = makeCompatibilityCallback(probe);
        ASSERT_NE(callback, nullptr);
        callbackReferences = Py_REFCNT(callback);
    }

    PyObject* result = nullptr;
    std::thread worker([&] {
        Base::PyGILStateLocker gil;
        result = PyObject_CallMethod(document, "commitCompatibilityMutation", "O", callback);
    });
    worker.join();

    {
        Base::PyGILStateLocker gil;
        ASSERT_NE(result, nullptr);
        PyObject* status = PyDict_GetItemString(result, "status");
        ASSERT_NE(status, nullptr);
        EXPECT_STREQ(PyUnicode_AsUTF8(status), "Unsupported");
        EXPECT_EQ(probe.calls, 0);
        EXPECT_EQ(Py_REFCNT(callback), callbackReferences);
        Py_DECREF(result);
        Py_DECREF(callback);
        Py_DECREF(document);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest, rejectsNonCallableInput)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "i", 7));
    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
}

}  // namespace
