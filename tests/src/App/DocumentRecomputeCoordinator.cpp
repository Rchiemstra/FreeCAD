// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentHandle.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/DynamicProperty.h>
#include <App/FeatureTest.h>
#include <App/MutationClassification.h>
#include <App/RecomputeHandle.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace App::Internal
{

class DocumentRecomputeCoordinatorTestAccess
{
public:
    static void checkAllocationLock(
        const DocumentRecomputeCoordinator& coordinator,
        const std::unique_lock<std::mutex>& lock,
        const DocumentRecomputeId admissionId)
    {
        EXPECT_EQ(lock.mutex(), &coordinator._stateMutex);
        EXPECT_TRUE(lock.owns_lock()) << "ID allocation must own the state mutex";
        (admissionId == 0 ? defaultAllocations : reservedBindings)
            .fetch_add(1, std::memory_order_relaxed);
    }

    class CheckAllocationLocks
    {
    public:
        CheckAllocationLocks()
        {
            defaultAllocations.store(0, std::memory_order_relaxed);
            reservedBindings.store(0, std::memory_order_relaxed);
            DocumentRecomputeCoordinator::_idAllocationTestHook.store(
                &checkAllocationLock, std::memory_order_release);
        }

        ~CheckAllocationLocks()
        {
            DocumentRecomputeCoordinator::_idAllocationTestHook.store(
                nullptr, std::memory_order_release);
        }
    };

    static inline std::atomic<std::size_t> defaultAllocations {0};
    static inline std::atomic<std::size_t> reservedBindings {0};
};

}  // namespace App::Internal

namespace
{

using namespace std::chrono_literals;

constexpr std::string_view RecomputeTestOperationType =
    "FreeCAD.Tests.DocumentRecompute";

class RecomputeScenario
{
public:
    void capture(const std::string& feature, std::string value)
    {
        std::lock_guard lock(_mutex);
        _captured.emplace(feature, std::move(value));
        _captureOrder.push_back(feature);
        _changed.notify_all();
    }

    bool enterTask(const std::string& feature,
                   const bool hold,
                   const std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        _started.insert(feature);
        _changed.notify_all();
        if (!hold) {
            return !stopToken.stop_requested();
        }
        return _changed.wait(lock, stopToken, [&] {
            return _released.contains(feature);
        });
    }

    void release(const std::string& feature)
    {
        {
            std::lock_guard lock(_mutex);
            _released.insert(feature);
        }
        _changed.notify_all();
    }

    bool waitCaptured(const std::string& feature,
                      const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] {
            return _captured.contains(feature);
        });
    }

    bool waitStarted(const std::string& feature,
                     const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] {
            return _started.contains(feature);
        });
    }

    bool captured(const std::string& feature) const
    {
        std::lock_guard lock(_mutex);
        return _captured.contains(feature);
    }

    std::optional<std::string> capturedValue(const std::string& feature) const
    {
        std::lock_guard lock(_mutex);
        const auto found = _captured.find(feature);
        return found == _captured.end()
            ? std::nullopt
            : std::optional<std::string>(found->second);
    }

    std::size_t captureCount() const
    {
        std::lock_guard lock(_mutex);
        return _captureOrder.size();
    }

    void prepared(const std::string& feature)
    {
        std::lock_guard lock(_mutex);
        _prepared.insert(feature);
        _changed.notify_all();
    }

    bool waitPrepared(const std::string& feature,
                      const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [&] {
            return _prepared.contains(feature);
        });
    }

    void applied(const std::string& feature)
    {
        std::lock_guard lock(_mutex);
        ++_applyCount[feature];
    }

    std::size_t applyCount(const std::string& feature) const
    {
        std::lock_guard lock(_mutex);
        const auto found = _applyCount.find(feature);
        return found == _applyCount.end() ? 0 : found->second;
    }

    void onDestruction(const std::string& feature, std::function<void()> callback)
    {
        std::lock_guard lock(_mutex);
        _destructionCallbacks[feature] = std::move(callback);
    }

    void destroyed(const std::string& feature)
    {
        std::function<void()> callback;
        {
            std::lock_guard lock(_mutex);
            const auto found = _destructionCallbacks.find(feature);
            if (found != _destructionCallbacks.end()) {
                callback = std::move(found->second);
                _destructionCallbacks.erase(found);
            }
        }
        if (callback) {
            callback();
        }
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable_any _changed;
    std::map<std::string, std::string> _captured;
    std::vector<std::string> _captureOrder;
    std::set<std::string> _started;
    std::set<std::string> _released;
    std::set<std::string> _prepared;
    std::map<std::string, std::size_t> _applyCount;
    std::map<std::string, std::function<void()>> _destructionCallbacks;
};

class RecomputeScenarioStore
{
public:
    static void add(const std::string& token,
                    const std::shared_ptr<RecomputeScenario>& scenario)
    {
        std::lock_guard lock(Mutex);
        Scenarios[token] = scenario;
    }

    static void remove(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        Scenarios.erase(token);
    }

    static std::shared_ptr<RecomputeScenario> get(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        const auto found = Scenarios.find(token);
        return found == Scenarios.end() ? nullptr : found->second.lock();
    }

private:
    static inline std::mutex Mutex;
    static inline std::map<std::string, std::weak_ptr<RecomputeScenario>> Scenarios;
};

class RecomputeSetLabelOperation final: public App::CollaborativeOperation
{
public:
    RecomputeSetLabelOperation(std::string target,
                               std::string identity,
                               std::string value,
                               std::string mode,
                               std::shared_ptr<RecomputeScenario> scenario,
                               std::string feature)
        : _target(std::move(target))
        , _identity(std::move(identity))
        , _value(std::move(value))
        , _mode(std::move(mode))
        , _scenario(std::move(scenario))
        , _feature(std::move(feature))
    {}

