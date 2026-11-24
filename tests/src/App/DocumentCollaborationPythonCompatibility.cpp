// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Stream.h>

#include "App/Application.h"
#include "App/AutoTransaction.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentExecutionLane.h"
#include "App/DocumentObserverPython.h"
#include "App/DocumentPy.h"
#include "App/DocumentRevisionIndex.h"
#include "App/FeatureTest.h"
#include "App/MainThreadSignal.h"
#include "App/MergeDocuments.h"
#include "App/MutationClassification.h"
#include "App/PropertyLinks.h"
#include "App/PropertyStandard.h"
#include "App/RecoverySnapshot.h"
#include "App/Transactions.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

#if defined(FREECAD_PERSISTENCE_TEST_API)
namespace Base::Internal
{
BaseExport void setPersistenceContentByteArrayFactoryForTest(PyObject* (*)(Py_ssize_t));
BaseExport void setPersistenceContentBeforeReadForTest(void (*)(PyObject*, std::istream&));
BaseExport void setPersistenceContentBufferAcquireForTest(int (*)(PyObject*, Py_buffer*, int));
}
#endif

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace App::Internal
{
AppExport void setPropertyContentByteArrayFactoryForTest(PyObject* (*)(Py_ssize_t));
AppExport void setPropertyContentBeforeReadForTest(void (*)(PyObject*, std::istream&));
AppExport void setPropertyContentBufferAcquireForTest(int (*)(PyObject*, Py_buffer*, int));
}
#endif

// Document::addObject loads the module named by the first namespace component.
// App is built in; a test-only top-level namespace would trigger a failed import.
namespace App::ConversionBoundaryTest
{
class InheritedList: public App::PropertyIntegerList
{
public:
    int calls = 0;
protected:
    void setPyValues(const std::vector<PyObject*>& values, const std::vector<int>& indices) override
    {
        ++calls;
        App::PropertyIntegerList::setPyValues(values, indices);
    }
};
// An unregistered subclass can deliberately give indexed assignment a different contract.
class LinkListOverride: public App::PropertyLinkList
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();
public:
    int calls = 0;
    void set1Value(int index, App::DocumentObject* const& value) override
    {
        ++calls;
        if (value) App::PropertyLinkList::set1Value(index, value);
    }
};
class RegisteredList: public App::PropertyIntegerList
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();
public:
    int calls = 0;
protected:
    void setPyValues(const std::vector<PyObject*>& values, const std::vector<int>& indices) override
    {
        ++calls;
        App::PropertyIntegerList::setPyValues(values, indices);
    }
};
// Reject a scalar during scalar interpretation, then accept it via the virtual list hook.
template<class Parent> class ColorHook: public Parent
{
public:
    int calls = 0;
protected:
    Base::Color getPyValue(PyObject* value) const override
    {
        if (PyLong_Check(value)) {
            PyErr_SetString(PyExc_TypeError, "probe scalar fallback");
            throw Base::TypeError("probe scalar fallback");
        }
        return Parent::getPyValue(value);
    }
    void setPyValues(const std::vector<PyObject*>& values, const std::vector<int>& indices) override
    {
        ++calls;
        if (indices.empty() && values.size() == 1 && PyLong_Check(values[0])) {
            this->setValue(Base::Color(0.25F, 0.5F, 0.75F, 1.0F));
            return;
        }
        Parent::setPyValues(values, indices);
    }
};
using InheritedColor = ColorHook<App::PropertyColorList>;
class RegisteredColor: public ColorHook<App::PropertyColorList>
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();
};
class Feature: public App::FeatureTest
{
    PROPERTY_HEADER_WITH_OVERRIDE(App::ConversionBoundaryTest::Feature);
public:
    Feature()
    {
        ADD_PROPERTY(Inherited, (1L));
        ADD_PROPERTY(Registered, (1L));
        ADD_PROPERTY(InheritedColour, (Base::Color()));
        ADD_PROPERTY(RegisteredColour, (Base::Color()));
    }
    InheritedList Inherited;
    RegisteredList Registered;
    InheritedColor InheritedColour;
    RegisteredColor RegisteredColour;
};
}
TYPESYSTEM_SOURCE(App::ConversionBoundaryTest::LinkListOverride, App::PropertyLinkList)
TYPESYSTEM_SOURCE(App::ConversionBoundaryTest::RegisteredList, App::PropertyIntegerList)
TYPESYSTEM_SOURCE(App::ConversionBoundaryTest::RegisteredColor, App::PropertyColorList)
PROPERTY_SOURCE(App::ConversionBoundaryTest::Feature, App::FeatureTest)

namespace
{

class SchemaAddingOnUnsetupFeature final: public App::FeatureTest
{
public:
    void unsetupObject() override
    {
        attempted = true;
        try {
            admitted = addDynamicProperty(
                           "App::PropertyString", "UnsetupCompatibilityProperty")
                != nullptr;
        }
        catch (const Base::Exception&) {
            rejected = true;
        }
        App::FeatureTest::unsetupObject();
    }

    bool attempted {false};
    bool admitted {false};
    bool rejected {false};
};

class StatusChangingOnUnsetupFeature final: public App::FeatureTest
{
public:
    void unsetupObject() override
    {
        attempted = true;
        try {
            String.setStatus(App::Property::ReadOnly, true);
            admitted = true;
        }
        catch (const Base::Exception&) {
            rejected = true;
        }
        App::FeatureTest::unsetupObject();
    }

    bool attempted {false};
    bool admitted {false};
    bool rejected {false};
};

class StructureAddingOnExecuteFeature final: public App::FeatureTest
{
public:
    App::DocumentObjectExecReturn* execute() override
    {
        ++executeCalls;
        if (!attemptStructure) {
            return App::DocumentObject::StdReturn;
        }
        try {
            admitted = getDocument()
                && getDocument()->addObject<App::FeatureTest>("ExecuteBorrowedStructure");
        }
        catch (const Base::Exception&) {
            rejected = true;
        }
        return App::DocumentObject::StdReturn;
    }

    int executeCalls {0};
    bool attemptStructure {false};
    bool admitted {false};
    bool rejected {false};
};

class LazyStructuralExecuteFeature final: public App::FeatureTest
{
public:
    static inline int TotalExecuteCalls {0};

    App::DocumentObjectExecReturn* execute() override
    {
        ++TotalExecuteCalls;
        ++executeCalls;
        if (addOwnSchema && !getPropertyByName("LazyExecuteProperty")) {
            addDynamicProperty("App::PropertyString", "LazyExecuteProperty");
        }
        if (changeEditorStatus && editorTarget) {
            auto* property = editorTarget->getPropertyByName("ExistingMetadataProperty");
            try {
                if (!property) {
                    throw std::runtime_error("missing editor-status target property");
                }
                property->setStatus(App::Property::ReadOnly, true);
                editorStatusChanged = true;
            }
            catch (const Base::Exception&) {
                editorStatusRejected = true;
            }
        }
        if (attemptRestrictedMutations && editorTarget) {
            try {
                static_cast<void>(editorTarget->addDynamicProperty(
                    "App::PropertyString", "ForbiddenExistingSchema"));
            }
            catch (const Base::Exception&) {
                existingSchemaRejected = true;
            }
            try {
                static_cast<void>(getDocument()->addObject<App::FeatureTest>(
                    "ForbiddenExecuteObject"));
            }
            catch (const Base::Exception&) {
                objectAddRejected = true;
            }
            try {
                getDocument()->removeObject(editorTarget->getNameInDocument());
            }
            catch (const Base::Exception&) {
                objectRemoveRejected = true;
            }
            try {
                static_cast<void>(getDocument()->openTransaction("forbidden execute transaction"));
            }
            catch (const Base::Exception&) {
                transactionControlRejected = true;
            }
            try {
                static_cast<void>(getDocument()->undo());
            }
            catch (const Base::Exception&) {
                historyControlRejected = true;
            }
        }
        return App::DocumentObject::StdReturn;
    }

    App::FeatureTest* editorTarget {nullptr};
    int executeCalls {0};
    bool addOwnSchema {false};
    bool changeEditorStatus {false};
    bool attemptRestrictedMutations {false};
    bool editorStatusChanged {false};
    bool editorStatusRejected {false};
    bool existingSchemaRejected {false};
    bool objectAddRejected {false};
    bool objectRemoveRejected {false};
    bool transactionControlRejected {false};
    bool historyControlRejected {false};
};

class RawStatusFeature final: public App::FeatureTest
{
public:
    void setNoTouchWithoutMutationGuard(const bool enabled)
    {
        StatusBits.set(App::ObjectStatus::NoTouch, enabled);
    }
};

class DetachedTransactionFeature final: public App::FeatureTest
{
public:
    const char* detachFromDocument() override
    {
        return "Detached";
    }
};

class ScopedTemporaryPath final
{
public:
    explicit ScopedTemporaryPath(const std::string& stem)
        : path(std::filesystem::temp_directory_path()
               / (stem + "_"
                  + std::to_string(
                      std::chrono::steady_clock::now().time_since_epoch().count())
                  + ".FCStd"))
    {}

    ~ScopedTemporaryPath()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::filesystem::path path;
};

std::string readFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read save-boundary test file");
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

class ScopedDocumentClose final
{
public:
    explicit ScopedDocumentClose(std::string name)
        : _name(std::move(name))
    {}

    ~ScopedDocumentClose()
    {
        try {
            if (App::GetApplication().getDocument(_name.c_str())) {
                static_cast<void>(App::GetApplication().closeDocument(_name.c_str()));
            }
        }
        catch (...) {
        }
    }

private:
    std::string _name;
};

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

// Declared after the GIL locker and before Python references: fatal assertions must
// not leave a pending exception on the thread state used by later tests.
struct ScopedPythonErrorClear final
{
    ~ScopedPythonErrorClear() { PyErr_Clear(); }
};

class PythonDocumentObserverGuard final
{
public:
    explicit PythonDocumentObserverGuard(PyObject* observer)
        : _observer(observer)
    {
        App::DocumentObserverPython::addObserver(_observer);
    }

    ~PythonDocumentObserverGuard()
    {
        App::DocumentObserverPython::removeObserver(_observer);
    }

    PythonDocumentObserverGuard(const PythonDocumentObserverGuard&) = delete;
    PythonDocumentObserverGuard& operator=(const PythonDocumentObserverGuard&) = delete;

private:
    Py::Object _observer;
};

struct PythonErrorInfo
{
    bool present {false};
    bool matchesExpected {false};
    std::string type;
    std::string message;
};

PythonErrorInfo takePythonError(PyObject* expectedType = nullptr)
{
    PythonErrorInfo info;
    info.present = PyErr_Occurred() != nullptr;
    info.matchesExpected = info.present && expectedType
        && PyErr_ExceptionMatches(expectedType);
    if (!info.present) {
        info.message = "<no Python exception>";
        return info;
    }

    PyObject* errorType = nullptr;
    PyObject* errorValue = nullptr;
    PyObject* errorTraceback = nullptr;
    PyErr_Fetch(&errorType, &errorValue, &errorTraceback);
    PyErr_NormalizeException(&errorType, &errorValue, &errorTraceback);

    if (errorValue) {
        if (const char* typeName = Py_TYPE(errorValue)->tp_name) {
            info.type = typeName;
        }
    }
    else if (errorType && PyType_Check(errorType)) {
        if (const char* typeName = reinterpret_cast<PyTypeObject*>(errorType)->tp_name) {
            info.type = typeName;
        }
    }

    PyObject* displayObject = errorValue ? errorValue : errorType;
    if (displayObject) {
        PyObjectRef errorText(PyObject_Str(displayObject));
        if (errorText.get()) {
            if (const char* utf8 = PyUnicode_AsUTF8(errorText.get())) {
                info.message = utf8;
            }
        }
    }
    if (info.message.empty()) {
        info.message = "<unable to format Python exception>";
    }

    Py_XDECREF(errorType);
    Py_XDECREF(errorValue);
    Py_XDECREF(errorTraceback);
    // Formatting an exception can itself fail. Never let a test assertion or
    // fixture teardown run with a stale Python exception pending.
    PyErr_Clear();
    return info;
}

::testing::AssertionResult pythonObjectAvailable(PyObject* object)
{
    if (object) {
        return ::testing::AssertionSuccess();
    }
    const auto error = takePythonError();
    return ::testing::AssertionFailure()
        << (error.type.empty() ? "Python call failed" : error.type) << ": "
        << error.message;
}

bool addGroupExtension(App::DocumentObject& object)
{
    PyObjectRef pythonObject(object.getPyObject());
    if (!pythonObject.get()) {
        return false;
    }
    PyObjectRef result(PyObject_CallMethod(
        pythonObject.get(), "addExtension", "s", "App::GroupExtensionPython"));
    return result.get() != nullptr;
}

bool setReadOnlyPropertyStatus(App::DocumentObject& object, const char* propertyName)
{
    PyObjectRef pythonObject(object.getPyObject());
    if (!pythonObject.get()) {
        return false;
    }
    PyObjectRef result(PyObject_CallMethod(
        pythonObject.get(), "setPropertyStatus", "ss", propertyName, "ReadOnly"));
    return result.get() != nullptr;
}

bool changePropertyMetadata(App::DocumentObject& object, const char* propertyName)
{
    PyObjectRef pythonObject(object.getPyObject());
    if (!pythonObject.get()) {
        return false;
    }
    PyObjectRef groupResult(PyObject_CallMethod(
        pythonObject.get(),
        "setGroupOfProperty",
        "ss",
        propertyName,
        "Compatibility Group"));
    if (!groupResult.get()) {
        return false;
    }
    PyObjectRef documentationResult(PyObject_CallMethod(
        pythonObject.get(),
        "setDocumentationOfProperty",
        "ss",
        propertyName,
        "Compatibility documentation"));
    return documentationResult.get() != nullptr;
}

bool renameDynamicProperty(
    App::DocumentObject& object,
    const char* oldName,
    const char* newName)
{
    PyObjectRef pythonObject(object.getPyObject());
    if (!pythonObject.get()) {
        return false;
    }
    PyObjectRef result(PyObject_CallMethod(
        pythonObject.get(), "renameProperty", "ss", oldName, newName));
    return result.get() != nullptr;
}

bool removeDynamicProperty(App::DocumentObject& object, const char* propertyName)
{
    PyObjectRef pythonObject(object.getPyObject());
    if (!pythonObject.get()) {
        return false;
    }
    PyObjectRef result(PyObject_CallMethod(
        pythonObject.get(), "removeProperty", "s", propertyName));
    return result.get() && PyObject_IsTrue(result.get()) == 1;
}

enum class SaveAttempt
{
    None,
    LegacyCanonical,
    OutcomeCanonical,
    ForceCanonical,
    LegacySaveAs,
    PolicySaveAs,
    OutcomeSaveAs,
    LegacyCopy,
    OutcomeCopy,
    RecoverySnapshot,
    RecoveryOutcomeReport
};

void performSaveAttempt(App::Document& document,
                        const SaveAttempt attempt,
                        const std::string& saveAsPath,
                        const std::string& copyPath)
{
    switch (attempt) {
        case SaveAttempt::None:
            return;
        case SaveAttempt::LegacyCanonical:
            static_cast<void>(document.save());
            return;
        case SaveAttempt::OutcomeCanonical:
            static_cast<void>(document.saveWithOutcome());
            return;
        case SaveAttempt::ForceCanonical:
            static_cast<void>(document.forceSave());
            return;
        case SaveAttempt::LegacySaveAs:
            static_cast<void>(document.saveAs(saveAsPath.c_str()));
            return;
        case SaveAttempt::PolicySaveAs:
            static_cast<void>(document.saveAsWithPolicy(saveAsPath.c_str(), true));
            return;
        case SaveAttempt::OutcomeSaveAs:
            static_cast<void>(document.saveAsWithOutcome(saveAsPath.c_str(), true));
            return;
        case SaveAttempt::LegacyCopy:
            static_cast<void>(document.saveCopy(copyPath.c_str()));
            return;
        case SaveAttempt::OutcomeCopy:
            static_cast<void>(document.saveCopyWithOutcome(copyPath.c_str()));
            return;
        case SaveAttempt::RecoverySnapshot:
            static_cast<void>(App::writeRecoverySnapshotToTransientDir(
                document, App::RecoverySnapshotSaveOptions {}));
            return;
        case SaveAttempt::RecoveryOutcomeReport:
            document.reportRecoverySaveOutcome(copyPath, true);
            return;
    }
}

void expectNoRecoveryArtifacts(const App::Document& document)
{
    const auto directory = std::filesystem::path(document.TransientDir.getStrValue());
    constexpr std::array names {
        "fc_recovery_file.xml",
        "fc_recovery_file.xml.tmp",
        "fc_recovery_file.fcstd",
        "fc_recovery_file.fcstd.tmp",
        "fc_recovery_files",
        "fc_recovery_files.tmp",
    };
    for (const auto* name : names) {
        EXPECT_FALSE(std::filesystem::exists(directory / name)) << name;
    }
}

struct DirectRevisionMutationProbe
{
    App::Document* targetDocument {nullptr};
    App::Document* foreignDocument {nullptr};
    App::DocumentRevisionPublicationReservation* foreignReservation {nullptr};
    bool targetPublishRejected {false};
    bool targetReserveRejected {false};
    bool targetBindRejected {false};
    bool foreignPublishRejected {false};
    bool foreignReserveRejected {false};
    bool foreignBindRejected {false};
    bool legacyNoIndexAdmissionRejected {false};
    bool foreignCancelCompleted {false};
};

void attemptDirectRevisionMutations(DirectRevisionMutationProbe& probe)
{
    try {
        App::enforceCollaborationRevisionMutationAllowed();
    }
    catch (const Base::Exception&) {
        probe.legacyNoIndexAdmissionRejected = true;
    }
    const auto attempt = [](App::Document& document,
                            bool& publishRejected,
                            bool& reserveRejected,
                            bool& bindRejected) {
        try {
            static_cast<void>(document.collaborationRevisions()
                                  .publishUnknownModelMutation());
        }
        catch (const Base::Exception&) {
            publishRejected = true;
        }
        try {
            auto reservation = document.collaborationRevisions().reservePublication({}, {});
            static_cast<void>(reservation.ready());
        }
        catch (const Base::Exception&) {
            reserveRejected = true;
        }
        try {
            const auto identity = document.collaborationIdentity();
            document.collaborationRevisions().bindDocumentIdentity(
                identity.instanceId, identity.lifecycleEpoch);
        }
        catch (const Base::Exception&) {
            bindRejected = true;
        }
    };

    if (probe.targetDocument) {
        attempt(*probe.targetDocument,
                probe.targetPublishRejected,
                probe.targetReserveRejected,
                probe.targetBindRejected);
    }
    if (probe.foreignDocument) {
        attempt(*probe.foreignDocument,
                probe.foreignPublishRejected,
                probe.foreignReserveRejected,
                probe.foreignBindRejected);
    }
    if (probe.foreignReservation) {
        probe.foreignReservation->cancel();
        probe.foreignCancelCompleted = !probe.foreignReservation->ready();
    }
}

void expectAllDirectRevisionMutationsRejected(
    const DirectRevisionMutationProbe& probe)
{
    EXPECT_TRUE(probe.targetPublishRejected);
    EXPECT_TRUE(probe.targetReserveRejected);
    EXPECT_TRUE(probe.targetBindRejected);
    EXPECT_TRUE(probe.foreignPublishRejected);
    EXPECT_TRUE(probe.foreignReserveRejected);
    EXPECT_TRUE(probe.foreignBindRejected);
    EXPECT_TRUE(probe.legacyNoIndexAdmissionRejected);
}

struct CallbackProbe
{
    App::Document* nativeDocument {nullptr};
    App::FeatureTest* target {nullptr};
    App::FeatureTest* foreignTarget {nullptr};
    PyObject* document {nullptr};
    int calls {0};
    int nestedCalls {0};
    bool gilHeld {false};
    bool raisePythonError {false};
    bool reenter {false};
    bool addTransient {false};
    bool addLazyStructuralFeature {false};
    bool mutateForeignTarget {false};
    bool lazyAddOwnSchema {false};
    bool lazyChangeEditorStatus {false};
    bool lazyAttemptRestrictedMutations {false};
    bool changeTransientStaticProperty {false};
    bool addDynamicProperty {false};
    bool addDynamicPropertyToTransient {false};
    bool setPropertyStatusOnTarget {false};
    bool setPropertyStatusOnTransient {false};
    bool setPropertyStatusOnImported {false};
    bool changePropertyMetadataOnTarget {false};
    bool changePropertyMetadataOnTransient {false};
    bool changePropertyMetadataOnImported {false};
    bool editDynamicPropertySchemaOnTarget {false};
    bool editDynamicPropertySchemaOnTransient {false};
    bool editDynamicPropertySchemaOnImported {false};
    bool addExtensionToTarget {false};
    bool addExtensionToTransient {false};
    bool addExtensionToImported {false};
    bool addReplacementTarget {false};
    bool removeTarget {false};
    bool removeTransient {false};
    bool clearDocument {false};
    bool openTransaction {false};
    bool undo {false};
    bool redo {false};
    bool transactionLocker {false};
    bool touchTarget {false};
    bool recomputeDocument {false};
    bool recomputeTarget {false};
    bool changeTargetLabel {false};
    bool markAppearanceFileChange {false};
    bool attemptThreadedMutations {false};
    bool threadedTargetMutationRejected {false};
    bool threadedForeignMutationRejected {false};
    bool threadedLifecycleMutationRejected {false};
    bool attemptHostileBoundaryRelease {false};
    bool hostileForeignMutationRejected {false};
    bool hostileLifecycleMutationRejected {false};
    bool hostileRevisionMutationRejected {false};
    DirectRevisionMutationProbe* revisionMutationProbe {nullptr};
    SaveAttempt saveAttempt {SaveAttempt::None};
    std::string saveAsPath;
    std::string copyPath;
    std::string threadedLifecycleDocumentName;
    int* executeCalls {nullptr};
    int executeCallsObservedAfterCallbackRecompute {-1};
    const std::string* importArchive {nullptr};
    Base::XMLReader* directImportReader {nullptr};
    App::MergeDocuments* retainedImporter {nullptr};
    int* importObserverCalls {nullptr};
    int* finishImportObserverCalls {nullptr};
    int* finishRestoreObserverCalls {nullptr};
    int* newObjectObserverCalls {nullptr};
    int* dynamicPropertyObserverCalls {nullptr};
    int* beforeExtensionObserverCalls {nullptr};
    int* addedExtensionObserverCalls {nullptr};
    bool importObserversDeferred {false};
    bool importedDynamicPropertyRestored {false};
    bool extensionObserversDeferred {false};
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

