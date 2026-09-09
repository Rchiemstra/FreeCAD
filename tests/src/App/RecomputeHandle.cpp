// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/FeatureTest.h>
#include <App/GenericIsolatedRecompute.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/Range.h>
#include <App/RecomputeHandle.h>
#include <App/Transactions.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using namespace std::chrono_literals;

constexpr std::string_view BlockingRecomputeOperationType =
    "FreeCAD.Tests.BlockingRecomputeHandle";
constexpr std::string_view RemovingRecomputeOperationType =
    "FreeCAD.Tests.RemovingRecomputeHandle";

static_assert(std::is_same_v<App::DocumentRecomputeId, std::uint64_t>);
static_assert(!std::is_pointer_v<App::DocumentRecomputeId>);

class BlockingRecomputeState
{
public:
    bool block(const std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        _started = true;
        _changed.notify_all();
        return _changed.wait(lock, stopToken, [&] { return _released; });
    }

    bool waitUntilStarted(const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] { return _started; });
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
    std::mutex _mutex;
    std::condition_variable_any _changed;
    bool _started {false};
    bool _released {false};
};

class BlockingRecomputeStore
{
public:
    static void add(const std::string& token,
                    const std::shared_ptr<BlockingRecomputeState>& state)
    {
        std::lock_guard lock(Mutex);
        States[token] = state;
    }

    static void remove(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        States.erase(token);
    }

    static std::shared_ptr<BlockingRecomputeState> get(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        const auto found = States.find(token);
        return found == States.end() ? nullptr : found->second.lock();
    }

private:
    static inline std::mutex Mutex;
    static inline std::map<std::string, std::weak_ptr<BlockingRecomputeState>> States;
};

class NoopRecomputeOperation final: public App::CollaborativeOperation
{
public:
    std::string_view typeId() const noexcept override
    {
        return BlockingRecomputeOperationType;
    }

    void apply(App::Document&) const override
    {}

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document&) const override
    {
        return {true, {}};
    }
};

class SchedulerProbeFeature final: public App::FeatureTest
{
public:
    App::DocumentObjectExecReturn* execute() override
    {
        ++executeCalls;
        sawPendingRecompute =
            sawPendingRecompute || testStatus(App::ObjectStatus::PendingRecompute);
        sawSecondPass = sawSecondPass || testStatus(App::ObjectStatus::Recompute2);
        if (touchOnce) {
            auto* target = std::exchange(touchOnce, nullptr);
            target->touch();
        }
        return App::DocumentObject::StdReturn;
    }

    App::DocumentObject* touchOnce {nullptr};
    int executeCalls {0};
    bool sawPendingRecompute {false};
    bool sawSecondPass {false};
};

class DestructionProbeFeature final: public App::FeatureTest
{
public:
    ~DestructionProbeFeature() override
    {
        if (destroyed) {
            *destroyed = true;
        }
    }

    std::shared_ptr<bool> destroyed;
};

class SelfRemovingFeature final: public App::FeatureTest
{
public:
    App::DocumentObjectExecReturn* execute() override
    {
        ++executeCalls;
        auto* document = getDocument();
        const std::string name = getNameInDocument();
        document->removeObject(name.c_str());
        sawDeferredRemoval = document->getObject(name.c_str()) == this;
        return App::DocumentObject::StdReturn;
    }

    ~SelfRemovingFeature() override
    {
        if (destroyed) {
            *destroyed = true;
        }
    }

    std::shared_ptr<bool> destroyed;
    int executeCalls {0};
    bool sawDeferredRemoval {false};
};

struct TransactionDestructionReentryState
{
    App::Document* document {nullptr};
    bool destroyed {false};
    bool transactionControlRejected {false};
    bool transactionControlAdmitted {false};
};

class TransactionDestructionReentryFeature final: public App::FeatureTest
{
public:
    ~TransactionDestructionReentryFeature() override
    {
        if (!state) {
            return;
        }
        if (auto* document = state->document) {
            try {
                const int transactionId =
                    document->openTransaction("destructor replay reentry probe");
                state->transactionControlAdmitted = transactionId != 0;
                if (transactionId != 0) {
                    document->abortTransaction();
                }
            }
            catch (const Base::Exception&) {
                state->transactionControlRejected = true;
            }
            catch (const std::exception&) {
                state->transactionControlRejected = true;
            }
        }
        state->destroyed = true;
    }

    std::shared_ptr<TransactionDestructionReentryState> state;
};

class RemovingRecomputeOperation final: public App::CollaborativeOperation
{
public:
    RemovingRecomputeOperation(std::string target, std::string identity)
        : _target(std::move(target))
        , _identity(std::move(identity))
    {}

    std::string_view typeId() const noexcept override
    {
        return RemovingRecomputeOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = document.getObject(_target.c_str());
        if (!target || document.collaborationObjectIdentity(*target) != _identity) {
            throw std::runtime_error("removing recompute target became stale");
        }
        Base::ObjectStatusLocker<App::Document::Status, App::Document> recomputing(
            App::Document::Recomputing, &document);
        document.removeObject(_target.c_str());
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        return {document.getObject(_target.c_str()) == nullptr,
                "removing recompute target must be absent"};
    }

private:
    std::string _target;
    std::string _identity;
};

void ensureBlockingRecomputeAdapterRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(BlockingRecomputeOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                if (intent.arguments.empty() || intent.arguments.size() > 4
                    || !intent.arguments.contains("token")
                    || std::ranges::any_of(intent.arguments, [](const auto& argument) {
                           return argument.first != "token"
                               && argument.first != "target"
                               && argument.first != "fail"
                               && argument.first != "no_publish"
                               && argument.first != "publish_existence";
                       })) {
                    throw std::invalid_argument("invalid blocking recompute intent");
                }
                const bool fail = intent.arguments.contains("fail");
                if (fail && intent.arguments.at("fail") != "1") {
                    throw std::invalid_argument(
                        "invalid blocking recompute failure mode");
                }
                const bool noPublish = intent.arguments.contains("no_publish");
                if (noPublish && intent.arguments.at("no_publish") != "1") {
                    throw std::invalid_argument(
                        "invalid blocking recompute publication mode");
                }
                const bool publishExistence =
                    intent.arguments.contains("publish_existence");
                if (publishExistence
                    && intent.arguments.at("publish_existence") != "1") {
                    throw std::invalid_argument(
                        "invalid blocking recompute existence mode");
                }
                if (noPublish && publishExistence) {
                    throw std::invalid_argument(
                        "blocking recompute publication modes conflict");
                }
                auto state = BlockingRecomputeStore::get(intent.arguments.at("token"));
                if (!state) {
                    throw std::invalid_argument("unknown blocking recompute state");
                }
                std::vector<App::DocumentRevisionKey> reads;
                std::vector<App::DocumentRevisionKey> writes;
                std::vector<App::DocumentRevisionPublicationRequest> effects;
                if (const auto targetArgument = intent.arguments.find("target");
                    targetArgument != intent.arguments.end()) {
                    const auto* target =
                        document.getObject(targetArgument->second.c_str());
                    if (!target) {
                        throw std::invalid_argument(
                            "unknown blocking recompute target");
                    }
                    const auto targetRevision = publishExistence
                        ? App::DocumentRevisionKey::objectExistence(
                              targetArgument->second)
                        : App::DocumentRevisionKey::objectModel(
                              targetArgument->second);
                    reads.push_back(targetRevision);
                    if (!noPublish) {
                        writes.push_back(targetRevision);
                        effects.push_back(
                            {targetRevision,
                             document.collaborationObjectIdentity(*target)});
                    }
                }
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [state = std::move(state), fail](const std::stop_token stopToken) {
                        if (!state->block(stopToken)) {
                            throw std::runtime_error("blocking recompute was cancelled");
                        }
                        if (fail) {
                            throw std::runtime_error(
                                "forced blocking recompute preparation failure");
                        }
                        return std::make_unique<const NoopRecomputeOperation>();
                    };
                return App::CollaborativeOperationPreparation {
                    std::move(reads),
                    std::move(writes),
                    std::move(effects),
                    std::move(task),
                    App::PreparationPolicy::DetachedInProcess};
            }));
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(RemovingRecomputeOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                if (intent.arguments.size() != 1
                    || !intent.arguments.contains("target")) {
                    throw std::invalid_argument("invalid removing recompute intent");
                }
                const std::string targetName = intent.arguments.at("target");
                const auto* target = document.getObject(targetName.c_str());
                if (!target) {
                    throw std::invalid_argument("unknown removing recompute target");
                }
                const std::string identity =
                    document.collaborationObjectIdentity(*target);
                const auto existence =
                    App::DocumentRevisionKey::objectExistence(targetName);
                const auto structure =
                    App::DocumentRevisionKey::objectStructure(targetName);
                const auto documentStructure =
                    App::DocumentRevisionKey::documentStructure();
                const auto wildcard =
                    App::DocumentRevisionKey::unknownModelMutation();
                std::vector<App::DocumentRevisionKey> revisions {
                    existence, structure, documentStructure, wildcard};
                std::vector<App::DocumentRevisionPublicationRequest> effects {
                    {existence, identity},
                    {structure, identity},
                    {documentStructure, std::nullopt},
                    {wildcard, std::nullopt}};
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [targetName, identity](const std::stop_token stopToken) {
                        if (stopToken.stop_requested()) {
                            throw std::runtime_error(
                                "removing recompute was cancelled");
                        }
                        return std::make_unique<const RemovingRecomputeOperation>(
                            targetName, identity);
                    };
                auto reads = revisions;
                auto writes = std::move(revisions);
                return App::CollaborativeOperationPreparation {
                    std::move(reads),
                    std::move(writes),
                    std::move(effects),
                    std::move(task),
                    App::PreparationPolicy::DetachedInProcess};
            }));
    });
}

class RecomputeHandleTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureBlockingRecomputeAdapterRegistered();
    }

    void SetUp() override
    {
        _documentName = App::GetApplication().getUniqueDocumentName("recomputeHandle");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "Recompute handle test");
        ASSERT_NE(_document, nullptr);
        _blockingToken = _documentName + "-blocking";
        _blocking = std::make_shared<BlockingRecomputeState>();
        BlockingRecomputeStore::add(_blockingToken, _blocking);
    }

    void TearDown() override
    {
        if (_blocking) {
            _blocking->release();
        }
        if (!_blockingToken.empty()) {
            BlockingRecomputeStore::remove(_blockingToken);
        }
        if (_document
            && App::GetApplication().getDocument(_documentName.c_str()) == _document) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
        for (const auto& name : _extraDocumentNames) {
            if (App::GetApplication().getDocument(name.c_str())) {
                App::GetApplication().closeDocument(name.c_str());
            }
        }
        if (!_savePath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(_savePath, ignored);
        }
        if (!_dependencyPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(_dependencyPath, ignored);
        }
    }

    std::unique_ptr<App::RecomputeHandle> blockingHandle(
        App::DocumentObject* target = nullptr,
        const bool fail = false,
        const bool publishTarget = true,
        const bool publishExistence = false)
    {
        App::DocumentRecomputeFeatureRequest feature;
        feature.featureId = target && target->getNameInDocument()
            ? target->getNameInDocument()
            : "blocking-probe";
        if (target) {
            feature.stableObjectIdentity =
                _document->collaborationObjectIdentity(*target);
            feature.presentationObjectModelRevision =
                _document->collaborationRevisions().current(
                    App::DocumentRevisionKey::objectModel(feature.featureId));
        }
        feature.operationId = "blocking-recompute-handle";
        feature.intent.operationType = std::string(BlockingRecomputeOperationType);
        feature.intent.arguments = {{"token", _blockingToken}};
        if (target && target->getNameInDocument()) {
            feature.intent.arguments.emplace("target", target->getNameInDocument());
        }
        if (fail) {
            feature.intent.arguments.emplace("fail", "1");
        }
        if (target && !publishTarget) {
            feature.intent.arguments.emplace("no_publish", "1");
        }
        if (target && publishExistence) {
            feature.intent.arguments.emplace("publish_existence", "1");
        }
        feature.provenance = "native recompute handle cancellation test";

        App::DocumentRecomputeRequest request;
        request.features.push_back(std::move(feature));
        const auto id = _document->recomputeCoordinator().submit(std::move(request));
        return std::make_unique<App::RecomputeHandle>(*_document, id);
    }

    std::string _documentName;
    std::string _blockingToken;
    App::Document* _document {nullptr};
    std::shared_ptr<BlockingRecomputeState> _blocking;
    std::vector<std::string> _extraDocumentNames;
    std::filesystem::path _savePath;
    std::filesystem::path _dependencyPath;
};

std::string readFileBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_F(RecomputeHandleTest, asyncFacadeCommitsThroughTheProductionFreeCADCmdBackend)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("AsyncProcessColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("E");
    ASSERT_EQ(feature->Value.getValue(), 0);

    auto handle = _document->recomputeAsync({feature});
    ASSERT_NE(handle, nullptr);
    EXPECT_GT(handle->id(), 0U);

    const auto snapshot = handle->wait(30s);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.id, handle->id());
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(snapshot.completedFeatures, 1U);
    EXPECT_EQ(snapshot.failedFeatures, 0U);
    EXPECT_EQ(snapshot.totalFeatures, 1U);
    EXPECT_DOUBLE_EQ(snapshot.progress, 1.0);
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_EQ(snapshot.features.front().featureId, feature->getNameInDocument());
    EXPECT_EQ(snapshot.features.front().state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(feature->Value.getValue(), 4);
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
}

TEST_F(RecomputeHandleTest,
       asyncBeforeObserverMutationIsCapturedBeforeDetachedPreparation)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("AsyncBeforeMutation");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");

    int beforeSignals = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            ++beforeSignals;
            feature->Column.setValue("E");
        });

    auto handle = _document->recomputeAsync({feature});
    ASSERT_NE(handle, nullptr);
    const auto terminal = handle->wait(30s);

    ASSERT_TRUE(terminal.terminal()) << terminal.diagnostic;
    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::Completed)
        << terminal.diagnostic;
    EXPECT_EQ(beforeSignals, 1);
    EXPECT_STREQ(feature->Column.getValue(), "E");
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("E"));
}

TEST_F(RecomputeHandleTest,
       asyncBeforeObserverCanAddACleanIndependentFeatureToTheFinalPlan)
{
    auto* initiallyDirty =
        _document->addObject<App::FeatureTestColumn>("AsyncBeforeInitial");
    auto* touchedByObserver =
        _document->addObject<App::FeatureTestColumn>("AsyncBeforeAdded");
    ASSERT_NE(initiallyDirty, nullptr);
    ASSERT_NE(touchedByObserver, nullptr);
    initiallyDirty->Column.setValue("B");
    touchedByObserver->Column.setValue("C");
    ASSERT_EQ(_document->recompute(), 2);
    initiallyDirty->Column.setValue("D");
    ASSERT_TRUE(initiallyDirty->mustRecompute());
    ASSERT_FALSE(touchedByObserver->mustRecompute());

    int beforeSignals = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            ++beforeSignals;
            touchedByObserver->Column.setValue("E");
        });

    auto handle = _document->recomputeAsync();
    ASSERT_NE(handle, nullptr);
    const auto terminal = handle->wait(45s);

    ASSERT_TRUE(terminal.terminal()) << terminal.diagnostic;
    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::Completed)
        << terminal.diagnostic;
    EXPECT_EQ(terminal.totalFeatures, 2U);
    EXPECT_EQ(beforeSignals, 1);
    EXPECT_EQ(initiallyDirty->Value.getValue(), App::decodeColumn("D"));
    EXPECT_EQ(touchedByObserver->Value.getValue(), App::decodeColumn("E"));
    EXPECT_FALSE(initiallyDirty->mustRecompute());
    EXPECT_FALSE(touchedByObserver->mustRecompute());
}

TEST_F(RecomputeHandleTest,
       identicalActiveAsyncFacadePlansPublishOneBeforeAndOneAggregate)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("AsyncCoalescingSignals");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("D");

    int beforeSignals = 0;
    int aggregateSignals = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) { ++beforeSignals; });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            ++aggregateSignals;
        });

    auto first = _document->recomputeAsync({feature});
    auto identical = _document->recomputeAsync({feature});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(identical, nullptr);
    EXPECT_EQ(first->id(), identical->id());
    EXPECT_EQ(beforeSignals, 1);

    const auto terminal = first->wait(30s);
    ASSERT_TRUE(terminal.terminal()) << terminal.diagnostic;
    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::Completed)
        << terminal.diagnostic;
    EXPECT_EQ(identical->status().state,
              App::DocumentRecomputeState::Completed);
    EXPECT_EQ(beforeSignals, 1);
    EXPECT_EQ(aggregateSignals, 1);
}

TEST_F(RecomputeHandleTest,
       asyncBeforeObserversRetainARemovedExplicitRootThroughCaptureBoundary)
{
    auto destroyed = std::make_shared<bool>(false);
    auto owner = std::make_unique<DestructionProbeFeature>();
    auto* target = owner.get();
    target->destroyed = destroyed;
    _document->addObject(owner.get(), "AsyncBeforeRemovalTarget");
    static_cast<void>(owner.release());

    bool laterObserverSawLiveObject = false;
    bool stableSawDestruction = false;
    auto removingConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            _document->removeObject("AsyncBeforeRemovalTarget");
            EXPECT_FALSE(*destroyed);
        });
    auto observingConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            laterObserverSawLiveObject = !*destroyed
                && _document->getObject("AsyncBeforeRemovalTarget") == target;
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { stableSawDestruction = *destroyed; });

    auto handle = _document->recomputeAsync({target});
    ASSERT_NE(handle, nullptr);
    const auto terminal = handle->wait(30s);

    EXPECT_TRUE(terminal.terminal()) << terminal.diagnostic;
    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(stableSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("AsyncBeforeRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       asyncSkipObserversRetainSelectedObjectsUntilEverySlotReturns)
{
    auto destroyed = std::make_shared<bool>(false);
    auto owner = std::make_unique<DestructionProbeFeature>();
    auto* target = owner.get();
    target->destroyed = destroyed;
    _document->addObject(owner.get(), "AsyncSkippedRemovalTarget");
    static_cast<void>(owner.release());
    _document->setStatus(App::Document::SkipRecompute, true);

    bool laterObserverSawLiveObject = false;
    bool stableSawDestruction = false;
    int beforeSignals = 0;
    int aggregateSignals = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) { ++beforeSignals; });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            ++aggregateSignals;
        });
    auto removingConnection = _document->signalSkipRecompute.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
            _document->removeObject("AsyncSkippedRemovalTarget");
        });
    auto observingConnection = _document->signalSkipRecompute.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            laterObserverSawLiveObject = !*destroyed
                && std::ranges::find(objects, target) != objects.end();
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { stableSawDestruction = *destroyed; });

    auto handle = _document->recomputeAsync({target});
    _document->setStatus(App::Document::SkipRecompute, false);
    ASSERT_NE(handle, nullptr);
    const auto terminal = handle->wait(30s);

    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::Completed);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(stableSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("AsyncSkippedRemovalTarget"), nullptr);
    EXPECT_EQ(beforeSignals, 0);
    EXPECT_EQ(aggregateSignals, 0);
}

TEST_F(RecomputeHandleTest,
       asyncFacadeRejectsDirtyWorkerUnsafeFeatureAndLeavesItUnresolved)
{
    App::FeatureTestAsyncBlocker::resetBlocker();
    std::mutex watchdogMutex;
    std::condition_variable_any watchdogChanged;
    std::jthread watchdog([&](const std::stop_token stopToken) {
        std::unique_lock lock(watchdogMutex);
        static_cast<void>(watchdogChanged.wait_for(
            lock, stopToken, 6s, [] { return false; }));
        if (!stopToken.stop_requested()) {
            App::FeatureTestAsyncBlocker::releaseBlocker();
        }
    });
    auto* feature =
        _document->addObject<App::FeatureTestAsyncBlocker>("WorkerUnsafeFeature");
    ASSERT_NE(feature, nullptr);
    feature->purgeTouched();
    feature->touch();
    ASSERT_TRUE(feature->mustRecompute());

    auto handle = _document->recomputeAsync({feature});
    ASSERT_NE(handle, nullptr);
    auto snapshot = handle->wait(30s);
    if (!snapshot.terminal()) {
        static_cast<void>(handle->cancel("worker-unsafe rejection test timeout"));
        snapshot = handle->wait(10s);
    }

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    EXPECT_EQ(snapshot.completedFeatures, 0U);
    EXPECT_EQ(snapshot.failedFeatures, 1U);
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_EQ(snapshot.features.front().state,
              App::DocumentRecomputeFeatureState::Failed);
    EXPECT_NE(
        snapshot.features.front().diagnostic.find(
            "has not opted into isolated execution"),
        std::string::npos);
    EXPECT_FALSE(
        App::FeatureTestAsyncBlocker::waitUntilStarted(std::chrono::milliseconds(0)));
    EXPECT_TRUE(feature->isTouched());
    EXPECT_TRUE(feature->isError());
}