    ~RecomputeSetLabelOperation() override
    {
        _scenario->destroyed(_feature);
    }

    std::string_view typeId() const noexcept override
    {
        return RecomputeTestOperationType;
    }

    void apply(App::Document& document) const override
    {
        if (_mode == "apply-fail") {
            throw std::runtime_error("requested recompute apply failure");
        }
        auto* target = document.getObject(_target.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _identity) {
            throw std::runtime_error("recompute test target became stale");
        }
        _scenario->applied(_feature);
        target->Label.setValue(_value);
    }

    App::CollaborativePostconditionResult checkPostcondition(
        const App::Document& document) const override
    {
        const auto* target = document.getObject(_target.c_str());
        const bool satisfied = target
            && document.collaborationObjectIdentity(*target) == _identity
            && target->Label.getStrValue() == _value;
        return {satisfied, "recompute test target must equal the detached value"};
    }

private:
    const std::string _target;
    const std::string _identity;
    const std::string _value;
    const std::string _mode;
    const std::shared_ptr<RecomputeScenario> _scenario;
    const std::string _feature;
};

void ensureRecomputeTestAdapterRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(RecomputeTestOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                const auto& scenarioToken = intent.arguments.at("scenario");
                const auto& feature = intent.arguments.at("feature");
                const auto& targetName = intent.arguments.at("target");
                const auto& sourceName = intent.arguments.at("source");
                const auto& literalValue = intent.arguments.at("value");
                const auto& suffix = intent.arguments.at("suffix");
                const auto& mode = intent.arguments.at("mode");
                const bool hold = intent.arguments.at("hold") == "true";
                auto scenario = RecomputeScenarioStore::get(scenarioToken);
                const auto* target = document.getObject(targetName.c_str());
                const auto* source = sourceName.empty()
                    ? nullptr
                    : document.getObject(sourceName.c_str());
                if (!scenario || !target || (!sourceName.empty() && !source)) {
                    throw std::invalid_argument("invalid document recompute test intent");
                }

                const std::string capturedValue = source
                    ? source->Label.getStrValue() + suffix
                    : literalValue;
                scenario->capture(feature, capturedValue);
                const std::string identity =
                    document.collaborationObjectIdentity(*target);

                std::vector<App::DocumentRevisionKey> reads {
                    App::DocumentRevisionKey::objectExistence(targetName),
                    App::DocumentRevisionKey::objectModel(targetName),
                    App::DocumentRevisionKey::objectStructure(targetName)};
                if (source) {
                    reads.push_back(
                        App::DocumentRevisionKey::objectExistence(sourceName));
                    reads.push_back(App::DocumentRevisionKey::objectModel(sourceName));
                    reads.push_back(
                        App::DocumentRevisionKey::objectStructure(sourceName));
                }
                std::sort(reads.begin(), reads.end());
                reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
                const auto model =
                    App::DocumentRevisionKey::objectModel(targetName);
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [scenario = std::move(scenario),
                     feature,
                     hold,
                     mode,
                     targetName,
                     identity,
                     capturedValue](const std::stop_token stopToken) {
                        if (!scenario->enterTask(feature, hold, stopToken)) {
                            throw std::runtime_error(
                                "document recompute task cancelled");
                        }
                        if (mode == "task-fail") {
                            throw std::runtime_error(
                                "requested recompute task failure");
                        }
                        auto operation =
                            std::make_unique<const RecomputeSetLabelOperation>(
                                targetName, identity, capturedValue, mode,
                                scenario, feature);
                        scenario->prepared(feature);
                        return operation;
                    };
                return App::CollaborativeOperationPreparation {
                    std::move(reads),
                    {model},
                    {{model, identity}},
                    std::move(task),
                    App::PreparationPolicy::DetachedInProcess};
            }));
    });
}

App::DocumentRecomputeFeatureRequest feature(
    std::string featureId,
    std::vector<std::string> dependencies,
    const std::string& scenario,
    std::string target,
    std::string value,
    std::string source = {},
    std::string suffix = {},
    const bool hold = false,
    std::string mode = {})
{
    const std::string operationId = "recompute-" + featureId;
    return {featureId,
            std::move(dependencies),
            operationId,
            {std::string(RecomputeTestOperationType),
             {{"scenario", scenario},
              {"feature", featureId},
              {"target", std::move(target)},
              {"source", std::move(source)},
              {"value", std::move(value)},
              {"suffix", std::move(suffix)},
              {"hold", hold ? "true" : "false"},
              {"mode", std::move(mode)}}},
            "native document recompute test"};
}

const App::DocumentRecomputeFeatureSnapshot& featureSnapshot(
    const App::DocumentRecomputeSnapshot& snapshot,
    const std::string& featureId)
{
    const auto found = std::ranges::find(snapshot.features,
                                         featureId,
                                         &App::DocumentRecomputeFeatureSnapshot::featureId);
    if (found == snapshot.features.end()) {
        throw std::runtime_error("recompute snapshot omitted feature " + featureId);
    }
    return *found;
}

App::DocumentRecomputeSnapshot waitTerminal(
    App::DocumentRecomputeCoordinator& coordinator,
    const App::DocumentRecomputeId id,
    const std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(coordinator.poll(id));
        const auto snapshot = coordinator.status(id);
        if (snapshot && snapshot->terminal()) {
            return *snapshot;
        }
        std::this_thread::sleep_for(1ms);
    }
    static_cast<void>(coordinator.cancel(id, "test timeout"));
    throw std::runtime_error("document recompute did not reach terminal state");
}

class DocumentRecomputeCoordinatorTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureRecomputeTestAdapterRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("documentRecomputeCoordinator");
        _document = App::GetApplication().newDocument(_documentName.c_str(),
                                                       "Recompute coordinator test");
        for (const auto* name : {"Upstream",
                                 "Child",
                                 "First",
                                 "Second",
                                 "Stale",
                                 "Failure",
                                 "Descendant",
                                 "Independent"}) {
            auto* object = _document->addObject<App::FeatureTest>(name);
            object->Label.setValue(std::string(name) + "-before");
        }
        _document->recompute();
        _scenario = std::make_shared<RecomputeScenario>();
        _scenarioToken = _documentName + "-scenario";
        RecomputeScenarioStore::add(_scenarioToken, _scenario);
    }

    void TearDown() override
    {
        RecomputeScenarioStore::remove(_scenarioToken);
        App::GetApplication().closeDocument(_documentName.c_str());
    }

    App::DocumentObject& object(const char* name)
    {
        auto* found = _document->getObject(name);
        if (!found) {
            throw std::runtime_error(std::string("missing test object ") + name);
        }
        return *found;
    }

    App::DocumentRecomputeRequest request(
        std::vector<App::DocumentRecomputeFeatureRequest> features,
        std::string coalescingKey = {}) const
    {
        return {std::move(features), std::move(coalescingKey)};
    }

    std::string _documentName;
    App::Document* _document {nullptr};
    std::shared_ptr<RecomputeScenario> _scenario;
    std::string _scenarioToken;
};

}  // namespace

TEST_F(DocumentRecomputeCoordinatorTest,
       childCaptureBeginsOnlyAfterUpstreamCommit)
{
    auto plan = request(
        {feature("root", {}, _scenarioToken, "Upstream", "committed", {}, {}, true),
         feature("child",
                 {"root"},
                 _scenarioToken,
                 "Child",
                 {},
                 "Upstream",
                 "/child")});
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(std::move(plan));

    ASSERT_TRUE(_scenario->waitCaptured("root"));
    EXPECT_FALSE(_scenario->captured("child"));
    EXPECT_EQ(object("Upstream").Label.getStrValue(), "Upstream-before");

    _scenario->release("root");
    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(featureSnapshot(snapshot, "root").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureSnapshot(snapshot, "child").state,
              App::DocumentRecomputeFeatureState::Committed);
    const auto childCapture = _scenario->capturedValue("child");
    ASSERT_TRUE(childCapture.has_value());
    EXPECT_EQ(*childCapture, "committed/child");
    EXPECT_EQ(object("Upstream").Label.getStrValue(), "committed");
    EXPECT_EQ(object("Child").Label.getStrValue(), "committed/child");
}

TEST_F(DocumentRecomputeCoordinatorTest, independentRootsComplete)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("first", {}, _scenarioToken, "First", "first-after"),
         feature("second", {}, _scenarioToken, "Second", "second-after")}));

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(snapshot.completedFeatures, 2U);
    EXPECT_EQ(object("First").Label.getStrValue(), "first-after");
    EXPECT_EQ(object("Second").Label.getStrValue(), "second-after");
}

TEST_F(DocumentRecomputeCoordinatorTest,
       preparedRecomputeAppliesOnceWithoutLivePresentationRefresh)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("ready", {}, _scenarioToken, "First", "committed-once")}));
    ASSERT_TRUE(_scenario->waitPrepared("ready"));

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(featureSnapshot(snapshot, "ready").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(_scenario->captureCount(), 1U);
    EXPECT_EQ(_scenario->applyCount("ready"), 1U);
    EXPECT_EQ(object("First").Label.getStrValue(), "committed-once");
    EXPECT_FALSE(coordinator.poll(id));
    EXPECT_EQ(_scenario->applyCount("ready"), 1U);
}

TEST_F(DocumentRecomputeCoordinatorTest,
       preparedRecomputeWaitsForLivePresentationRefreshAndAppliesOnce)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("ready", {}, _scenarioToken, "First", "committed-once")}));
    ASSERT_TRUE(_scenario->waitPrepared("ready"));

    {
        App::LivePresentationRefreshScope refresh;
        ASSERT_TRUE(refresh.acquired());
        // Drain the already-produced detached result while GUI refresh owns
        // admission. poll() must return promptly, retaining work for a later
        // owner-thread turn rather than failing or waiting inside this turn.
        for (int turn = 0; turn < 30; ++turn) {
            const auto start = std::chrono::steady_clock::now();
            static_cast<void>(coordinator.poll(id));
            EXPECT_LT(std::chrono::steady_clock::now() - start, 250ms);
            std::this_thread::sleep_for(1ms);
        }
        const auto pending = coordinator.status(id);
        ASSERT_TRUE(pending.has_value());
        EXPECT_FALSE(pending->terminal()) << pending->diagnostic;
        EXPECT_EQ(pending->completedFeatures, 0U);
        EXPECT_EQ(_scenario->applyCount("ready"), 0U);
        EXPECT_EQ(object("First").Label.getStrValue(), "First-before");
    }

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_EQ(featureSnapshot(snapshot, "ready").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(_scenario->captureCount(), 1U);
    EXPECT_EQ(_scenario->applyCount("ready"), 1U);
    EXPECT_EQ(object("First").Label.getStrValue(), "committed-once");
    EXPECT_FALSE(coordinator.poll(id));
    EXPECT_EQ(_scenario->applyCount("ready"), 1U);
}