    try {
        if (probe->attemptHostileBoundaryRelease && probe->nativeDocument) {
            // ABI-preserved public teardown surfaces do not own the prepared
            // coordinator target/audit and therefore cannot release it.
            App::endAtomicPresentationMutationTarget(*probe->nativeDocument);
            App::endCollaborationReadOnlyMutationTarget(*probe->nativeDocument);
            probe->nativeDocument->endCollaborationAtomicPresentationAudit();
            try {
                if (!probe->foreignTarget) {
                    throw std::runtime_error("missing hostile foreign target");
                }
                probe->foreignTarget->Label.setValue("Forbidden after hostile end");
            }
            catch (const Base::Exception&) {
                probe->hostileForeignMutationRejected = true;
            }
            try {
                static_cast<void>(App::GetApplication().newDocument(
                    probe->threadedLifecycleDocumentName.c_str()));
            }
            catch (const Base::Exception&) {
                probe->hostileLifecycleMutationRejected = true;
            }
            try {
                static_cast<void>(probe->nativeDocument->collaborationRevisions()
                                      .publishUnknownModelMutation());
            }
            catch (const Base::Exception&) {
                probe->hostileRevisionMutationRejected = true;
            }
        }
        if (probe->attemptThreadedMutations) {
            std::thread worker([probe] {
                try {
                    if (!probe->target) {
                        throw std::runtime_error("missing threaded target");
                    }
                    probe->target->Label.setValue("Forbidden threaded target label");
                }
                catch (const Base::Exception&) {
                    probe->threadedTargetMutationRejected = true;
                }
                catch (...) {
                }
                try {
                    if (!probe->foreignTarget) {
                        throw std::runtime_error("missing threaded foreign target");
                    }
                    probe->foreignTarget->Label.setValue(
                        "Forbidden threaded foreign label");
                }
                catch (const Base::Exception&) {
                    probe->threadedForeignMutationRejected = true;
                }
                catch (...) {
                }
                try {
                    static_cast<void>(App::GetApplication().newDocument(
                        probe->threadedLifecycleDocumentName.c_str()));
                }
                catch (const Base::Exception&) {
                    probe->threadedLifecycleMutationRejected = true;
                }
                catch (...) {
                }
            });
            worker.join();
        }
        if (probe->mutateForeignTarget) {
            if (!probe->foreignTarget) {
                throw std::runtime_error("missing foreign mutation target");
            }
            probe->foreignTarget->Label.setValue("Forbidden foreign callback label");
        }
        if (probe->revisionMutationProbe) {
            attemptDirectRevisionMutations(*probe->revisionMutationProbe);
        }
        if (probe->addTransient) {
            if (!probe->nativeDocument
                || !probe->nativeDocument->addObject<App::FeatureTest>("Transient")) {
                throw std::runtime_error("failed to add transient object");
            }
        }
        if (probe->addLazyStructuralFeature) {
            if (!probe->nativeDocument) {
                throw std::runtime_error("missing document for lazy structural feature");
            }
            auto feature = std::make_unique<LazyStructuralExecuteFeature>();
            feature->editorTarget = probe->target;
            feature->addOwnSchema = probe->lazyAddOwnSchema;
            feature->changeEditorStatus = probe->lazyChangeEditorStatus;
            feature->attemptRestrictedMutations =
                probe->lazyAttemptRestrictedMutations;
            probe->nativeDocument->addObject(feature.get(), "LazyStructuralFeature");
            feature->touch();
            static_cast<void>(feature.release());
        }
        if (probe->addDynamicProperty) {
            if (!probe->target
                || !probe->target->addDynamicProperty(
                    "App::PropertyString", "CompatibilityProperty")) {
                throw std::runtime_error("failed to add compatibility property");
            }
        }
        if (probe->addDynamicPropertyToTransient) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient
                || !transient->addDynamicProperty(
                    "App::PropertyString", "PostSetupCompatibilityProperty")) {
                throw std::runtime_error("failed to add post-setup compatibility property");
            }
        }
        if (probe->changeTransientStaticProperty) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient) {
                throw std::runtime_error("missing transient object for static property change");
            }
            transient->Label.setValue("Transient compatibility label");
        }
        if (probe->setPropertyStatusOnTarget
            && (!probe->target
                || !setReadOnlyPropertyStatus(*probe->target, "ExistingMetadataProperty"))) {
            if (PyErr_Occurred()) {
                return nullptr;
            }
            throw std::runtime_error("failed to change target property status");
        }
        if (probe->setPropertyStatusOnTransient) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient
                || !setReadOnlyPropertyStatus(
                    *transient, "PostSetupCompatibilityProperty")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to change transient property status");
            }
        }
        if (probe->changePropertyMetadataOnTarget
            && (!probe->target
                || !changePropertyMetadata(*probe->target, "ExistingMetadataProperty"))) {
            if (PyErr_Occurred()) {
                return nullptr;
            }
            throw std::runtime_error("failed to change target property metadata");
        }
        if (probe->changePropertyMetadataOnTransient) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient
                || !changePropertyMetadata(
                    *transient, "PostSetupCompatibilityProperty")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to change transient property metadata");
            }
        }
        if (probe->editDynamicPropertySchemaOnTarget) {
            if (!probe->target
                || !renameDynamicProperty(
                    *probe->target,
                    "ExistingMetadataProperty",
                    "RenamedMetadataProperty")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to rename target dynamic property");
            }
            if (!removeDynamicProperty(*probe->target, "RenamedMetadataProperty")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to remove target dynamic property");
            }
        }
        if (probe->editDynamicPropertySchemaOnTransient) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient
                || !transient->addDynamicProperty(
                    "App::PropertyString", "RenameCandidate")
                || !transient->addDynamicProperty(
                    "App::PropertyString", "RemoveCandidate")
                || !renameDynamicProperty(
                    *transient, "RenameCandidate", "RenamedCandidate")
                || !removeDynamicProperty(*transient, "RemoveCandidate")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to edit transient dynamic-property schema");
            }
        }
        if (probe->addExtensionToTarget
            && (!probe->target || !addGroupExtension(*probe->target))) {
            if (PyErr_Occurred()) {
                return nullptr;
            }
            throw std::runtime_error("failed to add extension to target");
        }
        if (probe->addExtensionToTransient) {
            auto* transient = probe->nativeDocument
                ? probe->nativeDocument->getObject("Transient")
                : nullptr;
            if (!transient || !addGroupExtension(*transient)) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to add extension to transient object");
            }
        }
        if (probe->removeTarget) {
            if (!probe->nativeDocument) {
                throw std::runtime_error("missing native document");
            }
            probe->nativeDocument->removeObject("Target");
        }
        if (probe->addReplacementTarget) {
            if (!probe->nativeDocument
                || !probe->nativeDocument->addObject<App::FeatureTest>("Target")) {
                throw std::runtime_error("failed to add replacement target");
            }
        }
        if (probe->removeTransient) {
            if (!probe->nativeDocument) {
                throw std::runtime_error("missing native document");
            }
            probe->nativeDocument->removeObject("Transient");
        }
        if (probe->clearDocument) {
            probe->nativeDocument->clearDocument();
        }
        if (probe->openTransaction) {
            static_cast<void>(probe->nativeDocument->openTransaction("nested"));
        }
        if (probe->undo) {
            static_cast<void>(probe->nativeDocument->undo());
        }
        if (probe->redo) {
            static_cast<void>(probe->nativeDocument->redo());
        }
        if (probe->transactionLocker) {
            App::TransactionLocker transactionLocker(probe->nativeDocument);
        }
        if (probe->touchTarget && probe->target) {
            probe->target->touch();
        }
        if (probe->recomputeDocument) {
            static_cast<void>(probe->nativeDocument->recompute());
            if (probe->recomputeTarget && probe->target) {
                static_cast<void>(probe->target->recomputeFeature(false));
            }
            probe->executeCallsObservedAfterCallbackRecompute = probe->executeCalls
                ? *probe->executeCalls
                : -1;
        }
        if (probe->changeTargetLabel && probe->target) {
            probe->target->Label.setValue("Compatibility callback");
        }
        if (probe->markAppearanceFileChange) {
            probe->nativeDocument->markFileChange(App::DocumentFileChange::Appearance);
        }
        if (probe->saveAttempt != SaveAttempt::None) {
            if (!probe->nativeDocument) {
                throw std::runtime_error("missing document for save attempt");
            }
            performSaveAttempt(*probe->nativeDocument,
                               probe->saveAttempt,
                               probe->saveAsPath,
                               probe->copyPath);
        }
        if (probe->directImportReader) {
            static_cast<void>(
                probe->nativeDocument->importObjects(*probe->directImportReader));
        }
        if (probe->importArchive) {
            Base::StringIStreambuf buffer(*probe->importArchive);
            std::istream input(&buffer);
            std::vector<App::DocumentObject*> imported;
            if (probe->retainedImporter) {
                imported = probe->retainedImporter->importObjects(input);
            }
            else {
                App::MergeDocuments importer(probe->nativeDocument);
                imported = importer.importObjects(input);
            }
            auto* object = probe->nativeDocument->getObject("ImportedFeature");
            auto* property = object
                ? freecad_cast<App::PropertyString*>(
                    object->getPropertyByName("ImportedText"))
                : nullptr;
            probe->importedDynamicPropertyRestored = imported.size() == 1U
                && property && property->getStrValue() == "restored";
            probe->importObserversDeferred = probe->importObserverCalls
                && probe->finishImportObserverCalls && probe->finishRestoreObserverCalls
                && probe->newObjectObserverCalls
                && probe->dynamicPropertyObserverCalls
                && *probe->importObserverCalls == 0
                && *probe->finishImportObserverCalls == 0
                && *probe->finishRestoreObserverCalls == 0
                && *probe->newObjectObserverCalls == 0
                && *probe->dynamicPropertyObserverCalls == 0;
        }
        if (probe->setPropertyStatusOnImported) {
            auto* imported = probe->nativeDocument
                ? probe->nativeDocument->getObject("ImportedFeature")
                : nullptr;
            if (!imported || !setReadOnlyPropertyStatus(*imported, "ImportedText")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to change imported property status");
            }
        }
        if (probe->changePropertyMetadataOnImported) {
            auto* imported = probe->nativeDocument
                ? probe->nativeDocument->getObject("ImportedFeature")
                : nullptr;
            if (!imported || !changePropertyMetadata(*imported, "ImportedText")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to change imported property metadata");
            }
        }
        if (probe->editDynamicPropertySchemaOnImported) {
            auto* imported = probe->nativeDocument
                ? probe->nativeDocument->getObject("ImportedFeature")
                : nullptr;
            if (!imported
                || !imported->addDynamicProperty(
                    "App::PropertyString", "ImportedRenameCandidate")
                || !imported->addDynamicProperty(
                    "App::PropertyString", "ImportedRemoveCandidate")
                || !renameDynamicProperty(
                    *imported,
                    "ImportedRenameCandidate",
                    "ImportedRenamedCandidate")
                || !removeDynamicProperty(*imported, "ImportedRemoveCandidate")) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to edit imported dynamic-property schema");
            }
        }
        if (probe->addExtensionToImported) {
            auto* imported = probe->nativeDocument
                ? probe->nativeDocument->getObject("ImportedFeature")
                : nullptr;
            if (!imported || !addGroupExtension(*imported)) {
                if (PyErr_Occurred()) {
                    return nullptr;
                }
                throw std::runtime_error("failed to add extension to imported object");
            }
        }
        if (probe->addExtensionToTarget || probe->addExtensionToTransient
            || probe->addExtensionToImported) {
            probe->extensionObserversDeferred = probe->beforeExtensionObserverCalls
                && probe->addedExtensionObserverCalls
                && *probe->beforeExtensionObserverCalls == 0
                && *probe->addedExtensionObserverCalls == 0;
        }
        if (!probe->removeTarget && !probe->removeTransient
            && !probe->addDynamicProperty && !probe->clearDocument
            && !probe->addLazyStructuralFeature && !probe->mutateForeignTarget
            && !probe->addDynamicPropertyToTransient
            && !probe->changeTransientStaticProperty
            && !probe->setPropertyStatusOnTarget
            && !probe->setPropertyStatusOnTransient
            && !probe->setPropertyStatusOnImported
            && !probe->changePropertyMetadataOnTarget
            && !probe->changePropertyMetadataOnTransient
            && !probe->changePropertyMetadataOnImported
            && !probe->editDynamicPropertySchemaOnTarget
            && !probe->editDynamicPropertySchemaOnTransient
            && !probe->editDynamicPropertySchemaOnImported
            && !probe->addExtensionToTarget && !probe->addExtensionToTransient
            && !probe->addExtensionToImported
            && !probe->addReplacementTarget
            && !probe->openTransaction && !probe->undo && !probe->redo
            && !probe->recomputeDocument
            && !probe->recomputeTarget
            && !probe->changeTargetLabel
            && !probe->attemptThreadedMutations
            && probe->saveAttempt == SaveAttempt::None
            && !probe->importArchive
            && probe->target) {
            probe->target->Label.setValue("Compatibility callback");
        }
    }
    catch (const Base::Exception& exception) {
        PyErr_SetString(PyExc_RuntimeError, exception.what());
        return nullptr;
    }
    catch (const std::exception& exception) {
        PyErr_SetString(PyExc_RuntimeError, exception.what());
        return nullptr;
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

struct PostconditionProbe
{
    App::Document* document {nullptr};
    App::FeatureTest* target {nullptr};
    App::Document* foreignDocument {nullptr};
    App::FeatureTest* foreignTarget {nullptr};
    RawStatusFeature* rawStatusTarget {nullptr};
    int calls {0};
    bool satisfied {true};
    bool returnFalseyInteger {false};
    bool raisePythonError {false};
    bool requireLazyFeature {false};
    bool attemptRestrictedMutations {false};
    bool attemptOrdinaryValueMutation {false};
    bool attemptTouch {false};
    bool attemptRecompute {false};
    bool attemptForeignMutation {false};
    bool attemptNewDocument {false};
    bool attemptCloseDocument {false};
    bool attemptActiveDocumentChange {false};
    bool attemptRevisionPublication {false};
    DirectRevisionMutationProbe* revisionMutationProbe {nullptr};
    bool bypassNoTouchGuard {false};
    bool attemptThreadedMutations {false};
    bool threadedTargetMutationRejected {false};
    bool threadedForeignMutationRejected {false};
    bool threadedLifecycleMutationRejected {false};
    bool attemptHostileBoundaryRelease {false};
    bool hostileTargetMutationRejected {false};
    bool hostileForeignMutationRejected {false};
    bool hostileLifecycleMutationRejected {false};
    bool hostileRevisionMutationRejected {false};
    SaveAttempt saveAttempt {SaveAttempt::None};
    std::string saveAsPath;
    std::string copyPath;
    std::string threadedLifecycleDocumentName;
    bool saveRejected {false};
    bool existingSchemaRejected {false};
    bool newObjectSchemaRejected {false};
    bool objectAddRejected {false};
    bool objectRemoveRejected {false};
    bool transactionControlRejected {false};
    bool historyControlRejected {false};
    bool ordinaryValueRejected {false};
    bool touchRejected {false};
    bool recomputeRejected {false};
    bool foreignMutationRejected {false};
    bool newDocumentRejected {false};
    bool closeDocumentRejected {false};
    bool activeDocumentChangeRejected {false};
    bool revisionPublicationRejected {false};
};

PyObject* runCompatibilityPostcondition(PyObject* self, PyObject*)
{
    auto* probe = static_cast<PostconditionProbe*>(
        PyCapsule_GetPointer(self, "App.DocumentCompatibilityPostconditionProbe"));
    if (!probe) {
        return nullptr;
    }
    ++probe->calls;
    if (probe->attemptHostileBoundaryRelease && probe->document) {
        App::endAtomicPresentationMutationTarget(*probe->document);
        App::endCollaborationReadOnlyMutationTarget(*probe->document);
        probe->document->endCollaborationAtomicPresentationAudit();
        try {
            if (!probe->target) {
                throw std::runtime_error("missing hostile postcondition target");
            }
            probe->target->Label.setValue("Forbidden after hostile postcondition end");
        }
        catch (const Base::Exception&) {
            probe->hostileTargetMutationRejected = true;
        }
        try {
            if (!probe->foreignTarget) {
                throw std::runtime_error("missing hostile postcondition foreign target");
            }
            probe->foreignTarget->Label.setValue(
                "Forbidden foreign after hostile postcondition end");
        }
        catch (const Base::Exception&) {
            probe->hostileForeignMutationRejected = true;
        }
        try {
            static_cast<void>(App::GetApplication().newDocument(
                probe->threadedLifecycleDocumentName.c_str()));
        }
        catch (const Base::Exception&) {
            probe->hostileLifecycleMutationRejected = true;
        }
        try {
            static_cast<void>(probe->document->collaborationRevisions()
                                  .publishUnknownModelMutation());
        }
        catch (const Base::Exception&) {
            probe->hostileRevisionMutationRejected = true;
        }
    }
    if (probe->requireLazyFeature) {
        auto* object = probe->document
            ? probe->document->getObject("LazyStructuralFeature")
            : nullptr;
        if (!object || !object->isValid()
            || !object->getPropertyByName("LazyExecuteProperty")) {
            PyErr_SetString(PyExc_AssertionError,
                            "lazy structural feature was not valid after recompute");
            return nullptr;
        }
    }
    if (probe->attemptThreadedMutations) {
        std::thread worker([probe] {
            try {
                if (!probe->target) {
                    throw std::runtime_error("missing threaded postcondition target");
                }
                probe->target->Label.setValue(
                    "Forbidden threaded postcondition target label");
            }
            catch (const Base::Exception&) {
                probe->threadedTargetMutationRejected = true;
            }
            catch (...) {
            }
            try {
                if (!probe->foreignTarget) {
                    throw std::runtime_error(
                        "missing threaded postcondition foreign target");
                }
                probe->foreignTarget->Label.setValue(
                    "Forbidden threaded postcondition foreign label");
            }
            catch (const Base::Exception&) {
                probe->threadedForeignMutationRejected = true;
            }
            catch (...) {
            }
            try {
                static_cast<void>(App::GetApplication().newDocument(
                    probe->threadedLifecycleDocumentName.c_str()));
            }
            catch (const Base::Exception&) {
                probe->threadedLifecycleMutationRejected = true;
            }
            catch (...) {
            }
        });
        worker.join();
    }
    if (probe->attemptRestrictedMutations && probe->document && probe->target) {
        try {
            static_cast<void>(probe->target->addDynamicProperty(
                "App::PropertyString", "ForbiddenPostconditionSchema"));
        }
        catch (const Base::Exception&) {
            probe->existingSchemaRejected = true;
        }
        try {
            auto* newObject = probe->document->getObject("LazyStructuralFeature");
            if (!newObject) {
                throw Base::RuntimeError("missing new structural feature");
            }
            static_cast<void>(newObject->addDynamicProperty(
                "App::PropertyString", "ForbiddenPostconditionNewObjectSchema"));
        }
        catch (const Base::Exception&) {
            probe->newObjectSchemaRejected = true;
        }
        try {
            static_cast<void>(probe->document->addObject<App::FeatureTest>(
                "ForbiddenPostconditionObject"));
        }
        catch (const Base::Exception&) {
            probe->objectAddRejected = true;
        }
        try {
            probe->document->removeObject(probe->target->getNameInDocument());
        }
        catch (const Base::Exception&) {
            probe->objectRemoveRejected = true;
        }
        try {
            static_cast<void>(
                probe->document->openTransaction("forbidden postcondition transaction"));
        }
        catch (const Base::Exception&) {
            probe->transactionControlRejected = true;
        }
        try {
            static_cast<void>(probe->document->redo());
        }
        catch (const Base::Exception&) {
            probe->historyControlRejected = true;
        }
    }
    if (probe->attemptOrdinaryValueMutation && probe->target) {
        try {
            probe->target->Label.setValue("Forbidden postcondition label");
        }
        catch (const Base::Exception&) {
            probe->ordinaryValueRejected = true;
        }
    }
    if (probe->attemptTouch && probe->target) {
        try {
            probe->target->touch();
        }
        catch (const Base::Exception&) {
            probe->touchRejected = true;
        }
    }
    if (probe->attemptRecompute && probe->document) {
        try {
            static_cast<void>(probe->document->recompute());
        }
        catch (const Base::Exception&) {
            probe->recomputeRejected = true;
        }
    }
    if (probe->attemptForeignMutation && probe->foreignTarget) {
        try {
            probe->foreignTarget->Label.setValue("Forbidden foreign label");
        }
        catch (const Base::Exception&) {
            probe->foreignMutationRejected = true;
        }
    }
    if (probe->attemptNewDocument) {
        try {
            static_cast<void>(App::GetApplication().newDocument(
                "ForbiddenPostconditionDocument"));
        }
        catch (const Base::Exception&) {
            probe->newDocumentRejected = true;
        }
    }
    if (probe->attemptCloseDocument && probe->foreignDocument) {
        try {
            static_cast<void>(
                App::GetApplication().closeDocument(probe->foreignDocument));
        }
        catch (const Base::Exception&) {
            probe->closeDocumentRejected = true;
        }
    }
    if (probe->attemptActiveDocumentChange && probe->foreignDocument) {
        try {
            App::GetApplication().setActiveDocument(probe->foreignDocument);
        }
        catch (const Base::Exception&) {
            probe->activeDocumentChangeRejected = true;
        }
    }
    if (probe->attemptRevisionPublication && probe->document) {
        try {
            static_cast<void>(probe->document->collaborationRevisions()
                                  .publishUnknownModelMutation());
        }
        catch (const Base::Exception&) {
            probe->revisionPublicationRejected = true;
        }
    }
    if (probe->revisionMutationProbe) {
        attemptDirectRevisionMutations(*probe->revisionMutationProbe);
    }
    if (probe->bypassNoTouchGuard && probe->rawStatusTarget) {
        probe->rawStatusTarget->setNoTouchWithoutMutationGuard(true);
    }
    if (probe->saveAttempt != SaveAttempt::None && probe->document) {
        try {
            performSaveAttempt(*probe->document,
                               probe->saveAttempt,
                               probe->saveAsPath,
                               probe->copyPath);
        }
        catch (const Base::Exception&) {
            probe->saveRejected = true;
        }
    }
    if (probe->raisePythonError) {
        PyErr_SetString(PyExc_ValueError, "original compatibility postcondition failure");
        return nullptr;
    }
    if (probe->returnFalseyInteger) {
        return PyLong_FromLong(0);
    }
    return PyBool_FromLong(probe->satisfied ? 1 : 0);
}

PyObject* makeCompatibilityPostcondition(PostconditionProbe& probe)
{
    static PyMethodDef definition {
        "compatibilityPostcondition",
        runCompatibilityPostcondition,
        METH_NOARGS,
        nullptr,
    };
    PyObjectRef capsule(PyCapsule_New(
        &probe, "App.DocumentCompatibilityPostconditionProbe", nullptr));
    if (!capsule.get()) {
        return nullptr;
    }
    return PyCFunction_NewEx(&definition, capsule.get(), nullptr);
}

std::string makeStructuralImportArchive()
{
    const auto sourceName =
        App::GetApplication().getUniqueDocumentName("structuralImportSource");
    auto* source = App::GetApplication().newDocument(sourceName.c_str(), "Import source");
    auto* object = source->addObject<App::FeatureTest>("ImportedFeature");
    if (!object) {
        throw std::runtime_error("failed to create structural import source object");
    }
    auto* property = freecad_cast<App::PropertyString*>(
        object->addDynamicProperty("App::PropertyString", "ImportedText"));
    if (!property) {
        throw std::runtime_error("failed to create structural import source property");
    }
    property->setValue("restored");

    std::string archive;
    {
        Base::StringOStreambuf buffer(archive);
        std::ostream output(&buffer);
        App::MergeDocuments exportHooks(source);
        source->exportObjects({object}, output);
    }
    App::GetApplication().closeDocument(sourceName.c_str());
    return archive;
}

PyObject* callStructuralCompatibilityMutation(PyObject* document, PyObject* callback)
{
    PyObjectRef method(PyObject_GetAttrString(document, "commitCompatibilityMutation"));
    if (!method.get()) {
        return nullptr;
    }
    PyObjectRef positional(PyTuple_Pack(1, callback));
    if (!positional.get()) {
        return nullptr;
    }
    PyObjectRef keywords(Py_BuildValue("{s:O}", "structural", Py_True));
    if (!keywords.get()) {
        return nullptr;
    }
    return PyObject_Call(method.get(), positional.get(), keywords.get());
}

PyObject* callDeferredCompatibilityMutation(PyObject* document, PyObject* callback)
{
    PyObjectRef method(PyObject_GetAttrString(document, "commitCompatibilityMutation"));
    if (!method.get()) {
        return nullptr;
    }
    PyObjectRef positional(PyTuple_Pack(1, callback));
    if (!positional.get()) {
        return nullptr;
    }
    PyObjectRef keywords(Py_BuildValue("{s:O}", "recompute", Py_False));
    if (!keywords.get()) {
        return nullptr;
    }
    return PyObject_Call(method.get(), positional.get(), keywords.get());
}

PyObject* callCompatibilityMutationWithOptions(
    PyObject* document,
    PyObject* callback,
    const bool structural,
    const bool recompute,
    PyObject* postcondition)
{
    PyObjectRef method(PyObject_GetAttrString(document, "commitCompatibilityMutation"));
    if (!method.get()) {
        return nullptr;
    }
    PyObjectRef positional(PyTuple_Pack(1, callback));
    if (!positional.get()) {
        return nullptr;
    }
    PyObjectRef keywords(PyDict_New());
    if (!keywords.get()
        || PyDict_SetItemString(
               keywords.get(), "structural", structural ? Py_True : Py_False)
            < 0
        || PyDict_SetItemString(
               keywords.get(), "recompute", recompute ? Py_True : Py_False)
            < 0
        || PyDict_SetItemString(
               keywords.get(), "postcondition", postcondition ? postcondition : Py_None)
            < 0) {
        return nullptr;
    }
    return PyObject_Call(method.get(), positional.get(), keywords.get());
}

void expectStructuralPublication(
    const App::DocumentRevisionPublicationEvent& event,
    const std::string& objectName,
    const std::string& stableObjectIdentity)
{
    ASSERT_EQ(event.changes.size(), 4U);
    const auto find = [&](const App::DocumentRevisionKey& key) {
        return std::ranges::find_if(event.changes, [&](const auto& change) {
            return change.key == key;
        });
    };
    const auto existence = find(App::DocumentRevisionKey::objectExistence(objectName));
    const auto objectStructure = find(App::DocumentRevisionKey::objectStructure(objectName));
    const auto documentStructure = find(App::DocumentRevisionKey::documentStructure());
    const auto wildcard = find(App::DocumentRevisionKey::unknownModelMutation());
    ASSERT_NE(existence, event.changes.end());
    ASSERT_NE(objectStructure, event.changes.end());
    ASSERT_NE(documentStructure, event.changes.end());
    ASSERT_NE(wildcard, event.changes.end());
    EXPECT_EQ(existence->stableObjectIdentity, stableObjectIdentity);
    EXPECT_EQ(objectStructure->stableObjectIdentity, stableObjectIdentity);
    EXPECT_FALSE(documentStructure->stableObjectIdentity.has_value());
    EXPECT_FALSE(wildcard->stableObjectIdentity.has_value());
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

    /**
     * Drain MainThreadSignal tasks until @p pred is true.
     *
     * Off-owner collaboration now hops through DocumentExecutionLane; the lane
     * thread may re-enter MainThreadSignal while the worker waits. A single
     * waitUntilQueued/runOne pair can race past that hop and leave worker.join
     * deadlocked — keep pumping until the worker signals completion.
     */
    template<typename Pred>
    [[nodiscard]] bool pumpUntil(Pred&& pred,
                                 std::chrono::milliseconds timeout = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                abortPending();
                return false;
            }
            if (waitUntilQueued(50ms)) {
                static_cast<void>(runOne(50ms));
            }
        }
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
        // The test runner initializes one process-wide application/type registry. Suite setup
        // repeats under --gtest_repeat, but the registry survives each repetition.
        static const bool registered = [] {
            App::ConversionBoundaryTest::LinkListOverride::init();
            App::ConversionBoundaryTest::RegisteredList::init();
            App::ConversionBoundaryTest::RegisteredColor::init();
            App::ConversionBoundaryTest::Feature::init();
            return true;
        }();
        (void)registered;
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

    [[nodiscard]] App::DocumentRevisionCursor publicationCursor() const
    {
        const auto identity = _document->collaborationIdentity();
        App::DocumentRevisionCursor cursor {
            identity.instanceId,
            identity.lifecycleEpoch,
            0,
        };
        cursor.afterSequence =
            _document->collaborationRevisions().pollPublications(cursor, 0).latestSequence;
        return cursor;
    }

    App::Document* _document {nullptr};
    App::FeatureTest* _target {nullptr};
    std::string _documentName;
};

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       indexedLinkListRejectsInvalidLaterValueBeforeEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* links = dynamic_cast<App::PropertyLinkList*>(
        _target->addDynamicProperty("App::PropertyLinkList", "Links"));
    auto* a = _document->addObject<App::FeatureTest>("A");
    auto* b = _document->addObject<App::FeatureTest>("B");
    auto* c = _document->addObject<App::FeatureTest>("C");
    ASSERT_NE(links, nullptr);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    PyObjectRef owner(_target->getPyObject());
    PyObjectRef pa(a->getPyObject());
    PyObjectRef pb(b->getPyObject());
    PyObjectRef pc(c->getPyObject());
    ASSERT_NE(owner.get(), nullptr);
    ASSERT_NE(pa.get(), nullptr);
    ASSERT_NE(pb.get(), nullptr);
    ASSERT_NE(pc.get(), nullptr);
    PyObjectRef initial(Py_BuildValue("[OO]", pa.get(), pb.get()));
    ASSERT_NE(initial.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "Links", initial.get()), 0);
    const auto aBack = a->getInList();
    const auto bBack = b->getInList();
    const auto cBack = c->getInList();
    ASSERT_NE(std::find(aBack.begin(), aBack.end(), _target), aBack.end());
    ASSERT_NE(std::find(bBack.begin(), bBack.end(), _target), bBack.end());
    ASSERT_EQ(std::find(cBack.begin(), cBack.end(), _target), cBack.end());
    int before = 0;
    int after = 0;
    fastsignals::scoped_connection beforeConnection = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property& property) {
            if (&object == _target && &property == links) ++before;
        });
    fastsignals::scoped_connection afterConnection = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property& property) {
            if (&object == _target && &property == links) ++after;
        });
    PyObjectRef invalid(Py_BuildValue("{i:O,i:O}", 0, pc.get(), 1, Py_None));
    ASSERT_NE(invalid.get(), nullptr);
    const int status = PyObject_SetAttrString(owner.get(), "Links", invalid.get());
    const bool valueError = PyErr_ExceptionMatches(PyExc_ValueError);
    PyErr_Clear();
    EXPECT_EQ(status, -1);
    EXPECT_TRUE(valueError);
    EXPECT_EQ(links->getValues(), (std::vector<App::DocumentObject*> {a, b}));
    EXPECT_EQ(a->getInList(), aBack);
    EXPECT_EQ(b->getInList(), bBack);
    EXPECT_EQ(c->getInList(), cBack);
    EXPECT_EQ(before, 0);
    EXPECT_EQ(after, 0);
    PyObjectRef valid(Py_BuildValue("{i:O,i:O}", 0, pc.get(), -1, pa.get()));
    ASSERT_NE(valid.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "Links", valid.get()), 0);
    EXPECT_EQ(links->getValues(), (std::vector<App::DocumentObject*> {c, b, a}));
    EXPECT_NE(std::find(c->getInList().begin(), c->getInList().end(), _target), c->getInList().end());
    EXPECT_GT(before, 0);
    EXPECT_GT(after, 0);
    // The concrete mutator accepts a null assignment when it is already a no-op.
    links->setSize(4);
    PyObjectRef sameNull(Py_BuildValue("{i:O}", 3, Py_None));
    ASSERT_NE(sameNull.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "Links", sameNull.get()), 0);
    EXPECT_EQ(links->getValues().back(), nullptr);
    PyObjectRef customAdded(PyObject_CallMethod(owner.get(), "addProperty", "ss",
        "App::ConversionBoundaryTest::LinkListOverride", "CustomLinks"));
    ASSERT_NE(customAdded.get(), nullptr);
    auto* subclass = dynamic_cast<App::ConversionBoundaryTest::LinkListOverride*>(
        _target->getPropertyByName("CustomLinks"));
    ASSERT_NE(subclass, nullptr);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "CustomLinks", initial.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "CustomLinks", invalid.get()), 0);
    EXPECT_EQ(subclass->calls, 2);
    PyObjectRef customResult(PyObject_GetAttrString(owner.get(), "CustomLinks"));
    PyObjectRef customExpected(Py_BuildValue("[OO]", pc.get(), pb.get()));
    ASSERT_NE(customResult.get(), nullptr);
    ASSERT_NE(customExpected.get(), nullptr);
    EXPECT_EQ(PyObject_RichCompareBool(customResult.get(), customExpected.get(), Py_EQ), 1);
    PyObjectRef clear(Py_BuildValue("[O]", Py_None));
    ASSERT_NE(clear.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(owner.get(), "Links", clear.get()), 0);
    EXPECT_TRUE(links->getValues().empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       registeredLinkListVariantsPreflightAndPreserveNotificationOrdering)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef owner(_target->getPyObject());
    ASSERT_NE(owner.get(), nullptr);
    auto* a = _document->addObject<App::FeatureTest>("VariantA");
    auto* b = _document->addObject<App::FeatureTest>("VariantB");
    auto* c = _document->addObject<App::FeatureTest>("VariantC");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);
    PyObjectRef pa(a->getPyObject()), pb(b->getPyObject()), pc(c->getPyObject());
    ASSERT_NE(pa.get(), nullptr);
    ASSERT_NE(pb.get(), nullptr);
    ASSERT_NE(pc.get(), nullptr);
    const char* types[] = {"App::PropertyLinkListGlobal", "App::PropertyLinkListChild",
                           "App::PropertyLinkListHidden"};
    for (int i = 0; i < 3; ++i) {
        SCOPED_TRACE(types[i]);
        const bool hidden = i == 2;
        const std::string name = "VariantLinks" + std::to_string(i);
        PyObjectRef added(PyObject_CallMethod(owner.get(), "addProperty", "ss", types[i], name.c_str()));
        ASSERT_NE(added.get(), nullptr);
        auto* links = dynamic_cast<App::PropertyLinkList*>(_target->getPropertyByName(name.c_str()));
        ASSERT_NE(links, nullptr);
        PyObjectRef initial(Py_BuildValue("[OO]", pa.get(), pb.get()));
        ASSERT_NE(initial.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(owner.get(), name.c_str(), initial.get()), 0);
        const auto aBack = a->getInList();
        const auto bBack = b->getInList();
        const auto cBack = c->getInList();
        auto hasOwner = [&](const auto& list) {
            return std::find(list.begin(), list.end(), _target) != list.end();
        };
        EXPECT_EQ(hasOwner(aBack), !hidden);
        EXPECT_EQ(hasOwner(bBack), !hidden);
        EXPECT_FALSE(hasOwner(cBack));
        int before = 0, after = 0;
        fastsignals::scoped_connection beforeConnection = _document->signalBeforeChangeObject.connect(
            [&](const App::DocumentObject& object, const App::Property& property) {
                if (&object != _target || &property != links) return;
                ++before;
                EXPECT_EQ(links->getValues(), (std::vector<App::DocumentObject*> {a, b}));
                EXPECT_EQ(a->getInList(), aBack);
                EXPECT_EQ(b->getInList(), bBack);
                EXPECT_EQ(c->getInList(), cBack);
            });
        fastsignals::scoped_connection afterConnection = _document->signalChangedObject.connect(
            [&](const App::DocumentObject& object, const App::Property& property) {
                if (&object != _target || &property != links) return;
                ++after;
                EXPECT_EQ(links->getValues(), (std::vector<App::DocumentObject*> {c, a}));
                EXPECT_EQ(hasOwner(a->getInList()), !hidden);
                EXPECT_FALSE(hasOwner(b->getInList()));
                EXPECT_EQ(hasOwner(c->getInList()), !hidden);
            });
        PyObjectRef invalid(Py_BuildValue("{i:O,i:O}", 0, pc.get(), 1, Py_None));
        ASSERT_NE(invalid.get(), nullptr);
        EXPECT_EQ(PyObject_SetAttrString(owner.get(), name.c_str(), invalid.get()), -1);
        EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
        PyErr_Clear();
        PyObjectRef current(PyObject_GetAttrString(owner.get(), name.c_str()));
        ASSERT_NE(current.get(), nullptr);
        EXPECT_EQ(PyObject_RichCompareBool(current.get(), initial.get(), Py_EQ), 1);
        EXPECT_EQ(a->getInList(), aBack);
        EXPECT_EQ(b->getInList(), bBack);
        EXPECT_EQ(c->getInList(), cBack);
        EXPECT_EQ(before, 0);
        EXPECT_EQ(after, 0);
        PyObjectRef valid(Py_BuildValue("{i:O,i:O}", 0, pc.get(), 1, pa.get()));
        ASSERT_NE(valid.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(owner.get(), name.c_str(), valid.get()), 0);
        PyObjectRef result(PyObject_GetAttrString(owner.get(), name.c_str()));
        PyObjectRef expected(Py_BuildValue("[OO]", pc.get(), pa.get()));
        ASSERT_NE(result.get(), nullptr);
        ASSERT_NE(expected.get(), nullptr);
        EXPECT_EQ(PyObject_RichCompareBool(result.get(), expected.get(), Py_EQ), 1);
        EXPECT_EQ(before, 1);
        EXPECT_EQ(after, 1);
        beforeConnection.disconnect();
        afterConnection.disconnect();
        PyObjectRef clear(PyList_New(0));
        ASSERT_NE(clear.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(owner.get(), name.c_str(), clear.get()), 0);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       quantityVectorMatrixAndUnicodeEnumRejectInvalidInputBeforeEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    PyObjectRef quantity(PyLong_FromLong(23));
    PyObjectRef vector(Py_BuildValue("(lll)", 4L, 5L, 6L));
    ASSERT_NE(quantity.get(), nullptr);
    ASSERT_NE(vector.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "QuantityLength", quantity.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Vector", vector.get()), 0);
    PyObjectRef initialVectors(Py_BuildValue("[(lll)(lll)]", 1L, 2L, 3L, 4L, 5L, 6L));
    ASSERT_NE(initialVectors.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "VectorList", initialVectors.get()), 0);
    const auto matrixBefore = _target->Matrix.getValue();
    const auto vecBefore = _target->Vector.getValue();
    const auto listBefore = _target->VectorList.getValues();
    const auto quantityBefore = _target->QuantityLength.getValue();
    const auto angleBefore = _target->Angle.getValue();
    const auto enumBefore = _target->Enum.getValue();
    const auto enumNames = _target->Enum.getEnumVector();
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const auto undos = _document->getAvailableUndos();
    const auto fileState = _document->getFileChangeState();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    const auto reject = [&](const char* name, PyObject* input, PyObject* error) {
        const int status = PyObject_SetAttrString(target.get(), name, input);
        const bool matched = PyErr_ExceptionMatches(error);
        PyErr_Clear();
        EXPECT_EQ(status, -1) << name;
        EXPECT_TRUE(matched) << name;
        EXPECT_DOUBLE_EQ(_target->QuantityLength.getValue(), quantityBefore);
        const auto vec = _target->Vector.getValue();
        EXPECT_DOUBLE_EQ(vec.x, vecBefore.x);
        EXPECT_DOUBLE_EQ(vec.y, vecBefore.y);
        EXPECT_DOUBLE_EQ(vec.z, vecBefore.z);
        EXPECT_DOUBLE_EQ(_target->Angle.getValue(), angleBefore);
        const auto& list = _target->VectorList.getValues();
        ASSERT_EQ(list.size(), listBefore.size());
        for (std::size_t i = 0; i < list.size(); ++i) {
            EXPECT_DOUBLE_EQ(list[i].x, listBefore[i].x);
            EXPECT_DOUBLE_EQ(list[i].y, listBefore[i].y);
            EXPECT_DOUBLE_EQ(list[i].z, listBefore[i].z);
        }
        const auto& actual = _target->Matrix.getValue();
        for (int x = 0; x < 4; ++x) {
            for (int y = 0; y < 4; ++y) {
                EXPECT_DOUBLE_EQ(actual[x][y], matrixBefore[x][y]);
            }
        }
        EXPECT_EQ(_target->Enum.getValue(), enumBefore);
        EXPECT_EQ(_target->Enum.getEnumVector(), enumNames);
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(_document->getFileChangeState(), fileState);
        EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        EXPECT_EQ(beforeSignals, 0);
        EXPECT_EQ(afterSignals, 0);
    };
    char positive[] = "99999999999999999999999999999999999999999999999999999";
    char negative[] = "-99999999999999999999999999999999999999999999999999999";
    PyObjectRef tooLarge(PyLong_FromString(positive, nullptr, 10));
    PyObjectRef tooSmall(PyLong_FromString(negative, nullptr, 10));
    ASSERT_NE(tooLarge.get(), nullptr);
    ASSERT_NE(tooSmall.get(), nullptr);
    for (PyObject* overflow : {tooLarge.get(), tooSmall.get()}) {
        reject("QuantityLength", overflow, PyExc_OverflowError);
        reject("Angle", overflow, PyExc_OverflowError);
        for (int bad = 0; bad < 3; ++bad) {
            PyObjectRef tuple(PyTuple_New(3));
            ASSERT_NE(tuple.get(), nullptr);
            for (int i = 0; i < 3; ++i) {
                PyObject* value = i == bad ? overflow : PyLong_FromLong(i + 1);
                ASSERT_NE(value, nullptr);
                if (i == bad) Py_INCREF(value);
                PyTuple_SET_ITEM(tuple.get(), i, value);
            }
            reject("Vector", tuple.get(), PyExc_OverflowError);
            PyObjectRef list(Py_BuildValue("[(lll)O]", 1L, 2L, 3L, tuple.get()));
            ASSERT_NE(list.get(), nullptr);
            reject("VectorList", list.get(), PyExc_OverflowError);
            reject("VectorList", tuple.get(), PyExc_OverflowError);
            PyObjectRef indexed(PyDict_New());
            PyObjectRef firstKey(PyLong_FromLong(0));
            PyObjectRef secondKey(PyLong_FromLong(1));
            PyObjectRef firstVector(Py_BuildValue("(lll)", 7L, 8L, 9L));
            ASSERT_NE(indexed.get(), nullptr);
            ASSERT_NE(firstKey.get(), nullptr);
            ASSERT_NE(secondKey.get(), nullptr);
            ASSERT_NE(firstVector.get(), nullptr);
            ASSERT_EQ(PyDict_SetItem(indexed.get(), firstKey.get(), firstVector.get()), 0);
            ASSERT_EQ(PyDict_SetItem(indexed.get(), secondKey.get(), tuple.get()), 0);
            reject("VectorList", indexed.get(), PyExc_OverflowError);
            PyObjectRef badKey(PyDict_New());
            ASSERT_NE(badKey.get(), nullptr);
            ASSERT_EQ(PyDict_SetItem(badKey.get(), firstKey.get(), firstVector.get()), 0);
            ASSERT_EQ(PyDict_SetItem(badKey.get(), overflow, firstVector.get()), 0);
            reject("VectorList", badKey.get(), PyExc_OverflowError);
        }
        for (int bad = 0; bad < 16; ++bad) {
            PyObjectRef tuple(PyTuple_New(16));
            ASSERT_NE(tuple.get(), nullptr);
            for (int i = 0; i < 16; ++i) {
                PyObject* value = i == bad ? overflow : PyLong_FromLong(i + 1);
                ASSERT_NE(value, nullptr);
                if (i == bad) Py_INCREF(value);
                PyTuple_SET_ITEM(tuple.get(), i, value);
            }
            reject("Matrix", tuple.get(), PyExc_OverflowError);
        }
    }
    PyObjectRef surrogate(PyUnicode_FromOrdinal(0xd800));
    ASSERT_NE(surrogate.get(), nullptr);
    reject("Enum", surrogate.get(), PyExc_UnicodeEncodeError);
    reject("QuantityLength", surrogate.get(), PyExc_UnicodeEncodeError);
    PyObjectRef labels(Py_BuildValue("[sO]", "Valid", surrogate.get()));
    ASSERT_NE(labels.get(), nullptr);
    reject("Enum", labels.get(), PyExc_UnicodeEncodeError);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       quantityVectorAndMatrixPreserveValidSignedBoundaries)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    for (long value : {std::numeric_limits<long>::lowest(), -1L,
                       std::numeric_limits<long>::max()}) {
        PyObjectRef number(PyLong_FromLong(value));
        PyObjectRef vector(Py_BuildValue("(lll)", value, -1L, 0L));
        PyObjectRef matrix(PyTuple_New(16));
        ASSERT_NE(number.get(), nullptr);
        ASSERT_NE(vector.get(), nullptr);
        ASSERT_NE(matrix.get(), nullptr);
        for (int i = 0; i < 16; ++i) {
            PyObject* item = PyLong_FromLong(i == 15 ? value : -1L);
            ASSERT_NE(item, nullptr);
            PyTuple_SET_ITEM(matrix.get(), i, item);
        }
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "QuantityLength", number.get()), 0);
        EXPECT_DOUBLE_EQ(_target->QuantityLength.getValue(), static_cast<double>(value));
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "Vector", vector.get()), 0);
        EXPECT_DOUBLE_EQ(_target->Vector.getValue().x, static_cast<double>(value));
        EXPECT_DOUBLE_EQ(_target->Vector.getValue().y, -1.0);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "VectorList", vector.get()), 0);
        ASSERT_EQ(_target->VectorList.getValues().size(), 1U);
        EXPECT_DOUBLE_EQ(_target->VectorList.getValues()[0].x, static_cast<double>(value));
        PyObjectRef updates(PyDict_New());
        PyObjectRef key(PyLong_FromLong(0));
        ASSERT_NE(updates.get(), nullptr);
        ASSERT_NE(key.get(), nullptr);
        ASSERT_EQ(PyDict_SetItem(updates.get(), key.get(), vector.get()), 0);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "VectorList", updates.get()), 0);
        EXPECT_DOUBLE_EQ(_target->VectorList.getValues()[0].x, static_cast<double>(value));
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "Matrix", matrix.get()), 0);
        EXPECT_DOUBLE_EQ(_target->Matrix.getValue()[3][3], static_cast<double>(value));
        EXPECT_DOUBLE_EQ(_target->Matrix.getValue()[0][0], -1.0);
        EXPECT_EQ(PyErr_Occurred(), nullptr);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       quantityAndEnumerationAcceptValidUnicode)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef target(_target->getPyObject());
    PyObjectRef quantity(PyUnicode_FromString("2 mm"));
    PyObjectRef labels(Py_BuildValue("[ss]", "First", "\xc3\xa9"));
    PyObjectRef selection(PyUnicode_FromString("\xc3\xa9"));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(quantity.get(), nullptr);
    ASSERT_NE(labels.get(), nullptr);
    ASSERT_NE(selection.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "QuantityLength", quantity.get()), 0);
    EXPECT_DOUBLE_EQ(_target->QuantityLength.getValue(), 2.0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Enum", labels.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Enum", selection.get()), 0);
    EXPECT_STREQ(_target->Enum.getValueAsString(), "\xc3\xa9");
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       derivedNativeListsKeepIndexedVirtualDispatchThroughPythonBinding)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* feature = _document->addObject<App::ConversionBoundaryTest::Feature>("DerivedLists");
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(feature->Inherited.getTypeId(), App::PropertyIntegerList::getClassTypeId());
    ASSERT_NE(feature->Registered.getTypeId(), App::PropertyIntegerList::getClassTypeId());
    PyObjectRef target(feature->getPyObject());
    PyObjectRef updates(Py_BuildValue("{l:l,l:l}", 0L, 42L, -1L, 43L));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(updates.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Inherited", updates.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Registered", updates.get()), 0);
    EXPECT_EQ(feature->Inherited.calls, 1);
    EXPECT_EQ(feature->Registered.calls, 1);
    EXPECT_EQ(feature->Inherited.getValues(), (std::vector<long> {42L, 43L}));
    EXPECT_EQ(feature->Registered.getValues(), (std::vector<long> {42L, 43L}));
    ASSERT_EQ(feature->InheritedColour.getTypeId(), App::PropertyColorList::getClassTypeId());
    ASSERT_NE(feature->RegisteredColour.getTypeId(), App::PropertyColorList::getClassTypeId());
    PyObjectRef scalar(PyLong_FromLong(123));
    ASSERT_NE(scalar.get(), nullptr);
    for (const char* name : {"InheritedColour", "RegisteredColour"}) {
        ASSERT_EQ(PyObject_SetAttrString(target.get(), name, scalar.get()), 0) << name;
    }
    EXPECT_EQ(feature->InheritedColour.calls, 1);
    EXPECT_EQ(feature->RegisteredColour.calls, 1);
    const auto checkColors = [](const auto& property, bool indexed) {
        const auto& values = property.getValues();
        ASSERT_EQ(values.size(), indexed ? 2U : 1U);
        const auto check = [](const auto& color, float r, float g, float b, float a) {
            EXPECT_FLOAT_EQ(color.r, r);
            EXPECT_FLOAT_EQ(color.g, g);
            EXPECT_FLOAT_EQ(color.b, b);
            EXPECT_FLOAT_EQ(color.a, a);
        };
        if (indexed) {
            check(values[0], 0.1F, 0.2F, 0.3F, 0.4F);
            check(values[1], 0.5F, 0.6F, 0.7F, 0.8F);
        }
        else {
            check(values[0], 0.25F, 0.5F, 0.75F, 1.0F);
        }
    };
    checkColors(feature->InheritedColour, false);
    checkColors(feature->RegisteredColour, false);
    PyObjectRef colorUpdates(Py_BuildValue("{i:(ffff),i:(ffff)}", 0, 0.1, 0.2, 0.3, 0.4,
                                           -1, 0.5, 0.6, 0.7, 0.8));
    ASSERT_NE(colorUpdates.get(), nullptr);
    for (const char* name : {"InheritedColour", "RegisteredColour"}) {
        ASSERT_EQ(PyObject_SetAttrString(target.get(), name, colorUpdates.get()), 0) << name;
        PyObjectRef actual(PyObject_GetAttrString(target.get(), name));
        ASSERT_NE(actual.get(), nullptr);
        EXPECT_EQ(PySequence_Size(actual.get()), 2);
    }
    EXPECT_EQ(feature->InheritedColour.calls, 2);
    EXPECT_EQ(feature->RegisteredColour.calls, 2);
    checkColors(feature->InheritedColour, true);
    checkColors(feature->RegisteredColour, true);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       sharedIndexedListRejectsOverflowAndInvalidValuesBeforeEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* feature = _document->addObject<App::ConversionBoundaryTest::Feature>("SharedListProbe");
    ASSERT_NE(feature, nullptr);
    ASSERT_NE(feature->addDynamicProperty("App::PropertyPlacementList", "Placements"), nullptr);
    PyObjectRef target(feature->getPyObject());
    PyObjectRef globals(PyDict_New());
    PyObjectRef app(PyImport_ImportModule("FreeCAD"));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(globals.get(), nullptr);
    ASSERT_NE(app.get(), nullptr);
    ASSERT_EQ(PyDict_SetItemString(globals.get(), "__builtins__", PyEval_GetBuiltins()), 0);
    ASSERT_EQ(PyDict_SetItemString(globals.get(), "App", app.get()), 0);
    const auto eval = [&](const char* expression) {
        return PyRun_String(expression, Py_eval_input, globals.get(), globals.get());
    };
    PyObjectRef placements(eval("[App.Placement(App.Vector(1,0,0), App.Rotation()), App.Placement(App.Vector(2,0,0), App.Rotation())]"));
    PyObjectRef integers(eval("[1,2]"));
    ASSERT_NE(placements.get(), nullptr);
    ASSERT_NE(integers.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Placements", placements.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Registered", integers.get()), 0);
    int before = 0;
    int after = 0;
    fastsignals::scoped_connection beforeConnection = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) { if (&object == feature) ++before; });
    fastsignals::scoped_connection afterConnection = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) { if (&object == feature) ++after; });
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("SharedListProbe"),
        App::DocumentRevisionKey::objectStructure("SharedListProbe"),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    for (const auto& test : {std::pair<const char*, const char*> {"Placements", "{2**70: App.Placement()}"},
                             {"Placements", "{0: App.Placement(), 1: 'bad'}"},
                             {"Registered", "{2**70: 9}"},
                             {"Registered", "{0: 9, 1: 'bad'}"}}) {
        SCOPED_TRACE(test.second);
        PyObjectRef input(eval(test.second));
        ASSERT_NE(input.get(), nullptr);
        EXPECT_EQ(PyObject_SetAttrString(target.get(), test.first, input.get()), -1);
        EXPECT_NE(PyErr_Occurred(), nullptr);
        if (std::string(test.second).find("2**70") != std::string::npos) {
            EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_OverflowError));
        }
        PyErr_Clear();
        EXPECT_EQ(before, 0);
        EXPECT_EQ(after, 0);
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        EXPECT_EQ(feature->Registered.getValues(), (std::vector<long> {1, 2}));
        PyObjectRef actual(PyObject_GetAttrString(target.get(), "Placements"));
        ASSERT_NE(actual.get(), nullptr);
        ASSERT_EQ(PyList_Size(actual.get()), 2);
        for (Py_ssize_t i = 0; i < 2; ++i) {
            PyObjectRef base(PyObject_GetAttrString(PyList_GetItem(actual.get(), i), "Base"));
            ASSERT_NE(base.get(), nullptr);
            PyObjectRef x(PyObject_GetAttrString(base.get(), "x"));
            ASSERT_NE(x.get(), nullptr);
            EXPECT_DOUBLE_EQ(PyFloat_AsDouble(x.get()), static_cast<double>(i + 1));
        }
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       allBuiltInListIndexedRoutesPreserveValuesOrderAndFailureAtomicity)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    ASSERT_NE(_target->addDynamicProperty("App::PropertyIntPairList", "PairProbe"), nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef globals(PyDict_New());
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(globals.get(), nullptr);
    ASSERT_EQ(PyDict_SetItemString(globals.get(), "__builtins__", PyEval_GetBuiltins()), 0);
    const auto eval = [&](const char* expression) {
        return PyRun_String(expression, Py_eval_input, globals.get(), globals.get());
    };
    PyObjectRef normalize(eval("lambda v: tuple((x.x,x.y,x.z) if hasattr(x,'x') else tuple(x) if isinstance(x,(tuple,list)) else x for x in v)"));
    ASSERT_NE(normalize.get(), nullptr);
    struct Case {
        const char* name;
        const char* initial;
        const char* replace;
        const char* append;
        const char* afterReplace;
        const char* afterAppend;
        const char* badValue;
        const char* badKey;
        const char* fallback;
        const char* afterFallback;
    };
    const Case cases[] = {
        {"IntegerList", "[1,2]", "{0:9}", "{2:7,-1:8}", "(9,2)", "(9,2,7,8)", "{0:3,1:object()}", "{0:3,'bad':4}", "[5,6]", "(5,6)"},
        {"PairProbe", "[(1,2),(3,4)]", "{0:(9,8)}", "{2:(7,6),-1:(5,4)}", "((9,8),(3,4))", "((9,8),(3,4),(7,6),(5,4))", "{0:(2,1),1:object()}", "{0:(2,1),'bad':(4,3)}", "[(4,3),(2,1)]", "((4,3),(2,1))"},
        {"FloatList", "[1.0,2.0]", "{0:9.0}", "{2:7.0,-1:8.0}", "(9.0,2.0)", "(9.0,2.0,7.0,8.0)", "{0:3.0,1:object()}", "{0:3.0,'bad':4.0}", "[5.0,6.0]", "(5.0,6.0)"},
        {"BoolList", "[True,False]", "{0:False}", "{2:True,-1:False}", "(False,False)", "(False,False,True,False)", "{0:True,1:object()}", "{0:True,'bad':False}", "[True,True]", "(True,True)"},
        {"ColourList", "[(0.,0.,0.,1.),(1.,1.,1.,1.)]", "{0:(1.,0.,0.,1.)}", "{2:(0.,1.,0.,1.),-1:(0.,0.,1.,0.)}", "((1.,0.,0.,1.),(1.,1.,1.,1.))", "((1.,0.,0.,1.),(1.,1.,1.,1.),(0.,1.,0.,1.),(0.,0.,1.,0.))", "{0:(0.,0.,0.,0.),1:object()}", "{0:(0.,0.,0.,0.),'bad':(1.,1.,1.,1.)}", "[(0.,1.,1.,1.),(1.,0.,1.,1.)]", "((0.,1.,1.,1.),(1.,0.,1.,1.))"},
        {"StringList", "['a','b']", "{0:'c'}", "{2:'d',-1:'e'}", "('c','b')", "('c','b','d','e')", "{0:'changed',1:object()}", "{0:'changed','bad':'x'}", "['f','g']", "('f','g')"},
        {"VectorList", "[(1,2,3),(4,5,6)]", "{0:(9,8,7)}", "{2:(6,5,4),-1:(3,2,1)}", "((9.,8.,7.),(4.,5.,6.))", "((9.,8.,7.),(4.,5.,6.),(6.,5.,4.),(3.,2.,1.))", "{0:(3,4,5),1:object()}", "{0:(3,4,5),'bad':(1,2,3)}", "[(6,7,8),(9,10,11)]", "((6.,7.,8.),(9.,10.,11.))"},
    };
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    const auto write = [&](const char* name, const char* expression, bool success) {
        PyObjectRef input(eval(expression));
        if (!input.get()) { PyErr_Clear(); ADD_FAILURE() << expression; return; }
        const int status = PyObject_SetAttrString(target.get(), name, input.get());
        const bool hadError = PyErr_Occurred() != nullptr;
        PyErr_Clear();
        EXPECT_EQ(status, success ? 0 : -1) << name << ' ' << expression;
        EXPECT_EQ(hadError, !success) << name << ' ' << expression;
    };
    const auto check = [&](const char* name, const char* expression) {
        PyObjectRef actual(PyObject_GetAttrString(target.get(), name));
        if (!actual.get()) { PyErr_Clear(); ADD_FAILURE() << name; return; }
        PyObjectRef normalized(PyObject_CallFunctionObjArgs(normalize.get(), actual.get(), nullptr));
        PyObjectRef expected(eval(expression));
        if (!normalized.get() || !expected.get()) { PyErr_Clear(); ADD_FAILURE() << name; return; }
        const int equal = PyObject_RichCompareBool(normalized.get(), expected.get(), Py_EQ);
        if (equal < 0) PyErr_Clear();
        EXPECT_EQ(equal, 1) << name << " expected " << expression;
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        write(item.name, item.initial, true);
        beforeSignals = afterSignals = 0;
        write(item.name, item.replace, true);
        check(item.name, item.afterReplace);
        EXPECT_GT(beforeSignals, 0);
        EXPECT_GT(afterSignals, 0);
        beforeSignals = afterSignals = 0;
        write(item.name, item.append, true);
        check(item.name, item.afterAppend);
        EXPECT_GT(beforeSignals, 0);
        EXPECT_GT(afterSignals, 0);
        const auto revisions = _document->collaborationRevisions().capture(keys);
        const int undos = _document->getAvailableUndos();
        const int redos = _document->getAvailableRedos();
        const auto fileState = _document->getFileChangeState();
        const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
        beforeSignals = afterSignals = 0;
        for (const char* bad : {item.badValue, item.badKey}) {
            write(item.name, bad, false);
            check(item.name, item.afterAppend);
            EXPECT_EQ(beforeSignals, 0);
            EXPECT_EQ(afterSignals, 0);
            EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
            EXPECT_EQ(_document->getAvailableUndos(), undos);
            EXPECT_EQ(_document->getAvailableRedos(), redos);
            EXPECT_EQ(_document->getFileChangeState(), fileState);
            EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        }
        write(item.name, "{}", true);
        check(item.name, "()");
        EXPECT_GT(beforeSignals, 0);
        EXPECT_GT(afterSignals, 0);
        beforeSignals = afterSignals = 0;
        write(item.name, item.fallback, true);
        check(item.name, item.afterFallback);
        EXPECT_GT(beforeSignals, 0);
        EXPECT_GT(afterSignals, 0);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       unicodePropertiesRejectSurrogatesBeforeEffectsAndAcceptValidInput)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    ASSERT_NE(_target->addDynamicProperty("App::PropertyUUID", "UUIDProbe"), nullptr);
    ASSERT_NE(_target->addDynamicProperty("App::PropertyMap", "MapProbe"), nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef unicode(PyUnicode_FromString("caf\xc3\xa9"));
    PyObjectRef uuid(PyUnicode_FromString("123e4567-e89b-12d3-a456-426614174000"));
    PyObjectRef bits(PyUnicode_FromString("101"));
    PyObjectRef list(Py_BuildValue("[OO]", unicode.get(), unicode.get()));
    PyObjectRef map(PyDict_New());
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(unicode.get(), nullptr);
    ASSERT_NE(uuid.get(), nullptr);
    ASSERT_NE(bits.get(), nullptr);
    ASSERT_NE(list.get(), nullptr);
    ASSERT_NE(map.get(), nullptr);
    ASSERT_EQ(PyDict_SetItem(map.get(), unicode.get(), unicode.get()), 0);
    const std::array<std::pair<const char*, PyObject*>, 6> cases {{
        {"Path", unicode.get()}, {"String", unicode.get()}, {"UUIDProbe", uuid.get()},
        {"StringList", list.get()}, {"MapProbe", map.get()}, {"BoolList", bits.get()}}};
    for (const auto& [name, value] : cases) {
        ASSERT_EQ(PyObject_SetAttrString(target.get(), name, value), 0) << name;
        EXPECT_EQ(PyErr_Occurred(), nullptr);
    }
    std::vector<PyObjectRef*> snapshots;
    std::vector<std::unique_ptr<PyObjectRef>> owners;
    for (const auto& [name, value] : cases) {
        (void)value;
        owners.push_back(std::make_unique<PyObjectRef>(PyObject_GetAttrString(target.get(), name)));
        ASSERT_NE(owners.back()->get(), nullptr);
        snapshots.push_back(owners.back().get());
    }
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[0]->get(), unicode.get(), Py_EQ), 1);
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[1]->get(), unicode.get(), Py_EQ), 1);
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[2]->get(), uuid.get(), Py_EQ), 1);
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[3]->get(), list.get(), Py_EQ), 1);
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[4]->get(), map.get(), Py_EQ), 1);
    PyObjectRef expectedBits(Py_BuildValue("(OOO)", Py_True, Py_False, Py_True));
    ASSERT_NE(expectedBits.get(), nullptr);
    EXPECT_EQ(PyObject_RichCompareBool(snapshots[5]->get(), expectedBits.get(), Py_EQ), 1);
    _document->openTransaction("Unicode rejection");
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    const int redos = _document->getAvailableRedos();
    const auto fileState = _document->getFileChangeState();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    PyObjectRef bad(PyUnicode_FromOrdinal(0xd800));
    ASSERT_NE(bad.get(), nullptr);
    const auto reject = [&](const char* name, PyObject* value) {
        const int status = PyObject_SetAttrString(target.get(), name, value);
        const bool expected = PyErr_ExceptionMatches(PyExc_UnicodeEncodeError);
        PyErr_Clear();
        EXPECT_EQ(status, -1) << name;
        EXPECT_TRUE(expected) << name;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            PyObjectRef current(PyObject_GetAttrString(target.get(), cases[i].first));
            ASSERT_NE(current.get(), nullptr);
            EXPECT_EQ(PyObject_RichCompareBool(current.get(), snapshots[i]->get(), Py_EQ), 1)
                << cases[i].first;
        }
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(_document->getAvailableRedos(), redos);
        EXPECT_EQ(_document->getFileChangeState(), fileState);
        EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        EXPECT_EQ(beforeSignals, 0);
        EXPECT_EQ(afterSignals, 0);
    };
    for (const char* name : {"Path", "String", "UUIDProbe", "BoolList"}) {
        reject(name, bad.get());
    }
    PyObjectRef changedString(PyUnicode_FromString("changed"));
    ASSERT_NE(changedString.get(), nullptr);
    PyObjectRef badList(Py_BuildValue("[OO]", changedString.get(), bad.get()));
    PyObjectRef indexed(PyDict_New());
    PyObjectRef zero(PyLong_FromLong(0));
    PyObjectRef one(PyLong_FromLong(1));
    PyObjectRef badKeyMap(PyDict_New());
    PyObjectRef badValueMap(PyDict_New());
    ASSERT_NE(badList.get(), nullptr);
    ASSERT_NE(indexed.get(), nullptr);
    ASSERT_NE(zero.get(), nullptr);
    ASSERT_NE(one.get(), nullptr);
    ASSERT_NE(badKeyMap.get(), nullptr);
    ASSERT_NE(badValueMap.get(), nullptr);
    ASSERT_EQ(PyDict_SetItem(indexed.get(), zero.get(), changedString.get()), 0);
    ASSERT_EQ(PyDict_SetItem(indexed.get(), one.get(), bad.get()), 0);
    ASSERT_EQ(PyDict_SetItem(badKeyMap.get(), unicode.get(), unicode.get()), 0);
    ASSERT_EQ(PyDict_SetItem(badKeyMap.get(), bad.get(), unicode.get()), 0);
    ASSERT_EQ(PyDict_SetItem(badValueMap.get(), unicode.get(), unicode.get()), 0);
    PyObjectRef other(PyUnicode_FromString("other"));
    ASSERT_NE(other.get(), nullptr);
    ASSERT_EQ(PyDict_SetItem(badValueMap.get(), other.get(), bad.get()), 0);
    reject("StringList", badList.get());
    reject("StringList", indexed.get());
    reject("MapProbe", badKeyMap.get());
    reject("MapProbe", badValueMap.get());
    PyObjectRef validIndex(Py_BuildValue("{i:O,i:O}", 0, other.get(), -1, unicode.get()));
    ASSERT_NE(validIndex.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "StringList", validIndex.get()), 0);
    PyObjectRef updated(PyObject_GetAttrString(target.get(), "StringList"));
    PyObjectRef expected(Py_BuildValue("[OOO]", other.get(), unicode.get(), unicode.get()));
    ASSERT_NE(updated.get(), nullptr);
    ASSERT_NE(expected.get(), nullptr);
    EXPECT_EQ(PyObject_RichCompareBool(updated.get(), expected.get(), Py_EQ), 1);
    EXPECT_GT(beforeSignals, 0);
    EXPECT_GT(afterSignals, 0);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       integerAndFloatBindingsRejectOverflowBeforeAnyModelEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* percent = dynamic_cast<App::PropertyPercent*>(
        _target->addDynamicProperty("App::PropertyPercent", "PercentProbe"));
    ASSERT_NE(percent, nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef initial(PyLong_FromLong(42));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(initial.get(), nullptr);
    const char* names[] = {"Integer", "ConstraintInt", "PercentProbe", "Float", "ConstraintFloat"};
    for (const char* name : names) {
        ASSERT_EQ(PyObject_SetAttrString(target.get(), name, initial.get()), 0) << name;
    }
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectProperty("Target", "Integer"),
        App::DocumentRevisionKey::objectProperty("Target", "Float"),
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    const auto fileState = _document->getFileChangeState();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    char positive[] = "999999999999999999999999999999999999999999999999999999999999999999";
    char negative[] = "-999999999999999999999999999999999999999999999999999999999999999999";
    PyObjectRef tooLarge(PyLong_FromString(positive, nullptr, 10));
    PyObjectRef tooSmall(PyLong_FromString(negative, nullptr, 10));
    ASSERT_NE(tooLarge.get(), nullptr);
    ASSERT_NE(tooSmall.get(), nullptr);
    for (const char* name : names) {
        for (PyObject* value : {tooLarge.get(), tooSmall.get()}) {
            SCOPED_TRACE(name);
            const int status = PyObject_SetAttrString(target.get(), name, value);
            const bool overflow = PyErr_ExceptionMatches(PyExc_OverflowError);
            PyErr_Clear();
            EXPECT_EQ(status, -1);
            EXPECT_TRUE(overflow);
            EXPECT_EQ(_target->Integer.getValue(), 42);
            EXPECT_EQ(_target->ConstraintInt.getValue(), 42);
            EXPECT_EQ(percent->getValue(), 42);
            EXPECT_DOUBLE_EQ(_target->Float.getValue(), 42.0);
            EXPECT_DOUBLE_EQ(_target->ConstraintFloat.getValue(), 42.0);
            EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
            EXPECT_EQ(_document->getAvailableUndos(), undos);
            EXPECT_EQ(_document->getFileChangeState(), fileState);
            EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
            EXPECT_EQ(beforeSignals, 0);
            EXPECT_EQ(afterSignals, 0);
        }
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       integerAndFloatBindingsPreserveValidLongBoundsAndClamping)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* percent = dynamic_cast<App::PropertyPercent*>(
        _target->addDynamicProperty("App::PropertyPercent", "PercentProbe"));
    ASSERT_NE(percent, nullptr);
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    for (const long value : {std::numeric_limits<long>::lowest(), -1L,
                             std::numeric_limits<long>::max()}) {
        PyObjectRef pyValue(PyLong_FromLong(value));
        ASSERT_NE(pyValue.get(), nullptr);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "Integer", pyValue.get()), 0);
        EXPECT_EQ(_target->Integer.getValue(), value);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "Float", pyValue.get()), 0);
        EXPECT_DOUBLE_EQ(_target->Float.getValue(), static_cast<double>(value));
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "ConstraintInt", pyValue.get()), 0);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "PercentProbe", pyValue.get()), 0);
        ASSERT_EQ(PyObject_SetAttrString(target.get(), "ConstraintFloat", pyValue.get()), 0);
        const long clipped = std::clamp(value, 0L, 100L);
        EXPECT_EQ(_target->ConstraintInt.getValue(), clipped);
        EXPECT_EQ(percent->getValue(), clipped);
        EXPECT_DOUBLE_EQ(_target->ConstraintFloat.getValue(), static_cast<double>(clipped));
        EXPECT_EQ(PyErr_Occurred(), nullptr);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       integerListAndSetBindingsValidateEveryElementBeforeAnyModelEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* set = dynamic_cast<App::PropertyIntegerSet*>(
        _target->addDynamicProperty("App::PropertyIntegerSet", "IntegerSetProbe"));
    ASSERT_NE(set, nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef initial(Py_BuildValue("[lll]", 10L, 20L, 30L));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(initial.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerList", initial.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerSetProbe", initial.get()), 0);
    const auto listBefore = _target->IntegerList.getValues();
    const auto setBefore = set->getValues();
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::documentStructure(),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    char positive[] = "999999999999999999999999999999999999999999999999999999999999999999";
    char negative[] = "-999999999999999999999999999999999999999999999999999999999999999999";
    PyObjectRef tooLarge(PyLong_FromString(positive, nullptr, 10));
    PyObjectRef tooSmall(PyLong_FromString(negative, nullptr, 10));
    ASSERT_NE(tooLarge.get(), nullptr);
    ASSERT_NE(tooSmall.get(), nullptr);
    const auto reject = [&](const char* name, PyObject* value, PyObject* exception) {
        const int status = PyObject_SetAttrString(target.get(), name, value);
        const bool expected = PyErr_ExceptionMatches(exception);
        PyErr_Clear();
        EXPECT_EQ(status, -1) << name;
        EXPECT_TRUE(expected) << name;
        EXPECT_EQ(_target->IntegerList.getValues(), listBefore);
        EXPECT_EQ(set->getValues(), setBefore);
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(beforeSignals, 0);
        EXPECT_EQ(afterSignals, 0);
    };
    for (PyObject* overflow : {tooLarge.get(), tooSmall.get()}) {
        reject("IntegerList", overflow, PyExc_OverflowError);
        reject("IntegerSetProbe", overflow, PyExc_OverflowError);
        for (int position = 0; position < 3; ++position) {
            PyObjectRef items(PyList_New(3));
            ASSERT_NE(items.get(), nullptr);
            for (int index = 0; index < 3; ++index) {
                PyObject* item = index == position ? overflow : PyLong_FromLong(index + 1);
                ASSERT_NE(item, nullptr);
                if (index == position) Py_INCREF(item);
                PyList_SET_ITEM(items.get(), index, item);
            }
            reject("IntegerList", items.get(), PyExc_OverflowError);
            reject("IntegerSetProbe", items.get(), PyExc_OverflowError);
        }
        PyObjectRef indexed(PyDict_New());
        PyObjectRef zero(PyLong_FromLong(0));
        PyObjectRef one(PyLong_FromLong(1));
        PyObjectRef valid(PyLong_FromLong(77));
        ASSERT_NE(indexed.get(), nullptr);
        ASSERT_NE(zero.get(), nullptr);
        ASSERT_NE(one.get(), nullptr);
        ASSERT_NE(valid.get(), nullptr);
        ASSERT_EQ(PyDict_SetItem(indexed.get(), zero.get(), valid.get()), 0);
        ASSERT_EQ(PyDict_SetItem(indexed.get(), one.get(), overflow), 0);
        reject("IntegerList", indexed.get(), PyExc_OverflowError);
        PyObjectRef badKey(PyDict_New());
        ASSERT_NE(badKey.get(), nullptr);
        ASSERT_EQ(PyDict_SetItem(badKey.get(), overflow, valid.get()), 0);
        reject("IntegerList", badKey.get(), PyExc_OverflowError);
    }
    PyObjectRef badType(PyDict_New());
    PyObjectRef zero(PyLong_FromLong(0));
    PyObjectRef one(PyLong_FromLong(1));
    PyObjectRef valid(PyLong_FromLong(77));
    PyObjectRef invalid(PyUnicode_FromString("not an integer"));
    ASSERT_NE(badType.get(), nullptr);
    ASSERT_NE(zero.get(), nullptr);
    ASSERT_NE(one.get(), nullptr);
    ASSERT_NE(valid.get(), nullptr);
    ASSERT_NE(invalid.get(), nullptr);
    ASSERT_EQ(PyDict_SetItem(badType.get(), zero.get(), valid.get()), 0);
    ASSERT_EQ(PyDict_SetItem(badType.get(), one.get(), invalid.get()), 0);
    reject("IntegerList", badType.get(), PyExc_TypeError);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       integerListAndSetBindingsPreserveValidLongBounds)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* set = dynamic_cast<App::PropertyIntegerSet*>(
        _target->addDynamicProperty("App::PropertyIntegerSet", "IntegerSetProbe"));
    ASSERT_NE(set, nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef values(Py_BuildValue("[lll]", std::numeric_limits<long>::lowest(), -1L,
                                     std::numeric_limits<long>::max()));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(values.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerList", values.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerSetProbe", values.get()), 0);
    const std::vector<long> expected {std::numeric_limits<long>::lowest(), -1L,
                                      std::numeric_limits<long>::max()};
    EXPECT_EQ(_target->IntegerList.getValues(), expected);
    EXPECT_EQ(set->getValues(), (std::set<long>(expected.begin(), expected.end())));
    PyObjectRef indexed(Py_BuildValue("{l:l,l:l}", 0L, 9L, -1L, 8L));
    ASSERT_NE(indexed.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerList", indexed.get()), 0);
    EXPECT_EQ(_target->IntegerList.getValues(),
              (std::vector<long> {9L, -1L, std::numeric_limits<long>::max(), 8L}));
    PyObjectRef minusOne(PyLong_FromLong(-1));
    ASSERT_NE(minusOne.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerList", minusOne.get()), 0);
    EXPECT_EQ(_target->IntegerList.getValues(), (std::vector<long> {-1}));
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "IntegerSetProbe", minusOne.get()), 0);
    EXPECT_EQ(set->getValues(), (std::set<long> {-1}));
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       boolFloatColorAndPairListsRejectInvalidElementsWithoutPartialEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    auto* pairs = dynamic_cast<App::PropertyIntPairList*>(
        _target->addDynamicProperty("App::PropertyIntPairList", "PairProbe"));
    ASSERT_NE(pairs, nullptr);
    PyObjectRef target(_target->getPyObject());
    PyObjectRef numbers(Py_BuildValue("[lll]", 1L, 0L, 1L));
    PyObjectRef colors(Py_BuildValue("[(iii)(iii)]", 1, 2, 3, 4, 5, 6));
    PyObjectRef initialPairs(Py_BuildValue("[(ll)(ll)]", 1L, 2L, 3L, 4L));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(numbers.get(), nullptr);
    ASSERT_NE(colors.get(), nullptr);
    ASSERT_NE(initialPairs.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "BoolList", numbers.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "FloatList", numbers.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "ColourList", colors.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "PairProbe", initialPairs.get()), 0);
    const auto boolBefore = _target->BoolList.getValues();
    const auto floatBefore = _target->FloatList.getValues();
    PyObjectRef colorsBefore(PyObject_GetAttrString(target.get(), "ColourList"));
    const auto pairsBefore = pairs->getValues();
    ASSERT_NE(colorsBefore.get(), nullptr);
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    const auto fileState = _document->getFileChangeState();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    char positive[] = "999999999999999999999999999999999999999999999999999999999999999999";
    char negative[] = "-999999999999999999999999999999999999999999999999999999999999999999";
    PyObjectRef tooLarge(PyLong_FromString(positive, nullptr, 10));
    PyObjectRef tooSmall(PyLong_FromString(negative, nullptr, 10));
    ASSERT_NE(tooLarge.get(), nullptr);
    ASSERT_NE(tooSmall.get(), nullptr);
    const auto reject = [&](const char* name, PyObject* value,
                            PyObject* expectedException = PyExc_OverflowError) {
        const int status = PyObject_SetAttrString(target.get(), name, value);
        const bool expected = PyErr_ExceptionMatches(expectedException);
        PyErr_Clear();
        EXPECT_EQ(status, -1) << name;
        EXPECT_TRUE(expected) << name;
        EXPECT_EQ(_target->BoolList.getValues(), boolBefore);
        EXPECT_EQ(_target->FloatList.getValues(), floatBefore);
        PyObjectRef colorsAfter(PyObject_GetAttrString(target.get(), "ColourList"));
        ASSERT_NE(colorsAfter.get(), nullptr);
        EXPECT_EQ(PyObject_RichCompareBool(colorsBefore.get(), colorsAfter.get(), Py_EQ), 1);
        EXPECT_EQ(pairs->getValues(), pairsBefore);
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(_document->getFileChangeState(), fileState);
        EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        EXPECT_EQ(beforeSignals, 0);
        EXPECT_EQ(afterSignals, 0);
    };
    for (PyObject* overflow : {tooLarge.get(), tooSmall.get()}) {
        PyObjectRef numbersBad(Py_BuildValue("[lO]", 1L, overflow));
        PyObjectRef colorsBad(Py_BuildValue("[(iii)(Oii)]", 1, 2, 3, overflow, 5, 6));
        PyObjectRef pairsBad(Py_BuildValue("[(ll)(Ol)]", 1L, 2L, overflow, 4L));
        ASSERT_NE(numbersBad.get(), nullptr);
        ASSERT_NE(colorsBad.get(), nullptr);
        ASSERT_NE(pairsBad.get(), nullptr);
        reject("BoolList", overflow);
        reject("FloatList", overflow);
        reject("ColourList", overflow);
        reject("BoolList", numbersBad.get());
        reject("FloatList", numbersBad.get());
        reject("ColourList", colorsBad.get());
        reject("PairProbe", pairsBad.get());
        PyObjectRef validNumber(PyLong_FromLong(0));
        PyObjectRef validColor(Py_BuildValue("(iii)", 7, 8, 9));
        PyObjectRef invalidColor(Py_BuildValue("(Oii)", overflow, 2, 3));
        PyObjectRef validPair(Py_BuildValue("(ll)", 7L, 8L));
        PyObjectRef invalidPair(Py_BuildValue("(Ol)", overflow, 2L));
        ASSERT_NE(validNumber.get(), nullptr);
        ASSERT_NE(validColor.get(), nullptr);
        ASSERT_NE(invalidColor.get(), nullptr);
        ASSERT_NE(validPair.get(), nullptr);
        ASSERT_NE(invalidPair.get(), nullptr);
        const auto indexedReject = [&](const char* name, PyObject* first, PyObject* second) {
            PyObjectRef indexed(PyDict_New());
            PyObjectRef zero(PyLong_FromLong(0));
            PyObjectRef one(PyLong_FromLong(1));
            ASSERT_NE(indexed.get(), nullptr);
            ASSERT_NE(zero.get(), nullptr);
            ASSERT_NE(one.get(), nullptr);
            ASSERT_EQ(PyDict_SetItem(indexed.get(), zero.get(), first), 0);
            ASSERT_EQ(PyDict_SetItem(indexed.get(), one.get(), second), 0);
            reject(name, indexed.get());
        };
        indexedReject("BoolList", validNumber.get(), overflow);
        indexedReject("FloatList", validNumber.get(), overflow);
        indexedReject("ColourList", validColor.get(), invalidColor.get());
        indexedReject("PairProbe", validPair.get(), invalidPair.get());
    }
    PyObjectRef invalidType(PyUnicode_FromString("invalid element"));
    PyObjectRef validNumber(PyLong_FromLong(0));
    PyObjectRef validColor(Py_BuildValue("(iii)", 7, 8, 9));
    PyObjectRef validPair(Py_BuildValue("(ll)", 7L, 8L));
    PyObjectRef zero(PyLong_FromLong(0));
    PyObjectRef one(PyLong_FromLong(1));
    ASSERT_NE(invalidType.get(), nullptr);
    ASSERT_NE(validNumber.get(), nullptr);
    ASSERT_NE(validColor.get(), nullptr);
    ASSERT_NE(validPair.get(), nullptr);
    ASSERT_NE(zero.get(), nullptr);
    ASSERT_NE(one.get(), nullptr);
    for (const auto& entry : {std::pair<const char*, PyObject*> {"BoolList", validNumber.get()},
                              {"FloatList", validNumber.get()},
                              {"ColourList", validColor.get()},
                              {"PairProbe", validPair.get()}}) {
        PyObjectRef indexed(PyDict_New());
        ASSERT_NE(indexed.get(), nullptr);
        ASSERT_EQ(PyDict_SetItem(indexed.get(), zero.get(), entry.second), 0);
        ASSERT_EQ(PyDict_SetItem(indexed.get(), one.get(), invalidType.get()), 0);
        reject(entry.first, indexed.get(), PyExc_TypeError);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       colorAndEnumerationBindingsRejectOverflowBeforeAnyModelEffects)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef target(_target->getPyObject());
    PyObjectRef enumInitial(PyLong_FromLong(2));
    PyObjectRef colorInitial(Py_BuildValue("(iii)", 5, 6, 7));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(enumInitial.get(), nullptr);
    ASSERT_NE(colorInitial.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Enum", enumInitial.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Colour", colorInitial.get()), 0);
    const auto enumBefore = _target->Enum.getValue();
    const auto enumNamesBefore = _target->Enum.getEnumVector();
    const auto colorBefore = _target->Colour.getValue();
    const auto keys = std::vector<App::DocumentRevisionKey> {
        App::DocumentRevisionKey::objectModel("Target"),
        App::DocumentRevisionKey::objectStructure("Target"),
        App::DocumentRevisionKey::unknownModelMutation()};
    const auto revisions = _document->collaborationRevisions().capture(keys);
    const int undos = _document->getAvailableUndos();
    const auto fileState = _document->getFileChangeState();
    const auto fileChanges = _document->getPendingFileChanges().toUnderlyingType();
    int beforeSignals = 0;
    int afterSignals = 0;
    fastsignals::scoped_connection before = _document->signalBeforeChangeObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++beforeSignals;
        });
    fastsignals::scoped_connection after = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& object, const App::Property&) {
            if (&object == _target) ++afterSignals;
        });
    char positive[] = "999999999999999999999999999999999999999999999999999999999999999999";
    char negative[] = "-999999999999999999999999999999999999999999999999999999999999999999";
    PyObjectRef tooLarge(PyLong_FromString(positive, nullptr, 10));
    PyObjectRef tooSmall(PyLong_FromString(negative, nullptr, 10));
    ASSERT_NE(tooLarge.get(), nullptr);
    ASSERT_NE(tooSmall.get(), nullptr);
    const auto reject = [&](const char* name, PyObject* value) {
        const int status = PyObject_SetAttrString(target.get(), name, value);
        const bool overflow = PyErr_ExceptionMatches(PyExc_OverflowError);
        PyErr_Clear();
        EXPECT_EQ(status, -1) << name;
        EXPECT_TRUE(overflow) << name;
        EXPECT_EQ(_target->Enum.getValue(), enumBefore);
        EXPECT_EQ(_target->Enum.getEnumVector(), enumNamesBefore);
        const auto current = _target->Colour.getValue();
        EXPECT_FLOAT_EQ(current.r, colorBefore.r);
        EXPECT_FLOAT_EQ(current.g, colorBefore.g);
        EXPECT_FLOAT_EQ(current.b, colorBefore.b);
        EXPECT_FLOAT_EQ(current.a, colorBefore.a);
        EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
        EXPECT_EQ(_document->getAvailableUndos(), undos);
        EXPECT_EQ(_document->getFileChangeState(), fileState);
        EXPECT_EQ(_document->getPendingFileChanges().toUnderlyingType(), fileChanges);
        EXPECT_EQ(beforeSignals, 0);
        EXPECT_EQ(afterSignals, 0);
    };
    for (PyObject* overflow : {tooLarge.get(), tooSmall.get()}) {
        reject("Enum", overflow);
        PyObjectRef enumSequence(Py_BuildValue("([ss]O)", "First", "Second", overflow));
        ASSERT_NE(enumSequence.get(), nullptr);
        reject("Enum", enumSequence.get());
        reject("Colour", overflow);
        for (int position = 0; position < 4; ++position) {
            PyObjectRef tuple(PyTuple_New(4));
            ASSERT_NE(tuple.get(), nullptr);
            for (int index = 0; index < 4; ++index) {
                PyObject* item = index == position ? overflow : PyLong_FromLong(index + 1);
                ASSERT_NE(item, nullptr);
                if (index == position) Py_INCREF(item);
                PyTuple_SET_ITEM(tuple.get(), index, item);
            }
            reject("Colour", tuple.get());
        }
    }
    PyObjectRef minusOne(PyLong_FromLong(-1));
    ASSERT_NE(minusOne.get(), nullptr);
    reject("Colour", minusOne.get());
    PyObjectRef maxIndex(PyLong_FromLong(std::numeric_limits<long>::max()));
    ASSERT_NE(maxIndex.get(), nullptr);
    PyObjectRef badSelection(Py_BuildValue("([ss]O)", "First", "Second", maxIndex.get()));
    ASSERT_NE(badSelection.get(), nullptr);
    const int selectionStatus = PyObject_SetAttrString(target.get(), "Enum", badSelection.get());
    const bool rangeError = PyErr_ExceptionMatches(PyExc_ValueError);
    PyErr_Clear();
    EXPECT_EQ(selectionStatus, -1);
    EXPECT_TRUE(rangeError);
    EXPECT_EQ(_target->Enum.getValue(), enumBefore);
    EXPECT_EQ(_target->Enum.getEnumVector(), enumNamesBefore);
    EXPECT_EQ(_document->collaborationRevisions().capture(keys), revisions);
    EXPECT_EQ(beforeSignals, 0);
    EXPECT_EQ(afterSignals, 0);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       validBoolFloatColorAndEnumerationValuesPreserveExistingBehavior)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearPythonError;
    PyObjectRef target(_target->getPyObject());
    PyObjectRef negative(PyLong_FromLong(-1));
    PyObjectRef values(Py_BuildValue("[lll]", std::numeric_limits<long>::lowest(), -1L,
                                     std::numeric_limits<long>::max()));
    PyObjectRef color(Py_BuildValue("(iiii)", 0, 127, 255, 64));
    PyObjectRef index(PyLong_FromLong(3));
    ASSERT_NE(target.get(), nullptr);
    ASSERT_NE(negative.get(), nullptr);
    ASSERT_NE(values.get(), nullptr);
    ASSERT_NE(color.get(), nullptr);
    ASSERT_NE(index.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "FloatList", values.get()), 0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "BoolList", values.get()), 0);
    EXPECT_EQ(_target->FloatList.getValues().size(), 3U);
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[0], static_cast<double>(std::numeric_limits<long>::lowest()));
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[1], -1.0);
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[2], static_cast<double>(std::numeric_limits<long>::max()));
    EXPECT_TRUE(_target->BoolList.getValues().test(0));
    EXPECT_TRUE(_target->BoolList.getValues().test(1));
    EXPECT_TRUE(_target->BoolList.getValues().test(2));
    PyObjectRef indexed(Py_BuildValue("{l:l,l:l}", 0L, 0L, -1L, 7L));
    ASSERT_NE(indexed.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "FloatList", indexed.get()), 0);
    ASSERT_EQ(_target->FloatList.getValues().size(), 4U);
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[0], 0.0);
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[3], 7.0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "BoolList", indexed.get()), 0);
    ASSERT_EQ(_target->BoolList.getValues().size(), 4U);
    EXPECT_FALSE(_target->BoolList.getValues().test(0));
    EXPECT_TRUE(_target->BoolList.getValues().test(3));
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "FloatList", negative.get()), 0);
    ASSERT_EQ(_target->FloatList.getValues().size(), 1U);
    EXPECT_DOUBLE_EQ(_target->FloatList.getValues()[0], -1.0);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "BoolList", negative.get()), 0);
    ASSERT_EQ(_target->BoolList.getValues().size(), 1U);
    EXPECT_TRUE(_target->BoolList.getValues().test(0));
    PyObjectRef packedMax(PyLong_FromUnsignedLong(std::numeric_limits<unsigned long>::max()));
    ASSERT_NE(packedMax.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Colour", packedMax.get()), 0);
    Base::Color packedExpected;
    packedExpected.setPackedValue(static_cast<std::uint32_t>(std::numeric_limits<unsigned long>::max()));
    const auto packedActual = _target->Colour.getValue();
    EXPECT_FLOAT_EQ(packedActual.r, packedExpected.r);
    EXPECT_FLOAT_EQ(packedActual.g, packedExpected.g);
    EXPECT_FLOAT_EQ(packedActual.b, packedExpected.b);
    EXPECT_FLOAT_EQ(packedActual.a, packedExpected.a);
    PyObjectRef negativeComponent(Py_BuildValue("(lll)", -1L, 0L, 0L));
    ASSERT_NE(negativeComponent.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Colour", negativeComponent.get()), 0);
    EXPECT_FLOAT_EQ(_target->Colour.getValue().r, -1.0F / 255.0F);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Colour", color.get()), 0);
    const auto current = _target->Colour.getValue();
    EXPECT_FLOAT_EQ(current.r, 0.0F);
    EXPECT_FLOAT_EQ(current.g, 127.0F / 255.0F);
    EXPECT_FLOAT_EQ(current.b, 1.0F);
    EXPECT_FLOAT_EQ(current.a, 64.0F / 255.0F);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Enum", index.get()), 0);
    EXPECT_EQ(_target->Enum.getValue(), 3);
    PyObjectRef enumSequence(Py_BuildValue("([sss]i)", "First", "Second", "Third", 1));
    ASSERT_NE(enumSequence.get(), nullptr);
    ASSERT_EQ(PyObject_SetAttrString(target.get(), "Enum", enumSequence.get()), 0);
    EXPECT_EQ(_target->Enum.getValue(), 1);
    EXPECT_EQ(_target->Enum.getEnumVector(), (std::vector<std::string> {"First", "Second", "Third"}));
    const int status = PyObject_SetAttrString(target.get(), "Enum", negative.get());
    const bool rangeError = PyErr_ExceptionMatches(PyExc_ValueError);
    PyErr_Clear();
    EXPECT_EQ(status, -1);
    EXPECT_TRUE(rangeError);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       inheritedPersistenceReleasesBytearrayExportsAfterSuccessAndInvalidRestore)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearError;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    _target->String.setValue("saved value");
    PyObjectRef dump(PyObject_CallMethod(target.get(), "dumpContent", nullptr));
    ASSERT_NE(dump.get(), nullptr);
    ASSERT_TRUE(PyByteArray_Check(dump.get()));
    const auto originalSize = PyByteArray_Size(dump.get());
    ASSERT_GT(originalSize, 0);
    _target->String.setValue("changed value");
    PyObjectRef restored(PyObject_CallMethod(target.get(), "restoreContent", "O", dump.get()));
    ASSERT_NE(restored.get(), nullptr);
    EXPECT_STREQ(_target->String.getValue(), "saved value");
    EXPECT_EQ(PyByteArray_Resize(dump.get(), originalSize + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PyObjectRef invalid(PyByteArray_FromStringAndSize("not a zip archive", 17));
    ASSERT_NE(invalid.get(), nullptr);
    PyObjectRef failed(PyObject_CallMethod(target.get(), "restoreContent", "O", invalid.get()));
    EXPECT_EQ(failed.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_IOError));
    PyErr_Clear();
    EXPECT_EQ(PyByteArray_Resize(invalid.get(), 18), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PyObjectRef view(PyMemoryView_FromObject(invalid.get()));
    ASSERT_NE(view.get(), nullptr);
    PyObjectRef step(PyLong_FromLong(2));
    ASSERT_NE(step.get(), nullptr);
    PyObjectRef slice(PySlice_New(nullptr, nullptr, step.get()));
    ASSERT_NE(slice.get(), nullptr);
    PyObjectRef strided(PyObject_GetItem(view.get(), slice.get()));
    ASSERT_NE(strided.get(), nullptr);
    PyObjectRef failedStrided(PyObject_CallMethod(target.get(), "restoreContent", "O",
                                                  strided.get()));
    EXPECT_EQ(failedStrided.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_BufferError));
    PyErr_Clear();
}

#if defined(FREECAD_PERSISTENCE_TEST_API)
namespace
{
thread_local PyObject* observedPersistenceByteArray = nullptr;

// Own a copy of the raw exception triple at the operation boundary. Fetch and
// restore leave the *same* pending exception for the binding to propagate.
struct PersistenceContentErrorWitness final
{
    PyObject* type {nullptr};
    PyObject* value {nullptr};
    PyObject* traceback {nullptr};

    void record()
    {
        clear();
        PyErr_Fetch(&type, &value, &traceback);
        Py_XINCREF(type);
        Py_XINCREF(value);
        Py_XINCREF(traceback);
        PyErr_Restore(type, value, traceback); // steals the extra references
    }
    bool matchesAndClear()
    {
        PyObject* actualType = nullptr;
        PyObject* actualValue = nullptr;
        PyObject* actualTraceback = nullptr;
        PyErr_Fetch(&actualType, &actualValue, &actualTraceback);
        const bool matches = type && actualType == type && actualValue == value
            && actualTraceback == traceback;
        Py_XDECREF(actualType);
        Py_XDECREF(actualValue);
        Py_XDECREF(actualTraceback);
        return matches;
    }
    void clear()
    {
        Py_CLEAR(type);
        Py_CLEAR(value);
        Py_CLEAR(traceback);
    }
};
thread_local PersistenceContentErrorWitness persistenceContentErrorWitness;
thread_local Py_ssize_t persistenceContentFailureSize = PY_SSIZE_T_MAX;
thread_local PyObject* persistenceContentReadError = nullptr;
PyObject* witnessPersistenceByteArrayAllocationFailure(Py_ssize_t)
{
    PyObject* result = PyByteArray_FromStringAndSize(nullptr, persistenceContentFailureSize);
    if (!result) {
        persistenceContentErrorWitness.record();
    }
    return result;
}
int failPersistenceByteArrayBufferAcquire(PyObject* array, Py_buffer* view, int flags)
{
    Py_XDECREF(observedPersistenceByteArray);
    Py_INCREF(array);
    observedPersistenceByteArray = array;
    // A real readonly exporter rejects the actual writable buffer request.
    PyObject* readonly = PyMemoryView_FromMemory(PyByteArray_AsString(array),
                                                  PyByteArray_Size(array), PyBUF_READ);
    if (!readonly) return -1;
    const int status = PyObject_GetBuffer(readonly, view, flags);
    Py_DECREF(readonly);
    return status;
}
int acquirePersistenceStridedViewForTest(PyObject* object, Py_buffer* view, int)
{
    // Synthetic seam: request strides so a real strided exporter can reach the
    // explicit contiguity check. A normal PyBUF_SIMPLE request rejects it.
    return PyObject_GetBuffer(object, view, PyBUF_STRIDES);
}
void failPersistenceByteArrayRead(PyObject* array, std::istream& stream)
{
    Py_XDECREF(observedPersistenceByteArray);
    Py_INCREF(array);
    observedPersistenceByteArray = array;
    stream.setstate(std::ios::badbit);
}
void failPersistenceByteArrayReadWithError(PyObject* array, std::istream& stream)
{
    Py_XDECREF(observedPersistenceByteArray);
    Py_INCREF(array);
    observedPersistenceByteArray = array;
    PyErr_SetObject(PyExc_RuntimeError, persistenceContentReadError);
    persistenceContentErrorWitness.record();
    stream.setstate(std::ios::badbit);
}
struct PersistenceContentHooks final
{
    ~PersistenceContentHooks()
    {
        Base::Internal::setPersistenceContentByteArrayFactoryForTest(nullptr);
        Base::Internal::setPersistenceContentBeforeReadForTest(nullptr);
        Base::Internal::setPersistenceContentBufferAcquireForTest(nullptr);
        Py_CLEAR(observedPersistenceByteArray);
        Py_CLEAR(persistenceContentReadError);
        persistenceContentErrorWitness.clear();
        PyErr_Clear();
    }
};
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       inheritedPersistenceRejectsAndReleasesSyntheticStridedAcquisition)
{
    Base::PyGILStateLocker gil;
    PersistenceContentHooks hooks;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    PyObjectRef bytes(PyByteArray_FromStringAndSize("01234567", 8));
    ASSERT_NE(bytes.get(), nullptr);
    {
        PyObjectRef view(PyMemoryView_FromObject(bytes.get()));
        ASSERT_NE(view.get(), nullptr);
        PyObjectRef step(PyLong_FromLong(2));
        ASSERT_NE(step.get(), nullptr);
        PyObjectRef slice(PySlice_New(nullptr, nullptr, step.get()));
        ASSERT_NE(slice.get(), nullptr);
        PyObjectRef strided(PyObject_GetItem(view.get(), slice.get()));
        ASSERT_NE(strided.get(), nullptr);
        const auto references = Py_REFCNT(strided.get());
        Base::Internal::setPersistenceContentBufferAcquireForTest(&acquirePersistenceStridedViewForTest);
        PyObjectRef failed(PyObject_CallMethod(target.get(), "restoreContent", "O", strided.get()));
        EXPECT_EQ(failed.get(), nullptr);
        EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
        PyErr_Clear();
        EXPECT_EQ(Py_REFCNT(strided.get()), references);
        Base::Internal::setPersistenceContentBufferAcquireForTest(nullptr);
    }
    EXPECT_EQ(PyByteArray_Resize(bytes.get(), 9), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       inheritedPersistenceDumpReleasesResultAndBufferOnActualAllocationAndReadFailure)
{
    Base::PyGILStateLocker gil;
    PersistenceContentHooks hooks;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    Base::Internal::setPersistenceContentByteArrayFactoryForTest(&witnessPersistenceByteArrayAllocationFailure);
    for (const Py_ssize_t requested : {PY_SSIZE_T_MAX, PY_SSIZE_T_MAX - 1}) {
        persistenceContentFailureSize = requested;
        persistenceContentErrorWitness.clear();
        PyObjectRef allocation(PyObject_CallMethod(target.get(), "dumpContent", nullptr));
        EXPECT_EQ(allocation.get(), nullptr);
        // The second request reaches the allocator without the trailing-NUL
        // size overflow and must fail with MemoryError on supported hosts.
        if (requested == PY_SSIZE_T_MAX - 1) {
            EXPECT_EQ(persistenceContentErrorWitness.type, PyExc_MemoryError);
        }
        EXPECT_TRUE(persistenceContentErrorWitness.matchesAndClear());
    }
    Base::Internal::setPersistenceContentByteArrayFactoryForTest(nullptr);

    Base::Internal::setPersistenceContentBufferAcquireForTest(&failPersistenceByteArrayBufferAcquire);
    PyObjectRef failedAcquire(PyObject_CallMethod(target.get(), "dumpContent", nullptr));
    EXPECT_EQ(failedAcquire.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_BufferError));
    PyErr_Clear();
    Base::Internal::setPersistenceContentBufferAcquireForTest(nullptr);
    ASSERT_NE(observedPersistenceByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedPersistenceByteArray), 1);
    const auto acquiredSize = PyByteArray_Size(observedPersistenceByteArray);
    ASSERT_GE(acquiredSize, 0);
    EXPECT_EQ(PyByteArray_Resize(observedPersistenceByteArray, acquiredSize + 1), 0);
    Py_CLEAR(observedPersistenceByteArray);

    Base::Internal::setPersistenceContentBeforeReadForTest(&failPersistenceByteArrayRead);
    PyObjectRef failedRead(PyObject_CallMethod(target.get(), "dumpContent", nullptr));
    EXPECT_EQ(failedRead.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_IOError));
    PyErr_Clear();
    ASSERT_NE(observedPersistenceByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedPersistenceByteArray), 1);
    const auto size = PyByteArray_Size(observedPersistenceByteArray);
    ASSERT_GE(size, 0);
    EXPECT_EQ(PyByteArray_Resize(observedPersistenceByteArray, size + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    Py_CLEAR(observedPersistenceByteArray);

    persistenceContentReadError = PyObject_CallObject(PyExc_RuntimeError, nullptr);
    ASSERT_NE(persistenceContentReadError, nullptr);
    Base::Internal::setPersistenceContentBeforeReadForTest(&failPersistenceByteArrayReadWithError);
    PyObjectRef failedReadWithError(
        PyObject_CallMethod(target.get(), "dumpContent", nullptr));
    EXPECT_EQ(failedReadWithError.get(), nullptr);
    EXPECT_EQ(persistenceContentErrorWitness.type, PyExc_RuntimeError);
    EXPECT_EQ(persistenceContentErrorWitness.value, persistenceContentReadError);
    EXPECT_TRUE(persistenceContentErrorWitness.matchesAndClear());
    ASSERT_NE(observedPersistenceByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedPersistenceByteArray), 1);
    const auto errorSize = PyByteArray_Size(observedPersistenceByteArray);
    ASSERT_GE(errorSize, 0);
    EXPECT_EQ(PyByteArray_Resize(observedPersistenceByteArray, errorSize + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       propertyContentReleasesBytearrayExportsAfterSuccessAndInvalidRestore)
{
    Base::PyGILStateLocker gil;
    ScopedPythonErrorClear clearError;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    _target->Vector.setValue(4.0, 5.0, 6.0);
    PyObjectRef dump(PyObject_CallMethod(target.get(), "dumpPropertyContent", "s", "Vector"));
    ASSERT_NE(dump.get(), nullptr);
    ASSERT_TRUE(PyByteArray_Check(dump.get()));
    const auto originalSize = PyByteArray_Size(dump.get());
    ASSERT_GT(originalSize, 0);
    _target->Vector.setValue(0.0, 0.0, 0.0);
    PyObjectRef restored(PyObject_CallMethod(target.get(), "restorePropertyContent", "sO",
                                              "Vector", dump.get()));
    ASSERT_NE(restored.get(), nullptr);
    EXPECT_DOUBLE_EQ(_target->Vector.getValue().x, 4.0);
    EXPECT_DOUBLE_EQ(_target->Vector.getValue().y, 5.0);
    EXPECT_DOUBLE_EQ(_target->Vector.getValue().z, 6.0);
    EXPECT_EQ(PyByteArray_Resize(dump.get(), originalSize + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PyObjectRef invalid(PyByteArray_FromStringAndSize("not a zip archive", 17));
    ASSERT_NE(invalid.get(), nullptr);
    PyObjectRef failed(PyObject_CallMethod(target.get(), "restorePropertyContent", "sO",
                                            "Vector", invalid.get()));
    EXPECT_EQ(failed.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_IOError));
    PyErr_Clear();
    EXPECT_EQ(PyByteArray_Resize(invalid.get(), 18), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);

    PyObjectRef view(PyMemoryView_FromObject(invalid.get()));
    ASSERT_NE(view.get(), nullptr);
    PyObjectRef step(PyLong_FromLong(2));
    ASSERT_NE(step.get(), nullptr);
    PyObjectRef slice(PySlice_New(nullptr, nullptr, step.get()));
    ASSERT_NE(slice.get(), nullptr);
    PyObjectRef strided(PyObject_GetItem(view.get(), slice.get()));
    ASSERT_NE(strided.get(), nullptr);
    PyObjectRef failedStrided(PyObject_CallMethod(target.get(), "restorePropertyContent", "sO",
                                                  "Vector", strided.get()));
    EXPECT_EQ(failedStrided.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_BufferError));
    PyErr_Clear();
}

#if defined(FREECAD_DOCUMENTFILEWRITER_TEST_API)
namespace
{
thread_local PyObject* observedContentByteArray = nullptr;

// Own a copy of the raw exception triple at the operation boundary. Fetch and
// restore leave the *same* pending exception for the binding to propagate.
struct PropertyContentErrorWitness final
{
    PyObject* type {nullptr};
    PyObject* value {nullptr};
    PyObject* traceback {nullptr};

    void record()
    {
        clear();
        PyErr_Fetch(&type, &value, &traceback);
        Py_XINCREF(type);
        Py_XINCREF(value);
        Py_XINCREF(traceback);
        PyErr_Restore(type, value, traceback); // steals the extra references
    }
    bool matchesAndClear()
    {
        PyObject* actualType = nullptr;
        PyObject* actualValue = nullptr;
        PyObject* actualTraceback = nullptr;
        PyErr_Fetch(&actualType, &actualValue, &actualTraceback);
        const bool matches = type && actualType == type && actualValue == value
            && actualTraceback == traceback;
        Py_XDECREF(actualType);
        Py_XDECREF(actualValue);
        Py_XDECREF(actualTraceback);
        return matches;
    }
    void clear()
    {
        Py_CLEAR(type);
        Py_CLEAR(value);
        Py_CLEAR(traceback);
    }
};
thread_local PropertyContentErrorWitness propertyContentErrorWitness;
thread_local Py_ssize_t propertyContentFailureSize = PY_SSIZE_T_MAX;
thread_local PyObject* propertyContentReadError = nullptr;
PyObject* witnessPropertyByteArrayAllocationFailure(Py_ssize_t)
{
    PyObject* result = PyByteArray_FromStringAndSize(nullptr, propertyContentFailureSize);
    if (!result) {
        propertyContentErrorWitness.record();
    }
    return result;
}
int failPropertyByteArrayBufferAcquire(PyObject* array, Py_buffer* view, int flags)
{
    Py_XDECREF(observedContentByteArray);
    Py_INCREF(array);
    observedContentByteArray = array;
    // A real readonly exporter rejects the actual writable buffer request.
    PyObject* readonly = PyMemoryView_FromMemory(PyByteArray_AsString(array),
                                                  PyByteArray_Size(array), PyBUF_READ);
    if (!readonly) return -1;
    const int status = PyObject_GetBuffer(readonly, view, flags);
    Py_DECREF(readonly);
    return status;
}
int acquirePropertyStridedViewForTest(PyObject* object, Py_buffer* view, int)
{
    // Synthetic seam: request strides so a real strided exporter can reach the
    // explicit contiguity check. A normal PyBUF_SIMPLE request rejects it.
    return PyObject_GetBuffer(object, view, PyBUF_STRIDES);
}
void failPropertyByteArrayRead(PyObject* array, std::istream& stream)
{
    Py_XDECREF(observedContentByteArray);
    Py_INCREF(array);
    observedContentByteArray = array;
    stream.setstate(std::ios::badbit);
}
void failPropertyByteArrayReadWithError(PyObject* array, std::istream& stream)
{
    Py_XDECREF(observedContentByteArray);
    Py_INCREF(array);
    observedContentByteArray = array;
    PyErr_SetObject(PyExc_RuntimeError, propertyContentReadError);
    propertyContentErrorWitness.record();
    stream.setstate(std::ios::badbit);
}
struct PropertyContentHooks final
{
    ~PropertyContentHooks()
    {
        App::Internal::setPropertyContentByteArrayFactoryForTest(nullptr);
        App::Internal::setPropertyContentBeforeReadForTest(nullptr);
        App::Internal::setPropertyContentBufferAcquireForTest(nullptr);
        Py_CLEAR(observedContentByteArray);
        Py_CLEAR(propertyContentReadError);
        propertyContentErrorWitness.clear();
        PyErr_Clear();
    }
};
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       propertyContentRejectsAndReleasesSyntheticStridedAcquisition)
{
    Base::PyGILStateLocker gil;
    PropertyContentHooks hooks;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    PyObjectRef bytes(PyByteArray_FromStringAndSize("01234567", 8));
    ASSERT_NE(bytes.get(), nullptr);
    {
        PyObjectRef view(PyMemoryView_FromObject(bytes.get()));
        ASSERT_NE(view.get(), nullptr);
        PyObjectRef step(PyLong_FromLong(2));
        ASSERT_NE(step.get(), nullptr);
        PyObjectRef slice(PySlice_New(nullptr, nullptr, step.get()));
        ASSERT_NE(slice.get(), nullptr);
        PyObjectRef strided(PyObject_GetItem(view.get(), slice.get()));
        ASSERT_NE(strided.get(), nullptr);
        const auto references = Py_REFCNT(strided.get());
        App::Internal::setPropertyContentBufferAcquireForTest(&acquirePropertyStridedViewForTest);
        PyObjectRef failed(PyObject_CallMethod(target.get(), "restorePropertyContent", "sO",
                                                "Vector", strided.get()));
        EXPECT_EQ(failed.get(), nullptr);
        EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
        PyErr_Clear();
        EXPECT_EQ(Py_REFCNT(strided.get()), references);
        App::Internal::setPropertyContentBufferAcquireForTest(nullptr);
    }
    EXPECT_EQ(PyByteArray_Resize(bytes.get(), 9), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       propertyContentDumpReleasesResultAndBufferOnActualAllocationAndReadFailure)
{
    Base::PyGILStateLocker gil;
    PropertyContentHooks hooks;
    PyObjectRef target(_target->getPyObject());
    ASSERT_NE(target.get(), nullptr);
    App::Internal::setPropertyContentByteArrayFactoryForTest(&witnessPropertyByteArrayAllocationFailure);
    for (const Py_ssize_t requested : {PY_SSIZE_T_MAX, PY_SSIZE_T_MAX - 1}) {
        propertyContentFailureSize = requested;
        propertyContentErrorWitness.clear();
        PyObjectRef allocation(PyObject_CallMethod(target.get(), "dumpPropertyContent", "s", "Vector"));
        EXPECT_EQ(allocation.get(), nullptr);
        // The second request reaches the allocator without the trailing-NUL
        // size overflow and must fail with MemoryError on supported hosts.
        if (requested == PY_SSIZE_T_MAX - 1) {
            EXPECT_EQ(propertyContentErrorWitness.type, PyExc_MemoryError);
        }
        EXPECT_TRUE(propertyContentErrorWitness.matchesAndClear());
    }
    App::Internal::setPropertyContentByteArrayFactoryForTest(nullptr);

    App::Internal::setPropertyContentBufferAcquireForTest(&failPropertyByteArrayBufferAcquire);
    PyObjectRef failedAcquire(PyObject_CallMethod(target.get(), "dumpPropertyContent", "s", "Vector"));
    EXPECT_EQ(failedAcquire.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_BufferError));
    PyErr_Clear();
    App::Internal::setPropertyContentBufferAcquireForTest(nullptr);
    ASSERT_NE(observedContentByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedContentByteArray), 1);
    const auto acquiredSize = PyByteArray_Size(observedContentByteArray);
    ASSERT_GE(acquiredSize, 0);
    EXPECT_EQ(PyByteArray_Resize(observedContentByteArray, acquiredSize + 1), 0);
    Py_CLEAR(observedContentByteArray);

    App::Internal::setPropertyContentBeforeReadForTest(&failPropertyByteArrayRead);
    PyObjectRef failedRead(PyObject_CallMethod(target.get(), "dumpPropertyContent", "s", "Vector"));
    EXPECT_EQ(failedRead.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_IOError));
    PyErr_Clear();
    ASSERT_NE(observedContentByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedContentByteArray), 1);
    const auto size = PyByteArray_Size(observedContentByteArray);
    ASSERT_GE(size, 0);
    EXPECT_EQ(PyByteArray_Resize(observedContentByteArray, size + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
    Py_CLEAR(observedContentByteArray);

    propertyContentReadError = PyObject_CallObject(PyExc_RuntimeError, nullptr);
    ASSERT_NE(propertyContentReadError, nullptr);
    App::Internal::setPropertyContentBeforeReadForTest(&failPropertyByteArrayReadWithError);
    PyObjectRef failedReadWithError(
        PyObject_CallMethod(target.get(), "dumpPropertyContent", "s", "Vector"));
    EXPECT_EQ(failedReadWithError.get(), nullptr);
    EXPECT_EQ(propertyContentErrorWitness.type, PyExc_RuntimeError);
    EXPECT_EQ(propertyContentErrorWitness.value, propertyContentReadError);
    EXPECT_TRUE(propertyContentErrorWitness.matchesAndClear());
    ASSERT_NE(observedContentByteArray, nullptr);
    EXPECT_EQ(Py_REFCNT(observedContentByteArray), 1);
    const auto errorSize = PyByteArray_Size(observedContentByteArray);
    ASSERT_GE(errorSize, 0);
    EXPECT_EQ(PyByteArray_Resize(observedContentByteArray, errorSize + 1), 0);
    EXPECT_EQ(PyErr_Occurred(), nullptr);
}
#endif

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       saveAsOutcomeBindingPreservesTwoArgumentsAndAddsHashDurabilityAndWarnings)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));
    ScopedTemporaryPath canonical("fc_python_save_outcome_legacy_" + _documentName);
    PyObjectRef legacyResult(PyObject_CallMethod(document.get(),
                                                "saveAsWithOutcome",
                                                "sO",
                                                canonical.path.string().c_str(),
                                                Py_False));
    ASSERT_TRUE(pythonObjectAvailable(legacyResult.get()));
    ASSERT_TRUE(PyDict_Check(legacyResult.get()));
    EXPECT_EQ(PyObject_IsTrue(
                  PyDict_GetItemString(legacyResult.get(), "durability_verified")),
              1);
    PyObject* legacyWarnings = PyDict_GetItemString(legacyResult.get(), "warnings");
    ASSERT_NE(legacyWarnings, nullptr);
    EXPECT_TRUE(PyList_Check(legacyWarnings));

    ScopedTemporaryPath attempted("fc_python_save_outcome_hash_" + _documentName);
    const std::string expectedHash(64, 'a');
    PyObjectRef hashPolicyResult(PyObject_CallMethod(document.get(),
                                                    "saveAsWithOutcome",
                                                    "sOs",
                                                    attempted.path.string().c_str(),
                                                    Py_False,
                                                    expectedHash.c_str()));
    ASSERT_TRUE(pythonObjectAvailable(hashPolicyResult.get()));
    ASSERT_TRUE(PyDict_Check(hashPolicyResult.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(
                     PyDict_GetItemString(hashPolicyResult.get(), "error_code")),
                 "EXPECTED_DESTINATION_REQUIRES_OVERWRITE");
    EXPECT_EQ(PyObject_IsTrue(
                  PyDict_GetItemString(hashPolicyResult.get(), "file_written")),
              0);
    EXPECT_EQ(PyObject_IsTrue(
                  PyDict_GetItemString(hashPolicyResult.get(), "durability_verified")),
              0);
    PyObject* warnings = PyDict_GetItemString(hashPolicyResult.get(), "warnings");
    ASSERT_NE(warnings, nullptr);
    EXPECT_TRUE(PyList_Check(warnings));
    EXPECT_EQ(PyList_Size(warnings), 0);
    EXPECT_EQ(_document->FileName.getStrValue(), canonical.path.string());
    EXPECT_FALSE(std::filesystem::exists(attempted.path));
}

