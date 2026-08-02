// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <Base/Interpreter.h>

#include "App/Application.h"
#include "App/CollaborativeOperationRegistry.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentObject.h"
#include "App/DocumentObjectGroup.h"
#include "App/FeatureTest.h"
#include "App/PropertyLinks.h"
#include "App/private/CollaborativeOperationRegistryInternal.h"
#include <src/App/InitApplication.h>

#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace App;

namespace
{

constexpr std::string_view TestOperationType = "App.Test.SetLabel";

std::mutex InstrumentationMutex;
std::vector<std::thread::id> ApplyThreads;

class TestSetLabelOperation final: public CollaborativeOperation
{
public:
    TestSetLabelOperation(std::string objectName,
                          std::string objectIdentity,
                          std::string value,
                          std::string mode)
        : _objectName(std::move(objectName))
        , _objectIdentity(std::move(objectIdentity))
        , _value(std::move(value))
        , _mode(std::move(mode))
    {}

    std::string_view typeId() const noexcept override
    {
        return TestOperationType;
    }

    void apply(Document& document) const override
    {
        auto* object = document.getObject(_objectName.c_str());
        if (!object || document.collaborationObjectIdentity(*object) != _objectIdentity) {
            throw std::runtime_error("test target became stale");
        }
        if (_mode == "structural-add") {
            static_cast<void>(document.addObject<FeatureTest>("Transient"));
            return;
        }
        if (_mode == "schema-add") {
            static_cast<void>(object->addDynamicProperty("App::PropertyString", "Transient"));
            return;
        }
        if (_mode == "document-schema-add") {
            static_cast<void>(document.addDynamicProperty("App::PropertyString",
                                                          "TransientDocumentProperty"));
            return;
        }
        if (_mode == "clear-document") {
            document.clearDocument();
            return;
        }
        if (_mode == "recompute-failure") {
            auto* feature = dynamic_cast<FeatureTest*>(object);
            if (!feature) {
                throw std::runtime_error("recompute-failure target is not FeatureTest");
            }
            feature->ExceptionType.setValue(1);
            return;
        }
        if (_mode == "record-thread") {
            std::lock_guard lock(InstrumentationMutex);
            ApplyThreads.push_back(std::this_thread::get_id());
        }
        object->Label.setValue(_value);
        if (_mode == "transaction-open") {
            static_cast<void>(document.openTransaction("nested public transaction"));
        }
        if (_mode == "transaction-active") {
            static_cast<void>(document.setActiveTransaction(
                TransactionName {.name = "nested active transaction", .temporary = false}));
        }
        if (_mode == "transaction-rename") {
            document.renameTransaction("renamed prepared transaction",
                                       document.getBookedTransactionID());
        }
        if (_mode == "transaction-commit") {
            document.commitTransaction();
        }
        if (_mode == "transaction-abort") {
            document.abortTransaction();
        }
        if (_mode == "transaction-undo") {
            static_cast<void>(document.undo());
        }
        if (_mode == "transaction-redo") {
            static_cast<void>(document.redo());
        }
        if (_mode == "transaction-clear-undos") {
            document.clearUndos();
        }
        if (_mode == "transaction-lock") {
            document.lockTransaction();
        }
        if (_mode == "transaction-unlock") {
            document.unlockTransaction();
        }
        if (_mode == "transaction-undo-limit") {
            document.setUndoLimit(0);
        }
        if (_mode == "transaction-stack-limit") {
            document.setMaxUndoStackSize(0);
        }
        if (_mode == "transaction-mode") {
            document.setTransactionMode(0);
        }
        if (_mode == "reentrant-snapshot") {
            const auto nested =
                document.collaborationService().beginEditSession("reentrant snapshot");
            static_cast<void>(document.collaborationService().snapshotForEdit(
                nested.sessionId(), {DocumentRevisionKey::objectModel(_objectName)}));
        }
        if (_mode == "reentrant-prepare") {
            const auto nested =
                document.collaborationService().beginEditSession("reentrant preparation");
            CollaborativeOperationIntent nestedIntent;
            nestedIntent.operationType = std::string(TestOperationType);
            nestedIntent.arguments = {{"object", _objectName}, {"value", "Nested"}};
            static_cast<void>(document.collaborationService().prepareEdit(
                nested.sessionId(), "nested-preparation", nestedIntent, "reentrant-test"));
        }
        if (_mode == "apply-failure") {
            throw std::runtime_error("injected apply failure");
        }
    }

