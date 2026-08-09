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
#include <chrono>
#include <deque>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

using namespace App;

namespace App::Internal
{

class DocumentCollaborationServiceTestAccess
{
public:
    using Hook = void (*)();

    static void setPostSubmitHook(Hook hook)
    {
        DocumentCollaborationService::_postSubmitTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPostTakeResultHook(Hook hook)
    {
        DocumentCollaborationService::_postTakeResultTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPostCancelSessionHook(Hook hook)
    {
        DocumentCollaborationService::_postCancelSessionTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPostLifecycleAdmissionHook(Hook hook)
    {
        DocumentCollaborationService::_postLifecycleAdmissionTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPostMarkClosingHook(Hook hook)
    {
        Application::_postMarkCollaborationClosingTestHook.store(
            hook, std::memory_order_release);
    }

    static void setPostAccessDrainHook(Hook hook)
    {
        Application::_postCollaborationAccessDrainTestHook.store(
            hook, std::memory_order_release);
    }

    static DocumentCommitResult serializeAtomic(
        DocumentCollaborationService& service,
        CollaborationAtomicCompatibilityCallback callback,
        std::vector<CollaborationAtomicPresentationWrite> allowedWrites = {})
    {
        return service.serializeAtomicCompatibilityCallback(
            std::move(allowedWrites), std::move(callback));
    }

    static CollaborationAtomicCommitPointResult commitAtomic(
        DocumentCollaborationService& service)
    {
        return service.commitAtomicCompatibilityTransaction();
    }
};

class DocumentStructuralCompatibilityTestAccess
{
public:
    static std::string grantDiagnostic(Document& document)
    {
        try {
            auto grant = document.openCollaborationStructuralMutationGrant();
            return "accepted";
        }
        catch (const Base::Exception& exception) {
            return exception.what();
        }
    }

    static std::string withoutTransaction(Document& document)
    {
        document.beginCollaborationCommitNotificationBarrier();
        document.setCollaborationRevisionPublicationSuppressed(true);
        const auto diagnostic = grantDiagnostic(document);
        document.setCollaborationRevisionPublicationSuppressed(false);
        document.finishCollaborationCommitNotificationBarrier(false);
        return diagnostic;
    }

    static std::string withoutRevisionSuppression(Document& document)
    {
        beginBoundary(document, false);
        const auto diagnostic = grantDiagnostic(document);
        endBoundary(document);
        return diagnostic;
    }

    static std::string withForeignStableRead(Document& document)
    {
        beginBoundary(document, true);
        document.beginCollaborationStableReadCapture();
        const auto diagnostic = grantDiagnostic(document);
        document.finishCollaborationStableReadCapture();
        endBoundary(document);
        return diagnostic;
    }

    static std::string duringAtomicPresentationAudit(Document& document)
    {
        beginBoundary(document, true);
        document.beginCollaborationAtomicPresentationAudit({});
        const auto diagnostic = grantDiagnostic(document);
        document.endCollaborationAtomicPresentationAudit();
        endBoundary(document);
        return diagnostic;
    }

    static std::string onPoisonedDocument(Document& document)
    {
        beginBoundary(document, true);
        document.poisonCollaborationCommit("test poison");
        const auto diagnostic = grantDiagnostic(document);
        endBoundary(document);
        return diagnostic;
    }

    static std::string reentrant(Document& document)
    {
        beginBoundary(document, true);
        std::string diagnostic;
        {
            auto grant = document.openCollaborationStructuralMutationGrant();
            diagnostic = grantDiagnostic(document);
        }
        endBoundary(document);
        return diagnostic;
    }

private:
    static void beginBoundary(Document& document, bool suppressRevisions)
    {
        document.beginCollaborationCommitNotificationBarrier();
        document.setCollaborationRevisionPublicationSuppressed(suppressRevisions);
        if (document.openCollaborationCommitTransaction("structural grant test") == 0) {
            throw std::runtime_error("test failed to open collaboration transaction");
        }
    }

    static void endBoundary(Document& document)
    {
        if (document.hasPendingTransaction()) {
            static_cast<void>(document.rollbackCollaborationTransaction());
        }
        document.setCollaborationRevisionPublicationSuppressed(false);
        document.finishCollaborationCommitNotificationBarrier(false);
    }
};

}  // namespace App::Internal

namespace
{

constexpr std::string_view TestOperationType = "App.Test.SetLabel";
constexpr std::string_view DetachedTestOperationType = "App.Test.DetachedSetLabel";

std::mutex InstrumentationMutex;
std::vector<std::thread::id> ApplyThreads;

class TestSetLabelOperation final: public CollaborativeOperation
{
public:
    TestSetLabelOperation(std::string objectName,
                          std::string objectIdentity,
                          std::string value,
                          std::string mode,
                          std::string type = std::string(TestOperationType))
        : _objectName(std::move(objectName))
        , _objectIdentity(std::move(objectIdentity))
        , _value(std::move(value))
        , _mode(std::move(mode))
        , _type(std::move(type))
    {}

    std::string_view typeId() const noexcept override
    {
        return _type;
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
        if (_mode == "structural-remove") {
            document.removeObject(_objectName.c_str());
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
        if (_mode == "structural-add" || _mode == "structural-remove"
            || _mode == "schema-add"
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
    const std::string _type;
};

class HookBarrier
{
public:
    HookBarrier()
    {
        Active = this;
    }

    ~HookBarrier()
    {
        release();
        Active = nullptr;
    }

    static void invoke()
    {
        std::unique_lock lock(Active->_mutex);
        Active->_entered = true;
        Active->_changed.notify_all();
        Active->_changed.wait(lock, [] { return Active->_released; });
    }

    static void releaseActive()
    {
        if (Active) {
            Active->release();
        }
    }

    bool waitUntilEntered(std::chrono::milliseconds timeout = std::chrono::seconds(2))
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return _entered; });
    }

    void release()
    {
        {
            std::lock_guard lock(_mutex);
            _released = true;
        }
        _changed.notify_all();
    }

private:
    static inline HookBarrier* Active {nullptr};
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _entered {false};
    bool _released {false};
};

class CountedHookBarrier
{
public:
    explicit CountedHookBarrier(std::size_t expected)
        : _expected(expected)
    {
        Active = this;
    }

    ~CountedHookBarrier()
    {
        release();
        Active = nullptr;
    }

    static void invoke()
    {
        std::unique_lock lock(Active->_mutex);
        ++Active->_entered;
        Active->_changed.notify_all();
        Active->_changed.wait(lock, [] { return Active->_released; });
    }

    bool waitUntilExpected(std::chrono::milliseconds timeout = std::chrono::seconds(2))
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return _entered >= _expected; });
    }

    void release()
    {
        {
            std::lock_guard lock(_mutex);
            _released = true;
        }
        _changed.notify_all();
    }

private:
    static inline CountedHookBarrier* Active {nullptr};
    const std::size_t _expected;
    std::mutex _mutex;
    std::condition_variable _changed;
    std::size_t _entered {0};
    bool _released {false};
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
        Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(DetachedTestOperationType),
            [](const Document& document, const CollaborativeOperationIntent& intent) {
                const auto objectName = intent.arguments.at("object");
                const auto value = intent.arguments.at("value");
                const auto* object = document.getObject(objectName.c_str());
                if (!object) {
                    throw std::invalid_argument("detached test target does not exist");
                }
                const std::string stableIdentity =
                    document.collaborationObjectIdentity(*object);
                const auto existence = DocumentRevisionKey::objectExistence(objectName);
                const auto model = DocumentRevisionKey::objectModel(objectName);
                const auto structure = DocumentRevisionKey::objectStructure(objectName);
                CollaborativeOperationPreparation::DetachedTask task =
                    [objectName,
                     stableIdentity,
                     value](std::stop_token stopToken) {
                        if (stopToken.stop_requested()) {
                            throw std::runtime_error("detached test operation cancelled");
                        }
                        return std::make_unique<const TestSetLabelOperation>(
                            objectName,
                            stableIdentity,
                            value,
                            std::string {},
                            std::string(DetachedTestOperationType));
                    };
                return CollaborativeOperationPreparation {
                    {existence,
                     model,
                     structure,
                     DocumentRevisionKey::unknownModelMutation()},
                    {model},
                    {{model, stableIdentity}},
                    std::move(task)};
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

CollaborativeOperationIntent detachedIntent(std::string value)
{
    CollaborativeOperationIntent result;
    result.operationType = DetachedTestOperationType;
    result.arguments = {{"object", "Target"}, {"value", std::move(value)}};
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

TEST_F(DocumentCollaborationServiceTest, dirtyLiveDocumentKeepsStatusAndCancellationAvailable)
{
    const auto executionId = _document->collaborationService().prepareEditAsync(
        _session.sessionId(),
        "dirty-live-status",
        detachedIntent("After"),
        "native-detached-test");
    _target->touch();

    EXPECT_TRUE(_document->collaborationService()
                    .sessionStatus(_session.sessionId())
                    .has_value());
    EXPECT_TRUE(_document->collaborationService()
                    .preparedEditStatus(executionId)
                    .has_value());
    EXPECT_TRUE(_document->collaborationService().cancelEdit(
        _session.sessionId(), "dirty document cancellation"));
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

TEST_F(DocumentCollaborationServiceTest, closeDrainsPostSubmitRegistrationGap)
{
    HookBarrier barrier;
    Internal::DocumentCollaborationServiceTestAccess::setPostSubmitHook(
        &HookBarrier::invoke);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(
        &HookBarrier::releaseActive);
    BlockingTestDispatcher dispatcher;
    auto preparationFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().prepareEditAsync(
            _session.sessionId(),
            "registration-lifecycle-pin",
            detachedIntent("After"),
            "native-detached-test");
    });
    dispatcher.waitUntilQueued();

    bool hookEntered = false;
    bool closeResult = true;
    std::thread closeThread([&] {
        hookEntered = barrier.waitUntilEntered();
        if (hookEntered) {
            closeResult = App::GetApplication().closeDocument(_documentName.c_str());
        }
    });
    dispatcher.runOne();
    closeThread.join();
    Internal::DocumentCollaborationServiceTestAccess::setPostSubmitHook(nullptr);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(nullptr);

    EXPECT_TRUE(hookEntered);
    EXPECT_TRUE(closeResult);
    static_cast<void>(preparationFuture.get());
    if (closeResult) {
        _document = nullptr;
    }
}

TEST_F(DocumentCollaborationServiceTest, queuedDispatchPinsDocumentBeforeOwnerCallback)
{
    BlockingTestDispatcher dispatcher;
    auto preparationFuture = std::async(std::launch::async, [&] {
        return _document->collaborationService().prepareEditAsync(
            _session.sessionId(),
            "queued-dispatch-lifecycle-pin",
            detachedIntent("After"),
            "native-detached-test");
    });
    dispatcher.waitUntilQueued();

    EXPECT_FALSE(App::GetApplication().closeDocument(_documentName.c_str()));

    dispatcher.runOne();
    const auto executionId = preparationFuture.get();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<PreparedEditExecutionSnapshot> status;
    do {
        status = _document->collaborationService().preparedEditStatus(executionId);
        if (status && status->status == PreparedEditExecutionStatus::Completed) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(status->status, PreparedEditExecutionStatus::Completed);

    auto result = _document->collaborationService().takePreparedEdit(
        _session.sessionId(), executionId);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, PreparedEditExecutionStatus::Completed);
    EXPECT_NE(result->preparedEdit, nullptr);
}

TEST_F(DocumentCollaborationServiceTest, closeDrainsSessionCancellationGap)
{
    HookBarrier barrier;
    Internal::DocumentCollaborationServiceTestAccess::setPostCancelSessionHook(
        &HookBarrier::invoke);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(
        &HookBarrier::releaseActive);
    auto cancellation = std::async(std::launch::async, [&] {
        return _document->collaborationService().cancelEdit(
            _session.sessionId(), "cancel lifecycle gap");
    });
    const bool hookEntered = barrier.waitUntilEntered();
    EXPECT_TRUE(hookEntered);
    if (!hookEntered) {
        barrier.release();
        static_cast<void>(cancellation.get());
        Internal::DocumentCollaborationServiceTestAccess::setPostCancelSessionHook(nullptr);
        Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(nullptr);
        return;
    }

    const bool closed = App::GetApplication().closeDocument(_documentName.c_str());
    EXPECT_TRUE(cancellation.get());
    Internal::DocumentCollaborationServiceTestAccess::setPostCancelSessionHook(nullptr);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(nullptr);
    EXPECT_TRUE(closed);
    if (closed) {
        _document = nullptr;
    }
}

TEST_F(DocumentCollaborationServiceTest, closeDrainsResultCollectionGap)
{
    const auto executionId = _document->collaborationService().prepareEditAsync(
        _session.sessionId(),
        "collection-lifecycle-pin",
        detachedIntent("After"),
        "native-detached-test");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<PreparedEditExecutionSnapshot> status;
    do {
        status = _document->collaborationService().preparedEditStatus(executionId);
        if (status && status->status == PreparedEditExecutionStatus::Completed) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(status.has_value());
    ASSERT_EQ(status->status, PreparedEditExecutionStatus::Completed);

    HookBarrier barrier;
    Internal::DocumentCollaborationServiceTestAccess::setPostTakeResultHook(
        &HookBarrier::invoke);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(
        &HookBarrier::releaseActive);
    bool hookEntered = false;
    bool closeResult = true;
    std::thread closeThread([&] {
        hookEntered = barrier.waitUntilEntered();
        if (hookEntered) {
            closeResult = App::GetApplication().closeDocument(_documentName.c_str());
        }
    });
    auto result = _document->collaborationService().takePreparedEdit(
        _session.sessionId(), executionId);
    closeThread.join();
    Internal::DocumentCollaborationServiceTestAccess::setPostTakeResultHook(nullptr);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(nullptr);

    EXPECT_TRUE(hookEntered);
    EXPECT_TRUE(closeResult);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status, PreparedEditExecutionStatus::Cancelled);
    EXPECT_EQ(result->preparedEdit, nullptr);
    if (closeResult) {
        _document = nullptr;
    }
}

TEST_F(DocumentCollaborationServiceTest, closeDrainsStatusAndCancellationAdmittedAfterClosing)
{
    HookBarrier closeAfterMarking;
    CountedHookBarrier admittedCalls(2);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(
        &HookBarrier::invoke);

    auto close = std::async(std::launch::async, [&] {
        return App::GetApplication().closeDocument(_documentName.c_str());
    });
    const bool closeMarked = closeAfterMarking.waitUntilEntered();

    Internal::DocumentCollaborationServiceTestAccess::setPostLifecycleAdmissionHook(
        &CountedHookBarrier::invoke);
    auto status = std::async(std::launch::async, [&] {
        return _document->collaborationService().sessionStatus(_session.sessionId());
    });
    auto cancellation = std::async(std::launch::async, [&] {
        return _document->collaborationService().cancelEdit(
            _session.sessionId(), "closing rejection");
    });
    const bool callsAdmitted = admittedCalls.waitUntilExpected();

    admittedCalls.release();
    closeAfterMarking.release();
    Internal::DocumentCollaborationServiceTestAccess::setPostLifecycleAdmissionHook(nullptr);
    Internal::DocumentCollaborationServiceTestAccess::setPostMarkClosingHook(nullptr);

    EXPECT_TRUE(closeMarked);
    EXPECT_TRUE(callsAdmitted);
    ASSERT_EQ(status.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(cancellation.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(status.get().has_value());
    EXPECT_FALSE(cancellation.get());
    ASSERT_EQ(close.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const bool closed = close.get();
    EXPECT_TRUE(closed);
    if (closed) {
        _document = nullptr;
    }
}

TEST_F(DocumentCollaborationServiceTest, callsStartedAfterSealRejectWithoutTouchingDocument)
{
    auto* service = &_document->collaborationService();
    HookBarrier afterDrain;
    Internal::DocumentCollaborationServiceTestAccess::setPostAccessDrainHook(
        &HookBarrier::invoke);
    auto close = std::async(std::launch::async, [&] {
        return App::GetApplication().closeDocument(_documentName.c_str());
    });
    const bool drained = afterDrain.waitUntilEntered();

    auto status = std::async(std::launch::async, [&] {
        return service->sessionStatus(_session.sessionId());
    });
    auto cancellation = std::async(std::launch::async, [&] {
        return service->cancelEdit(_session.sessionId(), "sealed rejection");
    });

    const bool statusReady =
        status.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool cancellationReady =
        cancellation.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    afterDrain.release();
    Internal::DocumentCollaborationServiceTestAccess::setPostAccessDrainHook(nullptr);

    EXPECT_TRUE(drained);
    EXPECT_TRUE(statusReady);
    EXPECT_TRUE(cancellationReady);
    if (statusReady) {
        EXPECT_FALSE(status.get().has_value());
    }
    if (cancellationReady) {
        EXPECT_FALSE(cancellation.get());
    }
    ASSERT_EQ(close.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const bool closed = close.get();
    EXPECT_TRUE(closed);
    if (closed) {
        _document = nullptr;
    }
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

TEST_F(DocumentCollaborationServiceTest,
       wildcardMutationsConflictWhileTypedStructureRequiresRecomputeFirst)
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
    auto expectTypedMutationRequiresRecompute =
        [&](std::string operationId, auto&& mutate) {
            _document->recompute();
            auto pending = prepare(operationId, "Must Not Apply");
            std::forward<decltype(mutate)>(mutate)();
            const auto busy =
                _document->collaborationService().commitEdit(_session.sessionId(), pending);
            EXPECT_EQ(busy.status, DocumentCommitStatus::Busy);
            _document->recompute();
            const auto stale =
                _document->collaborationService().commitEdit(_session.sessionId(), pending);
            EXPECT_EQ(stale.status, DocumentCommitStatus::Conflict);
            EXPECT_EQ(_target->Label.getStrValue(), "Before");
        };

    auto* namespaceObject = _document->addObject<FeatureTest>("NamespaceMember");
    ASSERT_NE(namespaceObject, nullptr);

    auto* group = _document->addObject<DocumentObjectGroup>("Group");
    auto* firstMember = _document->addObject<FeatureTest>("FirstMember");
    auto* secondMember = _document->addObject<FeatureTest>("SecondMember");
    ASSERT_NE(group, nullptr);
    expectTypedMutationRequiresRecompute(
        "membership", [&] { group->addObject(firstMember); });
    expectTypedMutationRequiresRecompute("order", [&] {
        group->addObject(secondMember);
        group->removeObject(firstMember);
        group->addObject(firstMember);
    });

    expectOneRejectedEdit("tip", [&] { _document->Tip.setValue(namespaceObject); });

    auto* linkOwner = _document->addObject<FeatureTest>("LinkOwner");
    auto* link = dynamic_cast<PropertyLink*>(
        linkOwner->addDynamicProperty("App::PropertyLink", "TargetLink"));
    ASSERT_NE(link, nullptr);
    expectTypedMutationRequiresRecompute(
        "link", [&] { link->setValue(namespaceObject); });
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

    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto wildcardBefore =
        _document->collaborationRevisions().current(wildcard);
    auto removal = prepare("structural-remove", "unused", "structural-remove");
    const auto removalResult =
        _document->collaborationService().commitEdit(_session.sessionId(), removal);
    EXPECT_EQ(removalResult.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_document->getObject("Target"), _target);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcard), wildcardBefore);
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

TEST_F(DocumentCollaborationServiceTest, structuralGrantPreconditionsHaveDistinctDiagnostics)
{
    using Access = Internal::DocumentStructuralCompatibilityTestAccess;

    EXPECT_NE(Access::grantDiagnostic(*_document).find("notification barrier"),
              std::string::npos);
    EXPECT_NE(Access::withoutTransaction(*_document).find("native transaction"),
              std::string::npos);
    EXPECT_NE(Access::withoutRevisionSuppression(*_document).find("publication suppression"),
              std::string::npos);
    EXPECT_NE(Access::withForeignStableRead(*_document).find("foreign stable read"),
              std::string::npos);
    EXPECT_NE(Access::duringAtomicPresentationAudit(*_document).find(
                  "atomic presentation audit"),
              std::string::npos);
    EXPECT_NE(Access::reentrant(*_document).find("not reentrant"),
              std::string::npos);

    auto ownerDiagnostic = std::async(std::launch::async, [&] {
        return Access::grantDiagnostic(*_document);
    });
    EXPECT_NE(ownerDiagnostic.get().find("owner thread"), std::string::npos);
}

TEST_F(DocumentCollaborationServiceTest, poisonedDocumentCannotAcquireStructuralGrant)
{
    const auto diagnostic =
        Internal::DocumentStructuralCompatibilityTestAccess::onPoisonedDocument(*_document);
    EXPECT_NE(diagnostic.find("poisoned document"), std::string::npos);
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
    deletedConnection.disconnect();
    createdConnection.disconnect();
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

TEST_F(DocumentCollaborationServiceTest, recomputeFailureMessageNamesInvalidObject)
{
    auto prepared = prepare("recompute-failure", "unused", "recompute-failure");
    ASSERT_EQ(static_cast<FeatureTest*>(_target)->ExceptionType.getValue(), 0);

    const auto result =
        _document->collaborationService().commitEdit(_session.sessionId(), prepared);

    EXPECT_EQ(result.status, DocumentCommitStatus::RecomputeFailed);
    // Enriched message from DocumentCommitCoordinator::commitOnDocumentThread
    // must name the invalid object and carry its failure-time description
    // (FeatureTest ExceptionType=1 throws std::runtime_error("Test Exception")).
    EXPECT_NE(result.message.find("Target"), std::string::npos) << result.message;
    EXPECT_NE(result.message.find("Test Exception"), std::string::npos) << result.message;
    EXPECT_EQ(result.message.rfind("document recompute reported an object error", 0), 0)
        << result.message;
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
    EXPECT_EQ(nestedResult->status, DocumentCommitStatus::Busy)
        << nestedResult->message;
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

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationPrivateCommitStepIsExactOnceAndCannotRunOutsideBoundary)
{
    auto& service = _document->collaborationService();
    const auto inactive =
        Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service);
    EXPECT_FALSE(inactive.committed);

    std::optional<CollaborationAtomicCommitPointResult> first;
    std::optional<CollaborationAtomicCommitPointResult> second;
    const bool visibilityBefore = _target->Visibility.getValue();
    const std::vector<CollaborationAtomicPresentationWrite> allowedWrites {
        {_document->collaborationObjectIdentity(*_target), "Visibility"}};
    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] {
            _target->Visibility.setValue(!visibilityBefore);
            first.emplace(
                Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service));
            second.emplace(
                Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service));
        },
        allowedWrites);

    EXPECT_TRUE(result.committed()) << result.message;
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(first->committed) << first->diagnostic;
    EXPECT_FALSE(second->committed);
    EXPECT_NE(second->diagnostic.find("more than once"), std::string::npos);
    EXPECT_EQ(_target->Visibility.getValue(), !visibilityBefore);
    const auto after =
        Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service);
    EXPECT_FALSE(after.committed);
}

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationCannotBorrowStructuralCompatibilityAuthority)
{
    auto& service = _document->collaborationService();
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto revisionBefore = _document->collaborationRevisions().current(wildcard);

    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] { static_cast<void>(_document->addObject<FeatureTest>("AtomicTransient")); });

    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_document->getObject("AtomicTransient"), nullptr);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcard), revisionBefore);
}

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationMissingPrivateCommitStepRollsBackAndPublishesNothing)
{
    auto& service = _document->collaborationService();
    const bool visibilityBefore = _target->Visibility.getValue();
    const auto wildcard = DocumentRevisionKey::unknownModelMutation();
    const auto revisionBefore = _document->collaborationRevisions().current(wildcard);
    const std::vector<CollaborationAtomicPresentationWrite> allowedWrites {
        {_document->collaborationObjectIdentity(*_target), "Visibility"}};
    int observerCalls = 0;
    auto observer = _document->signalChangedObject.connect(
        [&](const DocumentObject&, const Property&) { ++observerCalls; });

    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] { _target->Visibility.setValue(!visibilityBefore); },
        allowedWrites);

    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(_target->Visibility.getValue(), visibilityBefore);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcard), revisionBefore);
    EXPECT_EQ(observerCalls, 0);
    observer.disconnect();
}

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationExceptionBeforePrivateCommitRollsBack)
{
    auto& service = _document->collaborationService();
    const bool visibilityBefore = _target->Visibility.getValue();
    const std::vector<CollaborationAtomicPresentationWrite> allowedWrites {
        {_document->collaborationObjectIdentity(*_target), "Visibility"}};

    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] {
            _target->Visibility.setValue(!visibilityBefore);
            throw std::runtime_error("injected pre-commit failure");
        },
        allowedWrites);

    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_NE(result.message.find("injected pre-commit failure"), std::string::npos);
    EXPECT_EQ(_target->Visibility.getValue(), visibilityBefore);
}

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationExceptionAfterPrivateCommitPoisonsFurtherCommits)
{
    auto& service = _document->collaborationService();
    const bool visibilityBefore = _target->Visibility.getValue();
    std::optional<CollaborationAtomicCommitPointResult> committed;
    const std::vector<CollaborationAtomicPresentationWrite> allowedWrites {
        {_document->collaborationObjectIdentity(*_target), "Visibility"}};

    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] {
            _target->Visibility.setValue(!visibilityBefore);
            committed.emplace(
                Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service));
            throw std::runtime_error("injected post-commit failure");
        },
        allowedWrites);

    ASSERT_TRUE(committed.has_value());
    ASSERT_TRUE(committed->committed) << committed->diagnostic;
    EXPECT_EQ(result.status, DocumentCommitStatus::RollbackFailed);
    EXPECT_EQ(_target->Visibility.getValue(), !visibilityBefore);
    const auto rejected = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service, [] {});
    EXPECT_EQ(rejected.status, DocumentCommitStatus::RollbackFailed);
}