TEST_F(RecomputeHandleTest,
       terminalSnapshotStatesCannotPresentAgainstSameNameReplacements)
{
    const std::vector<std::string> names {
        "CommittedReplacement", "FailedReplacement", "BlockedReplacement"};
    std::map<std::string, std::string> originalIdentities;
    for (const auto& name : names) {
        auto* object = _document->addObject<App::FeatureTest>(name.c_str());
        ASSERT_NE(object, nullptr);
        object->purgeTouched();
        originalIdentities.emplace(
            name, _document->collaborationObjectIdentity(*object));
    }

    App::DocumentRecomputeFeatureRequest committed;
    committed.featureId = names[0];
    committed.operationId = "same-name-committed";
    committed.intent.operationType = std::string(BlockingRecomputeOperationType);
    committed.intent.arguments = {{"token", _blockingToken}, {"target", names[0]}};
    committed.provenance = "same-name terminal presentation regression";
    committed.stableObjectIdentity = originalIdentities.at(names[0]);
    committed.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(names[0]));

    App::DocumentRecomputeFeatureRequest failed;
    failed.featureId = names[1];
    failed.operationId = "same-name-failed";
    failed.intent.operationType = "FreeCAD.Tests.UnregisteredRecomputeOperation";
    failed.provenance = "same-name terminal presentation regression";
    failed.stableObjectIdentity = originalIdentities.at(names[1]);
    failed.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(names[1]));

    App::DocumentRecomputeFeatureRequest blocked;
    blocked.featureId = names[2];
    blocked.dependencies = {names[1]};
    blocked.operationId = "same-name-blocked";
    blocked.intent.operationType = std::string(BlockingRecomputeOperationType);
    blocked.intent.arguments = {{"token", _blockingToken}, {"target", names[2]}};
    blocked.provenance = "same-name terminal presentation regression";
    blocked.stableObjectIdentity = originalIdentities.at(names[2]);
    blocked.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(names[2]));

    App::DocumentRecomputeRequest request;
    request.features = {
        std::move(committed), std::move(failed), std::move(blocked)};
    const auto id = _document->recomputeCoordinator().submit(std::move(request));
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));
    _blocking->release();

    // Drive only the pointer-free coordinator to terminal. Deliberately leave
    // handle presentation unclaimed until after the original object is gone.
    std::optional<App::DocumentRecomputeSnapshot> terminal;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(_document->recomputeCoordinator().poll(id));
        terminal = _document->recomputeCoordinator().status(id);
        if (terminal && terminal->terminal()) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    if (!terminal || !terminal->terminal()) {
        static_cast<void>(_document->recomputeCoordinator().cancel(
            id, "same-name regression timeout"));
        const auto cancellationDeadline = std::chrono::steady_clock::now() + 5s;
        do {
            static_cast<void>(_document->recomputeCoordinator().poll(id));
            terminal = _document->recomputeCoordinator().status(id);
            if (terminal && terminal->terminal()) {
                break;
            }
            std::this_thread::sleep_for(2ms);
        } while (std::chrono::steady_clock::now() < cancellationDeadline);
    }
    ASSERT_TRUE(terminal.has_value());
    ASSERT_TRUE(terminal->terminal()) << terminal->diagnostic;
    ASSERT_EQ(terminal->features.size(), 3U);
    const std::map<std::string, App::DocumentRecomputeFeatureState> expectedStates {
        {names[0], App::DocumentRecomputeFeatureState::Committed},
        {names[1], App::DocumentRecomputeFeatureState::Failed},
        {names[2], App::DocumentRecomputeFeatureState::Blocked}};
    for (const auto& feature : terminal->features) {
        EXPECT_EQ(feature.state, expectedStates.at(feature.featureId));
        EXPECT_EQ(feature.stableObjectIdentity,
                  originalIdentities.at(feature.featureId));
    }

    std::vector<App::DocumentObject*> replacements;
    for (const auto& name : names) {
        _document->removeObject(name.c_str());
        auto* replacement = _document->addObject<App::FeatureTest>(name.c_str());
        ASSERT_NE(replacement, nullptr);
        ASSERT_NE(_document->collaborationObjectIdentity(*replacement),
                  originalIdentities.at(name));
        replacement->purgeTouched();
        replacement->resetError();
        replacements.push_back(replacement);
    }
    EXPECT_FALSE(_document->recomputeCoordinator().hasUnresolvedWork());

    int replacementObjectSignals = 0;
    int replacementAggregateEntries = 0;
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (std::ranges::find(replacements, &object) != replacements.end()) {
                ++replacementObjectSignals;
            }
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            for (auto* replacement : replacements) {
                replacementAggregateEntries += static_cast<int>(
                    std::ranges::count(objects, replacement));
            }
        });

    App::RecomputeHandle handle(*_document, id);
    const auto presented = handle->status();

    EXPECT_EQ(presented.state, App::DocumentRecomputeState::PartialFailure);
    for (auto* replacement : replacements) {
        EXPECT_FALSE(replacement->mustRecompute());
        EXPECT_TRUE(replacement->isValid());
        EXPECT_FALSE(replacement->isError());
        EXPECT_EQ(_document->getErrorDescription(replacement), nullptr);
    }
    EXPECT_EQ(replacementObjectSignals, 0);
    EXPECT_EQ(replacementAggregateEntries, 0);
}

TEST_F(RecomputeHandleTest,
       waitingNodeCannotRetargetToASameNameReplacementBeforePreparation)
{
    auto* upstream = _document->addObject<App::FeatureTest>("IdentityFenceUpstream");
    auto* original =
        _document->addObject<App::FeatureTestColumn>("IdentityFenceDownstream");
    ASSERT_NE(upstream, nullptr);
    ASSERT_NE(original, nullptr);
    upstream->purgeTouched();
    original->Column.setValue("B");
    const auto upstreamIdentity =
        _document->collaborationObjectIdentity(*upstream);
    const auto originalIdentity =
        _document->collaborationObjectIdentity(*original);

    App::DocumentRecomputeFeatureRequest root;
    root.featureId = upstream->getNameInDocument();
    root.operationId = "identity-fence-upstream";
    root.intent.operationType = std::string(BlockingRecomputeOperationType);
    root.intent.arguments = {
        {"token", _blockingToken}, {"target", root.featureId}};
    root.provenance = "waiting-node identity fence regression";
    root.stableObjectIdentity = upstreamIdentity;
    root.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(root.featureId));

    App::DocumentRecomputeFeatureRequest downstream;
    downstream.featureId = original->getNameInDocument();
    downstream.dependencies = {root.featureId};
    downstream.operationId = "identity-fence-downstream";
    downstream.intent.operationType =
        std::string(App::GenericIsolatedRecomputeOperationType);
    downstream.intent.arguments = {
        {"feature", downstream.featureId},
        {"stable_object_identity", originalIdentity}};
    downstream.provenance = "waiting-node identity fence regression";
    downstream.stableObjectIdentity = originalIdentity;
    downstream.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(downstream.featureId));

    App::DocumentRecomputeRequest request;
    request.features = {std::move(root), std::move(downstream)};
    const auto id = _document->recomputeCoordinator().submit(std::move(request));
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    _document->removeObject("IdentityFenceDownstream");
    auto* replacement =
        _document->addObject<App::FeatureTestColumn>("IdentityFenceDownstream");
    ASSERT_NE(replacement, nullptr);
    replacement->Column.setValue("E");
    ASSERT_NE(_document->collaborationObjectIdentity(*replacement),
              originalIdentity);
    ASSERT_EQ(replacement->Value.getValue(), 0);

    _blocking->release();
    App::RecomputeHandle handle(*_document, id);
    const auto terminal = handle.wait(30s);

    ASSERT_TRUE(terminal.terminal()) << terminal.diagnostic;
    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure);
    const auto result = std::ranges::find(
        terminal.features,
        std::string("IdentityFenceDownstream"),
        &App::DocumentRecomputeFeatureSnapshot::featureId);
    ASSERT_NE(result, terminal.features.end());
    EXPECT_EQ(result->state, App::DocumentRecomputeFeatureState::Failed);
    EXPECT_NE(result->diagnostic.find("incarnation is stale"),
              std::string::npos);
    EXPECT_EQ(result->stableObjectIdentity, originalIdentity);
    EXPECT_EQ(replacement->Value.getValue(), 0);
}

TEST_F(RecomputeHandleTest,
       newerSynchronousRecomputeWinsOverAnOlderStaleAsyncResult)
{
    auto* feature = _document->addObject<App::FeatureTest>("SyncWinsFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(_document->recompute({feature}), 1);
    const int baselineExecuteCount = feature->ExecCount.getValue();
    feature->touch();

    auto handle = blockingHandle(feature);
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    bool syncHasError = true;
    EXPECT_EQ(_document->recompute({feature}, false, &syncHasError), 1);
    ASSERT_FALSE(syncHasError);
    ASSERT_EQ(feature->ExecCount.getValue(), baselineExecuteCount + 1);
    ASSERT_FALSE(feature->mustRecompute());
    ASSERT_TRUE(feature->isValid());

    _blocking->release();
    const auto snapshot = handle->wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_EQ(snapshot.features.front().state,
              App::DocumentRecomputeFeatureState::Stale)
        << snapshot.features.front().diagnostic;
    EXPECT_EQ(feature->ExecCount.getValue(), baselineExecuteCount + 1);
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    EXPECT_FALSE(feature->isError());
    EXPECT_FALSE(_document->recomputeCoordinator().hasUnresolvedWork());
}

TEST_F(RecomputeHandleTest,
       lateAsyncPreparationFailureCannotRedirtyANewerSynchronousResult)
{
    auto* feature = _document->addObject<App::FeatureTest>("LateFailureFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(_document->recompute({feature}), 1);
    const int baselineExecuteCount = feature->ExecCount.getValue();
    feature->touch();

    auto handle = blockingHandle(feature, /*fail=*/true);
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    bool syncHasError = true;
    ASSERT_EQ(_document->recompute({feature}, false, &syncHasError), 1);
    ASSERT_FALSE(syncHasError);
    ASSERT_EQ(feature->ExecCount.getValue(), baselineExecuteCount + 1);
    ASSERT_FALSE(feature->mustRecompute());
    ASSERT_TRUE(feature->isValid());
    ASSERT_FALSE(feature->isError());

    int objectSignals = 0;
    int aggregateEntries = 0;
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == feature) {
                ++objectSignals;
            }
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            aggregateEntries += static_cast<int>(
                std::ranges::count(objects, feature));
        });

    _blocking->release();
    const auto snapshot = handle->wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_EQ(snapshot.features.front().state,
              App::DocumentRecomputeFeatureState::Failed)
        << snapshot.features.front().diagnostic;
    ASSERT_TRUE(snapshot.features.front().presentationObjectModelRevision);
    EXPECT_NE(*snapshot.features.front().presentationObjectModelRevision,
              _document->collaborationRevisions().current(
                  App::DocumentRevisionKey::objectModel("LateFailureFeature")));
    EXPECT_EQ(feature->ExecCount.getValue(), baselineExecuteCount + 1);
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    EXPECT_FALSE(feature->isError());
    EXPECT_EQ(_document->getErrorDescription(feature), nullptr);
    EXPECT_EQ(objectSignals, 0);
    EXPECT_EQ(aggregateEntries, 0);
    EXPECT_FALSE(_document->recomputeCoordinator().hasUnresolvedWork());
}