TEST_F(DocumentRecomputeCoordinatorTest,
       preparedRecomputeRevalidatesRevisionAfterLivePresentationRefresh)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("ready", {}, _scenarioToken, "Stale", "must-not-apply")}));
    ASSERT_TRUE(_scenario->waitPrepared("ready"));

    {
        App::LivePresentationRefreshScope refresh;
        ASSERT_TRUE(refresh.acquired());
        for (int turn = 0; turn < 30; ++turn) {
            static_cast<void>(coordinator.poll(id));
            std::this_thread::sleep_for(1ms);
        }
        const auto pending = coordinator.status(id);
        ASSERT_TRUE(pending.has_value());
        EXPECT_FALSE(pending->terminal()) << pending->diagnostic;
        EXPECT_EQ(_scenario->applyCount("ready"), 0U);
        EXPECT_EQ(object("Stale").Label.getStrValue(), "Stale-before");
    }
    object("Stale").Label.setValue("external-change-before-resume");

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    EXPECT_EQ(featureSnapshot(snapshot, "ready").state,
              App::DocumentRecomputeFeatureState::Stale);
    EXPECT_EQ(_scenario->captureCount(), 1U);
    EXPECT_EQ(_scenario->applyCount("ready"), 0U);
    EXPECT_EQ(object("Stale").Label.getStrValue(),
              "external-change-before-resume");
    EXPECT_FALSE(coordinator.hasPendingWork());
}

TEST_F(DocumentRecomputeCoordinatorTest,
       preparedRecomputeCancellationWhileAwaitingLivePresentationRefreshNeverApplies)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("ready", {}, _scenarioToken, "First", "must-not-apply")}));
    ASSERT_TRUE(_scenario->waitPrepared("ready"));

    {
        App::LivePresentationRefreshScope refresh;
        ASSERT_TRUE(refresh.acquired());
        for (int turn = 0; turn < 30; ++turn) {
            static_cast<void>(coordinator.poll(id));
            std::this_thread::sleep_for(1ms);
        }
        const auto pending = coordinator.status(id);
        ASSERT_TRUE(pending.has_value());
        EXPECT_FALSE(pending->terminal()) << pending->diagnostic;
        EXPECT_EQ(_scenario->applyCount("ready"), 0U);
        EXPECT_EQ(object("First").Label.getStrValue(), "First-before");
    }
    EXPECT_TRUE(coordinator.cancel(id, "cancel while refresh admission is deferred"));

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Cancelled);
    EXPECT_EQ(featureSnapshot(snapshot, "ready").state,
              App::DocumentRecomputeFeatureState::Cancelled);
    EXPECT_EQ(_scenario->captureCount(), 1U);
    EXPECT_EQ(_scenario->applyCount("ready"), 0U);
    EXPECT_EQ(object("First").Label.getStrValue(), "First-before");
    EXPECT_FALSE(coordinator.hasPendingWork());
}

TEST_F(DocumentRecomputeCoordinatorTest,
       cancelledLivePresentationRefreshPayloadIsDestroyedWithoutHoldingStateMutex)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("ready", {}, _scenarioToken, "First", "must-not-apply")}));
    ASSERT_TRUE(_scenario->waitPrepared("ready"));
    {
        App::LivePresentationRefreshScope refresh;
        ASSERT_TRUE(refresh.acquired());
        for (int turn = 0; turn < 30; ++turn) {
            static_cast<void>(coordinator.poll(id));
            std::this_thread::sleep_for(1ms);
        }
        const auto pending = coordinator.status(id);
        ASSERT_TRUE(pending.has_value());
        ASSERT_FALSE(pending->terminal()) << pending->diagnostic;
    }

    std::promise<void> statusRead;
    auto statusReady = statusRead.get_future();
    std::jthread reader;
    bool statusReadableDuringDestruction = false;
    int destructions = 0;
    _scenario->onDestruction("ready", [&] {
        ++destructions;
        // Payload teardown is arbitrary extension code. A separate status
        // reader detects a retained state mutex without leaving this test
        // deadlocked if the owner destroys the payload under that mutex.
        reader = std::jthread([&] {
            static_cast<void>(coordinator.status(id));
            statusRead.set_value();
        });
        statusReadableDuringDestruction =
            statusReady.wait_for(250ms) == std::future_status::ready;
    });

    const bool cancelled = _document->collaborationService().runOnOwnerThread([&] {
        return coordinator.cancel(id, "dispose deferred payload safely");
    });
    const auto snapshot = waitTerminal(coordinator, id);
    if (reader.joinable()) {
        reader.join();
    }

    EXPECT_TRUE(cancelled);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Cancelled);
    EXPECT_EQ(destructions, 1);
    EXPECT_TRUE(statusReadableDuringDestruction);
    EXPECT_EQ(_scenario->applyCount("ready"), 0U);
    EXPECT_EQ(object("First").Label.getStrValue(), "First-before");
}

TEST_F(DocumentRecomputeCoordinatorTest,
       revisionFenceRefreshCapturesEachIndependentRootAfterThePriorCommit)
{
    auto& coordinator = _document->recomputeCoordinator();
    auto plan = request(
        {feature("first", {}, _scenarioToken, "First", "first-after", {}, {}, true),
         feature("second", {}, _scenarioToken, "Second", "second-after")});
    plan.refreshRevisionFenceAfterEachCommit = true;
    const auto id = coordinator.submit(std::move(plan));

    ASSERT_TRUE(_scenario->waitCaptured("first"));
    EXPECT_FALSE(_scenario->captured("second"));
    _scenario->release("first");

    const auto snapshot = waitTerminal(coordinator, id);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_TRUE(_scenario->captured("second"));
    EXPECT_EQ(object("First").Label.getStrValue(), "first-after");
    EXPECT_EQ(object("Second").Label.getStrValue(), "second-after");
}

TEST_F(DocumentRecomputeCoordinatorTest,
       staleRevisionIsRejectedWithoutOverwritingLiveState)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("stale", {}, _scenarioToken, "Stale", "detached", {}, {}, true)}));
    ASSERT_TRUE(_scenario->waitStarted("stale"));

    object("Stale").Label.setValue("external-change");
    _scenario->release("stale");
    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    EXPECT_EQ(featureSnapshot(snapshot, "stale").state,
              App::DocumentRecomputeFeatureState::Stale);
    EXPECT_EQ(object("Stale").Label.getStrValue(), "external-change");
}

