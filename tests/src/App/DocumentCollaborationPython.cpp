// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>

#include "App/Application.h"
#include "App/CollaborativeOperationRegistry.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/FeatureTest.h"
#include "App/private/CollaborativeOperationRegistryInternal.h"
#include <src/App/InitApplication.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace
{

constexpr std::string_view PythonDetachedOperationType = "App.Test.PythonDetachedSetLabel";

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

    PyObjectRef(PyObjectRef&& other) noexcept
        : _object(std::exchange(other._object, nullptr))
    {}

    PyObjectRef& operator=(PyObjectRef&& other) noexcept
    {
        if (this != &other) {
            Py_XDECREF(_object);
            _object = std::exchange(other._object, nullptr);
        }
        return *this;
    }

    [[nodiscard]] PyObject* get() const noexcept
    {
        return _object;
    }

private:
    PyObject* _object;
};

class PythonDetachedSetLabelOperation final: public App::CollaborativeOperation
{
public:
    PythonDetachedSetLabelOperation(std::string targetName,
                                    std::string targetIdentity,
                                    std::string value)
        : _targetName(std::move(targetName))
        , _targetIdentity(std::move(targetIdentity))
        , _value(std::move(value))
    {}

    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return PythonDetachedOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = document.getObject(_targetName.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _targetIdentity) {
            throw std::runtime_error("Python detached target became stale");
        }
        target->Label.setValue(_value);
    }

    [[nodiscard]] App::CollaborativePostconditionResult
    checkPostcondition(const App::Document& document) const override
    {
        const auto* target = document.getObject(_targetName.c_str());
        return {target
                    && document.collaborationObjectIdentity(*target) == _targetIdentity
                    && target->Label.getStrValue() == _value,
                "Python detached target must contain the prepared label"};
    }

private:
    const std::string _targetName;
    const std::string _targetIdentity;
    const std::string _value;
};

void ensurePythonDetachedAdapterRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(PythonDetachedOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                const auto& targetName = intent.arguments.at("target");
                const auto& value = intent.arguments.at("value");
                const bool delay = intent.arguments.contains("delay")
                    && intent.arguments.at("delay") == "true";
                const auto* target = document.getObject(targetName.c_str());
                if (!target) {
                    throw std::invalid_argument("Python detached target does not exist");
                }

                const std::string targetIdentity =
                    document.collaborationObjectIdentity(*target);
                const auto existence = App::DocumentRevisionKey::objectExistence(targetName);
                const auto model = App::DocumentRevisionKey::objectModel(targetName);
                const auto structure = App::DocumentRevisionKey::objectStructure(targetName);
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [targetName, targetIdentity, value, delay](std::stop_token stopToken) {
                        if (delay) {
                            for (int attempt = 0; attempt != 250; ++attempt) {
                                if (stopToken.stop_requested()) {
                                    throw std::runtime_error(
                                        "Python detached preparation cancelled");
                                }
                                std::this_thread::sleep_for(1ms);
                            }
                        }
                        if (stopToken.stop_requested()) {
                            throw std::runtime_error("Python detached preparation cancelled");
                        }
                        return std::make_unique<const PythonDetachedSetLabelOperation>(
                            targetName, targetIdentity, value);
                    };
                return App::CollaborativeOperationPreparation {
                    {existence,
                     model,
                     structure,
                     App::DocumentRevisionKey::unknownModelMutation()},
                    {model},
                    {{model, targetIdentity}},
                    std::move(task)};
            }));
    });
}

[[nodiscard]] bool terminalStatus(const char* status)
{
    return std::string_view(status) == "Completed" || std::string_view(status) == "Cancelled"
        || std::string_view(status) == "Failed";
}

class DocumentCollaborationPythonTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensurePythonDetachedAdapterRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("documentCollaborationPython");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Python test");
        _target = _document->addObject<App::FeatureTest>("Target");
        ASSERT_NE(_target, nullptr);
        _target->Label.setValue("Before");
        _document->recompute();
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::Document* _document {nullptr};
    App::FeatureTest* _target {nullptr};
    std::string _documentName;
};

