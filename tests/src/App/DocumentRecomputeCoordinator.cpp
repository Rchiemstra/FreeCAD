// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/FeatureTest.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <src/App/InitApplication.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

private:
    mutable std::mutex _mutex;
    std::condition_variable_any _changed;
    std::map<std::string, std::string> _captured;
    std::vector<std::string> _captureOrder;
    std::set<std::string> _started;
    std::set<std::string> _released;
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
                               std::string mode)
        : _target(std::move(target))
        , _identity(std::move(identity))
        , _value(std::move(value))
        , _mode(std::move(mode))
    {}

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
                        return std::make_unique<const RecomputeSetLabelOperation>(
                            targetName, identity, capturedValue, mode);
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