// The undo history shows a caller-supplied label instead of the operation UUID.
TEST_F(DocumentCollaborationPythonCompatibilityTest, labelNamesTheUndoTransaction)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_NE(document.get(), nullptr);
    CallbackProbe probe;
    probe.target = _target;
    probe.document = document.get();
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    PyObjectRef method(PyObject_GetAttrString(document.get(), "commitCompatibilityMutation"));
    ASSERT_NE(method.get(), nullptr);
    PyObjectRef positional(PyTuple_Pack(1, callback.get()));
    PyObjectRef keywords(Py_BuildValue("{s:s}", "label", "MCP: set_label"));
    ASSERT_NE(keywords.get(), nullptr);

    PyObjectRef labelled(PyObject_Call(method.get(), positional.get(), keywords.get()));
    ASSERT_NE(labelled.get(), nullptr);
    PyObject* status = PyDict_GetItemString(labelled.get(), "status");
    ASSERT_NE(status, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
    auto undoNames = _document->getAvailableUndoNames();
    ASSERT_FALSE(undoNames.empty());
    EXPECT_EQ(undoNames.front(), "MCP: set_label");

    // Restore the original value so the unlabelled callback records a real
    // change rather than repeating the same property value.
    ASSERT_TRUE(_document->undo());
    PyObjectRef unlabelled(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));
    ASSERT_NE(unlabelled.get(), nullptr);
    undoNames = _document->getAvailableUndoNames();
    ASSERT_FALSE(undoNames.empty());
    EXPECT_EQ(undoNames.front().rfind("Collaborative operation ", 0), 0U);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       emptyCompatibilityMutationPreservesUndoHead)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_NE(document.get(), nullptr);
    CallbackProbe changed;
    changed.target = _target;
    changed.document = document.get();
    PyObjectRef change(makeCompatibilityCallback(changed));
    PyObjectRef committed(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", change.get()));
    ASSERT_TRUE(pythonObjectAvailable(committed.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(committed.get(), "status")),
                 "Committed");
    const auto undoCount = _document->getAvailableUndos();
    const auto undoNames = _document->getAvailableUndoNames();
    ASSERT_GT(undoCount, 0);

    // With no target the callback only returns None, like a print-only script.
    CallbackProbe unchanged;
    unchanged.document = document.get();
    PyObjectRef noop(makeCompatibilityCallback(unchanged));
    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", noop.get()));
    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_EQ(_document->getAvailableUndos(), undoCount);
    EXPECT_EQ(_document->getAvailableUndoNames(), undoNames);
    ASSERT_TRUE(_document->undo());
    EXPECT_NE(_target->Label.getStrValue(), "Compatibility callback");
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       emptyCrossDocumentMarkerSurvivesOpeningNextTransaction)
{
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("crossDocumentUndoPeer");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    ASSERT_NE(foreign, nullptr);
    auto* target = foreign->addObject<App::FeatureTest>("Target");
    ASSERT_NE(target, nullptr);
    target->Label.setValue("Before");
    _document->clearUndos();
    foreign->clearUndos();

    // A only books the ID; B's edit creates the empty grouping marker in A.
    const auto id = _document->openTransaction("shared edit");
    ASSERT_NE(id, App::NullTransaction);
    ASSERT_EQ(foreign->openTransaction("shared edit", id), id);
    target->Label.setValue("Changed in B");
    ASSERT_TRUE(_document->hasPendingTransaction());
    ASSERT_EQ(_document->getTransactionID(true), id);
    ASSERT_EQ(foreign->getTransactionID(true), id);

    ASSERT_NE(_document->openTransaction("next"), App::NullTransaction);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(foreign->hasPendingTransaction());
    EXPECT_EQ(_document->getAvailableUndos(), 1);
    EXPECT_EQ(_document->getAvailableUndos(id), 1);
    EXPECT_EQ(_document->getAvailableUndoNames(),
              std::vector<std::string>({"-> shared edit"}));
    const auto jointUndoId = _document->getTransactionID(true);
    ASSERT_EQ(jointUndoId, id);
    ASSERT_EQ(foreign->getAvailableUndos(jointUndoId), 1);

    // App callers can undo both participants using A's preserved marker ID.
    ASSERT_TRUE(_document->undo(jointUndoId));
    ASSERT_TRUE(foreign->undo(jointUndoId));
    EXPECT_STREQ(target->Label.getValue(), "Before");
    EXPECT_STREQ(_target->Label.getValue(), "Before");
    EXPECT_EQ(_document->getAvailableUndos(), 0);
    EXPECT_EQ(foreign->getAvailableUndos(), 0);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       fileOnlyCompatibilityMutationRemainsUndoable)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_NE(document.get(), nullptr);
    const auto undoCount = _document->getAvailableUndos();
    ASSERT_FALSE(
        _document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));

    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.document = document.get();
    probe.markAppearanceFileChange = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));
    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    ASSERT_EQ(_document->getAvailableUndos(), undoCount + 1);
    EXPECT_TRUE(
        _document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_TRUE(_document->undo());
    EXPECT_FALSE(
        _document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
    ASSERT_TRUE(_document->redo());
    EXPECT_TRUE(
        _document->getPendingFileChanges().testFlag(App::DocumentFileChange::Appearance));
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       idlePresentationWaitReleasesPythonGilAndCommitMutexBeforeCallbackApplies)
{
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
    const auto wildcardBefore = wildcardRevision();
    const auto session = _document->collaborationService().beginEditSession("presentation-probe");
    std::optional<App::LivePresentationRefreshScope> refresh(std::in_place);
    ASSERT_TRUE(refresh->acquired());
    std::promise<void> entered;
    auto started = entered.get_future();
    auto result = _document->executionLane()->postToOwner([&] {
        Base::PyGILStateLocker gil;
        entered.set_value();
        return PyObject_CallMethod(document, "commitCompatibilityMutation", "O", callback);
    });
    started.wait();
    EXPECT_EQ(result.wait_for(100ms), std::future_status::timeout);
    // A GUI provider may need both the GIL and the document mutex to finish
    // its reserved refresh. Probe them before releasing that reservation.
    auto callbackAccess = std::async(std::launch::async, [&] {
        Base::PyGILStateLocker gil;
        const auto status = _document->collaborationService().sessionStatus(session.sessionId());
        return status.has_value() && _target->Label.getStrValue() == "Before";
    });
    EXPECT_EQ(callbackAccess.wait_for(100ms), std::future_status::ready);
    refresh.reset();
    EXPECT_TRUE(callbackAccess.get());
    PyObject* committed = result.get();
    {
        Base::PyGILStateLocker gil;
        ASSERT_NE(committed, nullptr);
        EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(committed, "status")), "Committed");
        EXPECT_EQ(probe.calls, 1);
        EXPECT_TRUE(probe.gilHeld);
        EXPECT_EQ(probe.callbackThread, _document->executionLane()->ownerThreadId());
        EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
        EXPECT_EQ(wildcardRevision(), wildcardBefore + 1);
        Py_DECREF(committed);
        Py_DECREF(callback);
        Py_DECREF(document);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       idlePresentationForeignAsyncCompatibilityHappyPathCommitsOnce)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.target = _target;
    probe.document = document.get();
    PyObjectRef callback(makeCompatibilityCallback(probe));
    PyObjectRef handle(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutationAsync", "O", callback.get()));
    ASSERT_NE(handle.get(), nullptr);
    ASSERT_TRUE(PyDict_Check(handle.get()));
    PyObject* wait = PyDict_GetItemString(handle.get(), "wait");
    ASSERT_NE(wait, nullptr);
    PyObjectRef result(PyObject_CallFunction(wait, "d", 5.0));
    ASSERT_NE(result.get(), nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")), "Committed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       idlePresentationNestedForeignAsyncCompatibilityRetainsTerminalBusyContext)
{
    const auto name = App::GetApplication().getUniqueDocumentName("foreignAsyncSerialized");
    ScopedDocumentClose closeForeign(name);
    auto* foreign = App::GetApplication().newDocument(name.c_str());
    auto* target = foreign->addObject<App::FeatureTest>("ForeignTarget");
    target->Label.setValue("Before");
    foreign->recompute();
    PyObject* document = nullptr;
    PyObject* callback = nullptr;
    CallbackProbe probe;
    probe.target = target;
    {
        Base::PyGILStateLocker gil;
        document = foreign->getPyObject();
        ASSERT_NE(document, nullptr);
        probe.document = document;
        callback = makeCompatibilityCallback(probe);
        ASSERT_NE(callback, nullptr);
    }
    const auto session = _document->collaborationService().beginEditSession("foreign-async-probe");
    PyObject* nested = nullptr;
    bool callFailed = false;
    std::optional<App::LivePresentationRefreshScope> refresh(std::in_place);
    ASSERT_TRUE(refresh->acquired());
    std::promise<void> entered;
    auto started = entered.get_future();
    auto result = _document->executionLane()->postToOwner([&] {
        Base::PyGILStateLocker gil;
        entered.set_value();
        return _document->collaborationService().serializeCompatibilityCallback([&] {
            PyObjectRef handle(PyObject_CallMethod(
                document, "commitCompatibilityMutationAsync", "O", callback));
            PyObject* wait = handle.get() && PyDict_Check(handle.get())
                ? PyDict_GetItemString(handle.get(), "wait") : nullptr;
            nested = wait ? PyObject_CallFunction(wait, "d", 5.0) : nullptr;
            callFailed = nested == nullptr;
            if (callFailed) {
                PyErr_Clear();
            }
        });
    });
    started.wait();
    EXPECT_EQ(result.wait_for(100ms), std::future_status::ready);
    auto callbackAccess = std::async(std::launch::async, [&] {
        return _document->collaborationService().sessionStatus(session.sessionId());
    });
    EXPECT_EQ(callbackAccess.wait_for(100ms), std::future_status::ready);
    auto gilAccess = std::async(std::launch::async, [] {
        Base::PyGILStateLocker gil;
        return PyGILState_Check() != 0;
    });
    EXPECT_EQ(gilAccess.wait_for(100ms), std::future_status::ready);
    refresh.reset();
    EXPECT_TRUE(gilAccess.get());
    EXPECT_TRUE(callbackAccess.get().has_value());
    EXPECT_TRUE(result.get().committed());
    {
        Base::PyGILStateLocker gil;
        EXPECT_FALSE(callFailed);
        if (nested) {
            EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(nested, "status")), "Busy");
        }
        EXPECT_EQ(probe.calls, 0);
        EXPECT_EQ(target->Label.getStrValue(), "Before");
        Py_XDECREF(nested);
        Py_DECREF(callback);
        Py_DECREF(document);
    }
}

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
    const auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(probe.callbackThread, lane->ownerThreadId());
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_EQ(wildcardRevision(), wildcardBefore + 1);
    EXPECT_EQ(Py_REFCNT(callback.get()), callbackReferences);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       applyCannotPublishReserveOrBindAndRevisionCancellationCannotLeakLock)
{
    Base::PyGILStateLocker gil;
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("revisionAdmissionForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    ASSERT_NE(foreign, nullptr);
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);

    const auto targetBefore = wildcardRevision();
    const auto foreignKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto foreignBefore = foreign->collaborationRevisions().current(foreignKey);
    auto foreignReservation = foreign->collaborationRevisions().reservePublication(
        {}, {{foreignKey, std::nullopt}});
    ASSERT_TRUE(foreignReservation.ready());

    DirectRevisionMutationProbe revisionProbe;
    revisionProbe.targetDocument = _document;
    revisionProbe.foreignDocument = foreign;
    revisionProbe.foreignReservation = &foreignReservation;
    CallbackProbe callbackProbe;
    callbackProbe.nativeDocument = _document;
    callbackProbe.target = _target;
    callbackProbe.foreignTarget = foreignTarget;
    callbackProbe.changeTargetLabel = true;
    callbackProbe.attemptHostileBoundaryRelease = true;
    callbackProbe.threadedLifecycleDocumentName =
        App::GetApplication().getUniqueDocumentName("HostileApplyLifecycle");
    callbackProbe.revisionMutationProbe = &revisionProbe;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PyObjectRef document(_document->getPyObject());

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, Py_None));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    expectAllDirectRevisionMutationsRejected(revisionProbe);
    EXPECT_TRUE(callbackProbe.hostileForeignMutationRejected);
    EXPECT_TRUE(callbackProbe.hostileLifecycleMutationRejected);
    EXPECT_TRUE(callbackProbe.hostileRevisionMutationRejected);
    EXPECT_EQ(App::GetApplication().getDocument(
                  callbackProbe.threadedLifecycleDocumentName.c_str()),
              nullptr);
    EXPECT_TRUE(revisionProbe.foreignCancelCompleted);
    EXPECT_FALSE(foreignReservation.ready());
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_EQ(wildcardRevision(), targetBefore + 1);
    EXPECT_EQ(foreign->collaborationRevisions().current(foreignKey), foreignBefore);

    foreignReservation.cancel();
    EXPECT_EQ(foreign->collaborationRevisions().current(foreignKey), foreignBefore);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       postconditionCannotPublishReserveOrBindTargetOrForeignRevisionState)
{
    Base::PyGILStateLocker gil;
    const auto foreignName = App::GetApplication().getUniqueDocumentName(
        "revisionPostconditionForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    ASSERT_NE(foreign, nullptr);
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    const auto targetBefore = wildcardRevision();
    const auto foreignKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto foreignBefore = foreign->collaborationRevisions().current(foreignKey);

    DirectRevisionMutationProbe revisionProbe;
    revisionProbe.targetDocument = _document;
    revisionProbe.foreignDocument = foreign;
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    callbackProbe.changeTargetLabel = true;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.document = _document;
    postconditionProbe.target = _target;
    postconditionProbe.foreignTarget = foreignTarget;
    postconditionProbe.attemptHostileBoundaryRelease = true;
    postconditionProbe.threadedLifecycleDocumentName =
        App::GetApplication().getUniqueDocumentName("HostilePostconditionLifecycle");
    postconditionProbe.revisionMutationProbe = &revisionProbe;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));
    PyObjectRef document(_document->getPyObject());

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PostconditionFailed");
    expectAllDirectRevisionMutationsRejected(revisionProbe);
    EXPECT_TRUE(postconditionProbe.hostileTargetMutationRejected);
    EXPECT_TRUE(postconditionProbe.hostileForeignMutationRejected);
    EXPECT_TRUE(postconditionProbe.hostileLifecycleMutationRejected);
    EXPECT_TRUE(postconditionProbe.hostileRevisionMutationRejected);
    EXPECT_EQ(App::GetApplication().getDocument(
                  postconditionProbe.threadedLifecycleDocumentName.c_str()),
              nullptr);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(wildcardRevision(), targetBefore);
    EXPECT_EQ(foreign->collaborationRevisions().current(foreignKey), foreignBefore);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_TRUE(_document->getMutationReadiness().ready);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       postconditionNoneAndTruePreserveSuccessfulLegacyCalls)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));

    CallbackProbe noneProbe;
    noneProbe.target = _target;
    PyObjectRef noneCallback(makeCompatibilityCallback(noneProbe));
    ASSERT_TRUE(pythonObjectAvailable(noneCallback.get()));
    PyObjectRef noneResult(callCompatibilityMutationWithOptions(
        document.get(), noneCallback.get(), false, true, Py_None));
    ASSERT_TRUE(pythonObjectAvailable(noneResult.get()));
    ASSERT_TRUE(PyDict_Check(noneResult.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(
                     PyDict_GetItemString(noneResult.get(), "status")),
                 "Committed");
    EXPECT_EQ(noneProbe.calls, 1);

    _target->Label.setValue("Before true postcondition");
    _document->recompute();
    CallbackProbe trueProbe;
    trueProbe.target = _target;
    PyObjectRef trueCallback(makeCompatibilityCallback(trueProbe));
    ASSERT_TRUE(pythonObjectAvailable(trueCallback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.document = _document;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));
    PyObjectRef trueResult(callCompatibilityMutationWithOptions(
        document.get(), trueCallback.get(), false, true, postcondition.get()));
    ASSERT_TRUE(pythonObjectAvailable(trueResult.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(
                     PyDict_GetItemString(trueResult.get(), "status")),
                 "Committed");
    EXPECT_EQ(trueProbe.calls, 1);
    EXPECT_EQ(postconditionProbe.calls, 1);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       falseyPostconditionReturnsStructuredFailureAndRollsBack)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.returnFalseyInteger = true;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));
    const auto wildcardBefore = wildcardRevision();

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PostconditionFailed");
    EXPECT_EQ(callbackProbe.calls, 1);
    EXPECT_EQ(postconditionProbe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       postconditionExceptionRollsBackThenRestoresOriginalPythonError)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.raisePythonError = true;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, postcondition.get()));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_ValueError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(error.message, "original compatibility postcondition failure");
    EXPECT_EQ(callbackProbe.calls, 1);
    EXPECT_EQ(postconditionProbe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       invalidPostconditionIsRejectedBeforeCallbackOrTransaction)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    PyObjectRef invalidPostcondition(PyLong_FromLong(7));

    PyObjectRef result(callCompatibilityMutationWithOptions(document.get(),
                                                            callback.get(),
                                                            false,
                                                            true,
                                                            invalidPostcondition.get()));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_TypeError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(callbackProbe.calls, 0);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       removedStructuralAuthorityKeywordIsRejectedBeforeCallbackOrTransaction)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PyObjectRef method(
        PyObject_GetAttrString(document.get(), "commitCompatibilityMutation"));
    PyObjectRef positional(PyTuple_Pack(1, callback.get()));
    PyObjectRef keywords(PyDict_New());
    ASSERT_TRUE(pythonObjectAvailable(method.get()));
    ASSERT_TRUE(pythonObjectAvailable(positional.get()));
    ASSERT_TRUE(pythonObjectAvailable(keywords.get()));
    const std::string removedKeyword = std::string("trusted_") + "structural";
    ASSERT_EQ(PyDict_SetItemString(
                  keywords.get(), removedKeyword.c_str(), Py_True),
              0);

    PyObjectRef result(PyObject_Call(method.get(), positional.get(), keywords.get()));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_TypeError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(callbackProbe.calls, 0);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       deferredPostconditionRunsExactlyOnceAfterApply)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    PostconditionProbe postconditionProbe;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, false, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_EQ(callbackProbe.calls, 1);
    EXPECT_EQ(postconditionProbe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       unsupportedStructuralFeatureFailsWithoutLiveExecutionAndRestoresExactBoundary)
{
    Base::PyGILStateLocker gil;
    auto* editorProperty = _target->addDynamicProperty(
        "App::PropertyString", "ExistingMetadataProperty");
    ASSERT_NE(editorProperty, nullptr);
    _document->recompute();
    const auto targetLabelBefore = _target->Label.getStrValue();
    const auto targetStatusBefore = _target->getStatus();
    const auto editorStatusBefore = editorProperty->getStatus();
    const auto undoCountBefore = _document->getAvailableUndos();
    const auto objectCountBefore = _document->getObjects().size();
    LazyStructuralExecuteFeature::TotalExecuteCalls = 0;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.nativeDocument = _document;
    callbackProbe.target = _target;
    callbackProbe.addLazyStructuralFeature = true;
    callbackProbe.lazyAddOwnSchema = true;
    callbackProbe.lazyChangeEditorStatus = true;
    callbackProbe.lazyAttemptRestrictedMutations = true;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), true, true, Py_None));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    ASSERT_TRUE(PyDict_Check(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "RecomputeFailed");
    EXPECT_EQ(callbackProbe.calls, 1);
    EXPECT_EQ(LazyStructuralExecuteFeature::TotalExecuteCalls, 0);
    EXPECT_EQ(_document->getObject("LazyStructuralFeature"), nullptr);
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_EQ(_target->Label.getStrValue(), targetLabelBefore);
    EXPECT_EQ(_target->getStatus(), targetStatusBefore);
    auto* restoredEditorProperty =
        _target->getPropertyByName("ExistingMetadataProperty");
    ASSERT_NE(restoredEditorProperty, nullptr);
    EXPECT_EQ(restoredEditorProperty->getStatus(), editorStatusBefore);
    EXPECT_EQ(_document->getAvailableUndos(), undoCountBefore);
    EXPECT_EQ(_document->getObjects().size(), objectCountBefore);
    EXPECT_EQ(_document->getObject("ForbiddenExecuteObject"), nullptr);
    EXPECT_EQ(_target->getPropertyByName("ForbiddenExistingSchema"), nullptr);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->getMutationReadiness().poisoned);
}




