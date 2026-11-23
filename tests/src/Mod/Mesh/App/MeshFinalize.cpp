// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <Base/Exception.h>
#include <Base/Interpreter.h>
#include <Mod/Mesh/App/MeshFeature.h>
#include <src/App/InitApplication.h>

#include <memory>
#include <new>
#include <string>

namespace {
class ThrowingFeature: public Mesh::Feature
{
public:
    enum class Failure { none, base, allocation, python };
    Failure failure {Failure::none};
    int changes {0};
    bool errorOnEntry {false};

    void onChanged(const App::Property* property) override
    {
        if (property == &Mesh) {
            ++changes;  // Represents an observer that already saw the mutation.
            errorOnEntry = PyErr_Occurred() != nullptr;
            switch (failure) {
                case Failure::base: throw Base::ValueError("mesh observer failure");
                case Failure::allocation: throw std::bad_alloc();
                case Failure::python: throw Py::ValueError("mesh observer failure");
                case Failure::none: break;
            }
        }
        Mesh::Feature::onChanged(property);
    }
};

class MeshFinalizeTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        Base::Interpreter().runString("import Mesh");
    }

    void resetMesh()
    {
        feature.failure = ThrowingFeature::Failure::none;
        Mesh::MeshObject mesh;
        mesh.addFacet(MeshCore::MeshGeomFacet(
            Base::Vector3f(0, 0, 0), Base::Vector3f(1, 0, 0), Base::Vector3f(0, 1, 0)));
        feature.Mesh.setValue(mesh);
        feature.changes = 0;
        feature.errorOnEntry = false;
    }
    void SetUp() override
    {
        resetMesh();
        binding = feature.Mesh.getPyObject();
        ASSERT_NE(binding, nullptr);
    }
    void TearDown() override
    {
        feature.failure = ThrowingFeature::Failure::none;
        Py_XDECREF(binding);
        PyErr_Clear();
    }
    PyObject* call(const char* method)
    {
        if (std::string(method) == "fillupHoles") {
            return PyObject_CallMethod(binding, method, "k", 0UL);
        }
        return PyObject_CallMethod(binding, method, nullptr);
    }
    float normalZ() const
    {
        return feature.Mesh.getValue().getKernel().GetFacet(0).GetNormal().z;
    }
    // Covers fixture construction, all Python calls, and feature destruction.
    Base::PyGILStateLocker gil;
    ThrowingFeature feature;
    PyObject* binding {nullptr};
};

TEST_F(MeshFinalizeTest, SuccessfulMutationAndNotification)
{
    const float before = normalZ();
    PyObject* result = call("flipNormals");
    ASSERT_NE(result, nullptr);
    Py_DECREF(result);
    EXPECT_FLOAT_EQ(normalZ(), -before);
    EXPECT_EQ(feature.changes, 1);
    EXPECT_FALSE(PyErr_Occurred());
}

TEST_F(MeshFinalizeTest, EveryHandlerPropagatesCompletionFailureOnce)
{
    const char* methods[] = {"flipNormals", "harmonizeNormals", "fillupHoles",
                             "optimizeTopology", "optimizeEdges", "smooth"};
    const ThrowingFeature::Failure failures[] = {ThrowingFeature::Failure::base,
                                                ThrowingFeature::Failure::allocation,
                                                ThrowingFeature::Failure::python};
    for (auto method : methods) {
        for (auto failure : failures) {
            SCOPED_TRACE(method);
            resetMesh();
            feature.failure = failure;
            PyObject* result = call(method);
            EXPECT_EQ(result, nullptr);
            Py_XDECREF(result);
            if (failure == ThrowingFeature::Failure::allocation) {
                EXPECT_TRUE(PyErr_ExceptionMatches(Base::PyExc_FC_GeneralError));
            }
            else {
                EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
            }
            PyErr_Clear();
            EXPECT_EQ(feature.changes, 1); // A partially delivered notification is not retried.
        }
    }
}

TEST_F(MeshFinalizeTest, ThrowingSignalLeavesMutationAndDoesNotRetry)
{
    int deliveries = 0;
    auto connection = feature.Mesh.signalChanged.connect([&](const App::Property&) {
        ++deliveries;
        throw std::bad_alloc();
    });
    const float before = normalZ();
    PyObject* result = call("flipNormals");
    bool error = PyErr_Occurred();
    PyErr_Clear();
    connection.disconnect();
    EXPECT_EQ(result, nullptr);
    Py_XDECREF(result);
    EXPECT_TRUE(error);
    EXPECT_FLOAT_EQ(normalZ(), -before);
    EXPECT_EQ(feature.changes, 1);
    EXPECT_EQ(deliveries, 1);
}

TEST_F(MeshFinalizeTest, BodyFailureStillNotifiesAndPreservesPythonError)
{
    PyObject* result = PyObject_CallMethod(binding, "smooth", "s", "unknown");
    EXPECT_EQ(result, nullptr);
    Py_XDECREF(result);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    EXPECT_EQ(feature.changes, 1);
    EXPECT_FALSE(feature.errorOnEntry);
}