TEST_F(DocumentRecomputeCoordinatorTest, cancellationDrainsToTerminalState)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("cancelled", {}, _scenarioToken, "First", "never", {}, {}, true)}));
    ASSERT_TRUE(_scenario->waitStarted("cancelled"));

    ASSERT_TRUE(coordinator.cancel(id, "native cancellation test"));
    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Cancelled);
    EXPECT_EQ(featureSnapshot(snapshot, "cancelled").state,
              App::DocumentRecomputeFeatureState::Cancelled);
    EXPECT_FALSE(coordinator.hasPendingWork());
    EXPECT_EQ(object("First").Label.getStrValue(), "First-before");
}

TEST_F(DocumentRecomputeCoordinatorTest,
       activeIdenticalPlansCoalesceAndMismatchedPlanIsRejected)
{
    auto plan = request(
        {feature("coalesced", {}, _scenarioToken, "First", "coalesced", {}, {}, true)},
        "same-active-plan");
    auto& coordinator = _document->recomputeCoordinator();
    const auto first = coordinator.submit(plan);
    const auto identical = coordinator.submit(plan);
    EXPECT_EQ(first, identical);

    auto mismatch = plan;
    mismatch.features.front().intent.arguments["value"] = "different";
    EXPECT_THROW(static_cast<void>(coordinator.submit(std::move(mismatch))),
                 std::invalid_argument);

    auto differentFence = plan;
    differentFence.refreshRevisionFenceAfterEachCommit = true;
    EXPECT_THROW(static_cast<void>(coordinator.submit(std::move(differentFence))),
                 std::invalid_argument);

    _scenario->release("coalesced");
    EXPECT_EQ(waitTerminal(coordinator, first).state,
              App::DocumentRecomputeState::Completed);
}

TEST_F(DocumentRecomputeCoordinatorTest,
       malformedDependencyPlansAreRejectedBeforeAnyWork)
{
    auto& coordinator = _document->recomputeCoordinator();

    EXPECT_THROW(static_cast<void>(coordinator.submit(request(
                     {feature("a", {"b"}, _scenarioToken, "First", "a"),
                      feature("b", {"a"}, _scenarioToken, "Second", "b")}))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.submit(request(
                     {feature("duplicate", {}, _scenarioToken, "First", "a"),
                      feature("duplicate", {}, _scenarioToken, "Second", "b")}))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.submit(request(
                     {feature("missing", {"unknown"}, _scenarioToken, "First", "a")}))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.submit(request(
                     {feature("self", {"self"}, _scenarioToken, "First", "a")}))),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(coordinator.submit(request(
                     {feature("root", {}, _scenarioToken, "First", "a"),
                      feature("duplicate-dependency",
                              {"root", "root"},
                              _scenarioToken,
                              "Second",
                              "b")}))),
                 std::invalid_argument);

    auto oversized = feature("oversized", {}, _scenarioToken, "First", "a");
    oversized.featureId.assign((1U << 20) + 1U, 'x');
    EXPECT_THROW(static_cast<void>(
                     coordinator.submit(request({std::move(oversized)}))),
                 std::invalid_argument);
    EXPECT_EQ(_scenario->captureCount(), 0U);
    EXPECT_FALSE(coordinator.hasPendingWork());
}

TEST_F(DocumentRecomputeCoordinatorTest,
       partialFailurePreservesCommitsRunsIndependentAndBlocksDescendants)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(request(
        {feature("upstream", {}, _scenarioToken, "Upstream", "upstream-after"),
         feature("failed", {}, _scenarioToken, "Failure", "never", {}, {}, false,
                 "task-fail"),
         feature("descendant",
                 {"failed"},
                 _scenarioToken,
                 "Descendant",
                 "never"),
         feature("independent", {}, _scenarioToken, "Independent", "independent-after")}));

    const auto snapshot = waitTerminal(coordinator, id);

    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::PartialFailure);
    EXPECT_EQ(featureSnapshot(snapshot, "upstream").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureSnapshot(snapshot, "independent").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureSnapshot(snapshot, "failed").state,
              App::DocumentRecomputeFeatureState::Failed);
    EXPECT_EQ(featureSnapshot(snapshot, "descendant").state,
              App::DocumentRecomputeFeatureState::Blocked);
    EXPECT_EQ(object("Upstream").Label.getStrValue(), "upstream-after");
    EXPECT_EQ(object("Independent").Label.getStrValue(), "independent-after");
    EXPECT_EQ(object("Failure").Label.getStrValue(), "Failure-before");
    EXPECT_EQ(object("Descendant").Label.getStrValue(), "Descendant-before");
}

