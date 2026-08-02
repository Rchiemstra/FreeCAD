// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include "App/Application.h"
#include "App/CollaborativeOperationRegistry.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/DocumentObject.h"
#include "App/FeatureTest.h"
#include "App/private/CollaborativeOperationRegistryInternal.h"
#include <src/App/InitApplication.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

using namespace std::chrono_literals;

namespace App::Internal
{

class DocumentCollaborationConcurrencyTestAccess
{
public:
    static std::recursive_mutex& commitMutex(Document& document) noexcept
    {
        return document.collaborationCommitMutex();
    }
};

}  // namespace App::Internal

namespace
{

constexpr std::string_view ConcurrencyOperationType =
    "App.Test.DetachedConcurrencyAcceptance";

class ScenarioProbe
{
public:
    [[nodiscard]] bool enterPreparation(std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        ++_preparationEntries;
        ++_activePreparations;
        _maxActivePreparations = std::max(_maxActivePreparations, _activePreparations);
        _changed.notify_all();
        _changed.wait(lock, stopToken, [this] { return _preparationsReleased; });
        --_activePreparations;
        _changed.notify_all();
        return !stopToken.stop_requested();
    }

    [[nodiscard]] bool waitForPreparationEntries(
        std::size_t count,
        std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [this, count] {
            return _preparationEntries >= count;
        });
    }

    void releasePreparations()
    {
        {
            std::lock_guard lock(_mutex);
            _preparationsReleased = true;
        }
        _changed.notify_all();
    }

    void blockCommits()
    {
        std::lock_guard lock(_mutex);
        _commitsBlocked = true;
        _commitsReleased = false;
    }

    void releaseCommits()
    {
        {
            std::lock_guard lock(_mutex);
            _commitsReleased = true;
        }
        _changed.notify_all();
    }

    void beginCommit()
    {
        std::unique_lock lock(_mutex);
        ++_commitEntries;
        ++_activeCommits;
        _maxActiveCommits = std::max(_maxActiveCommits, _activeCommits);
        _changed.notify_all();
        _changed.wait(lock, [this] { return !_commitsBlocked || _commitsReleased; });
    }

    void finishCommit()
    {
        std::lock_guard lock(_mutex);
        --_activeCommits;
        _changed.notify_all();
    }

    [[nodiscard]] std::size_t maxActivePreparations() const
    {
        std::lock_guard lock(_mutex);
        return _maxActivePreparations;
    }

    [[nodiscard]] std::size_t maxActiveCommits() const
    {
        std::lock_guard lock(_mutex);
        return _maxActiveCommits;
    }

    [[nodiscard]] std::size_t commitEntries() const
    {
        std::lock_guard lock(_mutex);
        return _commitEntries;
    }

    [[nodiscard]] bool waitForCommitEntries(
        std::size_t count,
        std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [this, count] {
            return _commitEntries >= count;
        });
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable_any _changed;
    std::size_t _preparationEntries {0};
    std::size_t _activePreparations {0};
    std::size_t _maxActivePreparations {0};
    std::size_t _commitEntries {0};
    std::size_t _activeCommits {0};
    std::size_t _maxActiveCommits {0};
    bool _preparationsReleased {false};
    bool _commitsBlocked {false};
    bool _commitsReleased {false};
};

class ScenarioStore
{
public:
    static std::string add(const std::shared_ptr<ScenarioProbe>& probe)
    {
        const auto sequence = NextSequence.fetch_add(1, std::memory_order_relaxed);
        std::string token = "concurrency-scenario-" + std::to_string(sequence);
        std::lock_guard lock(Mutex);
        Scenarios.emplace(token, probe);
        return token;
    }

    static std::shared_ptr<ScenarioProbe> find(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        const auto found = Scenarios.find(token);
        return found == Scenarios.end() ? nullptr : found->second;
    }

    static void remove(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        Scenarios.erase(token);
    }

private:
    static inline std::atomic_uint64_t NextSequence {1};
    static inline std::mutex Mutex;
    static inline std::unordered_map<std::string, std::shared_ptr<ScenarioProbe>> Scenarios;
};

class Scenario final
{
public:
    Scenario()
        : probe(std::make_shared<ScenarioProbe>())
        , token(ScenarioStore::add(probe))
    {}