TEST_F(RecomputeHandleTest,
       lateAsyncFailureKeepsNewerDirtyWorkUnresolvedWithoutOldPresentation)
{
    auto* feature = _document->addObject<App::FeatureTest>("LateDirtyFailureFeature");
    ASSERT_NE(feature, nullptr);
    ASSERT_EQ(_document->recompute({feature}), 1);
    feature->touch();

    auto handle = blockingHandle(feature, /*fail=*/true);
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    const auto revisionBeforeEdit =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel("LateDirtyFailureFeature"));
    feature->Label.setValue("newer unsatisfied edit");
    ASSERT_GT(
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel("LateDirtyFailureFeature")),
        revisionBeforeEdit);
    ASSERT_TRUE(feature->mustRecompute());

    int objectSignals = 0;
    int aggregateEntries = 0;
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == feature) {
                ++objectSignals;
            }
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            aggregateEntries += static_cast<int>(
                std::ranges::count(objects, feature));
        });

    _blocking->release();
    const auto snapshot = handle->wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_EQ(snapshot.features.front().state,
              App::DocumentRecomputeFeatureState::Failed);
    EXPECT_TRUE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    EXPECT_FALSE(feature->isError());
    EXPECT_EQ(_document->getErrorDescription(feature), nullptr);
    EXPECT_EQ(objectSignals, 0);
    EXPECT_EQ(aggregateEntries, 0);
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedWork());
    EXPECT_TRUE(
        _document->recomputeCoordinator().hasUnresolvedExecutableWork());
}

TEST_F(RecomputeHandleTest,
       removedTargetIsNotRetainedInTheTerminalFailureLedger)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("RemovedFailureTarget");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("D");

    auto handle = blockingHandle(feature, /*fail=*/true);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));
    _document->removeObject("RemovedFailureTarget");
    _blocking->release();
    const auto terminal = handle->wait(10s);

    ASSERT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure)
        << terminal.diagnostic;
    EXPECT_EQ(_document->getObject("RemovedFailureTarget"), nullptr);
    EXPECT_FALSE(_document->recomputeCoordinator().hasUnresolvedWork());
    EXPECT_FALSE(
        _document->recomputeCoordinator().hasUnresolvedExecutableWork());
}

TEST_F(RecomputeHandleTest,
       blockedCleanDependentReceivesATrustedFailureFenceAndBlocksSave)
{
    App::Internal::ensureGenericIsolatedRecomputeRegistered();
    auto* upstream =
        _document->addObject<App::FeatureTestColumn>("A_FailedUpstream");
    auto* dependent =
        _document->addObject<App::FeatureTest>("Z_BlockedDependent");
    ASSERT_NE(upstream, nullptr);
    ASSERT_NE(dependent, nullptr);
    upstream->purgeTouched();
    dependent->Source1.setValue(upstream);
    ASSERT_EQ(_document->recompute({dependent}), 1);
    ASSERT_FALSE(dependent->mustRecompute());
    upstream->Column.setValue("C");
    // Keep the dependent clean so only terminal failure presentation makes it
    // unresolved. Its real model link still puts the upstream revisions in
    // the blocked node's semantic presentation fence.
    dependent->purgeTouched();
    ASSERT_FALSE(dependent->mustRecompute());

    App::DocumentRecomputeFeatureRequest root;
    root.featureId = upstream->getNameInDocument();
    root.operationId = "blocked-fence-upstream";
    root.intent.operationType = std::string(BlockingRecomputeOperationType);
    root.intent.arguments = {
        {"token", _blockingToken}, {"target", root.featureId}, {"fail", "1"}};
    root.provenance = "blocked presentation fence regression";
    root.stableObjectIdentity =
        _document->collaborationObjectIdentity(*upstream);
    root.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(root.featureId));

    App::DocumentRecomputeFeatureRequest blocked;
    blocked.featureId = dependent->getNameInDocument();
    blocked.dependencies = {root.featureId};
    blocked.operationId = "blocked-fence-dependent";
    blocked.intent.operationType =
        std::string(App::GenericIsolatedRecomputeOperationType);
    blocked.intent.arguments = {
        {"feature", blocked.featureId},
        {"stable_object_identity",
         _document->collaborationObjectIdentity(*dependent)}};
    blocked.provenance = "blocked presentation fence regression";
    blocked.stableObjectIdentity =
        _document->collaborationObjectIdentity(*dependent);
    blocked.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(blocked.featureId));

    App::DocumentRecomputeRequest request;
    request.features = {std::move(root), std::move(blocked)};
    const auto id = _document->recomputeCoordinator().submit(std::move(request));
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    int upstreamTouchNotifications = 0;
    bool dependentWasPresentedBeforeObserver = false;
    int perObjectRecomputedNotifications = 0;
    int aggregateFailureEntries = 0;
    auto touchConnection = _document->signalTouchedObject.connect(
        [&](const App::DocumentObject& touched) {
            if (&touched != upstream) {
                return;
            }
            ++upstreamTouchNotifications;
            dependentWasPresentedBeforeObserver =
                dependent->isTouched() && dependent->isError();
            dependent->Label.setValue("observer edit after terminal presentation");
        });
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& candidate) {
            if (&candidate == upstream || &candidate == dependent) {
                ++perObjectRecomputedNotifications;
            }
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            aggregateFailureEntries += static_cast<int>(
                std::ranges::count(objects, upstream));
            aggregateFailureEntries += static_cast<int>(
                std::ranges::count(objects, dependent));
        });
    _blocking->release();
    App::RecomputeHandle handle(*_document, id);
    const auto terminal = handle.wait(10s);

    ASSERT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure)
        << terminal.diagnostic;
    const auto blockedResult = std::ranges::find(
        terminal.features,
        std::string("Z_BlockedDependent"),
        &App::DocumentRecomputeFeatureSnapshot::featureId);
    ASSERT_NE(blockedResult, terminal.features.end());
    EXPECT_EQ(blockedResult->state,
              App::DocumentRecomputeFeatureState::Blocked);
    EXPECT_TRUE(blockedResult->presentationObjectModelRevision);
    EXPECT_TRUE(blockedResult->presentationRevisionFenceComplete);
    EXPECT_TRUE(dependent->mustRecompute());
    EXPECT_EQ(upstreamTouchNotifications, 1);
    EXPECT_TRUE(dependentWasPresentedBeforeObserver)
        << "touch observers must not run inside the prevalidated apply phase";
    EXPECT_STREQ(dependent->Label.getValue(),
                 "observer edit after terminal presentation");
    EXPECT_EQ(perObjectRecomputedNotifications, 0);
    EXPECT_EQ(aggregateFailureEntries, 2);
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedExecutableWork());
}

TEST_F(RecomputeHandleTest,
       olderCommittedPresentationCannotEraseANewerFailureGeneration)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("GenerationTarget");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");

    auto older = blockingHandle(feature);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));
    _blocking->release();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    std::optional<App::DocumentRecomputeSnapshot> olderTerminal;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(
            _document->recomputeCoordinator().poll(older->id()));
        olderTerminal =
            _document->recomputeCoordinator().status(older->id());
        if (olderTerminal && olderTerminal->terminal()) {
            break;
        }
        std::this_thread::sleep_for(2ms);
    }
    ASSERT_TRUE(olderTerminal && olderTerminal->terminal());
    ASSERT_EQ(olderTerminal->state, App::DocumentRecomputeState::Completed);

    BlockingRecomputeStore::remove(_blockingToken);
    _blocking = std::make_shared<BlockingRecomputeState>();
    BlockingRecomputeStore::add(_blockingToken, _blocking);
    auto newer = blockingHandle(feature, /*fail=*/true);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));
    _blocking->release();
    ASSERT_EQ(newer->wait(10s).state,
              App::DocumentRecomputeState::PartialFailure);
    ASSERT_TRUE(_document->recomputeCoordinator().hasUnresolvedWork());

    // Make the live model superficially clean without satisfying the newer
    // failed generation. This is the exact state in which the old
    // generation-blind clear used to erase the newer ledger record.
    feature->purgeTouched();
    feature->resetError();
    ASSERT_FALSE(feature->mustRecompute());
    ASSERT_EQ(older->status().state, App::DocumentRecomputeState::Completed);
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedWork());

    feature->touch();
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedExecutableWork());
}

TEST_F(RecomputeHandleTest,
       detachedPresentationRetainsObjectsAndIsolatesThrowingObservers)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "DetachedPresentationTarget");
    static_cast<void>(targetOwner.release());
    target->purgeTouched();

    auto handle = blockingHandle(target);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    int removingObserverCalls = 0;
    int laterObjectObserverCalls = 0;
    int throwingAggregateCalls = 0;
    int laterAggregateCalls = 0;
    int stableCalls = 0;
    auto removingConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object != target) {
                return;
            }
            ++removingObserverCalls;
            _document->removeObject("DetachedPresentationTarget");
            throw std::runtime_error("injected detached object observer failure");
        });
    auto observingConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == target) {
                ++laterObjectObserverCalls;
                EXPECT_FALSE(*destroyed);
            }
        });
    auto throwingAggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            ++throwingAggregateCalls;
            EXPECT_FALSE(*destroyed);
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
            throw std::runtime_error("injected detached aggregate observer failure");
        });
    auto observingAggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            ++laterAggregateCalls;
            EXPECT_FALSE(*destroyed);
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            ++stableCalls;
            EXPECT_TRUE(*destroyed);
        });

    _blocking->release();
    const auto terminal = handle->wait(30s);

    EXPECT_EQ(terminal.state, App::DocumentRecomputeState::Completed)
        << terminal.diagnostic;
    EXPECT_EQ(removingObserverCalls, 1);
    EXPECT_EQ(laterObjectObserverCalls, 1);
    EXPECT_EQ(throwingAggregateCalls, 1);
    EXPECT_EQ(laterAggregateCalls, 1);
    EXPECT_EQ(stableCalls, 1);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("DetachedPresentationTarget"), nullptr);
    EXPECT_TRUE(_document->getMutationReadiness().ready);

    const auto repeated = handle->status();
    EXPECT_EQ(repeated.state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(removingObserverCalls, 1);
    EXPECT_EQ(laterObjectObserverCalls, 1);
    EXPECT_EQ(throwingAggregateCalls, 1);
    EXPECT_EQ(laterAggregateCalls, 1);
    EXPECT_EQ(stableCalls, 1);
}