TEST_F(DocumentRecomputeCoordinatorTest,
       commitObserverRejectsReentrantMutationsWithoutBlockingOuterCommit)
{
    auto& coordinator = _document->recomputeCoordinator();
    auto* target = _document->getObject("First");
    ASSERT_NE(target, nullptr);
    int changedNotifications = 0;
    int promptRejections = 0;
    bool statusObserved = false;
    bool attempted = false;
    std::chrono::steady_clock::duration callbackDuration {};
    App::DocumentRecomputeId outerId = 0;
    auto connection = _document->signalChangedObject.connect(
        [&](const App::DocumentObject& changedObject,
            const App::Property& changedProperty) {
            if (&changedObject != target || &changedProperty != &target->Label) {
                return;
            }
            ++changedNotifications;
            if (attempted) {
                return;
            }
            attempted = true;
            const auto started = std::chrono::steady_clock::now();
            const auto rejectsPromptly = [&](auto&& mutate) {
                try {
                    mutate();
                }
                catch (const std::runtime_error& error) {
                    if (std::string(error.what())
                        == "reentrant document recompute mutation is not supported") {
                        ++promptRejections;
                    }
                }
            };
            rejectsPromptly([&] {
                static_cast<void>(
                    coordinator.submit(App::DocumentRecomputeRequest {}));
            });
            rejectsPromptly([&] {
                static_cast<void>(coordinator.poll(outerId));
            });
            rejectsPromptly([&] {
                static_cast<void>(coordinator.cancel(outerId,
                                                     "reentrant observer cancel"));
            });
            statusObserved = coordinator.status(outerId).has_value();
            callbackDuration = std::chrono::steady_clock::now() - started;
        });

    outerId = coordinator.submit(request(
        {feature("reentrant", {}, _scenarioToken, "First", "observer-committed")}));
    const auto snapshot = waitTerminal(coordinator, outerId);
    connection.disconnect();

    EXPECT_TRUE(attempted);
    EXPECT_GE(changedNotifications, 1);
    EXPECT_EQ(promptRejections, 3);
    EXPECT_TRUE(statusObserved);
    EXPECT_LT(callbackDuration, 250ms);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(featureSnapshot(snapshot, "reentrant").state,
              App::DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(target->Label.getStrValue(), "observer-committed");
}

TEST_F(DocumentRecomputeCoordinatorTest, emptyPlanCompletesImmediately)
{
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit({});
    const auto snapshot = coordinator.status(id);

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_TRUE(snapshot->terminal());
    EXPECT_EQ(snapshot->state, App::DocumentRecomputeState::Completed);
    EXPECT_EQ(snapshot->totalFeatures, 0U);
    EXPECT_EQ(snapshot->progress, 1.0);
    EXPECT_FALSE(coordinator.hasPendingWork());
}

TEST_F(DocumentRecomputeCoordinatorTest, documentsOwnDistinctCoordinators)
{
    const std::string otherName =
        App::GetApplication().getUniqueDocumentName("otherRecomputeCoordinator");
    auto* other = App::GetApplication().newDocument(otherName.c_str(), "Other recompute");
    ASSERT_NE(other, nullptr);

    EXPECT_NE(&_document->recomputeCoordinator(), &other->recomputeCoordinator());
    EXPECT_FALSE(_document->recomputeCoordinator().hasPendingWork());
    EXPECT_FALSE(other->recomputeCoordinator().hasPendingWork());

    App::GetApplication().closeDocument(otherName.c_str());
}

TEST_F(
    DocumentRecomputeCoordinatorTest,
    committingStatusPollingDoesNotBlockDynamicPropertyCreationAcrossRefreshScope
)
{
    auto plan = request({feature("root", {}, _scenarioToken, "Upstream", "committed", {}, {}, true)});
    auto& coordinator = _document->recomputeCoordinator();
    const auto id = coordinator.submit(std::move(plan));
    ASSERT_TRUE(_scenario->waitStarted("root"));

    std::atomic<bool> keepPolling {true};
    std::atomic<int> preparingPolls {0};
    std::atomic<int> committingPolls {0};
    {
        App::LivePresentationRefreshScope refresh;
        struct PollerGuard
        {
            std::shared_ptr<RecomputeScenario> scenario;
            std::atomic<bool>& running;
            std::thread thread;
            ~PollerGuard()
            {
                running.store(false, std::memory_order_release);
                scenario->release("root");
                if (thread.joinable()) {
                    thread.join();
                }
            }
        } pollerGuard {_scenario, keepPolling, std::thread([&] {
                           while (keepPolling.load(std::memory_order_acquire)) {
                               static_cast<void>(coordinator.poll(id));
                               const auto snapshot = coordinator.status(id);
                               if (!snapshot || snapshot->features.empty()) {
                                   continue;
                               }
                               const auto state = snapshot->features.front().state;
                               if (state == App::DocumentRecomputeFeatureState::Preparing) {
                                   preparingPolls.fetch_add(1, std::memory_order_release);
                               }
                               else if (state == App::DocumentRecomputeFeatureState::Committing) {
                                   committingPolls.fetch_add(1, std::memory_order_release);
                               }
                           }
                       })};

        auto& probe = object("First");
        const auto probeDynamicProperty = [&](int& blockedCount) {
            try {
                if (probe.getDynamicPropertyByName("TransientProbe") != nullptr) {
                    static_cast<void>(probe.removeDynamicProperty("TransientProbe"));
                }
                if (!probe.addDynamicProperty(
                        "App::PropertyString",
                        "TransientProbe",
                        "Transient",
                        "",
                        static_cast<short>(App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden)
                    )) {
                    ++blockedCount;
                }
                else {
                    static_cast<void>(probe.removeDynamicProperty("TransientProbe"));
                }
            }
            catch (...) {
                ++blockedCount;
            }
        };

        int blockedPreparing = 0;
        const auto preparingDeadline = std::chrono::steady_clock::now() + 10s;
        while (preparingPolls.load(std::memory_order_acquire) < 50
               && std::chrono::steady_clock::now() < preparingDeadline) {
            probeDynamicProperty(blockedPreparing);
        }
        EXPECT_GE(preparingPolls.load(std::memory_order_acquire), 50);
        EXPECT_EQ(blockedPreparing, 0);

        _scenario->release("root");
        ASSERT_TRUE(_scenario->waitPrepared("root"));

        auto& upstream = object("Upstream");
        int blockedCommitting = 0;
        int attemptedProperties = 0;
        int failedDynamicProperties = 0;
        const auto committingDeadline = std::chrono::steady_clock::now() + 10s;
        while (committingPolls.load(std::memory_order_acquire) < 1
               && std::chrono::steady_clock::now() < committingDeadline) {
            std::this_thread::yield();
        }
        while (committingPolls.load(std::memory_order_acquire) < 300
               && std::chrono::steady_clock::now() < committingDeadline) {
            if (attemptedProperties < 64) {
                const auto propName = "TransientCache" + std::to_string(attemptedProperties++);
                try {
                    const App::DynamicPropertySchemaPublicationSuppressor suppressSchemaPublication(
                        upstream,
                        propName.c_str()
                    );
                    if (!upstream.addDynamicProperty(
                            "App::PropertyString",
                            propName.c_str(),
                            "Transient",
                            "",
                            static_cast<short>(App::Prop_NoPersist | App::Prop_Output | App::Prop_Hidden)
                        )) {
                        ++failedDynamicProperties;
                    }
                }
                catch (...) {
                    ++failedDynamicProperties;
                }
            }
            else {
                probeDynamicProperty(blockedCommitting);
            }
        }
        EXPECT_GE(committingPolls.load(std::memory_order_acquire), 300);
        EXPECT_EQ(blockedCommitting, 0);
        EXPECT_EQ(attemptedProperties, 64);
        EXPECT_EQ(failedDynamicProperties, 0);
    }

    const auto snapshot = waitTerminal(coordinator, id);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed);
}