    CollaborativePostconditionResult checkPostcondition(const Document& document) const override
    {
        if (_mode == "postcondition-failure") {
            return {false, "injected postcondition failure"};
        }
        if (_mode == "structural-add" || _mode == "schema-add"
            || _mode == "document-schema-add"
            || _mode == "clear-document"
            || _mode == "recompute-failure") {
            return {true, {}};
        }
        const auto* object = document.getObject(_objectName.c_str());
        return {object && document.collaborationObjectIdentity(*object) == _objectIdentity
                    && object->Label.getStrValue() == _value,
                "test label must equal prepared value"};
    }

private:
    const std::string _objectName;
    const std::string _objectIdentity;
    const std::string _value;
    const std::string _mode;
};

void ensureTestAdapterRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(TestOperationType),
            [](const Document& document, const CollaborativeOperationIntent& intent) {
                const auto objectName = intent.arguments.at("object");
                const auto value = intent.arguments.at("value");
                const auto mode = intent.arguments.contains("mode")
                    ? intent.arguments.at("mode")
                    : std::string {};
                const auto* object = document.getObject(objectName.c_str());
                if (!object) {
                    throw std::invalid_argument("test target does not exist");
                }
                const std::string stableIdentity =
                    document.collaborationObjectIdentity(*object);
                const auto existence = DocumentRevisionKey::objectExistence(objectName);
                const auto model = DocumentRevisionKey::objectModel(objectName);
                const auto structure = DocumentRevisionKey::objectStructure(objectName);
                return CollaborativeOperationPreparation {
                    {existence, model, structure, DocumentRevisionKey::unknownModelMutation()},
                    {model},
                    {{model, stableIdentity}},
                    std::make_unique<const TestSetLabelOperation>(objectName,
                                                                  stableIdentity,
                                                                  value,
                                                                  mode)};
            });
    });
}

CollaborativeOperationIntent intent(std::string value,
                                    std::string mode = {},
                                    std::string objectName = "Target")
{
    CollaborativeOperationIntent result;
    result.operationType = TestOperationType;
    result.arguments = {{"object", std::move(objectName)}, {"value", std::move(value)}};
    if (!mode.empty()) {
        result.arguments.emplace("mode", std::move(mode));
    }
    return result;
}

class BlockingTestDispatcher
{
public:
    BlockingTestDispatcher()
        : _owner(std::this_thread::get_id())
    {
        Active = this;
        MainThreadSignalConfig::setHooks(&isOwnerThread, &invoke);
    }

    ~BlockingTestDispatcher()
    {
        MainThreadSignalConfig::setHooks(nullptr, nullptr);
        Active = nullptr;
    }

    BlockingTestDispatcher(const BlockingTestDispatcher&) = delete;
    BlockingTestDispatcher& operator=(const BlockingTestDispatcher&) = delete;

    void runOne()
    {
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(_mutex);
            _changed.wait(lock, [&] { return !_tasks.empty(); });
            task = std::move(_tasks.front());
            _tasks.pop_front();
        }
        task->callback();
        {
            std::lock_guard lock(task->mutex);
            task->done = true;
        }
        task->changed.notify_all();
    }

    void waitUntilQueued()
    {
        std::unique_lock lock(_mutex);
        _changed.wait(lock, [&] { return !_tasks.empty(); });
    }

private:
    struct Task
    {
        std::function<void()> callback;
        std::mutex mutex;
        std::condition_variable changed;
        bool done {false};
    };

    static bool isOwnerThread()
    {
        return Active && std::this_thread::get_id() == Active->_owner;
    }

    static void invoke(std::function<void()>&& callback, bool blocking)
    {
        if (!Active || isOwnerThread()) {
            callback();
            return;
        }
        auto task = std::make_shared<Task>();
        task->callback = std::move(callback);
        {
            std::lock_guard lock(Active->_mutex);
            Active->_tasks.push_back(task);
        }
        Active->_changed.notify_one();
        if (blocking) {
            std::unique_lock lock(task->mutex);
            task->changed.wait(lock, [&] { return task->done; });
        }
    }

    static inline BlockingTestDispatcher* Active {nullptr};
    const std::thread::id _owner;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<Task>> _tasks;
};

class DocumentCollaborationServiceTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureTestAdapterRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("collaborationService");
        _document = App::GetApplication().newDocument(_documentName.c_str(), "Service test");
        _target = _document->addObject<FeatureTest>("Target");
        _target->Label.setValue("Before");
        _document->recompute();
        _session = _document->collaborationService().beginEditSession("actor-a");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    PreparedEdit prepare(std::string operationId,
                         std::string value,
                         std::string mode = {},
                         std::string objectName = "Target")
    {
        return _document->collaborationService().prepareEdit(_session.sessionId(),
                                                             std::move(operationId),
                                                             intent(std::move(value),
                                                                    std::move(mode),
                                                                    std::move(objectName)),
                                                             "native-test");
    }

    Document* _document {nullptr};
    DocumentObject* _target {nullptr};
    EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

static_assert(!std::is_constructible_v<DocumentCollaborationService, Document&>);
static_assert(!std::is_copy_constructible_v<DocumentCollaborationService>);

TEST_F(DocumentCollaborationServiceTest, sessionsAreAdvisoryAndCancellable)
{
    const auto status = _document->collaborationService().sessionStatus(_session.sessionId());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->actorId(), "actor-a");
    EXPECT_EQ(status->status(), EditSessionStatus::Active);

    EXPECT_TRUE(_document->collaborationService().cancelEdit(_session.sessionId(), "done"));
    const auto cancelled =
        _document->collaborationService().sessionStatus(_session.sessionId());
    ASSERT_TRUE(cancelled.has_value());
    EXPECT_EQ(cancelled->status(), EditSessionStatus::Cancelled);
    EXPECT_EQ(cancelled->cancellationReason(), std::optional<std::string>("done"));
}

TEST_F(DocumentCollaborationServiceTest, rejectsUnregisteredIntentBeforePreparation)
{
    CollaborativeOperationIntent spoofed;
    spoofed.operationType = "Part.CollaborativeBoolean";
    spoofed.arguments = {{"object", "Target"}};
    EXPECT_THROW(static_cast<void>(_document->collaborationService().prepareEdit(
                     _session.sessionId(), "spoof", spoofed, "untrusted-client")),
                 std::invalid_argument);
}

TEST_F(DocumentCollaborationServiceTest, snapshotIsCanonicalAndPointerFree)
{
    const auto snapshot = _document->collaborationService().snapshotForEdit(
        _session.sessionId(),
        {DocumentRevisionKey::objectModel("Target"),
         DocumentRevisionKey::objectExistence("Target")});
    EXPECT_EQ(snapshot.sessionId, _session.sessionId());
    EXPECT_EQ(snapshot.documentInstanceId, _session.documentInstanceId());
    ASSERT_EQ(snapshot.revisions.size(), 2U);
    EXPECT_LT(snapshot.revisions[0].key, snapshot.revisions[1].key);
}