    ~Scenario()
    {
        probe->releasePreparations();
        probe->releaseCommits();
        ScenarioStore::remove(token);
    }

    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;

    std::shared_ptr<ScenarioProbe> probe;
    std::string token;
};

class CommitGateRelease final
{
public:
    explicit CommitGateRelease(std::shared_ptr<ScenarioProbe> probe)
        : _probe(std::move(probe))
    {}

    ~CommitGateRelease()
    {
        release();
    }

    void release()
    {
        if (_probe) {
            _probe->releaseCommits();
            _probe.reset();
        }
    }

private:
    std::shared_ptr<ScenarioProbe> _probe;
};

class CommitProbeScope final
{
public:
    explicit CommitProbeScope(std::shared_ptr<ScenarioProbe> probe)
        : _probe(std::move(probe))
    {
        _probe->beginCommit();
    }

    ~CommitProbeScope()
    {
        _probe->finishCommit();
    }

private:
    std::shared_ptr<ScenarioProbe> _probe;
};

class ConcurrencyOperation final: public App::CollaborativeOperation
{
public:
    ConcurrencyOperation(std::string targetName,
                         std::string targetIdentity,
                         std::string value,
                         std::string scenarioToken)
        : _targetName(std::move(targetName))
        , _targetIdentity(std::move(targetIdentity))
        , _value(std::move(value))
        , _scenarioToken(std::move(scenarioToken))
    {}

    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return ConcurrencyOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto probe = ScenarioStore::find(_scenarioToken);
        if (!probe) {
            throw std::runtime_error("concurrency scenario expired before commit");
        }
        CommitProbeScope commitScope(std::move(probe));

        auto* target = document.getObject(_targetName.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _targetIdentity) {
            throw std::runtime_error("concurrency target became stale");
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
                "detached concurrency target must equal the captured value"};
    }

private:
    const std::string _targetName;
    const std::string _targetIdentity;
    const std::string _value;
    const std::string _scenarioToken;
};

void ensureConcurrencyAdapterRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(ConcurrencyOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                const auto& sourceName = intent.arguments.at("source");
                const auto& targetName = intent.arguments.at("target");
                const auto& suffix = intent.arguments.at("suffix");
                const auto& scenarioToken = intent.arguments.at("scenario");
                const auto* source = document.getObject(sourceName.c_str());
                const auto* target = document.getObject(targetName.c_str());
                auto probe = ScenarioStore::find(scenarioToken);
                if (!source || !target || !probe) {
                    throw std::invalid_argument("invalid detached concurrency scenario");
                }

                const std::string targetIdentity =
                    document.collaborationObjectIdentity(*target);
                const std::string value = source->Label.getStrValue() + "/" + suffix;
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [targetName,
                     targetIdentity,
                     value,
                     scenarioToken,
                     probe = std::move(probe)](std::stop_token stopToken) {
                        if (!probe->enterPreparation(stopToken)) {
                            throw std::runtime_error(
                                "detached concurrency preparation cancelled");
                        }
                        return std::make_unique<const ConcurrencyOperation>(
                            targetName, targetIdentity, value, scenarioToken);
                    };

                return App::CollaborativeOperationPreparation {
                    {App::DocumentRevisionKey::objectExistence(sourceName),
                     App::DocumentRevisionKey::objectModel(sourceName),
                     App::DocumentRevisionKey::objectStructure(sourceName),
                     App::DocumentRevisionKey::objectExistence(targetName),
                     App::DocumentRevisionKey::objectStructure(targetName)},
                    {App::DocumentRevisionKey::objectModel(targetName)},
                    {{App::DocumentRevisionKey::objectModel(targetName), targetIdentity}},
                    std::move(task)};
            }));
    });
}

App::CollaborativeOperationIntent makeIntent(const Scenario& scenario,
                                             std::string source,
                                             std::string target,
                                             std::string suffix)
{
    App::CollaborativeOperationIntent intent;
    intent.operationType = ConcurrencyOperationType;
    intent.arguments = {{"scenario", scenario.token},
                        {"source", std::move(source)},
                        {"suffix", std::move(suffix)},
                        {"target", std::move(target)}};
    return intent;
}