TEST_F(DocumentCollaborationPythonCompatibilityTest,
       stablePythonObserverRunsAfterReplayAndTargetUnwindDespiteThrowingSlots)
{
    Base::PyGILStateLocker gil;
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("stableObserverForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign before stable");
    foreign->recompute();

    fastsignals::scoped_connection applicationThrowingConnection =
        App::GetApplication().signalBecameStableDocument().connect(
            [&](const App::Document& document) {
                if (&document == _document) {
                    throw std::runtime_error("expected throwing application stable slot");
                }
            });

    PyObject* mainModule = PyImport_AddModule("__main__");
    ASSERT_TRUE(pythonObjectAvailable(mainModule));
    PyObject* globals = PyModule_GetDict(mainModule);
    ASSERT_TRUE(pythonObjectAvailable(globals));
    PyObjectRef pythonObserver(PyRun_String(
        "type('StableObserver', (), {"
        "'events': None, "
        "'__init__': lambda self: setattr(self, 'events', []), "
        "'slotRecomputedDocument': lambda self, doc: "
        "self.events.append(('recomputed', doc.getMutationReadiness())), "
        "'slotCommitTransaction': lambda self, doc: "
        "self.events.append(('commit', doc.getMutationReadiness())), "
        "'slotAbortTransaction': lambda self, doc: "
        "self.events.append(('abort', doc.getMutationReadiness())), "
        "'slotBecameStableDocument': lambda self, doc: "
        "self.events.append(('stable', doc.getMutationReadiness()))})()",
        Py_eval_input,
        globals,
        globals));
    ASSERT_TRUE(pythonObjectAvailable(pythonObserver.get()));
    PythonDocumentObserverGuard observerGuard(pythonObserver.get());

    int applicationSentinelCalls = 0;
    int documentSentinelCalls = 0;
    int crossDocumentMutationCalls = 0;
    fastsignals::scoped_connection applicationSentinelConnection =
        App::GetApplication().signalBecameStableDocument().connect(
            [&](const App::Document& document) {
                if (&document != _document) {
                    return;
                }
                ++applicationSentinelCalls;
                foreignTarget->Label.setValue("Foreign changed from stable");
                ++crossDocumentMutationCalls;
            });
    fastsignals::scoped_connection documentThrowingConnection =
        _document->signalBecameStable.connect(
        [&](const App::Document&) {
            throw std::runtime_error("expected throwing document stable slot");
        });
    fastsignals::scoped_connection documentSentinelConnection =
        _document->signalBecameStable.connect(
            [&](const App::Document&) { ++documentSentinelCalls; });

    PyObjectRef events(PyObject_GetAttrString(pythonObserver.get(), "events"));
    ASSERT_TRUE(pythonObjectAvailable(events.get()));
    ASSERT_TRUE(PyList_Check(events.get()));
    const auto eventName = [&](const Py_ssize_t index) {
        auto* event = PyList_GetItem(events.get(), index);
        if (!event || !PyTuple_Check(event) || PyTuple_Size(event) != 2) {
            return static_cast<const char*>(nullptr);
        }
        return PyUnicode_AsUTF8(PyTuple_GetItem(event, 0));
    };
    const auto eventReadiness = [&](const Py_ssize_t index) -> PyObject* {
        auto* event = PyList_GetItem(events.get(), index);
        return event && PyTuple_Check(event) && PyTuple_Size(event) == 2
            ? PyTuple_GetItem(event, 1)
            : nullptr;
    };
    const auto readinessFlag = [](PyObject* readiness, const char* key) {
        if (!readiness || !PyDict_Check(readiness)) {
            return -2;
        }
        auto* value = PyDict_GetItemString(readiness, key);
        return value ? PyObject_IsTrue(value) : -2;
    };

    PyObjectRef document(_document->getPyObject());
    CallbackProbe commitProbe;
    commitProbe.target = _target;
    PyObjectRef commitCallback(makeCompatibilityCallback(commitProbe));
    ASSERT_TRUE(pythonObjectAvailable(commitCallback.get()));
    PyObjectRef commitResult(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", commitCallback.get()));
    ASSERT_TRUE(pythonObjectAvailable(commitResult.get()));

    ASSERT_EQ(PyList_Size(events.get()), 3);
    EXPECT_STREQ(eventName(0), "recomputed");
    EXPECT_STREQ(eventName(1), "commit");
    EXPECT_STREQ(eventName(2), "stable");
    auto* recomputedReadiness = eventReadiness(0);
    auto* commitReadiness = eventReadiness(1);
    auto* stableReadiness = eventReadiness(2);
    ASSERT_NE(recomputedReadiness, nullptr);
    ASSERT_NE(commitReadiness, nullptr);
    ASSERT_NE(stableReadiness, nullptr);
    EXPECT_EQ(readinessFlag(recomputedReadiness, "notification_replay"), 1);
    EXPECT_EQ(readinessFlag(recomputedReadiness, "ready"), 0);
    EXPECT_EQ(readinessFlag(commitReadiness, "notification_replay"), 1);
    EXPECT_EQ(readinessFlag(commitReadiness, "ready"), 0);
    EXPECT_EQ(readinessFlag(stableReadiness, "stable_event_supported"), 1);
    EXPECT_EQ(readinessFlag(stableReadiness, "ready"), 1);
    EXPECT_EQ(readinessFlag(stableReadiness, "pending_removal"), 0);
    EXPECT_EQ(readinessFlag(stableReadiness, "notification_replay"), 0);
    EXPECT_EQ(applicationSentinelCalls, 1);
    EXPECT_EQ(documentSentinelCalls, 1);
    EXPECT_EQ(crossDocumentMutationCalls, 1);
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign changed from stable");

    ASSERT_EQ(PyList_SetSlice(events.get(), 0, PyList_Size(events.get()), nullptr), 0);
    _target->Label.setValue("Before failed stable boundary");
    _document->recompute();
    ASSERT_EQ(PyList_SetSlice(events.get(), 0, PyList_Size(events.get()), nullptr), 0);
    CallbackProbe abortProbe;
    abortProbe.target = _target;
    PyObjectRef abortCallback(makeCompatibilityCallback(abortProbe));
    PostconditionProbe falsePostconditionProbe;
    falsePostconditionProbe.satisfied = false;
    PyObjectRef falsePostcondition(
        makeCompatibilityPostcondition(falsePostconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(abortCallback.get()));
    ASSERT_TRUE(pythonObjectAvailable(falsePostcondition.get()));
    PyObjectRef abortResult(callCompatibilityMutationWithOptions(
        document.get(),
        abortCallback.get(),
        false,
        true,
        falsePostcondition.get()));
    ASSERT_TRUE(pythonObjectAvailable(abortResult.get()));
    ASSERT_EQ(PyList_Size(events.get()), 1);
    EXPECT_STREQ(eventName(0), "stable");
    auto* abortStableReadiness = eventReadiness(0);
    ASSERT_NE(abortStableReadiness, nullptr);
    EXPECT_EQ(readinessFlag(abortStableReadiness, "ready"), 1);

    ASSERT_EQ(PyList_SetSlice(events.get(), 0, PyList_Size(events.get()), nullptr), 0);
    _target->touch();
    _document->recompute();
    ASSERT_EQ(PyList_Size(events.get()), 2);
    EXPECT_STREQ(eventName(0), "recomputed");
    EXPECT_STREQ(eventName(1), "stable");
    auto* recomputeStableReadiness = eventReadiness(1);
    ASSERT_NE(recomputeStableReadiness, nullptr);
    EXPECT_EQ(readinessFlag(recomputeStableReadiness, "ready"), 1);
    EXPECT_GE(applicationSentinelCalls, 3);
    EXPECT_GE(documentSentinelCalls, 3);
    EXPECT_GE(crossDocumentMutationCalls, 3);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       stablePythonObserverSeesPendingRemovalComplete)
{
    Base::PyGILStateLocker gil;
    auto* object = _document->addObject<App::FeatureTest>("PendingRemoval");
    ASSERT_NE(object, nullptr);
    object->setStatus(App::ObjectStatus::PendingRecompute, true);
    _document->removeObject(object);
    object->setStatus(App::ObjectStatus::PendingRecompute, false);

    PyObject* mainModule = PyImport_AddModule("__main__");
    ASSERT_TRUE(pythonObjectAvailable(mainModule));
    PyObject* globals = PyModule_GetDict(mainModule);
    ASSERT_TRUE(pythonObjectAvailable(globals));
    PyObjectRef pythonObserver(PyRun_String(
        "type('PendingRemovalStableObserver', (), {"
        "'removed': False, "
        "'ready': False, "
        "'slotBecameStableDocument': lambda self, doc: "
        "(setattr(self, 'removed', doc.getObject('PendingRemoval') is None), "
        "setattr(self, 'ready', doc.getMutationReadiness()['ready']))})()",
        Py_eval_input,
        globals,
        globals));
    ASSERT_TRUE(pythonObjectAvailable(pythonObserver.get()));
    PythonDocumentObserverGuard observerGuard(pythonObserver.get());

    _document->recompute();

    PyObjectRef removed(PyObject_GetAttrString(pythonObserver.get(), "removed"));
    ASSERT_TRUE(pythonObjectAvailable(removed.get()));
    EXPECT_EQ(PyObject_IsTrue(removed.get()), 1);
    PyObjectRef ready(PyObject_GetAttrString(pythonObserver.get(), "ready"));
    ASSERT_TRUE(pythonObjectAvailable(ready.get()));
    EXPECT_EQ(PyObject_IsTrue(ready.get()), 1);
    EXPECT_EQ(_document->getObject("PendingRemoval"), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       postconditionIsReadOnlyAndCannotEscapeDocumentLifecycleBoundary)
{
    Base::PyGILStateLocker gil;
    auto rawStatus = std::make_unique<RawStatusFeature>();
    auto* rawStatusTarget = rawStatus.get();
    _document->addObject(rawStatus.get(), "RawStatusTarget");
    static_cast<void>(rawStatus.release());
    _document->recompute();

    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("postconditionForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign before");
    foreign->recompute();
    App::GetApplication().setActiveDocument(_document);

    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.nativeDocument = _document;
    callbackProbe.target = _target;
    callbackProbe.changeTargetLabel = true;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.document = _document;
    postconditionProbe.target = _target;
    postconditionProbe.foreignDocument = foreign;
    postconditionProbe.foreignTarget = foreignTarget;
    postconditionProbe.rawStatusTarget = rawStatusTarget;
    postconditionProbe.attemptRestrictedMutations = true;
    postconditionProbe.attemptOrdinaryValueMutation = true;
    postconditionProbe.attemptTouch = true;
    postconditionProbe.attemptRecompute = true;
    postconditionProbe.attemptForeignMutation = true;
    postconditionProbe.attemptNewDocument = true;
    postconditionProbe.attemptCloseDocument = true;
    postconditionProbe.attemptActiveDocumentChange = true;
    postconditionProbe.attemptRevisionPublication = true;
    postconditionProbe.bypassNoTouchGuard = true;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));
    const auto wildcardBefore = wildcardRevision();

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), true, true, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PostconditionFailed");
    EXPECT_TRUE(postconditionProbe.existingSchemaRejected);
    EXPECT_TRUE(postconditionProbe.newObjectSchemaRejected);
    EXPECT_TRUE(postconditionProbe.objectAddRejected);
    EXPECT_TRUE(postconditionProbe.objectRemoveRejected);
    EXPECT_TRUE(postconditionProbe.transactionControlRejected);
    EXPECT_TRUE(postconditionProbe.historyControlRejected);
    EXPECT_TRUE(postconditionProbe.ordinaryValueRejected);
    EXPECT_TRUE(postconditionProbe.touchRejected);
    EXPECT_TRUE(postconditionProbe.recomputeRejected);
    EXPECT_TRUE(postconditionProbe.foreignMutationRejected);
    EXPECT_TRUE(postconditionProbe.newDocumentRejected);
    EXPECT_TRUE(postconditionProbe.closeDocumentRejected);
    EXPECT_TRUE(postconditionProbe.activeDocumentChangeRejected);
    EXPECT_TRUE(postconditionProbe.revisionPublicationRejected);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(rawStatusTarget->testStatus(App::ObjectStatus::NoTouch));
    EXPECT_EQ(_document->getObject("LazyStructuralFeature"), nullptr);
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign before");
    EXPECT_EQ(App::GetApplication().getDocument(foreignName.c_str()), foreign);
    EXPECT_EQ(App::GetApplication().getDocument("ForbiddenPostconditionDocument"),
              nullptr);
    EXPECT_EQ(App::GetApplication().getActiveDocument(), _document);
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       compatibilityCallbackCannotMutateAnotherDocument)
{
    Base::PyGILStateLocker gil;
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("callbackForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign before");
    foreign->recompute();

    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.nativeDocument = _document;
    callbackProbe.target = _target;
    callbackProbe.foreignTarget = foreignTarget;
    callbackProbe.mutateForeignTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, Py_None));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_RuntimeError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(callbackProbe.calls, 1);
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign before");
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       joinedWorkerCannotEscapePreparedApplyMutationAdmission)
{
    Base::PyGILStateLocker gil;
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("threadedApplyForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign before");
    foreign->recompute();

    const auto lifecycleName =
        App::GetApplication().getUniqueDocumentName("threadedApplyLifecycle");
    ScopedDocumentClose closeLifecycle(lifecycleName);
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.nativeDocument = _document;
    callbackProbe.target = _target;
    callbackProbe.foreignTarget = foreignTarget;
    callbackProbe.changeTargetLabel = true;
    callbackProbe.attemptThreadedMutations = true;
    callbackProbe.threadedLifecycleDocumentName = lifecycleName;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, Py_None));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_TRUE(callbackProbe.threadedTargetMutationRejected);
    EXPECT_TRUE(callbackProbe.threadedForeignMutationRejected);
    EXPECT_TRUE(callbackProbe.threadedLifecycleMutationRejected);
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign before");
    EXPECT_EQ(App::GetApplication().getDocument(lifecycleName.c_str()), nullptr);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       joinedWorkerPostconditionMutationIsRejectedAndRollsBack)
{
    Base::PyGILStateLocker gil;
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("threadedPostconditionForeign");
    ScopedDocumentClose closeForeign(foreignName);
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str());
    auto* foreignTarget = foreign->addObject<App::FeatureTest>("ForeignTarget");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign before");
    foreign->recompute();

    const auto lifecycleName = App::GetApplication().getUniqueDocumentName(
        "threadedPostconditionLifecycle");
    ScopedDocumentClose closeLifecycle(lifecycleName);
    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    callbackProbe.changeTargetLabel = true;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    PostconditionProbe postconditionProbe;
    postconditionProbe.document = _document;
    postconditionProbe.target = _target;
    postconditionProbe.foreignDocument = foreign;
    postconditionProbe.foreignTarget = foreignTarget;
    postconditionProbe.attemptThreadedMutations = true;
    postconditionProbe.threadedLifecycleDocumentName = lifecycleName;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PostconditionFailed");
    EXPECT_TRUE(postconditionProbe.threadedTargetMutationRejected);
    EXPECT_TRUE(postconditionProbe.threadedForeignMutationRejected);
    EXPECT_TRUE(postconditionProbe.threadedLifecycleMutationRejected);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign before");
    EXPECT_EQ(App::GetApplication().getDocument(lifecycleName.c_str()), nullptr);
    EXPECT_FALSE(_document->hasPendingTransaction());
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       preparedApplyBlocksEverySaveIntentBeforeBookkeepingOrFilesystemAccess)
{
    Base::PyGILStateLocker gil;
    ScopedTemporaryPath canonical("fc_prepared_apply_save_" + _documentName);
    ScopedTemporaryPath saveAsDestination(
        "fc_prepared_apply_save_as_" + _documentName);
    ScopedTemporaryPath copyDestination(
        "fc_prepared_apply_copy_" + _documentName);
    ASSERT_EQ(_document->saveAsWithOutcome(canonical.path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
    ASSERT_FALSE(_document->lastCanonicalSaveFailed());

    const auto canonicalBytes = readFileBytes(canonical.path);
    const auto canonicalMtime = std::filesystem::last_write_time(canonical.path);
    const auto canonicalName = _document->FileName.getStrValue();
    const auto documentLabel = _document->Label.getStrValue();
    int startSaveSignals = 0;
    int saveOutcomeSignals = 0;
    auto startConnection = _document->signalStartSave.connect(
        [&](const App::Document&, const std::string&) { ++startSaveSignals; });
    auto outcomeConnection = _document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) {
            ++saveOutcomeSignals;
        });
    constexpr std::array attempts {
        SaveAttempt::LegacyCanonical,
        SaveAttempt::OutcomeCanonical,
        SaveAttempt::ForceCanonical,
        SaveAttempt::LegacySaveAs,
        SaveAttempt::PolicySaveAs,
        SaveAttempt::OutcomeSaveAs,
        SaveAttempt::LegacyCopy,
        SaveAttempt::OutcomeCopy,
        SaveAttempt::RecoverySnapshot,
        SaveAttempt::RecoveryOutcomeReport,
    };
    expectNoRecoveryArtifacts(*_document);
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));

    for (const auto attempt : attempts) {
        SCOPED_TRACE(static_cast<int>(attempt));
        CallbackProbe callbackProbe;
        callbackProbe.nativeDocument = _document;
        callbackProbe.target = _target;
        callbackProbe.changeTargetLabel = true;
        callbackProbe.saveAttempt = attempt;
        callbackProbe.saveAsPath = saveAsDestination.path.string();
        callbackProbe.copyPath = copyDestination.path.string();
        PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
        ASSERT_TRUE(pythonObjectAvailable(callback.get()));

        PyObjectRef result(callCompatibilityMutationWithOptions(
            document.get(), callback.get(), false, true, Py_None));

        EXPECT_EQ(result.get(), nullptr);
        const auto error = takePythonError(PyExc_RuntimeError);
        EXPECT_TRUE(error.present);
        EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
        EXPECT_EQ(_target->Label.getStrValue(), "Before");
        EXPECT_EQ(_document->FileName.getStrValue(), canonicalName);
        EXPECT_EQ(_document->Label.getStrValue(), documentLabel);
        EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
        EXPECT_FALSE(_document->hasPendingFileChanges());
        EXPECT_FALSE(_document->lastCanonicalSaveFailed());
        EXPECT_FALSE(_document->hasPendingTransaction());
        EXPECT_FALSE(_document->mustExecute());
        EXPECT_EQ(readFileBytes(canonical.path), canonicalBytes);
        EXPECT_TRUE(std::filesystem::last_write_time(canonical.path)
                    == canonicalMtime);
        EXPECT_FALSE(std::filesystem::exists(saveAsDestination.path));
        EXPECT_FALSE(std::filesystem::exists(copyDestination.path));
        expectNoRecoveryArtifacts(*_document);
        EXPECT_EQ(startSaveSignals, 0);
        EXPECT_EQ(saveOutcomeSignals, 0);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       readOnlyPostconditionBlocksEverySaveIntentWithoutFailureOverlay)
{
    Base::PyGILStateLocker gil;
    ScopedTemporaryPath canonical("fc_postcondition_save_" + _documentName);
    ScopedTemporaryPath saveAsDestination(
        "fc_postcondition_save_as_" + _documentName);
    ScopedTemporaryPath copyDestination(
        "fc_postcondition_copy_" + _documentName);
    ASSERT_EQ(_document->saveAsWithOutcome(canonical.path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
    ASSERT_FALSE(_document->lastCanonicalSaveFailed());

    const auto canonicalBytes = readFileBytes(canonical.path);
    const auto canonicalMtime = std::filesystem::last_write_time(canonical.path);
    const auto canonicalName = _document->FileName.getStrValue();
    const auto documentLabel = _document->Label.getStrValue();
    int startSaveSignals = 0;
    int saveOutcomeSignals = 0;
    auto startConnection = _document->signalStartSave.connect(
        [&](const App::Document&, const std::string&) { ++startSaveSignals; });
    auto outcomeConnection = _document->signalSaveOutcome().connect(
        [&](const App::Document&, const App::DocumentSaveOutcome&) {
            ++saveOutcomeSignals;
        });
    constexpr std::array attempts {
        SaveAttempt::LegacyCanonical,
        SaveAttempt::OutcomeCanonical,
        SaveAttempt::ForceCanonical,
        SaveAttempt::LegacySaveAs,
        SaveAttempt::PolicySaveAs,
        SaveAttempt::OutcomeSaveAs,
        SaveAttempt::LegacyCopy,
        SaveAttempt::OutcomeCopy,
        SaveAttempt::RecoverySnapshot,
        SaveAttempt::RecoveryOutcomeReport,
    };
    expectNoRecoveryArtifacts(*_document);
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));

    for (const auto attempt : attempts) {
        SCOPED_TRACE(static_cast<int>(attempt));
        CallbackProbe callbackProbe;
        callbackProbe.target = _target;
        callbackProbe.changeTargetLabel = true;
        PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
        ASSERT_TRUE(pythonObjectAvailable(callback.get()));
        PostconditionProbe postconditionProbe;
        postconditionProbe.document = _document;
        postconditionProbe.saveAttempt = attempt;
        postconditionProbe.saveAsPath = saveAsDestination.path.string();
        postconditionProbe.copyPath = copyDestination.path.string();
        PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
        ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));

        PyObjectRef result(callCompatibilityMutationWithOptions(
            document.get(), callback.get(), false, true, postcondition.get()));

        ASSERT_TRUE(pythonObjectAvailable(result.get()));
        EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                     "PostconditionFailed");
        EXPECT_TRUE(postconditionProbe.saveRejected);
        EXPECT_EQ(_target->Label.getStrValue(), "Before");
        EXPECT_EQ(_document->FileName.getStrValue(), canonicalName);
        EXPECT_EQ(_document->Label.getStrValue(), documentLabel);
        EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
        EXPECT_FALSE(_document->hasPendingFileChanges());
        EXPECT_FALSE(_document->lastCanonicalSaveFailed());
        EXPECT_FALSE(_document->hasPendingTransaction());
        EXPECT_FALSE(_document->mustExecute());
        EXPECT_EQ(readFileBytes(canonical.path), canonicalBytes);
        EXPECT_TRUE(std::filesystem::last_write_time(canonical.path)
                    == canonicalMtime);
        EXPECT_FALSE(std::filesystem::exists(saveAsDestination.path));
        EXPECT_FALSE(std::filesystem::exists(copyDestination.path));
        expectNoRecoveryArtifacts(*_document);
        EXPECT_EQ(startSaveSignals, 0);
        EXPECT_EQ(saveOutcomeSignals, 0);
    }
}