TEST_F(DocumentCollaborationServiceTest, pythonFacadeReturnsOpaquePreparedHandleAndStructuredResults)
{
    Base::PyGILStateLocker gil;
    PyObject* pythonDocument = _document->getPyObject();
    ASSERT_NE(pythonDocument, nullptr);

    PyObject* session =
        PyObject_CallMethod(pythonDocument, "beginEditSession", "s", "python-actor");
    ASSERT_NE(session, nullptr);
    PyObject* sessionIdObject = PyDict_GetItemString(session, "session_id");
    ASSERT_NE(sessionIdObject, nullptr);
    const char* sessionIdText = PyUnicode_AsUTF8(sessionIdObject);
    ASSERT_NE(sessionIdText, nullptr);
    const std::string sessionId(sessionIdText);

    PyObject* revisionKeys = Py_BuildValue("[{s:s,s:s}]",
                                           "kind",
                                           "ObjectModel",
                                           "subject",
                                           "Target");
    ASSERT_NE(revisionKeys, nullptr);
    PyObject* snapshot = PyObject_CallMethod(pythonDocument,
                                             "snapshotForEdit",
                                             "sO",
                                             sessionId.c_str(),
                                             revisionKeys);
    ASSERT_NE(snapshot, nullptr);
    PyObject* revisions = PyDict_GetItemString(snapshot, "revisions");
    ASSERT_NE(revisions, nullptr);
    EXPECT_EQ(PyList_Size(revisions), 1);

    PyObject* arguments = Py_BuildValue("{s:s,s:s,s:s}",
                                        "object",
                                        "Target",
                                        "value",
                                        "From Python",
                                        "mode",
                                        "");
    ASSERT_NE(arguments, nullptr);
    PyObject* prepared = PyObject_CallMethod(pythonDocument,
                                             "prepareEdit",
                                             "sssOs",
                                             sessionId.c_str(),
                                             "python-label",
                                             TestOperationType.data(),
                                             arguments,
                                             "python-test");
    ASSERT_NE(prepared, nullptr);
    EXPECT_TRUE(PyCapsule_IsValid(prepared, "App.PreparedEdit"));

    PyObject* commit = PyObject_CallMethod(pythonDocument,
                                           "commitEdit",
                                           "sO",
                                           sessionId.c_str(),
                                           prepared);
    ASSERT_NE(commit, nullptr);
    PyObject* status = PyDict_GetItemString(commit, "status");
    ASSERT_NE(status, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(status), "Committed");
    EXPECT_EQ(_target->Label.getStrValue(), "From Python");
    EXPECT_NE(PyDict_GetItemString(commit, "published_revisions"), nullptr);

    PyObject* cancelled = PyObject_CallMethod(pythonDocument,
                                              "cancelEdit",
                                              "ss",
                                              sessionId.c_str(),
                                              "binding test done");
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(PyObject_IsTrue(cancelled), 1);
    PyObject* finalStatus = PyObject_CallMethod(pythonDocument,
                                                "editSessionStatus",
                                                "s",
                                                sessionId.c_str());
    ASSERT_NE(finalStatus, nullptr);
    EXPECT_STREQ(PyUnicode_AsUTF8(PyDict_GetItemString(finalStatus, "status")),
                 "Cancelled");

    Py_DECREF(finalStatus);
    Py_DECREF(cancelled);
    Py_DECREF(commit);
    Py_DECREF(prepared);
    Py_DECREF(arguments);
    Py_DECREF(snapshot);
    Py_DECREF(revisionKeys);
    Py_DECREF(session);
    Py_DECREF(pythonDocument);
}

TEST_F(DocumentCollaborationServiceTest, commitsBehindRevisionAndObserverBoundary)
{
    auto prepared = prepare("set-label", "After");
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto before = _document->collaborationRevisions().current(modelKey);
    bool observerRan = false;
    auto connection = _document->signalCommitTransaction.connect([&](const Document&) {
        observerRan = true;
        EXPECT_EQ(_target->Label.getStrValue(), "After");
        EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before + 1);
    });

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_TRUE(result.committed());
    EXPECT_TRUE(observerRan);
    EXPECT_EQ(_target->Label.getStrValue(), "After");
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before + 1);
}

TEST_F(DocumentCollaborationServiceTest, observerFailureCannotSplitCommitAndPublication)
{
    auto prepared = prepare("throwing-observer", "After");
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto before = _document->collaborationRevisions().current(modelKey);
    auto connection = _document->signalCommitTransaction.connect([](const Document&) {
        throw std::runtime_error("injected observer failure");
    });

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_TRUE(result.committed());
    EXPECT_EQ(_target->Label.getStrValue(), "After");
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before + 1);
}

TEST_F(DocumentCollaborationServiceTest, applyFailureRestoresValueWithoutPublication)
{
    auto prepared = prepare("apply-failure", "Transient", "apply-failure");
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto before = _document->collaborationRevisions().current(modelKey);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before);
}

TEST_F(DocumentCollaborationServiceTest, preparedOperationCannotEscapeCoordinatorTransaction)
{
    const std::vector<std::string> modes {
        "transaction-open",
        "transaction-active",
        "transaction-rename",
        "transaction-commit",
        "transaction-abort",
        "transaction-undo",
        "transaction-redo",
        "transaction-clear-undos",
        "transaction-lock",
        "transaction-unlock",
        "transaction-undo-limit",
        "transaction-stack-limit",
        "transaction-mode",
    };
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto revisionBefore = _document->collaborationRevisions().current(modelKey);

    for (const auto& mode : modes) {
        auto escaped = prepare("escape-" + mode, "Transient", mode);
        const auto result =
            _document->collaborationService().commitEdit(_session.sessionId(), escaped);
        EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed) << mode;
        EXPECT_EQ(_target->Label.getStrValue(), "Before") << mode;
        EXPECT_EQ(_document->collaborationRevisions().current(modelKey), revisionBefore) << mode;
        EXPECT_TRUE(result.publishedRevisions.empty()) << mode;
        EXPECT_FALSE(_document->hasPendingTransaction()) << mode;
        EXPECT_FALSE(_document->isTransactionLocked()) << mode;
    }

    auto valid = prepare("after-transaction-escape-attempts", "After");
    const auto validResult =
        _document->collaborationService().commitEdit(_session.sessionId(), valid);
    EXPECT_TRUE(validResult.committed());
    EXPECT_EQ(_target->Label.getStrValue(), "After");
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), revisionBefore + 1);
}

