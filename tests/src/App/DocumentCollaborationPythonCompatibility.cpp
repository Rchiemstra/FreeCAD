// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Stream.h>

#include "App/Application.h"
#include "App/AutoTransaction.h"
#include "App/Document.h"
#include "App/DocumentPy.h"
#include "App/DocumentRevisionIndex.h"
#include "App/FeatureTest.h"
#include "App/MainThreadSignal.h"
#include "App/MergeDocuments.h"
#include "App/PropertyLinks.h"
#include "App/PropertyStandard.h"
#include "App/Transactions.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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

struct CallbackProbe
{
    App::Document* nativeDocument {nullptr};
    App::FeatureTest* target {nullptr};
    PyObject* document {nullptr};
    int calls {0};
    int nestedCalls {0};
    bool gilHeld {false};
    bool raisePythonError {false};
    bool reenter {false};
    bool addTransient {false};
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
        if (probe->addTransient) {
            if (!probe->nativeDocument
                || !probe->nativeDocument->addObject<App::FeatureTest>("Transient")) {
                throw std::runtime_error("failed to add transient object");
            }
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
       structuralCallbackDocumentAndObjectRecomputeDeferExecuteUntilAfterGrantCloses)
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

    if (!result.get()) {
        PyErr_Print();
        FAIL() << "structural compatibility recompute deferral failed";
        return;
    }
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(result.get(), "status")),
                 "Committed");
    EXPECT_EQ(probe.executeCallsObservedAfterCallbackRecompute, 0);
    EXPECT_EQ(executeProbe->executeCalls, 1);
    EXPECT_TRUE(executeProbe->rejected);
    EXPECT_FALSE(executeProbe->admitted);
    EXPECT_EQ(_document->getObject("ExecuteBorrowedStructure"), nullptr);
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