TEST_F(RecomputeHandleTest,
       fullDocumentFacadeDoesNotMakeIndependentSiblingCommitsStale)
{
    auto* first = _document->addObject<App::FeatureTest>("IndependentFirst");
    auto* second = _document->addObject<App::FeatureTest>("IndependentSecond");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    first->touch();
    second->touch();

    auto handle = _document->recomputeAsync();
    ASSERT_NE(handle, nullptr);
    const auto snapshot = handle->wait(60s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(snapshot.completedFeatures, 2U);
    EXPECT_EQ(snapshot.failedFeatures, 0U);
    EXPECT_EQ(snapshot.totalFeatures, 2U);
    EXPECT_FALSE(first->mustRecompute());
    EXPECT_FALSE(second->mustRecompute());
    EXPECT_TRUE(first->isValid());
    EXPECT_TRUE(second->isValid());
}

TEST_F(RecomputeHandleTest,
       fullDocumentFacadeDoesNotExecuteTouchedNoRecomputeStorageObject)
{
    auto* executable = _document->addObject<App::FeatureTest>("ExecutableFeature");
    auto* storage = _document->addObject<App::DocumentObject>("NoRecomputeStorage");
    ASSERT_NE(executable, nullptr);
    ASSERT_NE(storage, nullptr);
    auto* value = dynamic_cast<App::PropertyInteger*>(storage->addDynamicProperty(
        "App::PropertyInteger", "Value", "Data", "", App::Prop_NoRecompute));
    ASSERT_NE(value, nullptr);
    value->setValue(7);
    executable->touch();

    ASSERT_TRUE(storage->isTouched());
    ASSERT_EQ(storage->mustRecompute(), 0);
    auto handle = _document->recomputeAsync();
    ASSERT_NE(handle, nullptr);
    const auto snapshot = handle->wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(snapshot.completedFeatures, 2U);
    EXPECT_EQ(snapshot.failedFeatures, 0U);
    EXPECT_EQ(snapshot.totalFeatures, 2U);
    ASSERT_EQ(snapshot.features.size(), 2U);
    const auto storageResult = std::ranges::find(
        snapshot.features,
        std::string(storage->getNameInDocument()),
        &App::DocumentRecomputeFeatureSnapshot::featureId);
    ASSERT_NE(storageResult, snapshot.features.end());
    EXPECT_EQ(storageResult->state, App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(value->getValue(), 7);
    EXPECT_FALSE(storage->isTouched());
    EXPECT_EQ(storage->mustRecompute(), 0);
}

TEST_F(RecomputeHandleTest, standaloneSyncAndAsyncFacadesDoNotCreateUserUndoEntries)
{
    auto* feature = _document->addObject<App::FeatureTest>("UndoNeutralFeature");
    ASSERT_NE(feature, nullptr);
    feature->Label.setValue("Before redo sentinel");
    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    _document->openTransaction("standalone recompute redo sentinel");
    feature->Label.setValue("After redo sentinel");
    _document->commitTransaction();
    ASSERT_EQ(_document->getAvailableUndos(), 1);
    ASSERT_TRUE(_document->undo());
    ASSERT_STREQ(feature->Label.getValue(), "Before redo sentinel");

    feature->touch();
    const auto undosBefore = _document->getAvailableUndos();
    const auto redosBefore = _document->getAvailableRedos();
    const auto redoNamesBefore = _document->getAvailableRedoNames();
    ASSERT_EQ(undosBefore, 0);
    ASSERT_EQ(redosBefore, 1);
    ASSERT_EQ(redoNamesBefore.size(), 1U);
    ASSERT_EQ(redoNamesBefore.front(), "standalone recompute redo sentinel");

    bool syncHasError = true;
    EXPECT_EQ(_document->recompute({feature}, false, &syncHasError), 1);
    EXPECT_FALSE(syncHasError);
    EXPECT_EQ(feature->ExecCount.getValue(), 1);
    EXPECT_EQ(feature->ExecResult.getStrValue(), "Exec");
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    EXPECT_EQ(_document->getAvailableUndos(), undosBefore);
    EXPECT_EQ(_document->getAvailableRedos(), redosBefore);
    EXPECT_EQ(_document->getAvailableRedoNames(), redoNamesBefore);

    feature->touch();
    ASSERT_EQ(_document->getAvailableRedos(), redosBefore);
    ASSERT_EQ(_document->getAvailableRedoNames(), redoNamesBefore);
    auto handle = _document->recomputeAsync({feature});
    ASSERT_NE(handle, nullptr);
    const auto snapshot = handle->wait(30s);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(snapshot.completedFeatures, 1U);
    EXPECT_EQ(snapshot.failedFeatures, 0U);
    EXPECT_EQ(feature->ExecCount.getValue(), 2);
    EXPECT_EQ(feature->ExecResult.getStrValue(), "Exec");
    EXPECT_FALSE(feature->mustRecompute());
    EXPECT_TRUE(feature->isValid());
    EXPECT_EQ(_document->getAvailableUndos(), undosBefore);
    EXPECT_EQ(_document->getAvailableRedos(), redosBefore);
    EXPECT_EQ(_document->getAvailableRedoNames(), redoNamesBefore);

    ASSERT_TRUE(_document->redo());
    EXPECT_STREQ(feature->Label.getValue(), "After redo sentinel");
}

TEST_F(RecomputeHandleTest, syncRecomputeJoinsCallerTransactionAndUndoesDerivedOutput)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("TransactionalColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    ASSERT_EQ(_document->recompute({feature}), 1);
    ASSERT_EQ(feature->Value.getValue(), App::decodeColumn("B"));

    _document->setMaxUndoStackSize(20);
    _document->clearUndos();
    const int transactionId = _document->openTransaction("input plus recompute");
    ASSERT_GT(transactionId, 0);
    feature->Column.setValue("E");

    bool hasError = true;
    EXPECT_EQ(_document->recompute({feature}, false, &hasError), 1);
    EXPECT_FALSE(hasError);
    EXPECT_TRUE(_document->hasPendingTransaction());
    EXPECT_EQ(_document->getBookedTransactionID(), transactionId);
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("E"));

    _document->commitTransaction();
    ASSERT_EQ(_document->getAvailableUndos(), 1);
    ASSERT_TRUE(_document->undo());
    EXPECT_STREQ(feature->Column.getValue(), "B");
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("B"));
    ASSERT_TRUE(_document->redo());
    EXPECT_STREQ(feature->Column.getValue(), "E");
    EXPECT_EQ(feature->Value.getValue(), App::decodeColumn("E"));
}

TEST_F(RecomputeHandleTest,
       beforeRecomputeRemovalRetainsExplicitRootUntilTeardown)
{
    auto destroyed = std::make_shared<bool>(false);
    auto owner = std::make_unique<DestructionProbeFeature>();
    auto* target = owner.get();
    target->destroyed = destroyed;
    _document->addObject(owner.get(), "BeforeRemovalTarget");
    static_cast<void>(owner.release());

    int beforeCalls = 0;
    int aggregateExecuteCount = -1;
    bool laterBeforeSawLiveObject = false;
    bool aggregateSawLiveObject = false;
    bool stableSawDestruction = false;
    auto removingConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            ++beforeCalls;
            _document->removeObject("BeforeRemovalTarget");
            EXPECT_FALSE(*destroyed);
        });
    auto observingConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            ++beforeCalls;
            laterBeforeSawLiveObject = !*destroyed
                && _document->getObject("BeforeRemovalTarget") == target;
            ASSERT_NE(target->getNameInDocument(), nullptr);
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            aggregateSawLiveObject = !*destroyed
                && std::ranges::find(objects, target) != objects.end();
            aggregateExecuteCount = target->ExecCount.getValue();
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { stableSawDestruction = *destroyed; });

    bool hasError = true;
    EXPECT_EQ(_document->recompute({target}, false, &hasError), 1);

    EXPECT_FALSE(hasError);
    EXPECT_EQ(beforeCalls, 2);
    EXPECT_TRUE(laterBeforeSawLiveObject);
    EXPECT_TRUE(aggregateSawLiveObject);
    EXPECT_EQ(aggregateExecuteCount, 1);
    EXPECT_TRUE(stableSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("BeforeRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       throwingBeforeRecomputeAbortsExecutionAndStillCompletesTeardown)
{
    auto destroyed = std::make_shared<bool>(false);
    auto owner = std::make_unique<DestructionProbeFeature>();
    auto* target = owner.get();
    target->destroyed = destroyed;
    _document->addObject(owner.get(), "ThrowingBeforeTarget");
    static_cast<void>(owner.release());

    int aggregateCalls = 0;
    int stableCalls = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) {
            _document->removeObject("ThrowingBeforeTarget");
            throw std::runtime_error("injected before-recompute failure");
        });
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            ++aggregateCalls;
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            ++stableCalls;
            EXPECT_TRUE(*destroyed);
        });

    bool hasError = false;
    EXPECT_THROW(
        static_cast<void>(_document->recompute({target}, false, &hasError)),
        std::runtime_error);

    EXPECT_TRUE(hasError);
    EXPECT_EQ(aggregateCalls, 0);
    EXPECT_EQ(stableCalls, 1);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("ThrowingBeforeTarget"), nullptr);
    EXPECT_FALSE(_document->testStatus(App::Document::Recomputing));
    EXPECT_FALSE(_document->getMutationReadiness().pendingRemoval);
    EXPECT_TRUE(_document->getMutationReadiness().ready);
}

TEST_F(RecomputeHandleTest,
       skipRecomputeObserversRetainSelectedObjectsUntilEverySlotReturns)
{
    auto destroyed = std::make_shared<bool>(false);
    auto owner = std::make_unique<DestructionProbeFeature>();
    auto* target = owner.get();
    target->destroyed = destroyed;
    _document->addObject(owner.get(), "SkippedRemovalTarget");
    static_cast<void>(owner.release());
    _document->setStatus(App::Document::SkipRecompute, true);

    bool laterObserverSawLiveObject = false;
    bool stableSawDestruction = false;
    auto removingConnection = _document->signalSkipRecompute.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
            _document->removeObject("SkippedRemovalTarget");
        });
    auto observingConnection = _document->signalSkipRecompute.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            laterObserverSawLiveObject = !*destroyed
                && std::ranges::find(objects, target) != objects.end();
            ASSERT_NE(target->getNameInDocument(), nullptr);
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { stableSawDestruction = *destroyed; });

    EXPECT_EQ(_document->recompute({target}), 0);
    _document->setStatus(App::Document::SkipRecompute, false);

    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(stableSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("SkippedRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       crossDocumentDependencyPreservesLegacyExecutionAndCanBeExplicitlyExcluded)
{
    _extraDocumentNames.push_back(
        App::GetApplication().getUniqueDocumentName("recomputeForeign"));
    auto* foreignDocument = App::GetApplication().newDocument(
        _extraDocumentNames.back().c_str(), "Foreign recompute dependency");
    ASSERT_NE(foreignDocument, nullptr);
    auto* foreign =
        foreignDocument->addObject<App::FeatureTest>("ForeignDependency");
    auto* local = _document->addObject<App::FeatureTest>("LocalDependent");
    ASSERT_NE(foreign, nullptr);
    ASSERT_NE(local, nullptr);
    auto* externalLink = dynamic_cast<App::PropertyXLink*>(
        local->addDynamicProperty("App::PropertyXLink", "ExternalSource"));
    ASSERT_NE(externalLink, nullptr);
    _document->FileName.setValue(
        App::Application::getTempFileName("sync-recompute-owner.FCStd"));
    foreignDocument->FileName.setValue(
        App::Application::getTempFileName("sync-recompute-foreign.FCStd"));
    try {
        externalLink->setValue(foreign);
    }
    catch (const Base::Exception& error) {
        GTEST_SKIP() << "cross-document links are unavailable: " << error.what();
    }
    local->touch();
    foreign->touch();
    const int localExecutions = local->ExecCount.getValue();
    const int foreignExecutions = foreign->ExecCount.getValue();
    const auto localModel = App::DocumentRevisionKey::objectModel("LocalDependent");
    const auto foreignModel =
        App::DocumentRevisionKey::objectModel("ForeignDependency");
    const auto localRevision =
        _document->collaborationRevisions().current(localModel);
    const auto foreignRevision =
        foreignDocument->collaborationRevisions().current(foreignModel);
    std::vector<std::string> recomputeOrder;
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            recomputeOrder.emplace_back(object.getNameInDocument());
        });

    bool hasError = false;
    EXPECT_EQ(_document->recompute({local}, false, &hasError), 2);
    EXPECT_FALSE(hasError);
    EXPECT_EQ(local->ExecCount.getValue(), localExecutions + 1);
    EXPECT_EQ(foreign->ExecCount.getValue(), foreignExecutions + 1);
    EXPECT_FALSE(local->mustRecompute());
    EXPECT_FALSE(foreign->mustRecompute());
    ASSERT_EQ(recomputeOrder.size(), 2);
    EXPECT_EQ(recomputeOrder[0], "ForeignDependency");
    EXPECT_EQ(recomputeOrder[1], "LocalDependent");
    EXPECT_GT(
        _document->collaborationRevisions().current(localModel),
        localRevision);
    EXPECT_GT(
        foreignDocument->collaborationRevisions().current(foreignModel),
        foreignRevision);

    recomputeOrder.clear();
    local->touch();
    foreign->touch();
    const auto localRevisionBeforeExcluded =
        _document->collaborationRevisions().current(localModel);
    const auto foreignRevisionBeforeExcluded =
        foreignDocument->collaborationRevisions().current(foreignModel);

    hasError = true;
    EXPECT_EQ(
        _document->recompute(
            {local}, false, &hasError, App::Document::DepNoXLinked),
        1);
    EXPECT_FALSE(hasError);
    EXPECT_EQ(local->ExecCount.getValue(), localExecutions + 2);
    EXPECT_EQ(foreign->ExecCount.getValue(), foreignExecutions + 1);
    EXPECT_FALSE(local->mustRecompute());
    EXPECT_TRUE(foreign->mustRecompute());
    ASSERT_EQ(recomputeOrder.size(), 1);
    EXPECT_EQ(recomputeOrder.front(), "LocalDependent");
    EXPECT_GT(
        _document->collaborationRevisions().current(localModel),
        localRevisionBeforeExcluded);
    EXPECT_EQ(
        foreignDocument->collaborationRevisions().current(foreignModel),
        foreignRevisionBeforeExcluded);
}