TEST_F(DocumentCollaborationServiceTest, stableReadAdmissionClosesDuringPreparedCommit)
{
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto revisionBefore = _document->collaborationRevisions().current(modelKey);

    for (const auto& mode : {"reentrant-snapshot", "reentrant-prepare"}) {
        auto reentrant = prepare("blocked-" + std::string(mode), "Transient", mode);
        const auto result =
            _document->collaborationService().commitEdit(_session.sessionId(), reentrant);
        EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed) << mode;
        EXPECT_EQ(_target->Label.getStrValue(), "Before") << mode;
        EXPECT_EQ(_document->collaborationRevisions().current(modelKey), revisionBefore) << mode;
        EXPECT_TRUE(result.publishedRevisions.empty()) << mode;
    }
}

TEST_F(DocumentCollaborationServiceTest, stableReadAdmissionRequiresCleanNativeState)
{
    _target->touch();
    EXPECT_THROW(static_cast<void>(_document->collaborationService().snapshotForEdit(
                     _session.sessionId(), {DocumentRevisionKey::objectModel("Target")})),
                 Base::RuntimeError);
    EXPECT_THROW(static_cast<void>(_document->collaborationService().prepareEdit(
                     _session.sessionId(),
                     "dirty-preparation",
                     intent("Must Not Prepare"),
                     "native-test")),
                 Base::RuntimeError);

    _document->recompute();
    ASSERT_NE(_document->openTransaction("ordinary native transaction"), 0);
    EXPECT_THROW(static_cast<void>(_document->collaborationService().snapshotForEdit(
                     _session.sessionId(), {DocumentRevisionKey::objectModel("Target")})),
                 Base::RuntimeError);
    EXPECT_THROW(static_cast<void>(_document->collaborationService().prepareEdit(
                     _session.sessionId(),
                     "transaction-preparation",
                     intent("Must Not Prepare"),
                     "native-test")),
                 Base::RuntimeError);
    _document->abortTransaction();
}

TEST_F(DocumentCollaborationServiceTest, postconditionFailureRestoresWithoutPublication)
{
    auto prepared = prepare("postcondition-failure", "Transient", "postcondition-failure");
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto before = _document->collaborationRevisions().current(modelKey);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::PostconditionFailed);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before);
}

TEST_F(DocumentCollaborationServiceTest, sameObjectPreparedEditsConflictExactlyOnce)
{
    auto first = prepare("first", "First");
    auto stale = prepare("stale", "Stale");

    const auto firstResult =
        _document->collaborationService().commitEdit(_session.sessionId(), first);
    const auto staleResult =
        _document->collaborationService().commitEdit(_session.sessionId(), stale);
    EXPECT_TRUE(firstResult.committed());
    EXPECT_EQ(staleResult.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(_target->Label.getStrValue(), "First");
    EXPECT_FALSE(staleResult.conflicts.empty());
}

TEST_F(DocumentCollaborationServiceTest, deleteVersusWriteRejectsBeforeApply)
{
    auto stale = prepare("delete-versus-write", "Must Not Apply");
    _document->removeObject("Target");

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), stale);
    EXPECT_EQ(result.status, DocumentCommitStatus::Conflict);
    EXPECT_EQ(_document->getObject("Target"), nullptr);
}