bool terminal(App::PreparedEditExecutionStatus status)
{
    return status == App::PreparedEditExecutionStatus::Completed
        || status == App::PreparedEditExecutionStatus::Cancelled
        || status == App::PreparedEditExecutionStatus::Failed;
}

std::optional<App::PreparedEditExecutionSnapshot> waitForTerminal(
    App::DocumentCollaborationService& service,
    App::PreparedEditExecutionId executionId,
    std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = service.preparedEditStatus(executionId);
        if (status && terminal(status->status)) {
            return status;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

class AsyncBlockerRelease final
{
public:
    ~AsyncBlockerRelease()
    {
        App::FeatureTestAsyncBlocker::releaseBlocker();
    }
};

class DocumentCollaborationConcurrencyTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
        ensureConcurrencyAdapterRegistered();
    }

    void SetUp() override
    {
        _documentName =
            App::GetApplication().getUniqueDocumentName("collaborationConcurrency");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "Concurrency acceptance test");
        for (const char* name : {"SourceA", "SourceB", "TargetA", "TargetB"}) {
            auto* object = _document->addObject<App::FeatureTest>(name);
            ASSERT_NE(object, nullptr);
            object->Label.setValue(std::string(name) + "-before");
        }
        _document->recompute();
        _session = _document->collaborationService().beginEditSession("concurrency-actor");
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
        }
    }

    App::PreparedEditExecutionId submit(const Scenario& scenario,
                                        std::string operationId,
                                        std::string source,
                                        std::string target,
                                        std::string suffix)
    {
        return _document->collaborationService().prepareEditAsync(
            _session.sessionId(),
            std::move(operationId),
            makeIntent(scenario,
                       std::move(source),
                       std::move(target),
                       std::move(suffix)),
            "phase-3-concurrency-acceptance");
    }

    std::optional<App::CollaborationPreparedEditResult>
    collect(App::PreparedEditExecutionId executionId)
    {
        if (!waitForTerminal(_document->collaborationService(), executionId)) {
            return std::nullopt;
        }
        return _document->collaborationService().takePreparedEdit(
            _session.sessionId(), executionId);
    }

    App::Document* _document {nullptr};
    App::EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

static_assert(std::is_integral_v<App::PreparedEditExecutionId>);
static_assert(!std::is_pointer_v<App::PreparedEditExecutionId>);