TEST_F(DocumentCollaborationPythonCompatibilityTest,
       falsePostconditionPreservesPreexistingStableErrorWithoutPoisoningRollback)
{
    Base::PyGILStateLocker gil;
    auto* existingError =
        _document->addObject<App::FeatureTest>("PreexistingStableError");
    ASSERT_NE(existingError, nullptr);
    existingError->ExceptionType.setValue(1);
    bool recomputeHasError = false;
    static_cast<void>(_document->recompute({}, true, &recomputeHasError));
    ASSERT_TRUE(recomputeHasError);
    ASSERT_TRUE(existingError->isError());
    const char* errorDescription = _document->getErrorDescription(existingError);
    ASSERT_NE(errorDescription, nullptr);
    const auto errorDescriptionBefore = std::string(errorDescription);
    existingError->purgeTouched();
    ASSERT_FALSE(_document->mustExecute());
    const auto errorStatusBefore = existingError->getStatus();

    PyObjectRef document(_document->getPyObject());
    CallbackProbe callbackProbe;
    callbackProbe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
    PostconditionProbe postconditionProbe;
    postconditionProbe.satisfied = false;
    PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));

    PyObjectRef result(callCompatibilityMutationWithOptions(
        document.get(), callback.get(), false, true, postcondition.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PostconditionFailed");
    EXPECT_FALSE(_document->getMutationReadiness().poisoned);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(existingError->getStatus(), errorStatusBefore);
    EXPECT_TRUE(existingError->isError());
    EXPECT_STREQ(_document->getErrorDescription(existingError),
                 errorDescriptionBefore.c_str());
    EXPECT_FALSE(_document->mustExecute());
}


TEST_F(DocumentCollaborationPythonCompatibilityTest,
       deferredPolicyCommitsWithoutConsumingPendingRecompute)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));
    auto pending = std::make_unique<LazyStructuralExecuteFeature>();
    auto* pendingFeature = pending.get();
    _document->addObject(pending.get(), "PendingDeferredExecute");
    static_cast<void>(pending.release());
    _document->recompute();
    pendingFeature->executeCalls = 0;
    pendingFeature->touch();
    ASSERT_TRUE(_document->mustExecute());

    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.document = document.get();
    probe.recomputeDocument = true;
    probe.recomputeTarget = true;
    probe.changeTargetLabel = true;
    probe.executeCalls = &pendingFeature->executeCalls;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));
    const auto wildcardBefore = wildcardRevision();

    PyObjectRef result(callDeferredCompatibilityMutation(document.get(), callback.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    ASSERT_TRUE(PyDict_Check(result.get()));
    PyObject* status = PyDict_GetItemString(result.get(), "status");
    ASSERT_NE(status, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.executeCallsObservedAfterCallbackRecompute, 0);
    EXPECT_EQ(pendingFeature->executeCalls, 0);
    EXPECT_EQ(_target->Label.getStrValue(), "Compatibility callback");
    EXPECT_TRUE(pendingFeature->isTouched());
    EXPECT_TRUE(_document->mustExecute());
    EXPECT_EQ(wildcardRevision(), wildcardBefore + 1);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       eagerPolicySettlesPreexistingPendingRecomputeBeforeCompatibilityCommit)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    _target->touch();
    ASSERT_TRUE(_document->mustExecute());

    CallbackProbe probe;
    probe.target = _target;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    PyObject* status = PyDict_GetItemString(result.get(), "status");
    ASSERT_NE(status, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_FALSE(_document->mustExecute());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       deferredFailureRestoresMutationWithoutConsumingPendingRecompute)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));
    auto unrelatedHolder = std::make_unique<LazyStructuralExecuteFeature>();
    auto* unrelated = unrelatedHolder.get();
    _document->addObject(unrelatedHolder.get(), "PendingUnrelated");
    static_cast<void>(unrelatedHolder.release());
    _document->recompute();
    unrelated->executeCalls = 0;
    ASSERT_FALSE(_target->isTouched());
    ASSERT_FALSE(_target->isError());
    unrelated->touch();
    ASSERT_TRUE(_document->mustExecute());
    const bool touchedBefore = _target->isTouched();
    const bool errorBefore = _target->isError();
    const auto targetStatusBefore = _target->getStatus();
    const auto unrelatedStatusBefore = unrelated->getStatus();
    const auto wildcardBefore = wildcardRevision();

    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.recomputeDocument = true;
    probe.recomputeTarget = true;
    probe.changeTargetLabel = true;
    probe.executeCalls = &unrelated->executeCalls;
    probe.raisePythonError = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(callDeferredCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_ValueError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.executeCallsObservedAfterCallbackRecompute, 0);
    EXPECT_EQ(unrelated->executeCalls, 0);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(_target->isTouched(), touchedBefore);
    EXPECT_EQ(_target->isError(), errorBefore);
    EXPECT_EQ(_target->getStatus(), targetStatusBefore);
    EXPECT_EQ(unrelated->getStatus(), unrelatedStatusBefore);
    EXPECT_TRUE(unrelated->isTouched());
    EXPECT_FALSE(unrelated->isError());
    EXPECT_TRUE(_document->mustExecute());
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       deferredFailureAtCleanBoundaryRestoresStableDocument)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));
    ASSERT_FALSE(_document->mustExecute());
    ASSERT_FALSE(_target->isTouched());
    ASSERT_FALSE(_target->isError());
    const auto wildcardBefore = wildcardRevision();

    CallbackProbe probe;
    probe.target = _target;
    probe.raisePythonError = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(callDeferredCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    const auto error = takePythonError(PyExc_ValueError);
    EXPECT_TRUE(error.present);
    EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_FALSE(_target->isTouched());
    EXPECT_FALSE(_target->isError());
    EXPECT_FALSE(_document->mustExecute());
    EXPECT_EQ(wildcardRevision(), wildcardBefore);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralMutationRequiresExplicitKeywordOptIn)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());

    PyObjectRef positionalResult(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "OO", callback.get(), Py_True));
    EXPECT_EQ(positionalResult.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();

    PyObjectRef method(
        PyObject_GetAttrString(document.get(), "commitCompatibilityMutation"));
    ASSERT_NE(method.get(), nullptr);
    PyObjectRef noPositional(PyTuple_New(0));
    ASSERT_NE(noPositional.get(), nullptr);
    PyObjectRef callbackKeyword(Py_BuildValue("{s:O}", "callback", callback.get()));
    ASSERT_NE(callbackKeyword.get(), nullptr);
    PyObjectRef keywordCallbackResult(
        PyObject_Call(method.get(), noPositional.get(), callbackKeyword.get()));
    EXPECT_EQ(keywordCallbackResult.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       nonStructuralCompatibilityRejectsDynamicExtensionBeforeNotification)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    const auto extensionType = Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(extensionType.isBad());
    int beforeExtension = 0;
    int addedExtension = 0;
    fastsignals::scoped_connection beforeConnection =
        App::GetApplication().signalBeforeAddingDynamicExtension.connect(
            [&](const auto&, const auto&) { ++beforeExtension; });
    fastsignals::scoped_connection addedConnection =
        App::GetApplication().signalAddedDynamicExtension.connect(
            [&](const auto&, const auto&) { ++addedExtension; });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.addExtensionToTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    EXPECT_FALSE(_target->hasExtension(extensionType, false));
    EXPECT_EQ(beforeExtension, 0);
    EXPECT_EQ(addedExtension, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       nonStructuralCompatibilityRejectsPropertyStatusAndMetadataChanges)
{
    Base::PyGILStateLocker gil;
    auto* property = _target->addDynamicProperty(
        "App::PropertyString",
        "ExistingMetadataProperty",
        "Original Group",
        "Original documentation");
    ASSERT_NE(property, nullptr);
    ASSERT_FALSE(property->testStatus(App::Property::ReadOnly));
    PyObjectRef document(_document->getPyObject());
    const auto cursor = publicationCursor();

    for (const std::string_view action : {"status", "metadata"}) {
        SCOPED_TRACE(action);
        CallbackProbe probe;
        probe.nativeDocument = _document;
        probe.target = _target;
        probe.setPropertyStatusOnTarget = action == "status";
        probe.changePropertyMetadataOnTarget = action == "metadata";
        PyObjectRef callback(makeCompatibilityCallback(probe));
        ASSERT_NE(callback.get(), nullptr);

        PyObjectRef result(PyObject_CallMethod(
            document.get(), "commitCompatibilityMutation", "O", callback.get()));

        EXPECT_EQ(result.get(), nullptr);
        EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
        PyErr_Clear();
        EXPECT_FALSE(property->testStatus(App::Property::ReadOnly));
        EXPECT_STREQ(property->getGroup(), "Original Group");
        EXPECT_STREQ(property->getDocumentation(), "Original documentation");
    }
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralScopeDoesNotGrantSchemaClearOrTransactionControl)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    auto* property = _target->addDynamicProperty(
        "App::PropertyString",
        "ExistingMetadataProperty",
        "Original Group",
        "Original documentation");
    ASSERT_NE(property, nullptr);
    const auto cursor = publicationCursor();

    const auto extensionType = Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(extensionType.isBad());
    for (const std::string_view action : {
             "schema",
             "schema-edit",
             "status",
             "metadata",
             "extension",
             "clear",
             "transaction",
             "undo",
             "redo",
             "locker"}) {
        SCOPED_TRACE(action);
        CallbackProbe probe;
        probe.nativeDocument = _document;
        probe.target = _target;
        probe.addDynamicProperty = action == "schema";
        probe.editDynamicPropertySchemaOnTarget = action == "schema-edit";
        probe.setPropertyStatusOnTarget = action == "status";
        probe.changePropertyMetadataOnTarget = action == "metadata";
        probe.addExtensionToTarget = action == "extension";
        probe.clearDocument = action == "clear";
        probe.openTransaction = action == "transaction";
        probe.undo = action == "undo";
        probe.redo = action == "redo";
        probe.transactionLocker = action == "locker";
        PyObjectRef callback(makeCompatibilityCallback(probe));
        ASSERT_NE(callback.get(), nullptr);

        PyObjectRef result(
            callStructuralCompatibilityMutation(document.get(), callback.get()));

        EXPECT_EQ(result.get(), nullptr);
        EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
        PyErr_Clear();
        EXPECT_EQ(_document->getObject("Target"), _target);
        EXPECT_EQ(_target->getPropertyByName("CompatibilityProperty"), nullptr);
        EXPECT_EQ(_target->getPropertyByName("ExistingMetadataProperty"), property);
        EXPECT_EQ(_target->getPropertyByName("RenamedMetadataProperty"), nullptr);
        EXPECT_FALSE(property->testStatus(App::Property::ReadOnly));
        EXPECT_STREQ(property->getGroup(), "Original Group");
        EXPECT_STREQ(property->getDocumentation(), "Original documentation");
        EXPECT_FALSE(_target->hasExtension(extensionType, false));
        EXPECT_FALSE(_document->hasPendingTransaction());
    }
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralCreateAllowsPostAddSchemaAndExtensionWithOrderedCommitReplay)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    const auto extensionType = Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(extensionType.isBad());
    std::vector<std::string> notifications;
    int beforeExtension = 0;
    int addedExtension = 0;
    int renamedSchema = 0;
    int removedSchema = 0;
    int appendedRenamedSchema = 0;
    int appendedRemovedSchema = 0;
    bool publicationVisible = false;
    const auto cursor = publicationCursor();
    fastsignals::scoped_connection newConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Transient") {
                notifications.emplace_back("new");
                publicationVisible = !_document->collaborationRevisions()
                                          .pollPublications(cursor)
                                          .events.empty();
            }
        });
    fastsignals::scoped_connection beforeConnection =
        App::GetApplication().signalBeforeAddingDynamicExtension.connect(
            [&](const App::ExtensionContainer& container, std::string extension) {
                if (&container == _document->getObject("Transient")) {
                    ++beforeExtension;
                    notifications.emplace_back("before-extension:" + extension);
                }
            });
    fastsignals::scoped_connection renameConnection =
        App::GetApplication().signalRenameDynamicProperty.connect(
            [&](const App::Property& property, const char* oldName) {
                if (property.getContainer() == _document->getObject("Transient")
                    && property.getName()
                    && std::string_view(property.getName()) == "RenamedCandidate"
                    && oldName && std::string_view(oldName) == "RenameCandidate") {
                    ++renamedSchema;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
            });
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _document->getObject("Transient")
                    && property.getName()
                    && std::string_view(property.getName()) == "RemoveCandidate") {
                    ++removedSchema;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
            });
    fastsignals::scoped_connection appendSchemaConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() != _document->getObject("Transient")
                    || !property.getName()) {
                    return;
                }
                if (std::string_view(property.getName()) == "RenamedCandidate") {
                    ++appendedRenamedSchema;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
                if (std::string_view(property.getName()) == "RemoveCandidate") {
                    ++appendedRemovedSchema;
                }
            });
    fastsignals::scoped_connection addedConnection =
        App::GetApplication().signalAddedDynamicExtension.connect(
            [&](const App::ExtensionContainer& container, std::string extension) {
                if (&container == _document->getObject("Transient")) {
                    ++addedExtension;
                    notifications.emplace_back("added-extension:" + extension);
                }
            });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    probe.changeTransientStaticProperty = true;
    probe.addDynamicPropertyToTransient = true;
    probe.setPropertyStatusOnTransient = true;
    probe.changePropertyMetadataOnTransient = true;
    probe.editDynamicPropertySchemaOnTransient = true;
    probe.addExtensionToTransient = true;
    probe.beforeExtensionObserverCalls = &beforeExtension;
    probe.addedExtensionObserverCalls = &addedExtension;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "new-object schema and extension mutation failed";
        return;
    }
    auto* transient = _document->getObject("Transient");
    ASSERT_NE(transient, nullptr);
    auto* property = transient->getPropertyByName("PostSetupCompatibilityProperty");
    ASSERT_NE(property, nullptr);
    EXPECT_TRUE(probe.extensionObserversDeferred);
    EXPECT_TRUE(property->testStatus(App::Property::ReadOnly));
    EXPECT_STREQ(property->getGroup(), "Compatibility Group");
    EXPECT_STREQ(property->getDocumentation(), "Compatibility documentation");
    EXPECT_NE(transient->getPropertyByName("RenamedCandidate"), nullptr);
    EXPECT_EQ(transient->getPropertyByName("RenameCandidate"), nullptr);
    EXPECT_EQ(transient->getPropertyByName("RemoveCandidate"), nullptr);
    EXPECT_TRUE(transient->hasExtension(extensionType, false));
    EXPECT_EQ(beforeExtension, 1);
    EXPECT_EQ(addedExtension, 1);
    EXPECT_EQ(renamedSchema, 0);
    EXPECT_EQ(removedSchema, 0);
    EXPECT_EQ(appendedRenamedSchema, 1);
    EXPECT_EQ(appendedRemovedSchema, 0);
    EXPECT_EQ(
        notifications,
        (std::vector<std::string> {
            "new",
            "before-extension:App::GroupExtensionPython",
            "added-extension:App::GroupExtensionPython",
        }));
    EXPECT_TRUE(publicationVisible);
    const auto poll = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    expectStructuralPublication(
        poll.events.front(),
        "Transient",
        _document->collaborationObjectIdentity(*transient));
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       transactionOwnedNewObjectApplyAndPostconditionFailuresRestoreCleanBaseline)
{
    Base::PyGILStateLocker gil;
    ScopedTemporaryPath canonical(
        "fc_new_object_failure_tokens_" + _documentName);
    ASSERT_EQ(_document->saveAsWithOutcome(canonical.path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
    ASSERT_FALSE(_document->hasPendingFileChanges());
    const auto undosBefore = _document->getAvailableUndos();
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));

    for (const bool failDuringApply : {true, false}) {
        SCOPED_TRACE(failDuringApply ? "apply" : "postcondition");
        CallbackProbe callbackProbe;
        callbackProbe.nativeDocument = _document;
        callbackProbe.addTransient = true;
        callbackProbe.addDynamicPropertyToTransient = true;
        callbackProbe.setPropertyStatusOnTransient = true;
        callbackProbe.changePropertyMetadataOnTransient = true;
        callbackProbe.raisePythonError = failDuringApply;
        PyObjectRef callback(makeCompatibilityCallback(callbackProbe));
        ASSERT_TRUE(pythonObjectAvailable(callback.get()));

        PostconditionProbe postconditionProbe;
        postconditionProbe.satisfied = failDuringApply;
        PyObjectRef postcondition(makeCompatibilityPostcondition(postconditionProbe));
        ASSERT_TRUE(pythonObjectAvailable(postcondition.get()));

        PyObjectRef result(callCompatibilityMutationWithOptions(
            document.get(), callback.get(), true, true, postcondition.get()));

        if (failDuringApply) {
            EXPECT_EQ(result.get(), nullptr);
            const auto error = takePythonError(PyExc_ValueError);
            EXPECT_TRUE(error.present);
            EXPECT_TRUE(error.matchesExpected) << error.type << ": " << error.message;
            EXPECT_EQ(postconditionProbe.calls, 0);
        }
        else {
            ASSERT_TRUE(pythonObjectAvailable(result.get()));
            EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                         "PostconditionFailed");
            EXPECT_EQ(postconditionProbe.calls, 1);
        }
        EXPECT_EQ(_document->getObject("Transient"), nullptr);
        EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
        EXPECT_FALSE(_document->hasPendingFileChanges());
        EXPECT_FALSE(_document->hasPendingTransaction());
        EXPECT_EQ(_document->getAvailableUndos(), undosBefore);
    }
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       isObjectNewDistinguishesAddedRemovedAndChangedTransactionRecords)
{
    App::Transaction addedRecord;
    addedRecord.addObjectDel(_target);
    ASSERT_TRUE(addedRecord.hasObject(_target));
    EXPECT_TRUE(addedRecord.isObjectNew(_target));

    App::Transaction changedRecord;
    changedRecord.addObjectChange(_target, &_target->Label);
    ASSERT_TRUE(changedRecord.hasObject(_target));
    EXPECT_FALSE(changedRecord.isObjectNew(_target));

    App::Transaction removedRecord;
    auto detached = std::make_unique<DetachedTransactionFeature>();
    removedRecord.addObjectNew(detached.get());
    ASSERT_TRUE(removedRecord.hasObject(detached.get()));
    auto* transactionOwnedDetachedObject = detached.release();
    EXPECT_FALSE(removedRecord.isObjectNew(transactionOwnedDetachedObject));
    // removedRecord owns and deletes the detached New-status snapshot.
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       transactionOwnedNewObjectStatusAndMetadataFollowCommittedUndoRedoFileState)
{
    Base::PyGILStateLocker gil;
    ScopedTemporaryPath canonical(
        "fc_new_object_history_tokens_" + _documentName);
    ASSERT_EQ(_document->saveAsWithOutcome(canonical.path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
    ASSERT_FALSE(_document->hasPendingFileChanges());
    PyObjectRef document(_document->getPyObject());
    ASSERT_TRUE(pythonObjectAvailable(document.get()));

    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    probe.addDynamicPropertyToTransient = true;
    probe.setPropertyStatusOnTransient = true;
    probe.changePropertyMetadataOnTransient = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_TRUE(pythonObjectAvailable(callback.get()));

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    ASSERT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    auto assertCommittedObject = [&]() {
        auto* object = _document->getObject("Transient");
        ASSERT_NE(object, nullptr);
        auto* property = object->getPropertyByName("PostSetupCompatibilityProperty");
        ASSERT_NE(property, nullptr);
        EXPECT_TRUE(property->testStatus(App::Property::ReadOnly));
        EXPECT_STREQ(property->getGroup(), "Compatibility Group");
        EXPECT_STREQ(property->getDocumentation(), "Compatibility documentation");
    };
    assertCommittedObject();
    EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Modified);
    EXPECT_TRUE(_document->getPendingFileChanges().testFlag(
        App::DocumentFileChange::Model));

    ASSERT_TRUE(_document->undo());
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);
    EXPECT_FALSE(_document->hasPendingFileChanges());

    ASSERT_TRUE(_document->redo());
    assertCommittedObject();
    EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Modified);
    EXPECT_TRUE(_document->getPendingFileChanges().testFlag(
        App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       existingObjectStatusAndMetadataRemainStickyAcrossAbort)
{
    auto* property = _target->addDynamicProperty(
        "App::PropertyString",
        "ExistingStickyMetadata",
        "Original Group",
        "Original documentation");
    ASSERT_NE(property, nullptr);
    ScopedTemporaryPath canonical(
        "fc_existing_object_sticky_tokens_" + _documentName);
    ASSERT_EQ(_document->saveAsWithOutcome(canonical.path.string().c_str()).disposition,
              App::DocumentSaveDisposition::Written);
    ASSERT_EQ(_document->getFileChangeState(), App::DocumentFileState::Clean);

    ASSERT_NE(_document->openTransaction("existing status and metadata"), 0);
    property->setStatus(App::Property::Hidden, true);
    ASSERT_TRUE(_target->changeDynamicProperty(
        property, "Changed Group", "Changed documentation"));
    _document->abortTransaction();

    EXPECT_TRUE(property->testStatus(App::Property::Hidden));
    EXPECT_STREQ(property->getGroup(), "Changed Group");
    EXPECT_STREQ(property->getDocumentation(), "Changed documentation");
    EXPECT_EQ(_document->getFileChangeState(), App::DocumentFileState::Modified);
    EXPECT_TRUE(_document->getPendingFileChanges().testFlag(
        App::DocumentFileChange::Model));
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       failedNewObjectExtensionRollsBackWithoutObserversOrPublication)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    int beforeExtension = 0;
    int addedExtension = 0;
    int propertyEditorSignals = 0;
    int renamedSchema = 0;
    int removedSchema = 0;
    fastsignals::scoped_connection beforeConnection =
        App::GetApplication().signalBeforeAddingDynamicExtension.connect(
            [&](const auto&, const auto&) { ++beforeExtension; });
    fastsignals::scoped_connection addedConnection =
        App::GetApplication().signalAddedDynamicExtension.connect(
            [&](const auto&, const auto&) { ++addedExtension; });
    fastsignals::scoped_connection propertyEditorConnection =
        _document->signalChangePropertyEditor.connect(
            [&](const App::Document&, const App::Property& property) {
                if (property.getName()
                    && std::string_view(property.getName())
                        == "PostSetupCompatibilityProperty") {
                    ++propertyEditorSignals;
                }
            });
    fastsignals::scoped_connection renameConnection =
        App::GetApplication().signalRenameDynamicProperty.connect(
            [&](const auto&, const auto*) { ++renamedSchema; });
    fastsignals::scoped_connection removeConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const auto&) { ++removedSchema; });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    probe.addDynamicPropertyToTransient = true;
    probe.setPropertyStatusOnTransient = true;
    probe.changePropertyMetadataOnTransient = true;
    probe.editDynamicPropertySchemaOnTransient = true;
    probe.addExtensionToTransient = true;
    probe.raisePythonError = true;
    probe.beforeExtensionObserverCalls = &beforeExtension;
    probe.addedExtensionObserverCalls = &addedExtension;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    EXPECT_TRUE(probe.extensionObserversDeferred);
    EXPECT_EQ(beforeExtension, 0);
    EXPECT_EQ(addedExtension, 0);
    EXPECT_EQ(propertyEditorSignals, 0);
    EXPECT_EQ(renamedSchema, 0);
    EXPECT_EQ(removedSchema, 0);
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralScopeRejectsDirectImportWithoutOwnedReplayScope)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    std::istringstream input("<Document/>");
    Base::XMLReader reader("<memory>", input);
    int importSignals = 0;
    int finishSignals = 0;
    fastsignals::scoped_connection importConnection = _document->signalImportObjects.connect(
        [&](const auto&, Base::XMLReader&) { ++importSignals; });
    fastsignals::scoped_connection finishConnection = _document->signalFinishImportObjects.connect(
        [&](const auto&) { ++finishSignals; });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.directImportReader = &reader;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    EXPECT_EQ(importSignals, 0);
    EXPECT_EQ(finishSignals, 0);
    EXPECT_EQ(_document->getObjects().size(), 1U);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralCallbackRecomputeRejectsUnserializableRuntimeTypeWithoutLiveExecution)
{
    Base::PyGILStateLocker gil;
    auto* executeProbe = new StructureAddingOnExecuteFeature;
    _document->addObject(executeProbe, "ExecuteProbe");
    _document->recompute();
    executeProbe->executeCalls = 0;
    executeProbe->attemptStructure = true;

    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = executeProbe;
    probe.touchTarget = true;
    probe.recomputeDocument = true;
    probe.recomputeTarget = true;
    probe.executeCalls = &executeProbe->executeCalls;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    ASSERT_TRUE(pythonObjectAvailable(result.get()));
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "RecomputeFailed");
    EXPECT_EQ(probe.executeCallsObservedAfterCallbackRecompute, 0);
    EXPECT_EQ(executeProbe->executeCalls, 0);
    EXPECT_FALSE(executeProbe->rejected);
    EXPECT_FALSE(executeProbe->admitted);
    EXPECT_EQ(_document->getObject("ExecuteBorrowedStructure"), nullptr);
    EXPECT_EQ(_document->getObject("ExecuteProbe"), executeProbe);
    EXPECT_FALSE(executeProbe->isTouched());
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       existingObjectUnsetupCannotBorrowNewObjectSchemaAuthority)
{
    Base::PyGILStateLocker gil;
    _document->removeObject("Target");
    _target = nullptr;
    auto* target = new SchemaAddingOnUnsetupFeature;
    _document->addObject(target, "Target");
    _document->recompute();

    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    int appendedDynamicProperty = 0;
    fastsignals::scoped_connection connection =
        App::GetApplication().signalAppendDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getName()
                && std::string_view(property.getName())
                    == "UnsetupCompatibilityProperty") {
                ++appendedDynamicProperty;
            }
        });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural delete with guarded unsetup failed";
        return;
    }
    EXPECT_TRUE(target->attempted);
    EXPECT_TRUE(target->rejected);
    EXPECT_FALSE(target->admitted);
    EXPECT_EQ(target->getPropertyByName("UnsetupCompatibilityProperty"), nullptr);
    EXPECT_EQ(appendedDynamicProperty, 0);
    EXPECT_EQ(_document->getObject("Target"), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       removingObjectOwnStatusCleanupUsesRemovalAuthority)
{
    Base::PyGILStateLocker gil;
    _document->removeObject("Target");
    _target = nullptr;
    auto* target = new StatusChangingOnUnsetupFeature;
    _document->addObject(target, "Target");
    _document->recompute();

    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural delete with status-changing unsetup failed";
        return;
    }
    EXPECT_TRUE(target->attempted);
    EXPECT_TRUE(target->admitted);
    EXPECT_FALSE(target->rejected);
    EXPECT_EQ(_document->getObject("Target"), nullptr);
    _target = nullptr;
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralRemovalOfPendingRecomputeObjectFailsClosed)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    _target->setStatus(App::ObjectStatus::PendingRecompute, true);
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
    _target->setStatus(App::ObjectStatus::PendingRecompute, false);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralCreatePublishesOneExactEventAfterObserverVisibility)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();
    int created = 0;
    bool revisionVisibleToObserver = false;
    fastsignals::scoped_connection connection = _document->signalNewObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Transient") {
                ++created;
                revisionVisibleToObserver = wildcardRevision() > 0;
            }
        });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural create compatibility mutation failed";
        return;
    }
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    auto* transient = _document->getObject("Transient");
    ASSERT_NE(transient, nullptr);
    EXPECT_EQ(created, 1);
    EXPECT_TRUE(revisionVisibleToObserver);
    const auto poll = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    expectStructuralPublication(
        poll.events.front(), "Transient", _document->collaborationObjectIdentity(*transient));
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralBulkImportDefersObserversRestoresDynamicPropertiesAndPublishesOnce)
{
    Base::PyGILStateLocker gil;
    const auto archive = makeStructuralImportArchive();
    PyObjectRef document(_document->getPyObject());
    int importedSignals = 0;
    int finishedSignals = 0;
    int finishRestoreSignals = 0;
    int newObjectSignals = 0;
    int dynamicPropertySignals = 0;
    int beforeExtensionSignals = 0;
    int addedExtensionSignals = 0;
    int renamedSchemaSignals = 0;
    int removedSchemaSignals = 0;
    int appendedRenamedSchemaSignals = 0;
    int appendedRemovedSchemaSignals = 0;
    fastsignals::scoped_connection importConnection = _document->signalImportObjects.connect(
        [&](const auto&, Base::XMLReader&) { ++importedSignals; });
    fastsignals::scoped_connection finishConnection = _document->signalFinishImportObjects.connect(
        [&](const auto&) { ++finishedSignals; });
    fastsignals::scoped_connection finishRestoreConnection =
        _document->signalFinishRestoreObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedFeature") {
                ++finishRestoreSignals;
            }
        });
    fastsignals::scoped_connection newConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedFeature") {
                ++newObjectSignals;
            }
        });
    fastsignals::scoped_connection dynamicConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getName()
                && std::string_view(property.getName()) == "ImportedText") {
                ++dynamicPropertySignals;
            }
        });
    fastsignals::scoped_connection beforeExtensionConnection =
        App::GetApplication().signalBeforeAddingDynamicExtension.connect(
            [&](const App::ExtensionContainer& container, const std::string& extension) {
                if (&container == _document->getObject("ImportedFeature")
                    && extension == "App::GroupExtensionPython") {
                    ++beforeExtensionSignals;
                }
            });
    fastsignals::scoped_connection addedExtensionConnection =
        App::GetApplication().signalAddedDynamicExtension.connect(
            [&](const App::ExtensionContainer& container, const std::string& extension) {
                if (&container == _document->getObject("ImportedFeature")
                    && extension == "App::GroupExtensionPython") {
                    ++addedExtensionSignals;
                }
            });
    fastsignals::scoped_connection renameSchemaConnection =
        App::GetApplication().signalRenameDynamicProperty.connect(
            [&](const App::Property& property, const char* oldName) {
                if (property.getContainer() == _document->getObject("ImportedFeature")
                    && property.getName()
                    && std::string_view(property.getName())
                        == "ImportedRenamedCandidate"
                    && oldName
                    && std::string_view(oldName) == "ImportedRenameCandidate") {
                    ++renamedSchemaSignals;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
            });
    fastsignals::scoped_connection removeSchemaConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() == _document->getObject("ImportedFeature")
                    && property.getName()
                    && std::string_view(property.getName())
                        == "ImportedRemoveCandidate") {
                    ++removedSchemaSignals;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
            });
    fastsignals::scoped_connection appendSchemaConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getContainer() != _document->getObject("ImportedFeature")
                    || !property.getName()) {
                    return;
                }
                if (std::string_view(property.getName())
                    == "ImportedRenamedCandidate") {
                    ++appendedRenamedSchemaSignals;
                    EXPECT_FALSE(_document->hasPendingTransaction());
                }
                if (std::string_view(property.getName())
                    == "ImportedRemoveCandidate") {
                    ++appendedRemovedSchemaSignals;
                }
            });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.importArchive = &archive;
    App::MergeDocuments retainedImporter(_document);
    probe.retainedImporter = &retainedImporter;
    probe.importObserverCalls = &importedSignals;
    probe.finishImportObserverCalls = &finishedSignals;
    probe.finishRestoreObserverCalls = &finishRestoreSignals;
    probe.newObjectObserverCalls = &newObjectSignals;
    probe.dynamicPropertyObserverCalls = &dynamicPropertySignals;
    probe.setPropertyStatusOnImported = true;
    probe.changePropertyMetadataOnImported = true;
    probe.editDynamicPropertySchemaOnImported = true;
    probe.addExtensionToImported = true;
    probe.beforeExtensionObserverCalls = &beforeExtensionSignals;
    probe.addedExtensionObserverCalls = &addedExtensionSignals;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural bulk import compatibility mutation failed";
        return;
    }
    EXPECT_TRUE(probe.importObserversDeferred);
    EXPECT_TRUE(probe.importedDynamicPropertyRestored);
    EXPECT_TRUE(probe.extensionObserversDeferred);
    EXPECT_EQ(importedSignals, 1);
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(finishRestoreSignals, 1);
    EXPECT_EQ(newObjectSignals, 1);
    EXPECT_EQ(dynamicPropertySignals, 1);
    EXPECT_EQ(beforeExtensionSignals, 1);
    EXPECT_EQ(addedExtensionSignals, 1);
    EXPECT_EQ(renamedSchemaSignals, 0);
    EXPECT_EQ(removedSchemaSignals, 0);
    EXPECT_EQ(appendedRenamedSchemaSignals, 1);
    EXPECT_EQ(appendedRemovedSchemaSignals, 0);
    auto* imported = _document->getObject("ImportedFeature");
    ASSERT_NE(imported, nullptr);
    const auto extensionType = Base::Type::fromName("App::GroupExtensionPython");
    ASSERT_FALSE(extensionType.isBad());
    EXPECT_TRUE(imported->hasExtension(extensionType, false));
    auto* property = freecad_cast<App::PropertyString*>(
        imported->getPropertyByName("ImportedText"));
    ASSERT_NE(property, nullptr);
    EXPECT_EQ(property->getStrValue(), "restored");
    EXPECT_TRUE(property->testStatus(App::Property::ReadOnly));
    EXPECT_STREQ(property->getGroup(), "Compatibility Group");
    EXPECT_STREQ(property->getDocumentation(), "Compatibility documentation");
    EXPECT_NE(imported->getPropertyByName("ImportedRenamedCandidate"), nullptr);
    EXPECT_EQ(imported->getPropertyByName("ImportedRenameCandidate"), nullptr);
    EXPECT_EQ(imported->getPropertyByName("ImportedRemoveCandidate"), nullptr);
    const auto poll = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    expectStructuralPublication(
        poll.events.front(),
        "ImportedFeature",
        _document->collaborationObjectIdentity(*imported));

    Base::StringIStreambuf reimportBuffer(archive);
    std::istream reimportInput(&reimportBuffer);
    const auto reimported = retainedImporter.importObjects(reimportInput);
    ASSERT_EQ(reimported.size(), 1U);
    auto* reimportedProperty = freecad_cast<App::PropertyString*>(
        reimported.front()->getPropertyByName("ImportedText"));
    ASSERT_NE(reimportedProperty, nullptr);
    EXPECT_EQ(reimportedProperty->getStrValue(), "restored");
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       ordinaryBulkImportRemainsSynchronousAndNonStructuralCompatibilityRejects)
{
    Base::PyGILStateLocker gil;
    const auto archive = makeStructuralImportArchive();
    PyObjectRef document(_document->getPyObject());
    int importedSignals = 0;
    int finishedSignals = 0;
    fastsignals::scoped_connection importConnection = _document->signalImportObjects.connect(
        [&](const auto&, Base::XMLReader&) { ++importedSignals; });
    fastsignals::scoped_connection finishConnection = _document->signalFinishImportObjects.connect(
        [&](const auto&) { ++finishedSignals; });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.importArchive = &archive;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef rejected(PyObject_CallMethod(
        document.get(), "commitCompatibilityMutation", "O", callback.get()));

    EXPECT_EQ(rejected.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_RuntimeError));
    PyErr_Clear();
    EXPECT_EQ(_document->getObject("ImportedFeature"), nullptr);
    EXPECT_EQ(importedSignals, 0);
    EXPECT_EQ(finishedSignals, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());

    Base::StringIStreambuf buffer(archive);
    std::istream input(&buffer);
    App::MergeDocuments importer(_document);
    const auto imported = importer.importObjects(input);
    ASSERT_EQ(imported.size(), 1U);
    EXPECT_EQ(importedSignals, 1);
    EXPECT_EQ(finishedSignals, 1);
    EXPECT_EQ(_document->getObject("ImportedFeature"), imported.front());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       failedStructuralBulkImportRollsBackWithoutObserverOrPublicationLeak)
{
    Base::PyGILStateLocker gil;
    const auto archive = makeStructuralImportArchive();
    PyObjectRef document(_document->getPyObject());
    int importedSignals = 0;
    int finishedSignals = 0;
    int finishRestoreSignals = 0;
    int newObjectSignals = 0;
    int dynamicPropertySignals = 0;
    int renamedSchemaSignals = 0;
    int removedSchemaSignals = 0;
    fastsignals::scoped_connection importConnection = _document->signalImportObjects.connect(
        [&](const auto&, Base::XMLReader&) { ++importedSignals; });
    fastsignals::scoped_connection finishConnection = _document->signalFinishImportObjects.connect(
        [&](const auto&) { ++finishedSignals; });
    fastsignals::scoped_connection finishRestoreConnection =
        _document->signalFinishRestoreObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedFeature") {
                ++finishRestoreSignals;
            }
        });
    fastsignals::scoped_connection newConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "ImportedFeature") {
                ++newObjectSignals;
            }
        });
    fastsignals::scoped_connection dynamicConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
        [&](const App::Property& property) {
            if (property.getName()
                && std::string_view(property.getName()) == "ImportedText") {
                ++dynamicPropertySignals;
            }
        });
    fastsignals::scoped_connection renameSchemaConnection =
        App::GetApplication().signalRenameDynamicProperty.connect(
            [&](const auto&, const auto*) { ++renamedSchemaSignals; });
    fastsignals::scoped_connection removeSchemaConnection =
        App::GetApplication().signalRemoveDynamicProperty.connect(
            [&](const auto&) { ++removedSchemaSignals; });
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.importArchive = &archive;
    probe.importObserverCalls = &importedSignals;
    probe.finishImportObserverCalls = &finishedSignals;
    probe.finishRestoreObserverCalls = &finishRestoreSignals;
    probe.newObjectObserverCalls = &newObjectSignals;
    probe.dynamicPropertyObserverCalls = &dynamicPropertySignals;
    probe.editDynamicPropertySchemaOnImported = true;
    probe.raisePythonError = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    EXPECT_TRUE(probe.importObserversDeferred);
    EXPECT_TRUE(probe.importedDynamicPropertyRestored);
    EXPECT_EQ(importedSignals, 0);
    EXPECT_EQ(finishedSignals, 0);
    EXPECT_EQ(finishRestoreSignals, 0);
    EXPECT_EQ(newObjectSignals, 0);
    EXPECT_EQ(dynamicPropertySignals, 0);
    EXPECT_EQ(renamedSchemaSignals, 0);
    EXPECT_EQ(removedSchemaSignals, 0);
    EXPECT_EQ(_document->getObject("ImportedFeature"), nullptr);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
    EXPECT_FALSE(_document->hasPendingTransaction());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralDeletePublishesOneExactEventAndOneObserverNotification)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();
    const auto targetIdentity = _document->collaborationObjectIdentity(*_target);
    int deleted = 0;
    fastsignals::scoped_connection connection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Target") {
                ++deleted;
            }
        });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural delete compatibility mutation failed";
        return;
    }
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_EQ(_document->getObject("Target"), nullptr);
    EXPECT_EQ(deleted, 1);
    const auto poll = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    expectStructuralPublication(poll.events.front(), "Target", targetIdentity);
    _target = nullptr;
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       structuralDeletePublishesLinkedDependentStructureInTheSameEvent)
{
    Base::PyGILStateLocker gil;
    auto* linkOwner = _document->addObject<App::FeatureTest>("LinkOwner");
    ASSERT_NE(linkOwner, nullptr);
    auto* link = dynamic_cast<App::PropertyLink*>(
        linkOwner->addDynamicProperty("App::PropertyLink", "TargetLink"));
    ASSERT_NE(link, nullptr);
    link->setValue(_target);
    _document->recompute();

    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();
    const auto targetIdentity = _document->collaborationObjectIdentity(*_target);
    const auto ownerIdentity = _document->collaborationObjectIdentity(*linkOwner);

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "linked structural delete compatibility mutation failed";
        return;
    }
    EXPECT_EQ(link->getValue(), nullptr);
    const auto poll = _document->collaborationRevisions().pollPublications(cursor);
    ASSERT_EQ(poll.events.size(), 1U);
    const auto& changes = poll.events.front().changes;
    ASSERT_EQ(changes.size(), 5U);
    const auto find = [&](const App::DocumentRevisionKey& key) {
        return std::ranges::find_if(changes, [&](const auto& change) {
            return change.key == key;
        });
    };
    const auto targetExistence =
        find(App::DocumentRevisionKey::objectExistence("Target"));
    const auto targetStructure =
        find(App::DocumentRevisionKey::objectStructure("Target"));
    const auto ownerStructure =
        find(App::DocumentRevisionKey::objectStructure("LinkOwner"));
    ASSERT_NE(targetExistence, changes.end());
    ASSERT_NE(targetStructure, changes.end());
    ASSERT_NE(ownerStructure, changes.end());
    EXPECT_EQ(targetExistence->stableObjectIdentity, targetIdentity);
    EXPECT_EQ(targetStructure->stableObjectIdentity, targetIdentity);
    EXPECT_EQ(ownerStructure->stableObjectIdentity, ownerIdentity);
    EXPECT_NE(find(App::DocumentRevisionKey::documentStructure()), changes.end());
    EXPECT_NE(find(App::DocumentRevisionKey::unknownModelMutation()), changes.end());
    _target = nullptr;
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       failedStructuralMutationRollsBackWithoutPublicationOrObserverLeak)
{
    Base::PyGILStateLocker gil;
    auto* sibling = _document->addObject<App::FeatureTest>("Sibling");
    auto* activeLast = _document->addObject<App::FeatureTest>("ActiveLast");
    ASSERT_NE(sibling, nullptr);
    ASSERT_NE(activeLast, nullptr);
    _document->recompute();
    ASSERT_FALSE(_document->mustExecute());
    ASSERT_EQ(_document->getActiveObject(), activeLast);
    const auto objectsBefore = _document->getObjects();
    std::vector<unsigned long> objectStatusBefore;
    objectStatusBefore.reserve(objectsBefore.size());
    for (const auto* object : objectsBefore) {
        objectStatusBefore.push_back(object->getStatus());
    }
    const auto pendingRecomputeBefore = _document->mustExecute();
    const auto targetIdentity = _document->collaborationObjectIdentity(*_target);
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.addTransient = true;
    probe.removeTarget = true;
    probe.raisePythonError = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();
    int created = 0;
    int deleted = 0;
    fastsignals::scoped_connection createdConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject&) { ++created; });
    fastsignals::scoped_connection deletedConnection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject&) { ++deleted; });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    EXPECT_EQ(result.get(), nullptr);
    EXPECT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_EQ(_document->getObjects(), objectsBefore);
    ASSERT_EQ(_document->getObjects().size(), objectStatusBefore.size());
    for (std::size_t index = 0; index < objectsBefore.size(); ++index) {
        EXPECT_EQ(objectsBefore[index]->getStatus(), objectStatusBefore[index]);
    }
    EXPECT_EQ(_document->mustExecute(), pendingRecomputeBefore);
    EXPECT_EQ(_document->getActiveObject(), activeLast);
    EXPECT_EQ(_document->collaborationObjectIdentity(*_target), targetIdentity);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       conflictingStructuralIdentitiesFailPublicationAndRollBack)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.removeTarget = true;
    probe.addReplacementTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto cursor = publicationCursor();
    const auto targetIdentity = _document->collaborationObjectIdentity(*_target);
    const auto objectsBefore = _document->getObjects();
    auto* activeBefore = _document->getActiveObject();
    int created = 0;
    int deleted = 0;
    fastsignals::scoped_connection createdConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject&) { ++created; });
    fastsignals::scoped_connection deletedConnection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject&) { ++deleted; });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    ASSERT_NE(result.get(), nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "PublicationFailed");
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_EQ(_document->getObjects(), objectsBefore);
    EXPECT_EQ(_document->getActiveObject(), activeBefore);
    EXPECT_EQ(_document->collaborationObjectIdentity(*_target), targetIdentity);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);
    EXPECT_TRUE(_document->collaborationRevisions().pollPublications(cursor).events.empty());
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       addThenRemoveTransientObjectIsPointerSafeAndObserverInvisible)
{
    Base::PyGILStateLocker gil;
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    probe.changeTransientStaticProperty = true;
    probe.addDynamicPropertyToTransient = true;
    probe.addExtensionToTransient = true;
    probe.removeTransient = true;
    int beforeExtension = 0;
    int addedExtension = 0;
    probe.beforeExtensionObserverCalls = &beforeExtension;
    probe.addedExtensionObserverCalls = &addedExtension;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    int created = 0;
    int deleted = 0;
    int appendedDynamicProperty = 0;
    int changedObject = 0;
    int changedPropertyEditor = 0;
    fastsignals::scoped_connection createdConnection = _document->signalNewObject.connect(
        [&](const App::DocumentObject&) { ++created; });
    fastsignals::scoped_connection deletedConnection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject&) { ++deleted; });
    fastsignals::scoped_connection dynamicPropertyConnection =
        App::GetApplication().signalAppendDynamicProperty.connect(
            [&](const App::Property& property) {
                if (property.getName()
                    && std::string_view(property.getName())
                        == "PostSetupCompatibilityProperty") {
                    ++appendedDynamicProperty;
                }
            });
    fastsignals::scoped_connection changedObjectConnection =
        _document->signalChangedObject.connect(
            [&](const App::DocumentObject& object, const App::Property&) {
                if (object.getNameInDocument()
                    && std::string_view(object.getNameInDocument()) == "Transient") {
                    ++changedObject;
                }
            });
    fastsignals::scoped_connection propertyEditorConnection =
        _document->signalChangePropertyEditor.connect(
            [&](const App::Document&, const App::Property& property) {
                if (property.getName()
                    && std::string_view(property.getName())
                        == "PostSetupCompatibilityProperty") {
                    ++changedPropertyEditor;
                }
            });
    fastsignals::scoped_connection beforeExtensionConnection =
        App::GetApplication().signalBeforeAddingDynamicExtension.connect(
            [&](const auto&, const auto&) { ++beforeExtension; });
    fastsignals::scoped_connection addedExtensionConnection =
        App::GetApplication().signalAddedDynamicExtension.connect(
            [&](const auto&, const auto&) { ++addedExtension; });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    ASSERT_NE(result.get(), nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);
    EXPECT_EQ(appendedDynamicProperty, 0);
    EXPECT_EQ(changedObject, 0);
    EXPECT_EQ(changedPropertyEditor, 0);
    EXPECT_TRUE(probe.extensionObserversDeferred);
    EXPECT_EQ(beforeExtension, 0);
    EXPECT_EQ(addedExtension, 0);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       zeroUndoLimitRetainsStructuralPointersThroughObserverReplay)
{
    Base::PyGILStateLocker gil;
    _document->setMaxUndoStackSize(0);
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.addTransient = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    int appended = 0;
    bool transactionWasLive = false;
    fastsignals::scoped_connection connection = _document->signalTransactionAppend.connect(
        [&](const App::DocumentObject& object, App::Transaction* transaction) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Transient") {
                ++appended;
                transactionWasLive = transaction != nullptr;
            }
        });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    ASSERT_NE(result.get(), nullptr);
    EXPECT_EQ(appended, 1);
    EXPECT_TRUE(transactionWasLive);
    EXPECT_EQ(_document->getTransactionID(true, 0), 0);
    EXPECT_NE(_document->getObject("Transient"), nullptr);
}