TEST_F(DocumentCollaborationServiceTest, namespaceMembershipOrderTipAndLinkMutationsStaleEdits)
{
    auto expectOneRejectedEdit = [&](std::string operationId, auto&& mutate) {
        _document->recompute();
        auto stale = prepare(std::move(operationId), "Must Not Apply");
        std::forward<decltype(mutate)>(mutate)();
        const auto result =
            _document->collaborationService().commitEdit(_session.sessionId(), stale);
        EXPECT_EQ(result.status, DocumentCommitStatus::Conflict);
        EXPECT_EQ(_target->Label.getStrValue(), "Before");
    };

    DocumentObject* namespaceObject = nullptr;
    expectOneRejectedEdit("namespace", [&] {
        namespaceObject = _document->addObject<FeatureTest>("NamespaceMember");
    });
    ASSERT_NE(namespaceObject, nullptr);

    auto* group = _document->addObject<DocumentObjectGroup>("Group");
    auto* firstMember = _document->addObject<FeatureTest>("FirstMember");
    auto* secondMember = _document->addObject<FeatureTest>("SecondMember");
    ASSERT_NE(group, nullptr);
    expectOneRejectedEdit("membership", [&] { group->addObject(firstMember); });
    expectOneRejectedEdit("order", [&] {
        group->addObject(secondMember);
        group->removeObject(firstMember);
        group->addObject(firstMember);
    });

    expectOneRejectedEdit("tip", [&] { _document->Tip.setValue(namespaceObject); });

    auto* linkOwner = _document->addObject<FeatureTest>("LinkOwner");
    auto* link = dynamic_cast<PropertyLink*>(
        linkOwner->addDynamicProperty("App::PropertyLink", "TargetLink"));
    ASSERT_NE(link, nullptr);
    expectOneRejectedEdit("link", [&] { link->setValue(namespaceObject); });
}

TEST_F(DocumentCollaborationServiceTest, cancellationRejectsPreparedCommit)
{
    auto prepared = prepare("cancelled", "After");
    ASSERT_TRUE(_document->collaborationService().cancelEdit(_session.sessionId()));

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::Cancelled);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
}

TEST_F(DocumentCollaborationServiceTest, queuedCancellationWinsBeforeCommitAdmission)
{
    auto prepared = prepare("queued-cancellation", "Must Not Apply");
    BlockingTestDispatcher dispatcher;
    auto future = std::async(std::launch::async, [&] {
        return _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    });
    dispatcher.waitUntilQueued();
    ASSERT_TRUE(_document->collaborationService().cancelEdit(_session.sessionId(), "queued"));
    dispatcher.runOne();

    const auto result = future.get();
    EXPECT_EQ(result.status, DocumentCommitStatus::Cancelled);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
}

TEST_F(DocumentCollaborationServiceTest, queuedCancellationWinsBeforeStableReadAdmission)
{
    BlockingTestDispatcher dispatcher;

    const auto snapshotSession =
        _document->collaborationService().beginEditSession("queued snapshot");
    auto snapshotFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().snapshotForEdit(
            snapshotSession.sessionId(), {DocumentRevisionKey::objectModel("Target")});
    });
    dispatcher.waitUntilQueued();
    ASSERT_TRUE(_document->collaborationService().cancelEdit(snapshotSession.sessionId()));
    dispatcher.runOne();
    EXPECT_THROW(static_cast<void>(snapshotFuture.get()), Base::RuntimeError);

    const auto prepareSession =
        _document->collaborationService().beginEditSession("queued preparation");
    const auto preparedIntent = intent("Must Not Prepare");
    auto prepareFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().prepareEdit(prepareSession.sessionId(),
                                                             "queued-preparation",
                                                             preparedIntent,
                                                             "native-test");
    });
    dispatcher.waitUntilQueued();
    ASSERT_TRUE(_document->collaborationService().cancelEdit(prepareSession.sessionId()));
    dispatcher.runOne();
    EXPECT_THROW(static_cast<void>(prepareFuture.get()), Base::RuntimeError);
}

TEST_F(DocumentCollaborationServiceTest, staleDocumentPrecedesCancelledSession)
{
    const auto foreignName =
        App::GetApplication().getUniqueDocumentName("foreignCollaborationService");
    auto* foreign = App::GetApplication().newDocument(foreignName.c_str(), "Foreign service test");
    auto* foreignTarget = foreign->addObject<FeatureTest>("Target");
    ASSERT_NE(foreignTarget, nullptr);
    foreignTarget->Label.setValue("Foreign Before");
    foreign->recompute();
    const auto foreignSession = foreign->collaborationService().beginEditSession("foreign actor");
    auto foreignEdit = foreign->collaborationService().prepareEdit(foreignSession.sessionId(),
                                                                   "foreign-edit",
                                                                   intent("Foreign After"),
                                                                   "native-test");
    ASSERT_TRUE(_document->collaborationService().cancelEdit(_session.sessionId()));

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), foreignEdit);
    EXPECT_EQ(result.status, DocumentCommitStatus::StaleDocument);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(foreignTarget->Label.getStrValue(), "Foreign Before");

    App::GetApplication().closeDocument(foreignName.c_str());
}

