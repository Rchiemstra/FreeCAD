// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Stream.h>

#include "App/Application.h"
#include "App/AutoTransaction.h"
#include "App/Document.h"
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
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

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
       eagerPolicyStillRejectsPreexistingPendingRecomputeByDefault)
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
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Busy");
    EXPECT_EQ(probe.calls, 0);
    EXPECT_TRUE(_document->mustExecute());
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
    ASSERT_EQ(_document->getActiveObject(), activeLast);
    const auto objectsBefore = _document->getObjects();
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