TEST_F(DocumentCollaborationServiceTest,
       atomicPresentationRejectsVisibilityOutsideDeclaredStableIdentity)
{
    auto& service = _document->collaborationService();
    auto* other = _document->addObject<FeatureTest>("OtherVisibilityTarget");
    ASSERT_NE(other, nullptr);
    const bool otherBefore = other->Visibility.getValue();
    std::optional<CollaborationAtomicCommitPointResult> privateCommit;
    const std::vector<CollaborationAtomicPresentationWrite> allowedWrites {
        {_document->collaborationObjectIdentity(*_target), "Visibility"}};

    const auto result = Internal::DocumentCollaborationServiceTestAccess::serializeAtomic(
        service,
        [&] {
            other->Visibility.setValue(!otherBefore);
            privateCommit.emplace(
                Internal::DocumentCollaborationServiceTestAccess::commitAtomic(service));
        },
        allowedWrites);

    ASSERT_TRUE(privateCommit.has_value());
    EXPECT_FALSE(privateCommit->committed);
    EXPECT_NE(privateCommit->diagnostic.find("undeclared"), std::string::npos);
    EXPECT_EQ(result.status, DocumentCommitStatus::ApplyFailed);
    EXPECT_EQ(other->Visibility.getValue(), otherBefore);
}