TEST_F(DocumentCollaborationPythonCompatibilityTest,
       zeroUndoLimitRetainsDeletedObjectAndTransactionThroughObserverReplay)
{
    Base::PyGILStateLocker gil;
    _document->setMaxUndoStackSize(0);
    PyObjectRef document(_document->getPyObject());
    CallbackProbe probe;
    probe.nativeDocument = _document;
    probe.target = _target;
    probe.removeTarget = true;
    PyObjectRef callback(makeCompatibilityCallback(probe));
    ASSERT_NE(callback.get(), nullptr);
    const auto wildcardBefore = wildcardRevision();
    int deleted = 0;
    int transactionRemoved = 0;
    int transactionId = 0;
    bool deletedTypeWasLive = false;
    bool transactionObjectWasLive = false;
    bool committedStateWasVisible = false;
    bool revisionWasVisible = false;
    fastsignals::scoped_connection deletedConnection =
        _document->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Target") {
                ++deleted;
                deletedTypeWasLive = std::string_view(object.getTypeId().getName())
                    == "App::FeatureTest";
                committedStateWasVisible = _document->getObject("Target") == nullptr;
                revisionWasVisible = wildcardRevision() > wildcardBefore;
            }
        });
    fastsignals::scoped_connection transactionConnection =
        _document->signalTransactionRemove.connect(
        [&](const App::DocumentObject& object, App::Transaction* transaction) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument()) == "Target") {
                ++transactionRemoved;
                transactionObjectWasLive = std::string_view(object.getTypeId().getName())
                    == "App::FeatureTest";
                transactionId = transaction ? transaction->getID() : 0;
            }
        });

    PyObjectRef result(callStructuralCompatibilityMutation(document.get(), callback.get()));

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "zero-undo structural delete compatibility mutation failed";
        return;
    }
    EXPECT_EQ(deleted, 1);
    EXPECT_EQ(transactionRemoved, 1);
    EXPECT_TRUE(deletedTypeWasLive);
    EXPECT_TRUE(transactionObjectWasLive);
    EXPECT_GT(transactionId, 0);
    EXPECT_TRUE(committedStateWasVisible);
    EXPECT_TRUE(revisionWasVisible);
    EXPECT_EQ(_document->getObject("Target"), nullptr);
    EXPECT_EQ(_document->getTransactionID(true, 0), 0);
    _target = nullptr;
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
    std::atomic_bool workerFinished {false};
    std::thread worker([&] {
        Base::PyGILStateLocker gil;
        result = PyObject_CallMethod(document, "commitCompatibilityMutation", "O", callback);
        callFailed = result == nullptr;
        workerFinished.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(dispatcher.pumpUntil(
        [&] { return workerFinished.load(std::memory_order_acquire); }));
    worker.join();

    {
        Base::PyGILStateLocker gil;
        ASSERT_FALSE(callFailed);
        ASSERT_NE(result, nullptr);
        EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result, "status")), "Committed");
        EXPECT_EQ(probe.calls, 1);
        EXPECT_TRUE(probe.gilHeld);
        const auto* lane = _document->executionLane();
        ASSERT_NE(lane, nullptr);
        EXPECT_EQ(probe.callbackThread, lane->ownerThreadId());
        EXPECT_NE(probe.callbackThread, std::this_thread::get_id());
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
    std::atomic_bool workerFinished {false};
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
        workerFinished.store(true, std::memory_order_release);
    });

    ASSERT_TRUE(dispatcher.pumpUntil(
        [&] { return workerFinished.load(std::memory_order_acquire); }));
    worker.join();

    EXPECT_EQ(result, nullptr);
    EXPECT_TRUE(exactType);
    EXPECT_EQ(message, "compatibility callback failed");
    EXPECT_EQ(probe.calls, 1);
    EXPECT_TRUE(probe.gilHeld);
    const auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_EQ(probe.callbackThread, lane->ownerThreadId());
    EXPECT_NE(probe.callbackThread, std::this_thread::get_id());
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
       offOwnerCallWithoutMainThreadHooksUsesLaneAndCommits)
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
        // Headless App tests install no MainThreadSignal hooks; the execution
        // lane still owns the document and must admit the off-owner commit.
        EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
        EXPECT_EQ(probe.calls, 1);
        const auto* lane = _document->executionLane();
        ASSERT_NE(lane, nullptr);
        EXPECT_EQ(probe.callbackThread, lane->ownerThreadId());
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