TEST_F(RecomputeHandleTest, syncRecomputePreservesTwoPassStatusesAndOneStableBoundary)
{
    auto sourceOwner = std::make_unique<SchedulerProbeFeature>();
    auto* source = sourceOwner.get();
    _document->addObject(sourceOwner.get(), "SchedulerSource");
    static_cast<void>(sourceOwner.release());

    auto dependentOwner = std::make_unique<SchedulerProbeFeature>();
    auto* dependent = dependentOwner.get();
    _document->addObject(dependentOwner.get(), "SchedulerDependent");
    static_cast<void>(dependentOwner.release());
    dependent->Source1.setValue(source);

    ASSERT_GT(_document->recompute(), 0);
    source->executeCalls = 0;
    source->sawPendingRecompute = false;
    source->sawSecondPass = false;
    dependent->executeCalls = 0;
    dependent->sawPendingRecompute = false;
    dependent->sawSecondPass = false;
    source->purgeTouched();
    dependent->purgeTouched();
    dependent->touchOnce = source;
    dependent->touch();

    int beforeSignals = 0;
    int recomputedSignals = 0;
    int stableSignals = 0;
    auto beforeConnection = _document->signalBeforeRecompute.connect(
        [&](const App::Document&) { ++beforeSignals; });
    auto recomputedConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            ++recomputedSignals;
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { ++stableSignals; });

    bool hasError = true;
    EXPECT_EQ(_document->recompute({}, false, &hasError), 3);

    EXPECT_FALSE(hasError);
    EXPECT_EQ(source->executeCalls, 1);
    EXPECT_EQ(dependent->executeCalls, 2);
    EXPECT_TRUE(source->sawPendingRecompute);
    EXPECT_TRUE(source->sawSecondPass);
    EXPECT_TRUE(dependent->sawPendingRecompute);
    EXPECT_FALSE(dependent->sawSecondPass);
    EXPECT_FALSE(source->mustRecompute());
    EXPECT_FALSE(dependent->mustRecompute());
    EXPECT_TRUE(source->isValid());
    EXPECT_TRUE(dependent->isValid());
    EXPECT_FALSE(source->testStatus(App::ObjectStatus::PendingRecompute));
    EXPECT_FALSE(source->testStatus(App::ObjectStatus::Recompute2));
    EXPECT_FALSE(dependent->testStatus(App::ObjectStatus::PendingRecompute));
    EXPECT_FALSE(dependent->testStatus(App::ObjectStatus::Recompute2));
    EXPECT_EQ(beforeSignals, 1);
    EXPECT_EQ(recomputedSignals, 1);
    EXPECT_EQ(stableSignals, 1);
}

TEST_F(RecomputeHandleTest,
       aggregateRecomputeObserversRetainObjectsUntilEveryObserverReturns)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "AggregateRemovalTarget");
    static_cast<void>(targetOwner.release());
    auto* teardownGuard = _document->addObject<App::FeatureTest>("TeardownGuard");
    ASSERT_NE(teardownGuard, nullptr);
    teardownGuard->purgeTouched();
    teardownGuard->touch();
    const int guardExecuteCountBefore = teardownGuard->ExecCount.getValue();

    int aggregateSignals = 0;
    bool laterObserverSawLiveObject = false;
    bool stableObserverSawDestruction = false;
    bool teardownFeatureResult = true;
    auto removingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            ++aggregateSignals;
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
            _document->removeObject("AggregateRemovalTarget");
        });
    auto observingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            laterObserverSawLiveObject = !*destroyed;
            EXPECT_FALSE(
                target->testStatus(App::ObjectStatus::PendingRecompute));
            EXPECT_FALSE(target->testStatus(App::ObjectStatus::Recompute2));
        });
    auto deletedConnection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            if (object.getNameInDocument()
                && std::string_view(object.getNameInDocument())
                    == "AggregateRemovalTarget") {
                teardownFeatureResult =
                    _document->recomputeFeature(teardownGuard, false);
            }
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            stableObserverSawDestruction = *destroyed;
        });

    bool hasError = true;
    EXPECT_EQ(_document->recompute({target}, false, &hasError), 1);

    EXPECT_FALSE(hasError);
    EXPECT_EQ(aggregateSignals, 1);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(stableObserverSawDestruction);
    EXPECT_FALSE(teardownFeatureResult);
    EXPECT_EQ(teardownGuard->ExecCount.getValue(), guardExecuteCountBefore);
    EXPECT_TRUE(teardownGuard->mustRecompute());
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("AggregateRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       nestedRecomputeDoesNotDrainAnUnrelatedDocumentsPendingRemoval)
{
    _extraDocumentNames.push_back(
        App::GetApplication().getUniqueDocumentName("unrelatedRecompute"));
    auto* unrelatedDocument = App::GetApplication().newDocument(
        _extraDocumentNames.back().c_str(), "Unrelated recompute teardown");
    ASSERT_NE(unrelatedDocument, nullptr);

    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* unrelatedTarget = targetOwner.get();
    unrelatedTarget->destroyed = destroyed;
    unrelatedDocument->addObject(targetOwner.get(), "UnrelatedRemovalTarget");
    static_cast<void>(targetOwner.release());

    auto* nestedTarget = _document->addObject<App::FeatureTest>("NestedDocumentTarget");
    ASSERT_NE(nestedTarget, nullptr);

    bool nestedRecomputeSawLiveObject = false;
    bool laterObserverSawLiveObject = false;
    auto removingConnection = unrelatedDocument->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            EXPECT_NE(std::ranges::find(objects, unrelatedTarget), objects.end());
            unrelatedDocument->removeObject("UnrelatedRemovalTarget");
            EXPECT_EQ(_document->recompute({nestedTarget}), 1);
            nestedRecomputeSawLiveObject = !*destroyed;
        });
    auto observingConnection = unrelatedDocument->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>&) {
            laterObserverSawLiveObject = !*destroyed;
        });

    EXPECT_EQ(unrelatedDocument->recompute({unrelatedTarget}), 1);

    EXPECT_TRUE(nestedRecomputeSawLiveObject);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(unrelatedDocument->getObject("UnrelatedRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       nestedFeatureRecomputeCannotDrainAnOuterAggregateRemoval)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "NestedDrainTarget");
    static_cast<void>(targetOwner.release());
    auto* nested = _document->addObject<App::FeatureTest>("NestedDrainFeature");
    ASSERT_NE(nested, nullptr);
    nested->purgeTouched();
    nested->touch();

    bool nestedResult = false;
    bool firstObserverRetained = false;
    bool laterObserverRetained = false;
    bool stableSawDestruction = false;
    auto removingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            if (std::ranges::find(objects, target) == objects.end()) {
                return;
            }
            _document->removeObject("NestedDrainTarget");
            nestedResult = _document->recomputeFeature(nested, false);
            firstObserverRetained = !*destroyed;
        });
    auto observingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            if (std::ranges::find(objects, target) != objects.end()) {
                laterObserverRetained = !*destroyed;
                ASSERT_NE(target->getNameInDocument(), nullptr);
            }
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) { stableSawDestruction = *destroyed; });

    bool hasError = true;
    EXPECT_EQ(_document->recompute({target}, false, &hasError), 1);

    EXPECT_FALSE(hasError);
    EXPECT_TRUE(nestedResult);
    EXPECT_TRUE(firstObserverRetained);
    EXPECT_TRUE(laterObserverRetained);
    EXPECT_TRUE(stableSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("NestedDrainTarget"), nullptr);
    EXPECT_FALSE(nested->mustRecompute());
}

TEST_F(RecomputeHandleTest,
       detachedAggregateRejectsClearDocumentAndContinuesLaterObservers)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "ClearDocumentPresentationTarget");
    static_cast<void>(targetOwner.release());
    target->purgeTouched();

    auto handle = blockingHandle(target);
    ASSERT_TRUE(_blocking->waitUntilStarted(10s));

    int clearAttempts = 0;
    bool laterObserverSawLiveObject = false;
    auto clearingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            if (std::ranges::find(objects, target) != objects.end()) {
                ++clearAttempts;
                _document->clearDocument();
            }
        });
    auto observingConnection = _document->signalRecomputed.connect(
        [&](const App::Document&,
            const std::vector<App::DocumentObject*>& objects) {
            if (std::ranges::find(objects, target) != objects.end()) {
                laterObserverSawLiveObject = !*destroyed
                    && _document->getObject("ClearDocumentPresentationTarget")
                        == target;
                ASSERT_NE(target->getNameInDocument(), nullptr);
            }
        });

    _blocking->release();
    const auto snapshot = handle->wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(clearAttempts, 1);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_FALSE(*destroyed);
    EXPECT_EQ(_document->getObject("ClearDocumentPresentationTarget"), target);
}