TEST_F(DocumentCollaborationPythonTest,
       generatedDocumentSurfacePreservesSynchronousAndAddsAsyncPointerFreeMethods)
{
    Base::PyGILStateLocker gil;
    PyObjectRef app(PyImport_ImportModule("FreeCAD"));
    ASSERT_NE(app.get(), nullptr);
    PyObjectRef documentType(PyObject_GetAttrString(app.get(), "Document"));
    ASSERT_NE(documentType.get(), nullptr);
    PyObjectRef document(_document->getPyObject());
    ASSERT_NE(document.get(), nullptr);
    EXPECT_EQ(PyObject_IsInstance(document.get(), documentType.get()), 1);

    for (const char* method : {"beginEditSession",
                               "snapshotForEdit",
                               "prepareEdit",
                               "commitEdit",
                               "cancelEdit",
                               "editSessionStatus",
                               "prepareEditAsync",
                               "preparedEditStatus",
                               "cancelPreparedEdit",
                               "takePreparedEdit"}) {
        EXPECT_EQ(PyObject_HasAttrString(document.get(), method), 1) << method;
    }

    PyObjectRef session(
        PyObject_CallMethod(document.get(), "beginEditSession", "s", "python-actor"));
    ASSERT_NE(session.get(), nullptr);
    PyObject* sessionIdObject = PyDict_GetItemString(session.get(), "session_id");
    ASSERT_NE(sessionIdObject, nullptr);
    const char* sessionId = PyUnicode_AsUTF8(sessionIdObject);
    ASSERT_NE(sessionId, nullptr);

    PyObjectRef arguments(Py_BuildValue("{s:s,s:s}", "target", "Target", "value", "Async"));
    ASSERT_NE(arguments.get(), nullptr);
    PyObjectRef executionIdObject(PyObject_CallMethod(document.get(),
                                                       "prepareEditAsync",
                                                       "sssOs",
                                                       sessionId,
                                                       "python-async",
                                                       PythonDetachedOperationType.data(),
                                                       arguments.get(),
                                                       "python-binding-test"));
    ASSERT_NE(executionIdObject.get(), nullptr);
    ASSERT_TRUE(PyLong_Check(executionIdObject.get()));
    const auto executionId = PyLong_AsUnsignedLongLong(executionIdObject.get());
    ASSERT_FALSE(PyErr_Occurred());
    EXPECT_NE(executionId, 0U);

    PyObjectRef collected;
    for (int attempt = 0; attempt != 200; ++attempt) {
        PyObjectRef status(PyObject_CallMethod(
            document.get(), "preparedEditStatus", "K", executionId));
        ASSERT_NE(status.get(), nullptr);
        ASSERT_NE(status.get(), Py_None);
        EXPECT_TRUE(PyDict_Check(status.get()));
        PyObject* observedId = PyDict_GetItemString(status.get(), "execution_id");
        PyObject* observedStatus = PyDict_GetItemString(status.get(), "status");
        PyObject* diagnostic = PyDict_GetItemString(status.get(), "diagnostic");
        ASSERT_NE(observedId, nullptr);
        ASSERT_NE(observedStatus, nullptr);
        ASSERT_NE(diagnostic, nullptr);
        EXPECT_TRUE(PyLong_Check(observedId));
        EXPECT_TRUE(PyUnicode_Check(observedStatus));
        EXPECT_TRUE(PyUnicode_Check(diagnostic));
        const char* statusText = PyUnicode_AsUTF8(observedStatus);
        ASSERT_NE(statusText, nullptr);
        if (terminalStatus(statusText)) {
            collected = PyObjectRef(PyObject_CallMethod(
                document.get(), "takePreparedEdit", "sK", sessionId, executionId));
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_NE(collected.get(), nullptr);
    ASSERT_NE(collected.get(), Py_None);
    PyObject* collectionStatus = PyDict_GetItemString(collected.get(), "status");
    PyObject* prepared = PyDict_GetItemString(collected.get(), "prepared_edit");
    ASSERT_NE(collectionStatus, nullptr);
    ASSERT_NE(prepared, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(collectionStatus), "Completed");
    EXPECT_TRUE(PyCapsule_IsValid(prepared, "App.PreparedEdit"));

    PyObjectRef commit(
        PyObject_CallMethod(document.get(), "commitEdit", "sO", sessionId, prepared));
    ASSERT_NE(commit.get(), nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(commit.get(), "status")), "Committed");
    EXPECT_EQ(_target->Label.getStrValue(), "Async");

    PyObjectRef delayedArguments(
        Py_BuildValue("{s:s,s:s,s:s}", "target", "Target", "value", "Cancelled", "delay", "true"));
    ASSERT_NE(delayedArguments.get(), nullptr);
    PyObjectRef cancelledIdObject(PyObject_CallMethod(document.get(),
                                                       "prepareEditAsync",
                                                       "sssO",
                                                       sessionId,
                                                       "python-cancelled",
                                                       PythonDetachedOperationType.data(),
                                                       delayedArguments.get()));
    ASSERT_NE(cancelledIdObject.get(), nullptr);
    const auto cancelledId = PyLong_AsUnsignedLongLong(cancelledIdObject.get());
    ASSERT_FALSE(PyErr_Occurred());
    PyObjectRef cancelled(
        PyObject_CallMethod(document.get(), "cancelPreparedEdit", "K", cancelledId));
    ASSERT_NE(cancelled.get(), nullptr);
    EXPECT_EQ(PyObject_IsTrue(cancelled.get()), 1);

    PyObjectRef cancelledResult;
    for (int attempt = 0; attempt != 200; ++attempt) {
        PyObjectRef status(PyObject_CallMethod(
            document.get(), "preparedEditStatus", "K", cancelledId));
        ASSERT_NE(status.get(), nullptr);
        ASSERT_NE(status.get(), Py_None);
        PyObject* statusObject = PyDict_GetItemString(status.get(), "status");
        ASSERT_NE(statusObject, nullptr);
        const char* statusText = PyUnicode_AsUTF8(statusObject);
        ASSERT_NE(statusText, nullptr);
        if (terminalStatus(statusText)) {
            cancelledResult = PyObjectRef(PyObject_CallMethod(
                document.get(), "takePreparedEdit", "sK", sessionId, cancelledId));
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_NE(cancelledResult.get(), nullptr);
    ASSERT_NE(cancelledResult.get(), Py_None);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(cancelledResult.get(), "status")),
                 "Cancelled");
    EXPECT_EQ(PyDict_GetItemString(cancelledResult.get(), "prepared_edit"), Py_None);
}

}  // namespace