TEST_F(DocumentCollaborationServiceTest, rollbackPublishesNoPropertyOrObjectNotifications)
{
    auto prepared = prepare("silent-rollback", "Transient", "apply-failure");
    int documentChanged = 0;
    int objectBefore = 0;
    int objectChanged = 0;
    auto documentConnection = _document->signalChangedObject.connect(
        [&](const DocumentObject&, const Property&) { ++documentChanged; });
    auto beforeConnection = _target->signalBeforeChange.connect(
        [&](const DocumentObject&, const Property&) { ++objectBefore; });
    auto changedConnection = _target->signalChanged.connect(
        [&](const DocumentObject&, const Property&) { ++objectChanged; });

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(documentChanged, 0);
    EXPECT_EQ(objectBefore, 0);
    EXPECT_EQ(objectChanged, 0);
}

TEST_F(DocumentCollaborationServiceTest, structuralAndSchemaMutationRejectBeforeVisibility)
{
    int created = 0;
    int deleted = 0;
    auto createdConnection = _document->signalNewObject.connect(
        [&](const DocumentObject&) { ++created; });
    auto deletedConnection = _document->signalDeletedObject.connect(
        [&](const DocumentObject&) { ++deleted; });

    auto structural = prepare("structural", "unused", "structural-add");
    const auto structuralResult =
        _document->collaborationService().commitEdit(_session.sessionId(), structural);
    EXPECT_EQ(structuralResult.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_document->getObject("Transient"), nullptr);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);

    auto schema = prepare("schema", "unused", "schema-add");
    const auto schemaResult =
        _document->collaborationService().commitEdit(_session.sessionId(), schema);
    EXPECT_EQ(schemaResult.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_target->getPropertyByName("Transient"), nullptr);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);

    auto documentSchema =
        prepare("document-schema", "unused", "document-schema-add");
    const auto documentSchemaResult =
        _document->collaborationService().commitEdit(_session.sessionId(), documentSchema);
    EXPECT_EQ(documentSchemaResult.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_document->getPropertyByName("TransientDocumentProperty"), nullptr);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(deleted, 0);
}

TEST_F(DocumentCollaborationServiceTest, clearDocumentRejectsBeforeSignalsOrObjectLoss)
{
    int applicationDeleted = 0;
    int applicationCreated = 0;
    auto deletedConnection = GetApplication().signalDeleteDocument.connect(
        [&](const Document& document) {
            if (&document == _document) {
                ++applicationDeleted;
            }
        });
    auto createdConnection = GetApplication().signalNewDocument.connect(
        [&](const Document& document, bool) {
            if (&document == _document) {
                ++applicationCreated;
            }
        });

    auto prepared = prepare("clear-document", "unused", "clear-document");
    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_EQ(_document->getObjects().size(), 1U);
    EXPECT_EQ(applicationDeleted, 0);
    EXPECT_EQ(applicationCreated, 0);
}

TEST_F(DocumentCollaborationServiceTest, recomputeFailureRestoresPreparedMutation)
{
    auto prepared = prepare("recompute-failure", "unused", "recompute-failure");
    const auto modelKey = DocumentRevisionKey::objectModel("Target");
    const auto before = _document->collaborationRevisions().current(modelKey);
    ASSERT_EQ(static_cast<FeatureTest*>(_target)->ExceptionType.getValue(), 0);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    EXPECT_EQ(result.status, DocumentCommitStatus::RecomputeFailed);
    EXPECT_EQ(static_cast<FeatureTest*>(_target)->ExceptionType.getValue(), 0);
    EXPECT_EQ(_document->collaborationRevisions().current(modelKey), before);
}