TEST_F(RecomputeHandleTest,
       throwingAggregateObserverCannotStrandPendingRemovalOrReadiness)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "ThrowingAggregateTarget");
    static_cast<void>(targetOwner.release());

    int aggregateCalls = 0;
    int stableCalls = 0;
    auto aggregateConnection = _document->signalRecomputed.connect(
        [&](const App::Document&, const std::vector<App::DocumentObject*>& objects) {
            ++aggregateCalls;
            EXPECT_NE(std::ranges::find(objects, target), objects.end());
            _document->removeObject("ThrowingAggregateTarget");
            throw std::runtime_error("injected synchronous aggregate observer failure");
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            ++stableCalls;
            EXPECT_TRUE(*destroyed);
        });

    EXPECT_THROW(
        static_cast<void>(_document->recompute({target})), std::runtime_error);

    EXPECT_EQ(aggregateCalls, 1);
    EXPECT_EQ(stableCalls, 1);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("ThrowingAggregateTarget"), nullptr);
    EXPECT_FALSE(_document->getMutationReadiness().pendingRemoval);
    EXPECT_TRUE(_document->getMutationReadiness().ready);
}

TEST_F(RecomputeHandleTest,
       featureRecomputeObserversRetainTheFeatureUntilNotificationReturns)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "FeatureRemovalTarget");
    static_cast<void>(targetOwner.release());

    int objectSignals = 0;
    bool laterObserverSawLiveObject = false;
    bool stableObserverSawDestruction = false;
    auto removingConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object != target) {
                return;
            }
            ++objectSignals;
            _document->removeObject("FeatureRemovalTarget");
        });
    auto observingConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object == target) {
                laterObserverSawLiveObject = !*destroyed;
            }
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            stableObserverSawDestruction = *destroyed;
        });

    const bool recomputed = _document->recomputeFeature(target, false);

    EXPECT_TRUE(recomputed);
    EXPECT_EQ(objectSignals, 1);
    EXPECT_TRUE(laterObserverSawLiveObject);
    EXPECT_TRUE(stableObserverSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("FeatureRemovalTarget"), nullptr);
}

TEST_F(RecomputeHandleTest,
       nonrecursiveFeatureRecomputeRetainsASelfRemovingFeatureThroughPresentation)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<SelfRemovingFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "SelfRemovingFeature");
    static_cast<void>(targetOwner.release());

    int objectSignals = 0;
    bool observerSawLiveObject = false;
    bool observerSawDeferredRemoval = false;
    int observerSawExecuteCalls = 0;
    bool stableObserverSawDestruction = false;
    auto objectConnection = _document->signalRecomputedObject.connect(
        [&](const App::DocumentObject& object) {
            if (&object != target) {
                return;
            }
            ++objectSignals;
            observerSawLiveObject = !*destroyed;
            observerSawDeferredRemoval = target->sawDeferredRemoval;
            observerSawExecuteCalls = target->executeCalls;
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            stableObserverSawDestruction = *destroyed;
        });

    const bool recomputed = _document->recomputeFeature(target, false);

    EXPECT_TRUE(recomputed);
    EXPECT_EQ(objectSignals, 1);
    EXPECT_TRUE(observerSawLiveObject);
    EXPECT_TRUE(observerSawDeferredRemoval);
    EXPECT_EQ(observerSawExecuteCalls, 1);
    EXPECT_TRUE(stableObserverSawDestruction);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(_document->getObject("SelfRemovingFeature"), nullptr);
    EXPECT_FALSE(_document->getMutationReadiness().pendingRemoval);
    EXPECT_TRUE(_document->getMutationReadiness().ready);
}

TEST_F(RecomputeHandleTest, noUndoRecomputeRetainsRemovalReferentsThroughReplay)
{
    auto destroyed = std::make_shared<bool>(false);
    auto targetOwner = std::make_unique<DestructionProbeFeature>();
    auto* target = targetOwner.get();
    target->destroyed = destroyed;
    _document->addObject(targetOwner.get(), "RemovalTarget");
    static_cast<void>(targetOwner.release());
    auto* replayGuard = _document->addObject<App::FeatureTest>("ReplayGuard");
    ASSERT_NE(replayGuard, nullptr);
    ASSERT_GT(_document->recompute({target, replayGuard}), 0);
    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    int deletedSignals = 0;
    int removeSignals = 0;
    int stableSignals = 0;
    int replayTransactionId = 0;
    int nestedRecomputeResult = -1;
    bool nestedRecomputeHasError = false;
    bool nestedFeatureResult = true;
    auto deletedConnection = _document->signalDeletedObject.connect(
        [&](const App::DocumentObject& object) {
            ++deletedSignals;
            EXPECT_FALSE(*destroyed);
            ASSERT_NE(object.getNameInDocument(), nullptr);
            EXPECT_STREQ(object.getNameInDocument(), "RemovalTarget");
            EXPECT_TRUE(object.isDerivedFrom<App::FeatureTest>());
        });
    auto removeConnection = _document->signalTransactionRemove.connect(
        [&](const App::DocumentObject& object, App::Transaction* transaction) {
            ++removeSignals;
            EXPECT_FALSE(*destroyed);
            ASSERT_NE(object.getNameInDocument(), nullptr);
            EXPECT_STREQ(object.getNameInDocument(), "RemovalTarget");
            ASSERT_NE(transaction, nullptr);
            replayTransactionId = transaction->getID();
            nestedRecomputeResult =
                _document->recompute({replayGuard}, false, &nestedRecomputeHasError);
            nestedFeatureResult = _document->recomputeFeature(replayGuard, false);
        });
    auto stableConnection = _document->signalBecameStable.connect(
        [&](const App::Document&) {
            ++stableSignals;
            EXPECT_TRUE(*destroyed);
        });

    App::DocumentRecomputeFeatureRequest feature;
    feature.featureId = "RemovalTarget";
    feature.stableObjectIdentity =
        _document->collaborationObjectIdentity(*target);
    feature.presentationObjectModelRevision =
        _document->collaborationRevisions().current(
            App::DocumentRevisionKey::objectModel(feature.featureId));
    feature.operationId = "remove-object-recompute";
    feature.intent.operationType = std::string(RemovingRecomputeOperationType);
    feature.intent.arguments = {{"target", "RemovalTarget"}};
    feature.provenance = "native no-history recompute lifetime test";
    App::DocumentRecomputeRequest request;
    request.features.push_back(std::move(feature));

    const auto id = _document->recomputeCoordinator().submit(std::move(request));
    App::RecomputeHandle handle(*_document, id);
    const auto snapshot = handle.wait(30s);

    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(_document->getObject("RemovalTarget"), nullptr);
    EXPECT_TRUE(*destroyed);
    EXPECT_EQ(deletedSignals, 1);
    EXPECT_EQ(removeSignals, 1);
    EXPECT_GT(replayTransactionId, 0);
    EXPECT_EQ(nestedRecomputeResult, 0);
    EXPECT_TRUE(nestedRecomputeHasError);
    EXPECT_FALSE(nestedFeatureResult);
    EXPECT_EQ(stableSignals, 1);
    EXPECT_EQ(_document->getAvailableUndos(), 0);
}

TEST_F(RecomputeHandleTest,
       undoHistoryEvictionKeepsTransactionControlClosedDuringDestruction)
{
    auto reentry = std::make_shared<TransactionDestructionReentryState>();
    reentry->document = _document;
    auto targetOwner = std::make_unique<TransactionDestructionReentryFeature>();
    auto* target = targetOwner.get();
    target->state = reentry;
    _document->addObject(targetOwner.get(), "EvictedUndoTarget");
    static_cast<void>(targetOwner.release());
    auto* survivor = _document->addObject<App::FeatureTest>("EvictionCommitTarget");
    ASSERT_NE(survivor, nullptr);

    _document->setMaxUndoStackSize(1);
    _document->clearUndos();
    ASSERT_GT(_document->openTransaction("retain deleted object"), 0);
    _document->removeObject(target);
    _document->commitTransaction();
    ASSERT_EQ(_document->getAvailableUndos(), 1);
    ASSERT_FALSE(reentry->destroyed);

    int commitObserverCalls = 0;
    bool evictedObjectAliveDuringObserver = false;
    int observerUndoCount = -1;
    std::vector<std::string> observerUndoNames;
    auto commitConnection = _document->signalCommitTransaction.connect(
        [&](const App::Document&) {
            ++commitObserverCalls;
            evictedObjectAliveDuringObserver = !reentry->destroyed;
            observerUndoCount = _document->getAvailableUndos();
            observerUndoNames = _document->getAvailableUndoNames();
        });

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    App::DocumentCommitResult commit;
    try {
        commit = _document->collaborationService().commitCompatibilityMutationWithPolicy(
            std::move(mutation),
            [survivor] { survivor->Label.setValue("Eviction trigger"); },
            App::CollaborationCompatibilityRecomputePolicy::Deferred);
    }
    catch (...) {
        reentry->document = nullptr;
        throw;
    }
    reentry->document = nullptr;

    EXPECT_EQ(commit.status, App::DocumentCommitStatus::Committed)
        << commit.message;
    EXPECT_TRUE(reentry->destroyed);
    EXPECT_TRUE(reentry->transactionControlRejected);
    EXPECT_FALSE(reentry->transactionControlAdmitted);
    EXPECT_EQ(commitObserverCalls, 1);
    EXPECT_TRUE(evictedObjectAliveDuringObserver);
    EXPECT_EQ(observerUndoCount, 1);
    EXPECT_EQ(observerUndoNames.size(), 1U);
    EXPECT_EQ(std::ranges::count(observerUndoNames,
                                 std::string("retain deleted object")),
              0);
    EXPECT_EQ(_document->getAvailableUndos(), 1);
    EXPECT_STREQ(survivor->Label.getValue(), "Eviction trigger");
}

TEST_F(RecomputeHandleTest,
       committedRecomputeWithoutBoundTargetPublicationFailsAndBlocksSave)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("MissingPublicationTarget");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("D");

    auto handle = blockingHandle(
        feature, /*fail=*/false, /*publishTarget=*/false);
    ASSERT_TRUE(_blocking->waitUntilStarted());
    _blocking->release();
    const auto terminal = handle->wait(10s);

    ASSERT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure)
        << terminal.diagnostic;
    ASSERT_EQ(terminal.features.size(), 1U);
    EXPECT_EQ(terminal.features.front().state,
              App::DocumentRecomputeFeatureState::Failed);
    EXPECT_FALSE(terminal.features.front().targetPublicationConfirmed);
    EXPECT_NE(terminal.features.front().diagnostic.find(
                  "did not publish its bound target"),
              std::string::npos);
    EXPECT_TRUE(feature->mustRecompute());
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedExecutableWork());

    _savePath = std::filesystem::temp_directory_path()
        / (_documentName + "-missing-publication.FCStd");
    const auto save =
        _document->saveAsWithOutcome(_savePath.string().c_str());
    EXPECT_EQ(save.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(save.errorCode, "RECOMPUTE_PENDING");
    EXPECT_FALSE(std::filesystem::exists(_savePath));
}