TEST_F(MeshFinalizeTest, BodyAndNotificationFailureReportPartialOutcome)
{
    feature.failure = ThrowingFeature::Failure::allocation;
    PyObject* result = PyObject_CallMethod(binding, "smooth", "s", "unknown");
    EXPECT_EQ(result, nullptr);
    Py_XDECREF(result);
    ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    using Ref = std::unique_ptr<PyObject, decltype(&Py_DecRef)>;
    Ref typeRef(type, Py_DecRef), valueRef(value, Py_DecRef), tracebackRef(traceback, Py_DecRef);
    ASSERT_NE(value, nullptr);
    Ref text(PyObject_Str(value), Py_DecRef);
    ASSERT_NE(text, nullptr);
    const char* message = PyUnicode_AsUTF8(text.get());
    ASSERT_NE(message, nullptr);
    EXPECT_NE(std::string(message).find("partial changes or notifications may remain"),
              std::string::npos);
    Ref finish(PyException_GetCause(value), Py_DecRef);
    ASSERT_NE(finish, nullptr);
    Ref finishText(PyObject_Str(finish.get()), Py_DecRef);
    ASSERT_NE(finishText, nullptr);
    ASSERT_GT(PyUnicode_GetLength(finishText.get()), 0);
    Ref body(PyException_GetCause(finish.get()), Py_DecRef);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(PyObject_IsInstance(body.get(), PyExc_ValueError), 1);
    Ref bodyText(PyObject_Str(body.get()), Py_DecRef);
    ASSERT_NE(bodyText, nullptr);
    const char* bodyMessage = PyUnicode_AsUTF8(bodyText.get());
    ASSERT_NE(bodyMessage, nullptr);
    EXPECT_STREQ(bodyMessage, "No such smoothing algorithm");
    EXPECT_EQ(feature.changes, 1);
}

TEST_F(MeshFinalizeTest, PythonNotificationFailureRetainsBothCauses)
{
    feature.failure = ThrowingFeature::Failure::python;
    PyObject* result = PyObject_CallMethod(binding, "smooth", "s", "unknown");
    EXPECT_EQ(result, nullptr);
    Py_XDECREF(result);
    ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyObject *type = nullptr, *value = nullptr, *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    using Ref = std::unique_ptr<PyObject, decltype(&Py_DecRef)>;
    Ref typeRef(type, Py_DecRef), valueRef(value, Py_DecRef), tracebackRef(traceback, Py_DecRef);
    ASSERT_NE(value, nullptr);
    Ref finish(PyException_GetCause(value), Py_DecRef);
    ASSERT_NE(finish, nullptr);
    EXPECT_EQ(PyObject_IsInstance(finish.get(), PyExc_ValueError), 1);
    Ref finishText(PyObject_Str(finish.get()), Py_DecRef);
    ASSERT_NE(finishText, nullptr);
    const char* finishMessage = PyUnicode_AsUTF8(finishText.get());
    ASSERT_NE(finishMessage, nullptr);
    EXPECT_STREQ(finishMessage, "mesh observer failure");
    Ref body(PyException_GetCause(finish.get()), Py_DecRef);
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(PyObject_IsInstance(body.get(), PyExc_ValueError), 1);
    Ref bodyText(PyObject_Str(body.get()), Py_DecRef);
    ASSERT_NE(bodyText, nullptr);
    const char* bodyMessage = PyUnicode_AsUTF8(bodyText.get());
    ASSERT_NE(bodyMessage, nullptr);
    EXPECT_STREQ(bodyMessage, "No such smoothing algorithm");
    EXPECT_EQ(feature.changes, 1);
    EXPECT_FALSE(feature.errorOnEntry);
}


TEST_F(MeshFinalizeTest, AttachedDocumentObserverCannotConsumeBodyError)
{
    // Exercise the real registered Python observer path, which catches and
    // reports its own exception from inside finishEditing(). The finally block
    // unregisters before closing even if an assertion fails.
    const char* script = R"PY(
import FreeCAD as App
class MeshFinalizeObserver:
    def __init__(self, target):
        self.target = target
        self.calls = 0
    def slotChangedObject(self, obj, prop):
        if obj is self.target and prop == 'Mesh':
            self.calls += 1
            raise RuntimeError('observer intentionally failed')
doc = App.newDocument('MeshFinalizeObserverTest')
observer = None
registered = False
try:
    obj = doc.addObject('Mesh::Feature', 'Mesh')
    mesh = obj.Mesh
    observer = MeshFinalizeObserver(obj)
    App.addDocumentObserver(observer)
    registered = True
    try:
        mesh.smooth('unknown')
        raise AssertionError('smooth unexpectedly succeeded')
    except ValueError as error:
        assert str(error) == 'No such smoothing algorithm', repr(error)
    assert observer.calls == 1, observer.calls
finally:
    try:
        if registered:
            App.removeDocumentObserver(observer)
    finally:
        App.closeDocument(doc.Name)
)PY";
    EXPECT_EQ(PyRun_SimpleString(script), 0);
}

} // namespace