TEST_F(
    DocumentRecomputeCoordinatorTest,
    ownerDispatchSubmitAndConcurrentAdmissionAllocateDistinctPlanIds
)
{
    auto& coordinator = _document->recomputeCoordinator();
    auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);
    auto executionHandle = _document->executionHandle();
    using Access = App::Internal::DocumentRecomputeCoordinatorTestAccess;
    Access::CheckAllocationLocks checkAllocationLocks;

    // The hook observes the real lock immediately before _nextId is accessed.
    // It never acquires that mutex, so removing allocation protection fails
    // its owns_lock assertion regardless of how the two threads are scheduled.
    const auto initialIds = lane->postToOwner([&] {
        const auto baselineId = coordinator.submit({});
        const auto explicitId = baselineId + 10U;
        EXPECT_EQ(coordinator.submit({}, explicitId), explicitId);
        EXPECT_THROW(static_cast<void>(coordinator.submit({}, explicitId)),
                     std::invalid_argument);
        const auto nextId = coordinator.submit({});
        EXPECT_EQ(nextId, explicitId + 1U);
        return std::vector<App::DocumentRecomputeId> {baselineId, explicitId, nextId};
    }).get();
    std::set<App::DocumentRecomputeId> allocatedIds(initialIds.begin(), initialIds.end());
    ASSERT_EQ(allocatedIds.size(), 3U);

    const auto waitLane = [](App::DocumentCommandHandle& handle) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        auto snapshot = handle.status();
        while (!snapshot.terminal() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
            snapshot = handle.status();
        }
        return snapshot;
    };

    struct DispatchState
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool submitted {false};
        bool release {false};
        bool timedOut {false};
        App::DocumentRecomputeId id {0};
    };

    // Complete both plans, cancel only the dispatch plan, then cancel the
    // admitted command before binding. No simultaneous-start stress oracle.
    constexpr int rounds = 3;
    constexpr int dispatchCancelRound = 1;
    constexpr int admissionCancelRound = 2;
    for (int round = 0; round < rounds; ++round) {
        const std::string featureId = "dispatch-feature-" + std::to_string(round);
        const std::string beforeLabel = "First-before-" + std::to_string(round);
        const std::string afterLabel = "First-after-" + std::to_string(round);
        auto state = std::make_shared<DispatchState>();
        std::future<void> ownerFuture;
        std::optional<App::DocumentCommandHandle> commandHandle;

        struct Cleanup
        {
            std::shared_ptr<DispatchState> state;
            std::shared_ptr<RecomputeScenario> scenario;
            std::string featureId;
            std::future<void>& ownerFuture;
            std::optional<App::DocumentCommandHandle>& commandHandle;
            App::DocumentExecutionLane& lane;
            App::DocumentRecomputeCoordinator& coordinator;
            decltype(waitLane)& waitCommand;

            ~Cleanup()
            {
                // Release every test wait before any get()/join, including
                // fatal assertions before admission or before task startup.
                {
                    std::lock_guard lock(state->mutex);
                    state->release = true;
                }
                state->cv.notify_all();
                scenario->release(featureId);
                if (ownerFuture.valid()) {
                    try {
                        ownerFuture.get();
                    }
                    catch (...) {
                    }
                }
                if (commandHandle) {
                    try {
                        static_cast<void>(commandHandle->cancel("test cleanup"));
                        static_cast<void>(waitCommand(*commandHandle));
                    }
                    catch (...) {
                    }
                }
                // Independent coordinator work is not pumped by the lane's
                // command loop. Cancel and drain it explicitly on the owner.
                try {
                    lane.postToOwner([this] {
                        if (state->id != 0) {
                            static_cast<void>(coordinator.cancel(state->id, "test cleanup"));
                            static_cast<void>(waitTerminal(coordinator, state->id));
                        }
                    }).get();
                }
                catch (...) {
                }
            }
        } cleanup {state, _scenario, featureId, ownerFuture, commandHandle,
                   *lane, coordinator, waitLane};

        auto plan = request(
            {feature(featureId, {}, _scenarioToken, "First", afterLabel, {}, {}, true)},
            "dispatch-plan-" + std::to_string(round));
        ownerFuture = lane->postToOwner(
            [&, state, plan = std::move(plan), beforeLabel, round]() mutable {
                object("First").Label.setValue(beforeLabel);
                EXPECT_FALSE(_document->mustExecute()) << "before dispatch plan, round " << round;
                state->id = coordinator.submit(std::move(plan));
                // The detached adapter must capture a stable document. Touch
                // Second only after capture, but before the lane admission.
                object("Second").touch();
                EXPECT_TRUE(_document->mustExecute()) << "before admission, round " << round;
                std::unique_lock lock(state->mutex);
                state->submitted = true;
                state->cv.notify_all();
                state->timedOut = !state->cv.wait_for(lock, 5s, [&] { return state->release; });
            });
        {
            std::unique_lock lock(state->mutex);
            ASSERT_TRUE(state->cv.wait_for(lock, 5s, [&] { return state->submitted; }));
        }

        // The owner is still inside the ordinary dispatch, with no active
        // command. Admission runs on this other thread while dispatch is held.
        App::DocumentCommand command;
        command.kind = App::DocumentCommandKind::Recompute;
        command.document = executionHandle.identity();
        command.recompute.emplace();
        command.recompute->coalescingKey = "admission-overlap-" + std::to_string(round);
        command.recompute->featureIds = {"Second"};
        command.recompute->force = true;
        const auto outcome = executionHandle.trySubmit(std::move(command));
        if (outcome.result == App::DocumentCommandSubmitResult::Accepted) {
            commandHandle.emplace(outcome.commandId, executionHandle.identity());
        }
        ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted)
            << outcome.diagnostic;
        ASSERT_NE(outcome.commandId, 0U);
        const auto admitted = commandHandle->status();
        ASSERT_TRUE(admitted.recompute.has_value());
        const auto admissionId = static_cast<App::DocumentRecomputeId>(admitted.recompute->id);
        EXPECT_GT(admissionId, 0U);
        EXPECT_NE(state->id, admissionId);
        EXPECT_TRUE(allocatedIds.insert(state->id).second);
        EXPECT_TRUE(allocatedIds.insert(admissionId).second);

        const auto dispatchBeforeBinding = coordinator.status(state->id);
        ASSERT_TRUE(dispatchBeforeBinding.has_value());
        ASSERT_EQ(dispatchBeforeBinding->features.size(), 1U);
        EXPECT_EQ(dispatchBeforeBinding->features.front().featureId, featureId);
        ASSERT_EQ(dispatchBeforeBinding->state, App::DocumentRecomputeState::Running)
            << "round " << round << ": " << dispatchBeforeBinding->diagnostic;
        EXPECT_FALSE(coordinator.status(admissionId).has_value());
        if (round == admissionCancelRound) {
            // There is deliberately no coordinator job for admissionId yet.
            // cancelCommand records the lane flag even when coordinator.cancel
            // returns false; binding still creates the job before pump cancels.
            EXPECT_TRUE(commandHandle->cancel("cancel admitted command only"));
            const auto unaffected = coordinator.status(state->id);
            ASSERT_TRUE(unaffected.has_value());
            EXPECT_EQ(unaffected->state, App::DocumentRecomputeState::Running);
        }
        {
            std::lock_guard lock(state->mutex);
            state->release = true;
        }
        state->cv.notify_all();
        ownerFuture.get();
        EXPECT_FALSE(state->timedOut);
        ASSERT_TRUE(_scenario->waitStarted(featureId));
        const auto terminalCommand = waitLane(*commandHandle);
        ASSERT_TRUE(terminalCommand.terminal()) << terminalCommand.diagnostic;
        ASSERT_TRUE(terminalCommand.recompute.has_value());
        EXPECT_EQ(terminalCommand.recompute->id, admissionId);

        // Prebinding cancellation does not bypass submission on this source
        // base. Verify existence before inspecting the reserved job's features.
        const auto admittedJob = coordinator.status(admissionId);
        ASSERT_TRUE(admittedJob.has_value());
        EXPECT_EQ(admittedJob->id, admissionId);
        ASSERT_EQ(admittedJob->features.size(), 1U);
        EXPECT_EQ(admittedJob->features.front().featureId, "Second");
        const auto expectedCommandState = round == admissionCancelRound
            ? App::DocumentCommandState::Cancelled : App::DocumentCommandState::Completed;
        const auto expectedJobState = round == admissionCancelRound
            ? App::DocumentRecomputeState::Cancelled : App::DocumentRecomputeState::Completed;
        EXPECT_EQ(terminalCommand.state, expectedCommandState) << terminalCommand.diagnostic;
        EXPECT_EQ(admittedJob->state, expectedJobState);

        App::RecomputeHandle dispatchHandle(*_document, state->id);
        const auto heldDispatch = dispatchHandle.status();
        EXPECT_EQ(heldDispatch.id, state->id);
        EXPECT_EQ(heldDispatch.state, App::DocumentRecomputeState::Running);
        ASSERT_EQ(heldDispatch.features.size(), 1U);
        EXPECT_EQ(heldDispatch.features.front().featureId, featureId);
        EXPECT_EQ(lane->postToOwner([&] { return object("First").Label.getStrValue(); }).get(),
                  beforeLabel);
        if (round == dispatchCancelRound) {
            EXPECT_TRUE(dispatchHandle.cancel("cancel dispatch plan only"));
        }
        _scenario->release(featureId);
        const auto settled = lane->postToOwner([&] {
            return waitTerminal(coordinator, state->id);
        }).get();
        EXPECT_EQ(settled.state, round == dispatchCancelRound
                                     ? App::DocumentRecomputeState::Cancelled
                                     : App::DocumentRecomputeState::Completed);
        EXPECT_EQ(lane->postToOwner([&] { return object("First").Label.getStrValue(); }).get(),
                  round == dispatchCancelRound ? beforeLabel : afterLabel);
        const auto unaffectedAdmission = coordinator.status(admissionId);
        ASSERT_TRUE(unaffectedAdmission.has_value());
        EXPECT_EQ(unaffectedAdmission->state, expectedJobState);
    }

    EXPECT_EQ(allocatedIds.size(), 9U);
    const auto finalId = lane->postToOwner([&] { return coordinator.submit({}); }).get();
    EXPECT_EQ(finalId, initialIds.back() + 7U);
    EXPECT_TRUE(allocatedIds.insert(finalId).second);
    EXPECT_EQ(allocatedIds.size(), 10U);
    EXPECT_EQ(Access::defaultAllocations.load(std::memory_order_relaxed), 6U);
    EXPECT_EQ(Access::reservedBindings.load(std::memory_order_relaxed), 4U);
}