TEST_F(RecomputeHandleTest,
       existencePublicationCannotClaimRemovalWhileTargetRemainsLive)
{
    auto* feature =
        _document->addObject<App::FeatureTestColumn>("FalseRemovalTarget");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("C");

    auto handle = blockingHandle(feature,
                                 /*fail=*/false,
                                 /*publishTarget=*/true,
                                 /*publishExistence=*/true);
    ASSERT_TRUE(_blocking->waitUntilStarted());
    _blocking->release();
    const auto terminal = handle->wait(10s);

    ASSERT_EQ(terminal.state, App::DocumentRecomputeState::PartialFailure)
        << terminal.diagnostic;
    ASSERT_EQ(terminal.features.size(), 1U);
    EXPECT_EQ(terminal.features.front().state,
              App::DocumentRecomputeFeatureState::Failed);
    EXPECT_FALSE(terminal.features.front().targetPublicationConfirmed);
    EXPECT_EQ(_document->getObject("FalseRemovalTarget"), feature);
    EXPECT_TRUE(feature->mustRecompute());
    EXPECT_TRUE(_document->recomputeCoordinator().hasUnresolvedExecutableWork());
}

TEST_F(RecomputeHandleTest, zeroTimeoutObservesNonterminalWorkAndCancellationTerminates)
{
    auto handle = blockingHandle();
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto started = std::chrono::steady_clock::now();
    const auto pending = handle->wait(0ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, 250ms);
    EXPECT_EQ(pending.id, handle->id());
    EXPECT_FALSE(pending.terminal());
    EXPECT_EQ(pending.state, App::DocumentRecomputeState::Running);
    EXPECT_LT(pending.progress, 1.0);
    EXPECT_FALSE(handle->poll());

    ASSERT_TRUE(handle->cancel("native handle cancellation"));
    const auto cancelled = handle->wait(3s);
    EXPECT_TRUE(cancelled.terminal()) << cancelled.diagnostic;
    EXPECT_EQ(cancelled.state, App::DocumentRecomputeState::Cancelled);
    EXPECT_EQ(cancelled.diagnostic, "native handle cancellation");
    EXPECT_TRUE(handle->poll());
    EXPECT_FALSE(handle->cancel("already terminal"));
}

TEST_F(RecomputeHandleTest, canonicalSaveRefusesActiveRecomputeWithoutReplacingTheFile)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("PendingSaveFeature");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("E");
    auto initialRecompute = _document->recomputeAsync({feature});
    ASSERT_EQ(initialRecompute->wait(30s).state, App::DocumentRecomputeState::Completed);

    _savePath = std::filesystem::temp_directory_path()
        / (_documentName + "-pending-save.FCStd");
    const auto firstSave = _document->saveAsWithOutcome(_savePath.string().c_str());
    ASSERT_EQ(firstSave.disposition, App::DocumentSaveDisposition::Written);
    const auto canonicalBytes = readFileBytes(_savePath);
    ASSERT_FALSE(canonicalBytes.empty());

    feature->Label.setValue("unsaved while recompute is active");
    auto handle = blockingHandle();
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto pendingSave = _document->saveWithOutcome();
    EXPECT_EQ(pendingSave.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(pendingSave.errorCode, "RECOMPUTE_PENDING");
    EXPECT_NE(pendingSave.message.find(_documentName), std::string::npos);
    EXPECT_FALSE(pendingSave.fileWritten);
    EXPECT_FALSE(pendingSave.lastCanonicalSaveFailed);
    EXPECT_EQ(readFileBytes(_savePath), canonicalBytes);

    _blocking->release();
    const auto completed = handle->wait(3s);
    ASSERT_EQ(completed.state, App::DocumentRecomputeState::Completed)
        << completed.diagnostic;

    const auto written = _document->saveWithOutcome();
    EXPECT_EQ(written.disposition, App::DocumentSaveDisposition::Written);
    EXPECT_TRUE(written.fileWritten);
    EXPECT_NE(readFileBytes(_savePath), canonicalBytes);

    const auto unchanged = _document->saveWithOutcome();
    EXPECT_EQ(unchanged.disposition, App::DocumentSaveDisposition::Unchanged);
    EXPECT_FALSE(unchanged.fileWritten);
}

TEST_F(RecomputeHandleTest, terminalCancelledRecomputeDoesNotBrickCanonicalSave)
{
    auto handle = blockingHandle();
    ASSERT_TRUE(_blocking->waitUntilStarted());
    ASSERT_TRUE(handle->cancel("terminal cancellation before save"));
    const auto cancelled = handle->wait(3s);
    ASSERT_EQ(cancelled.state, App::DocumentRecomputeState::Cancelled)
        << cancelled.diagnostic;
    ASSERT_TRUE(_document->recomputeCoordinator().hasUnresolvedWork());
    ASSERT_FALSE(_document->mustExecute());
    ASSERT_TRUE(_document->getMutationReadiness().ready);

    _savePath = std::filesystem::temp_directory_path()
        / (_documentName + "-after-cancel.FCStd");
    const auto outcome = _document->saveAsWithOutcome(_savePath.string().c_str());

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written)
        << outcome.message;
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(std::filesystem::exists(_savePath));
}

TEST_F(RecomputeHandleTest, ordinaryTouchedDocumentWithoutCoordinatorWorkCanBeSaved)
{
    auto* feature = _document->addObject<App::FeatureTest>("UnsavedFeature");
    ASSERT_NE(feature, nullptr);
    feature->touch();
    ASSERT_TRUE(_document->mustExecute());
    ASSERT_FALSE(_document->recomputeCoordinator().hasUnresolvedWork());

    _savePath = std::filesystem::temp_directory_path()
        / (_documentName + "-ordinary-touched.FCStd");
    const auto outcome = _document->saveAsWithOutcome(_savePath.string().c_str());

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written)
        << outcome.message;
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(std::filesystem::exists(_savePath));
}

TEST_F(RecomputeHandleTest, activeRecomputeInUnrelatedDocumentDoesNotBlockTargetSave)
{
    auto handle = blockingHandle();
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const std::string targetName =
        App::GetApplication().getUniqueDocumentName("independentSaveTarget");
    auto* target = App::GetApplication().newDocument(targetName.c_str(),
                                                      "Independent save target");
    ASSERT_NE(target, nullptr);
    _savePath = std::filesystem::temp_directory_path()
        / (targetName + "-while-unrelated-recompute.FCStd");

    const auto outcome = target->saveAsWithOutcome(_savePath.string().c_str());
    const bool closed = App::GetApplication().closeDocument(targetName.c_str());

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Written)
        << outcome.message;
    EXPECT_TRUE(outcome.fileWritten);
    EXPECT_TRUE(std::filesystem::exists(_savePath));
    EXPECT_TRUE(closed);

    ASSERT_TRUE(handle->cancel("unrelated save regression complete"));
    EXPECT_EQ(handle->wait(3s).state, App::DocumentRecomputeState::Cancelled);
}

TEST_F(RecomputeHandleTest,
       activeRecomputeInPersistentExternalDependencyBlocksTargetSave)
{
    auto* source = _document->addObject<App::FeatureTest>("ExternalSource");
    ASSERT_NE(source, nullptr);
    _dependencyPath = std::filesystem::temp_directory_path()
        / (_documentName + "-external-source.FCStd");
    ASSERT_NO_THROW({
        const auto sourceSave =
            _document->saveAsWithOutcome(_dependencyPath.string().c_str());
        ASSERT_EQ(sourceSave.disposition, App::DocumentSaveDisposition::Written)
            << sourceSave.message;
    });

    const std::string targetName =
        App::GetApplication().getUniqueDocumentName("dependentSaveTarget");
    auto* target = App::GetApplication().newDocument(targetName.c_str(),
                                                      "Dependent save target");
    ASSERT_NE(target, nullptr);
    auto* consumer = target->addObject<App::FeatureTest>("Consumer");
    ASSERT_NE(consumer, nullptr);
    auto* externalLink = dynamic_cast<App::PropertyXLink*>(
        consumer->addDynamicProperty("App::PropertyXLink", "ExternalSource"));
    ASSERT_NE(externalLink, nullptr);
    _savePath = std::filesystem::temp_directory_path()
        / (targetName + "-while-dependency-recomputes.FCStd");
    const auto targetSave = target->saveAsWithOutcome(_savePath.string().c_str());
    ASSERT_EQ(targetSave.disposition, App::DocumentSaveDisposition::Written)
        << targetSave.message;
    const auto canonicalBytes = readFileBytes(_savePath);
    ASSERT_FALSE(canonicalBytes.empty());
    ASSERT_NO_THROW(externalLink->setValue(source));
    std::vector<App::Document*> dependencies;
    ASSERT_NO_THROW(dependencies = target->getDependentDocuments(false));
    ASSERT_NE(std::ranges::find(dependencies, _document), dependencies.end());

    auto handle = blockingHandle();
    ASSERT_TRUE(_blocking->waitUntilStarted());

    const auto outcome = target->saveWithOutcome();

    EXPECT_EQ(outcome.disposition, App::DocumentSaveDisposition::Failed);
    EXPECT_EQ(outcome.errorCode, "RECOMPUTE_PENDING");
    EXPECT_NE(outcome.message.find(targetName), std::string::npos);
    EXPECT_NE(outcome.message.find(_documentName), std::string::npos);
    EXPECT_FALSE(outcome.fileWritten);
    EXPECT_EQ(readFileBytes(_savePath), canonicalBytes);

    ASSERT_TRUE(handle->cancel("dependent save regression complete"));
    EXPECT_EQ(handle->wait(3s).state, App::DocumentRecomputeState::Cancelled);
    EXPECT_TRUE(App::GetApplication().closeDocument(targetName.c_str()));
}

TEST_F(RecomputeHandleTest, documentCloseLeavesAStablePointerFreeTerminalSnapshot)
{
    const auto id = _document->recomputeCoordinator().submit({});
    auto handle = std::make_unique<App::RecomputeHandle>(*_document, id);
    const auto beforeClose = handle->status();
    ASSERT_TRUE(beforeClose.terminal());
    ASSERT_EQ(beforeClose.state, App::DocumentRecomputeState::Completed);
    ASSERT_EQ(beforeClose.id, id);

    ASSERT_TRUE(App::GetApplication().closeDocument(_documentName.c_str()));
    _document = nullptr;

    const auto afterClose = handle->status();
    EXPECT_EQ(afterClose.id, id);
    EXPECT_TRUE(afterClose.terminal());
    EXPECT_EQ(afterClose.state, App::DocumentRecomputeState::Cancelled);
    EXPECT_EQ(afterClose.diagnostic, "recompute document is no longer live");
    EXPECT_TRUE(handle->poll());
    EXPECT_FALSE(handle->cancel("closed document"));
}