TEST_F(DocumentCollaborationServiceTest, pendingRecomputeOutsidePreparedClosureReturnsBusy)
{
    auto* unrelated = _document->addObject<FeatureTest>("Unrelated");
    ASSERT_NE(unrelated, nullptr);
    _document->recompute();
    auto prepared = prepare("clean-boundary", "Must Not Apply");
    // Inject scheduler state without changing any semantic revision: the
    // coordinator must still reject document-wide recompute from this boundary.
    unrelated->setStatus(ObjectStatus::Touch, true);
    ASSERT_TRUE(_document->mustExecute());
    const auto targetModel = DocumentRevisionKey::objectModel("Target");
    const auto targetRevisionBefore =
        _document->collaborationRevisions().current(targetModel);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    EXPECT_EQ(result.status, DocumentCommitStatus::Busy);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
    EXPECT_EQ(_document->collaborationRevisions().current(targetModel),
              targetRevisionBefore);
    EXPECT_TRUE(result.publishedRevisions.empty());
}

TEST_F(DocumentCollaborationServiceTest, reentrantObserverCommitReturnsBusy)
{
    auto outer = prepare("outer", "Outer");
    auto nested = prepare("nested", "Nested");
    std::optional<DocumentCommitResult> nestedResult;
    auto connection = _document->signalChangedObject.connect(
        [&](const DocumentObject&, const Property&) {
            if (!nestedResult) {
                nestedResult.emplace(
                    _document->collaborationService().commitEdit(_session.sessionId(), nested));
            }
        });

    const auto outerResult =
        _document->collaborationService().commitEdit(_session.sessionId(), outer);
    EXPECT_TRUE(outerResult.committed());
    ASSERT_TRUE(nestedResult.has_value());
    EXPECT_EQ(nestedResult->status, DocumentCommitStatus::Busy);
    EXPECT_EQ(_target->Label.getStrValue(), "Outer");
}

TEST_F(DocumentCollaborationServiceTest, noHookWorkerCommitIsRejectedOffOwnerThread)
{
    auto prepared = prepare("off-owner", "Worker");
    auto future = std::async(std::launch::async, [&] {
        return _document->collaborationService().commitEdit(_session.sessionId(), prepared);
    });

    const auto result = future.get();
    EXPECT_EQ(result.status, DocumentCommitStatus::Unsupported);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
}

TEST_F(DocumentCollaborationServiceTest, noHookWorkerSnapshotAndPreparationAreRejected)
{
    auto snapshotFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().snapshotForEdit(
            _session.sessionId(), {DocumentRevisionKey::objectModel("Target")});
    });
    EXPECT_THROW(static_cast<void>(snapshotFuture.get()), Base::RuntimeError);

    const auto preparedIntent = intent("Worker");
    auto prepareFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().prepareEdit(_session.sessionId(),
                                                             "worker-prepare",
                                                             preparedIntent,
                                                             "native-test");
    });
    EXPECT_THROW(static_cast<void>(prepareFuture.get()), Base::RuntimeError);
    EXPECT_EQ(_target->Label.getStrValue(), "Before");
}

TEST_F(DocumentCollaborationServiceTest, dispatcherRunsConcurrentAdmissionsOnOwnerThread)
{
    auto* other = _document->addObject<FeatureTest>("Other");
    other->Label.setValue("BeforeOther");
    _document->recompute();
    auto first = prepare("thread-first", "First", "record-thread", "Target");
    auto second = prepare("thread-second", "Second", "record-thread", "Other");
    {
        std::lock_guard lock(InstrumentationMutex);
        ApplyThreads.clear();
    }

    BlockingTestDispatcher dispatcher;
    auto firstFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().commitEdit(_session.sessionId(), first);
    });
    auto secondFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().commitEdit(_session.sessionId(), second);
    });
    dispatcher.runOne();
    dispatcher.runOne();

    const auto firstResult = firstFuture.get();
    const auto secondResult = secondFuture.get();
    EXPECT_TRUE(firstResult.committed());
    EXPECT_TRUE(secondResult.committed());
    EXPECT_EQ(_target->Label.getStrValue(), "First");
    EXPECT_EQ(other->Label.getStrValue(), "Second");
    std::lock_guard lock(InstrumentationMutex);
    ASSERT_EQ(ApplyThreads.size(), 2U);
    EXPECT_EQ(ApplyThreads[0], std::this_thread::get_id());
    EXPECT_EQ(ApplyThreads[1], std::this_thread::get_id());
}