TEST_F(DocumentCollaborationConcurrencyTest,
       independentPreparationsOverlapAndFinalCommitsSerialize)
{
    Scenario scenario;
    const auto firstId = submit(scenario, "overlap-a", "SourceA", "TargetA", "one");
    const auto secondId = submit(scenario, "overlap-b", "SourceB", "TargetB", "two");

    const bool preparationsOverlapped = scenario.probe->waitForPreparationEntries(2);
    scenario.probe->releasePreparations();
    EXPECT_TRUE(preparationsOverlapped);
    EXPECT_GE(scenario.probe->maxActivePreparations(), 2U);

    auto first = collect(firstId);
    auto second = collect(secondId);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_EQ(second->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_NE(first->preparedEdit, nullptr);
    ASSERT_NE(second->preparedEdit, nullptr);

    scenario.probe->blockCommits();
    CommitGateRelease releaseCommitGate(scenario.probe);
    std::atomic_bool verifierSawCommit {false};
    std::atomic_bool verifierAcquiredCommitMutex {false};
    std::jthread verifier([&] {
        const bool commitEntered = scenario.probe->waitForCommitEntries(1);
        verifierSawCommit.store(commitEntered, std::memory_order_release);
        if (commitEntered) {
            auto& commitMutex =
                App::Internal::DocumentCollaborationConcurrencyTestAccess::commitMutex(
                    *_document);
            const bool acquired = commitMutex.try_lock();
            verifierAcquiredCommitMutex.store(acquired, std::memory_order_release);
            if (acquired) {
                commitMutex.unlock();
            }
        }
        scenario.probe->releaseCommits();
    });

    const auto firstCommit = _document->collaborationService().commitEdit(
        _session.sessionId(), *first->preparedEdit);
    verifier.join();
    releaseCommitGate.release();

    EXPECT_TRUE(verifierSawCommit.load(std::memory_order_acquire));
    EXPECT_FALSE(verifierAcquiredCommitMutex.load(std::memory_order_acquire));
    EXPECT_TRUE(firstCommit.committed());

    const auto secondCommit = _document->collaborationService().commitEdit(
        _session.sessionId(), *second->preparedEdit);
    EXPECT_TRUE(secondCommit.committed());
    EXPECT_EQ(scenario.probe->maxActiveCommits(), 1U);
}

TEST_F(DocumentCollaborationConcurrencyTest,
       mutationOfActualReadDependencyRejectsDetachedResultAsStale)
{
    Scenario scenario;
    const auto executionId =
        submit(scenario, "actual-read-set", "SourceA", "TargetA", "captured");
    const bool preparationStarted = scenario.probe->waitForPreparationEntries(1);
    EXPECT_TRUE(preparationStarted);

    auto* source = _document->getObject("SourceA");
    ASSERT_NE(source, nullptr);
    source->Label.setValue("SourceA-changed-after-capture");
    scenario.probe->releasePreparations();

    auto prepared = collect(executionId);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(prepared->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_NE(prepared->preparedEdit, nullptr);

    const auto result = _document->collaborationService().commitEdit(
        _session.sessionId(), *prepared->preparedEdit);
    EXPECT_EQ(result.status, App::DocumentCommitStatus::Conflict);
    EXPECT_TRUE(std::ranges::any_of(result.conflicts, [](const auto& conflict) {
        return conflict.key == App::DocumentRevisionKey::objectModel("SourceA");
    }));
    EXPECT_EQ(_document->getObject("TargetA")->Label.getStrValue(), "TargetA-before");
    EXPECT_EQ(scenario.probe->commitEntries(), 0U);
}

TEST_F(DocumentCollaborationConcurrencyTest,
       detachedPreparationOverlapsLiveRecomputeWhileFinalCommitWaits)
{
    auto* blocker = dynamic_cast<App::FeatureTestAsyncBlocker*>(
        _document->addObject("App::FeatureTestAsyncBlocker", "RecomputeBlocker"));
    ASSERT_NE(blocker, nullptr);
    App::FeatureTestAsyncBlocker::resetBlocker();
    AsyncBlockerRelease releaseBlockerOnExit;
    App::FeatureTestAsyncBlocker::releaseBlocker();
    _document->recompute();
    App::FeatureTestAsyncBlocker::resetBlocker();

    Scenario commitScenario;
    commitScenario.probe->releasePreparations();
    const auto commitId =
        submit(commitScenario, "recompute-commit", "SourceA", "TargetA", "after");
    auto commitCandidate = collect(commitId);
    ASSERT_TRUE(commitCandidate.has_value());
    ASSERT_EQ(commitCandidate->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_NE(commitCandidate->preparedEdit, nullptr);

    Scenario activePreparation;
    const auto activeId =
        submit(activePreparation, "recompute-overlap", "SourceB", "TargetB", "later");
    const bool preparationStarted = activePreparation.probe->waitForPreparationEntries(1);
    EXPECT_TRUE(preparationStarted);

    blocker->touch();
    App::GetApplication().queueRecomputeRequest(
        App::RecomputeRequest::fromDocumentObject(*blocker));
    const bool recomputeStarted = App::FeatureTestAsyncBlocker::waitUntilStarted(2s);
    EXPECT_TRUE(recomputeStarted);
    EXPECT_EQ(activePreparation.probe->maxActivePreparations(), 1U);

    auto& commitMutex =
        App::Internal::DocumentCollaborationConcurrencyTestAccess::commitMutex(
            *_document);
    const bool commitMutexAvailable = commitMutex.try_lock();
    if (commitMutexAvailable) {
        commitMutex.unlock();
    }
    EXPECT_FALSE(commitMutexAvailable);
    EXPECT_EQ(commitScenario.probe->commitEntries(), 0U);

    App::FeatureTestAsyncBlocker::releaseBlocker();
    const auto commitResult = _document->collaborationService().commitEdit(
        _session.sessionId(), *commitCandidate->preparedEdit);

    EXPECT_TRUE(commitResult.committed());
    EXPECT_EQ(commitScenario.probe->commitEntries(), 1U);

    activePreparation.probe->releasePreparations();
    auto uncommitted = collect(activeId);
    ASSERT_TRUE(uncommitted.has_value());
    EXPECT_EQ(uncommitted->status, App::PreparedEditExecutionStatus::Completed);
}
