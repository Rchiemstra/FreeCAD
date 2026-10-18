// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QTimer>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentRevisionIndex.h>
#include <App/FeatureTest.h>
#include <App/MainThreadSignal.h>
#include <App/MutationClassification.h>
#include <Gui/DocumentExecutionIngress.h>
#include "CollaborationGuiTestHelpers.h"
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <Gui/Application.h>
#include <Gui/ApplicationPy.h>
#include <Base/Interpreter.h>
#include <Gui/Camera.h>
#include <Gui/Command.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/Tree.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <Gui/ViewProviderDocumentObject.h>
#include <src/App/InitApplication.h>

#include <Base/Exception.h>
#include <QScopeGuard>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

using namespace std::chrono_literals;

namespace
{

constexpr std::string_view ResponsiveOperationType =
    "Gui.Test.DetachedResponsivenessAcceptance";

class PreparationGate
{
public:
    [[nodiscard]] bool enter(std::stop_token stopToken)
    {
        std::unique_lock lock(_mutex);
        _entered = true;
        _changed.notify_all();
        _changed.wait(lock, stopToken, [this] { return _released; });
        return !stopToken.stop_requested();
    }

    [[nodiscard]] bool waitUntilEntered(std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(_mutex);
        return _changed.wait_for(lock, timeout, [this] { return _entered; });
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
    bool _entered {false};
    bool _released {false};
};

class GateStore
{
public:
    static std::string add(const std::shared_ptr<PreparationGate>& gate)
    {
        const auto sequence = NextSequence.fetch_add(1, std::memory_order_relaxed);
        std::string token = "gui-responsiveness-" + std::to_string(sequence);
        std::lock_guard lock(Mutex);
        Gates.emplace(token, gate);
        return token;
    }

    static std::shared_ptr<PreparationGate> find(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        const auto found = Gates.find(token);
        return found == Gates.end() ? nullptr : found->second;
    }

    static void remove(const std::string& token)
    {
        std::lock_guard lock(Mutex);
        Gates.erase(token);
    }

private:
    static inline std::atomic_uint64_t NextSequence {1};
    static inline std::mutex Mutex;
    static inline std::unordered_map<std::string, std::shared_ptr<PreparationGate>> Gates;
};

class GateScenario final
{
public:
    GateScenario()
        : gate(std::make_shared<PreparationGate>())
        , token(GateStore::add(gate))
    {}

    ~GateScenario()
    {
        gate->release();
        GateStore::remove(token);
    }

    GateScenario(const GateScenario&) = delete;
    GateScenario& operator=(const GateScenario&) = delete;

    std::shared_ptr<PreparationGate> gate;
    std::string token;
};

class LivePresentationProbe final: public Gui::ViewProviderDocumentObject
{
public:
    std::function<void()> refresh;

protected:
    void updateData(const App::Property* property) override
    {
        if (getObject() && property == &getObject()->Label && refresh) {
            // Clear before invoking: the probe's App write can notify this
            // provider recursively, and must not start a second attempt.
            auto callback = std::exchange(refresh, {});
            callback();
        }
        Gui::ViewProviderDocumentObject::updateData(property);
    }
};

class PendingRecomputeProbe final: public App::DocumentObject
{
public:
    mutable unsigned int probes {0};

    short mustExecute() const override
    {
        ++probes;
        return 1;
    }
};

class OwnerAdmissionAttempt final
{
public:
    OwnerAdmissionAttempt(App::Document& document, const std::atomic_bool& refreshing)
        : _document(document)
        , _refreshing(refreshing)
        , _worker([this] { run(); })
    {}

    ~OwnerAdmissionAttempt()
    {
        releaseAndJoin();
    }

    bool startAndWait()
    {
        std::unique_lock lock(_mutex);
        _start = true;
        _changed.notify_all();
        // A watchdog only: both successful admission and an explicit Busy
        // refusal complete this handshake. A timeout always fails the test.
        return _changed.wait_for(lock, 5s, [this] { return _attemptFinished; });
    }

    void release()
    {
        std::lock_guard lock(_mutex);
        _release = true;
        _changed.notify_all();
    }

    void releaseAndJoin()
    {
        release();
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    bool accepted {false};
    bool acceptedDuringRefresh {false};
    std::string rejection;
    std::exception_ptr failure;

private:
    void run()
    {
        try {
            {
                std::unique_lock lock(_mutex);
                _changed.wait(lock, [this] { return _start || _release; });
                if (!_start) {
                    return;
                }
            }
            auto* lane = _document.executionLane();
            if (!lane) {
                throw std::runtime_error("presentation probe requires a document owner lane");
            }
            lane->dispatchToOwner([this] {
                try {
                    App::beginAtomicPresentationMutationTarget(_document);
                    accepted = true;
                    acceptedDuringRefresh = _refreshing.load(std::memory_order_acquire);
                }
                catch (const Base::Exception& error) {
                    rejection = error.what();
                }
                const auto endAdmission = qScopeGuard([this] {
                    if (accepted) {
                        App::endAtomicPresentationMutationTarget(_document);
                    }
                });
                std::unique_lock lock(_mutex);
                _attemptFinished = true;
                _changed.notify_all();
                _changed.wait(lock, [this] { return _release; });
            });
        }
        catch (...) {
            std::lock_guard lock(_mutex);
            failure = std::current_exception();
            _attemptFinished = true;
            _changed.notify_all();
        }
    }

    App::Document& _document;
    const std::atomic_bool& _refreshing;
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _start {false};
    bool _release {false};
    bool _attemptFinished {false};
    std::thread _worker;
};

class ResponsiveOperation final: public App::CollaborativeOperation
{
public:
    ResponsiveOperation(std::string targetName,
                        std::string targetIdentity,
                        std::string value)
        : _targetName(std::move(targetName))
        , _targetIdentity(std::move(targetIdentity))
        , _value(std::move(value))
    {}

    [[nodiscard]] std::string_view typeId() const noexcept override
    {
        return ResponsiveOperationType;
    }

    void apply(App::Document& document) const override
    {
        auto* target = document.getObject(_targetName.c_str());
        if (!target
            || document.collaborationObjectIdentity(*target) != _targetIdentity) {
            throw std::runtime_error("responsiveness target became stale");
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
                "responsive detached edit must apply its captured value"};
    }

private:
    const std::string _targetName;
    const std::string _targetIdentity;
    const std::string _value;
};

void ensureResponsivenessAdapterRegistered()
{
    static std::once_flag once;
    std::call_once(once, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(ResponsiveOperationType),
            [](const App::Document& document,
               const App::CollaborativeOperationIntent& intent) {
                const auto& sourceName = intent.arguments.at("source");
                const auto& targetName = intent.arguments.at("target");
                const auto& scenarioToken = intent.arguments.at("scenario");
                const auto* source = document.getObject(sourceName.c_str());
                const auto* target = document.getObject(targetName.c_str());
                auto gate = GateStore::find(scenarioToken);
                if (!source || !target || !gate) {
                    throw std::invalid_argument("invalid GUI responsiveness scenario");
                }

                const std::string targetIdentity =
                    document.collaborationObjectIdentity(*target);
                const std::string value = source->Label.getStrValue() + "/detached";
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [targetName,
                     targetIdentity,
                     value,
                     gate = std::move(gate)](std::stop_token stopToken) {
                        if (!gate->enter(stopToken)) {
                            throw std::runtime_error(
                                "GUI responsiveness preparation cancelled");
                        }
                        return std::make_unique<const ResponsiveOperation>(
                            targetName, targetIdentity, value);
                    };

                return App::CollaborativeOperationPreparation {
                    {App::DocumentRevisionKey::objectExistence(sourceName),
                     App::DocumentRevisionKey::objectModel(sourceName),
                     App::DocumentRevisionKey::objectStructure(sourceName),
                     App::DocumentRevisionKey::objectExistence(targetName),
                     App::DocumentRevisionKey::objectStructure(targetName),
                     App::DocumentRevisionKey::unknownModelMutation()},
                    {App::DocumentRevisionKey::objectModel(targetName)},
                    {{App::DocumentRevisionKey::objectModel(targetName), targetIdentity}},
                    std::move(task)};
            }));
    });
}

void initializeHeadlessGui()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    static int argc = 1;
    static char executable[] = "Gui_tests_run";
    static char* argv[] = {executable, nullptr};
    if (!QApplication::instance()) {
        new QApplication(argc, argv);
    }

    tests::initApplication();
    if (!Gui::Application::Instance) {
        Gui::Application::initApplication();
        Gui::Application::initOpenInventor();
        new Gui::Application(true);
    }
    if (!Gui::MainWindow::getInstance()) {
        new Gui::MainWindow();
    }
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
        QApplication::processEvents();
        auto status = service.preparedEditStatus(executionId);
        if (status && terminal(status->status)) {
            return status;
        }
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

class CollaborationResponsivenessTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        initializeHeadlessGui();
        ensureResponsivenessAdapterRegistered();
    }

    void SetUp() override
    {
        App::DocumentInitFlags flags;
        flags.createView = false;
        _documentName =
            App::GetApplication().getUniqueDocumentName("collaborationResponsive");
        _document = App::GetApplication().newDocument(
            _documentName.c_str(), "GUI responsiveness acceptance", flags);
        ASSERT_NE(_document, nullptr);
        _source = _document->addObject("App::FeatureTest", "Source");
        _target = _document->addObject("App::FeatureTest", "Target");
        ASSERT_NE(_source, nullptr);
        ASSERT_NE(_target, nullptr);
        _source->Label.setValue("Source-before");
        _target->Label.setValue("Target-before");
        Gui::Test::recomputeWithoutBlockingGui(
            *_document, "gui-responsiveness-setup-recompute");
        _guiDocument = Gui::Application::Instance->getDocument(_document);
        ASSERT_NE(_guiDocument, nullptr);
        _session = _document->collaborationService().beginEditSession("gui-actor");
    }

    void TearDown() override
    {
        if (_document && App::GetApplication().getDocument(_documentName.c_str())) {
            App::GetApplication().closeDocument(_documentName.c_str());
            QApplication::processEvents();
        }
    }

    LivePresentationProbe* addLivePresentationProbe()
    {
        // DocumentObject has no default GUI provider, so this document can
        // take ownership of the probe without replacing an existing provider.
        auto* object = _document->addObject("App::DocumentObject", "RefreshProbe");
        if (!object) {
            return nullptr;
        }
        auto provider = std::make_unique<LivePresentationProbe>();
        provider->attach(object);
        _guiDocument->addViewProvider(provider.get());
        auto* result = provider.release();  // The GUI document owns the provider.
        Gui::Test::recomputeWithoutBlockingGui(*_document, "presentation-probe-setup");
        QApplication::processEvents();
        return result;
    }

    App::Document* _document {nullptr};
    App::DocumentObject* _source {nullptr};
    App::DocumentObject* _target {nullptr};
    Gui::Document* _guiDocument {nullptr};
    App::EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

TEST_F(CollaborationResponsivenessTest, recomputeCommandPreservesRequestForce)
{
    for (const bool force : {true, false}) {
        SCOPED_TRACE(force);
        Gui::DocumentRecomputeSubmitRequest request;
        request.force = force;
        const auto command = Gui::makeDocumentRecomputeCommand(*_document, request);
        ASSERT_TRUE(command.recompute.has_value());
        EXPECT_EQ(command.recompute->force, request.force);
    }
}

TEST_F(CollaborationResponsivenessTest, guiIngressRecomputesUntouchedFeatureOnlyWhenForced)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(_source);
    ASSERT_NE(feature, nullptr);
    auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);

    // Check normal admission both before and after the forced execution.
    for (const bool force : {false, true, false}) {
        SCOPED_TRACE(force);
        ASSERT_FALSE(feature->isTouched());
        ASSERT_EQ(feature->mustRecompute(), 0);
        const auto executionsBefore = feature->ExecCount.getValue();
        Gui::DocumentRecomputeSubmitRequest request;
        request.featureIds = {feature->getNameInDocument()};
        request.force = force;
        const auto outcome = Gui::submitDocumentRecompute(*_document, request);
        ASSERT_TRUE(outcome.accepted()) << outcome.diagnostic;
        App::DocumentCommandHandle handle(
            outcome.commandId, _document->executionHandle().identity());
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        auto snapshot = handle.status();
        while ((!snapshot.terminal() || !lane->isIdle())
               && std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            std::this_thread::sleep_for(1ms);
            snapshot = handle.status();
        }
        ASSERT_TRUE(snapshot.terminal()) << "GUI recompute did not finish before timeout";
        ASSERT_TRUE(lane->isIdle());
        ASSERT_EQ(snapshot.state, App::DocumentCommandState::Completed)
            << snapshot.diagnostic;
        EXPECT_EQ(feature->ExecCount.getValue(), executionsBefore + (force ? 1 : 0));
    }
}

TEST_F(CollaborationResponsivenessTest,
       detachedPreparationKeepsEventLoopAndCameraResponsiveWithoutStalingCommit)
{
    Gui::View3DInventor view(_guiDocument, nullptr);
    auto* viewer = view.getViewer();
    ASSERT_NE(viewer, nullptr);
    viewer->setAnimationEnabled(false);
    view.resize(240, 180);
    view.show();
    QApplication::processEvents();

    const auto initialOrientation = viewer->getCameraOrientation();
    auto targetOrientation = Gui::Camera::isometric();
    if (Gui::Camera::rotationsMatch(initialOrientation, targetOrientation)) {
        targetOrientation = Gui::Camera::front();
    }
    ASSERT_FALSE(Gui::Camera::rotationsMatch(initialOrientation, targetOrientation));

    GateScenario scenario;
    App::CollaborativeOperationIntent intent;
    intent.operationType = ResponsiveOperationType;
    intent.arguments = {{"scenario", scenario.token},
                        {"source", "Source"},
                        {"target", "Target"}};
    const auto executionId = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return _document->collaborationService().prepareEditAsync(
            _session.sessionId(),
            "gui-responsive-preparation",
            intent,
            "phase-3-gui-responsiveness-acceptance");
    });
    const bool preparationStarted = scenario.gate->waitUntilEntered();
    EXPECT_TRUE(preparationStarted);

    const auto wildcardKey = App::DocumentRevisionKey::unknownModelMutation();
    const auto wildcardBeforeCamera =
        _document->collaborationRevisions().current(wildcardKey);
    const auto documentDirtyBeforeCamera = _document->isTouched();
    const auto guiDirtyBeforeCamera = _guiDocument->isModified();
    bool timerFired = false;
    QTimer::singleShot(0, [&timerFired] { timerFired = true; });
    viewer->setCameraOrientation(targetOrientation);
    QApplication::processEvents();

    const auto runningStatus =
        _document->collaborationService().preparedEditStatus(executionId);
    EXPECT_TRUE(timerFired);
    EXPECT_TRUE(Gui::Camera::rotationsMatch(viewer->getCameraOrientation(), targetOrientation));
    ASSERT_TRUE(runningStatus.has_value());
    EXPECT_EQ(runningStatus->status, App::PreparedEditExecutionStatus::Running);
    EXPECT_EQ(_document->collaborationRevisions().current(wildcardKey),
              wildcardBeforeCamera);
    EXPECT_EQ(_document->isTouched(), documentDirtyBeforeCamera);
    EXPECT_EQ(_guiDocument->isModified(), guiDirtyBeforeCamera);

    scenario.gate->release();
    ASSERT_TRUE(waitForTerminal(_document->collaborationService(), executionId).has_value());
    auto prepared = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return _document->collaborationService().takePreparedEdit(
            _session.sessionId(), executionId);
    });
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(prepared->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_NE(prepared->preparedEdit, nullptr);

    const auto commit = Gui::Test::invokeOnOwnerWorkerWhilePumpingGui([&] {
        return _document->collaborationService().commitEdit(
            _session.sessionId(), *prepared->preparedEdit);
    });
    EXPECT_TRUE(commit.committed());
    EXPECT_EQ(_target->Label.getStrValue(), "Source-before/detached");
    EXPECT_TRUE(Gui::Camera::rotationsMatch(viewer->getCameraOrientation(), targetOrientation));
}

TEST_F(CollaborationResponsivenessTest, admissionDefersLivePresentationUpdates)
{
    QApplication::processEvents();
    ASSERT_TRUE(App::MainThreadSignalConfig::isMainThread());
    ASSERT_FALSE(Gui::documentExecutionLaneBusy(*_document));
    ASSERT_FALSE(_guiDocument->deferLivePresentationUpdates());

    App::beginAtomicPresentationMutationTarget(*_document);
    EXPECT_TRUE(_guiDocument->deferLivePresentationUpdates());
    App::endAtomicPresentationMutationTarget(*_document);
    EXPECT_FALSE(_guiDocument->deferLivePresentationUpdates());
}

TEST_F(CollaborationResponsivenessTest, idleLiveCatchUpDoesNotProbePendingRecompute)
{
    auto* provider = addLivePresentationProbe();
    ASSERT_NE(provider, nullptr);
    auto* pending = new PendingRecomputeProbe;
    _document->addObject(pending, "PendingRecomputeProbe");
    pending->purgeTouched();
    ASSERT_FALSE(Gui::documentExecutionLaneBusy(*_document));
    ASSERT_FALSE(_guiDocument->deferLivePresentationUpdates());

    int refreshCalls = 0;
    provider->refresh = [&] { ++refreshCalls; };
    const auto clearProbe = qScopeGuard([&] { provider->refresh = {}; });
    pending->probes = 0;

    // Document::mustExecute() visits the process-wide XLink registry before
    // reaching this virtual probe, even though this document has no XLinks.
    // An idle GUI refresh must neither inspect pending recompute nor wait for
    // dirty features: the lane, replay and presentation admission guard work
    // that is actually executing. Foreign close/unlink need not use that guard.
    EXPECT_NO_THROW(_guiDocument->catchUpIdleLivePresentation());
    EXPECT_EQ(pending->probes, 0u);
    EXPECT_EQ(refreshCalls, 1);
}

TEST_F(CollaborationResponsivenessTest, idleRefreshActionDoesNotProbePendingRecompute)
{
    Gui::Application::Instance->setActiveDocument(_guiDocument);
    ASSERT_EQ(Gui::Application::Instance->activeDocument(), _guiDocument);
    auto* pending = new PendingRecomputeProbe;
    _document->addObject(pending, "PendingRecomputeProbe");
    pending->purgeTouched();
    auto* command = Gui::Application::Instance->commandManager().getCommandByName("Std_Refresh");
    ASSERT_NE(command, nullptr);
    auto* importCommand =
        Gui::Application::Instance->commandManager().getCommandByName("Std_LinkImportAll");
    ASSERT_NE(importCommand, nullptr);
    ASSERT_FALSE(Gui::documentExecutionLaneBusy(*_document));
    pending->probes = 0;

    // MainWindow's action timer also used Document::mustExecute(), reaching
    // foreign XLinks even when the active document has none.
    EXPECT_TRUE(command->isActive());
    EXPECT_TRUE(importCommand->isActive());  // No global probe for XLink presence either.
    EXPECT_EQ(pending->probes, 0u);

    auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);
    PreparationGate gate;
    auto work = lane->postToOwner([&] {
        lane->beginRecoverySnapshotOwnerWork();
        const auto endWork = qScopeGuard([&] { lane->endRecoverySnapshotOwnerWork(); });
        return gate.enter({});
    });
    const auto release = qScopeGuard([&] {
        gate.release();
        work.wait();
    });
    ASSERT_TRUE(gate.waitUntilEntered());
    EXPECT_FALSE(command->isActive());
    EXPECT_FALSE(importCommand->isActive());
    EXPECT_EQ(pending->probes, 0u);

    gate.release();
    work.wait();
    {
        App::beginAtomicPresentationMutationTarget(*_document);
        const auto endAdmission = qScopeGuard([&] {
            App::endAtomicPresentationMutationTarget(*_document);
        });
        EXPECT_FALSE(command->isActive());
        EXPECT_FALSE(importCommand->isActive());
        EXPECT_EQ(pending->probes, 0u);
    }
    EXPECT_TRUE(command->isActive());
    EXPECT_TRUE(importCommand->isActive());
}

TEST_F(CollaborationResponsivenessTest,
       livePresentationRefreshKeepsNewOwnerAdmissionOutUntilRefreshEnds)
{
    auto* provider = addLivePresentationProbe();
    ASSERT_NE(provider, nullptr);
    ASSERT_FALSE(_guiDocument->deferLivePresentationUpdates());

    std::atomic_bool refreshing {false};
    OwnerAdmissionAttempt attempt(*_document, refreshing);
    bool attemptReturned = false;
    bool appWriteRejected = false;
    int refreshCalls = 0;
    provider->refresh = [&] {
        ++refreshCalls;
        refreshing.store(true, std::memory_order_release);
        const auto release = qScopeGuard([&] {
            refreshing.store(false, std::memory_order_release);
            attempt.release();
        });
        // updateData is reached strictly after catch-up's idle/deferral
        // checks. Admit the owner here to reproduce the former check/use gap.
        attemptReturned = attempt.startAndWait();
        try {
            provider->getObject()->Label.setValue("Refreshed after admission check");
        }
        catch (const Base::RuntimeError&) {
            appWriteRejected = true;
        }
    };
    const auto clearProbe = qScopeGuard([&] { provider->refresh = {}; });

    EXPECT_NO_THROW(_guiDocument->catchUpIdleLivePresentation());
    attempt.releaseAndJoin();
    ASSERT_TRUE(attemptReturned) << "owner admission did not return before the watchdog";
    ASSERT_EQ(attempt.failure, nullptr);
    EXPECT_EQ(refreshCalls, 1);
    EXPECT_FALSE(attempt.acceptedDuringRefresh)
        << "owner admission started after the deferral check but before refresh finished";
    EXPECT_FALSE(appWriteRejected)
        << "a live presentation App write escaped to a foreign atomic admission";
    EXPECT_EQ(provider->getObject()->Label.getStrValue(),
              "Refreshed after admission check");

    // Refusing the overlapping admission is temporary: the legitimate owner
    // can rejoin once the GUI refresh has returned.
    EXPECT_NO_THROW(Gui::Test::runOnDocumentOwnerWhilePumpingGui(*_document, [&] {
        App::beginAtomicPresentationMutationTarget(*_document);
        const auto endAdmission = qScopeGuard([&] {
            App::endAtomicPresentationMutationTarget(*_document);
        });
        App::enforceAtomicPresentationMutationTarget(*_document);
    }));
}

TEST_F(CollaborationResponsivenessTest,
       treeStatusRefreshKeepsNewOwnerAdmissionOutUntilRefreshEnds)
{
    Gui::TreeWidget tree("tree-status-admission-probe");
    App::DocumentInitFlags flags;
    flags.createView = false;
    const auto name = App::GetApplication().getUniqueDocumentName("treeRefresh");
    auto* document = App::GetApplication().newDocument(name.c_str(), "tree refresh", flags);
    const auto close = qScopeGuard([&] { App::GetApplication().closeDocument(name.c_str()); });
    auto* gui = Gui::Application::Instance->getDocument(document);
    ASSERT_NE(gui, nullptr);
    auto* object = document->addObject("App::DocumentObject", "RefreshProbe");
    auto ownedProvider = std::make_unique<LivePresentationProbe>();
    ownedProvider->attach(object);
    gui->addViewProvider(ownedProvider.get());
    auto* provider = ownedProvider.release();
    gui->signalNewObject(*provider);
    Gui::Test::recomputeWithoutBlockingGui(*document, "tree-refresh-setup");
    Gui::TreeWidget::updateStatus(false);
    QApplication::processEvents();

    std::atomic_bool refreshing {true};
    OwnerAdmissionAttempt attempt(*document, refreshing);
    bool attemptReturned = false;
    bool appWriteRejected = false;
    int probeCalls = 0;
    const auto connection = QObject::connect(
        &tree, &QTreeWidget::itemChanged,
        [&](QTreeWidgetItem* item, int) {
            if (probeCalls || item->text(0) != QStringLiteral("RefreshProbe")) {
                return;
            }
            ++probeCalls;
            const auto release = qScopeGuard([&] { attempt.release(); });
            // Icon/status changes run after the tree's idle checks. Admission
            // here recreates the live source-close/delete check/use race.
            attemptReturned = attempt.startAndWait();
            try {
                provider->getObject()->Label.setValue("Tree-refreshed");
            }
            catch (const Base::RuntimeError&) {
                appWriteRejected = true;
            }
        });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
    provider->getObject()->touch();
    EXPECT_NO_THROW(QMetaObject::invokeMethod(&tree, "onUpdateStatus", Qt::DirectConnection));
    attempt.releaseAndJoin();
    ASSERT_EQ(probeCalls, 1);
    ASSERT_TRUE(attemptReturned);
    ASSERT_EQ(attempt.failure, nullptr);
    EXPECT_FALSE(attempt.acceptedDuringRefresh);
    EXPECT_FALSE(appWriteRejected);
    EXPECT_EQ(provider->getObject()->Label.getStrValue(), "Tree-refreshed");
}

TEST_F(CollaborationResponsivenessTest,
       changedObjectRefreshKeepsNewOwnerAdmissionOutUntilRefreshEnds)
{
    auto* provider = addLivePresentationProbe();
    ASSERT_NE(provider, nullptr);
    ASSERT_FALSE(_guiDocument->deferLivePresentationUpdates());

    std::atomic_bool refreshing {false};
    OwnerAdmissionAttempt attempt(*_document, refreshing);
    bool attemptReturned = false;
    bool appWriteRejected = false;
    int refreshCalls = 0;
    provider->refresh = [&] {
        ++refreshCalls;
        refreshing.store(true, std::memory_order_release);
        const auto release = qScopeGuard([&] {
            refreshing.store(false, std::memory_order_release);
            attempt.release();
        });
        attemptReturned = attempt.startAndWait();
        try {
            provider->getObject()->Label.setValue("Refreshed after changed-object check");
        }
        catch (const Base::RuntimeError&) {
            appWriteRejected = true;
        }
    };
    const auto clearProbe = qScopeGuard([&] { provider->refresh = {}; });

    // Exercise the existing signal connection to slotChangedObject directly;
    // no pending App mutation or idle catch-up can supply the reservation.
    EXPECT_NO_THROW(_document->signalChangedObject(*provider->getObject(),
                                                  provider->getObject()->Label));
    attempt.releaseAndJoin();
    ASSERT_TRUE(attemptReturned) << "owner admission did not return before the watchdog";
    ASSERT_EQ(attempt.failure, nullptr);
    EXPECT_EQ(refreshCalls, 1);
    EXPECT_FALSE(attempt.acceptedDuringRefresh);
    EXPECT_FALSE(appWriteRejected);
    EXPECT_EQ(provider->getObject()->Label.getStrValue(),
              "Refreshed after changed-object check");

    EXPECT_NO_THROW(Gui::Test::runOnDocumentOwnerWhilePumpingGui(*_document, [&] {
        App::beginAtomicPresentationMutationTarget(*_document);
        const auto endAdmission = qScopeGuard([&] {
            App::endAtomicPresentationMutationTarget(*_document);
        });
        App::enforceAtomicPresentationMutationTarget(*_document);
    }));
}

TEST_F(CollaborationResponsivenessTest,
       deferredLivePresentationRefreshResumesAfterOwnerAdmissionEnds)
{
    auto* provider = addLivePresentationProbe();
    ASSERT_NE(provider, nullptr);
    std::atomic_bool refreshing {false};
    OwnerAdmissionAttempt attempt(*_document, refreshing);
    int refreshCalls = 0;
    provider->refresh = [&] {
        ++refreshCalls;
        provider->getObject()->Label.setValue("Refreshed after owner release");
    };
    const auto clearProbe = qScopeGuard([&] { provider->refresh = {}; });

    const bool attemptReturned = attempt.startAndWait();
    if (attemptReturned && !attempt.failure) {
        EXPECT_TRUE(attempt.accepted);
        EXPECT_NO_THROW(_guiDocument->catchUpIdleLivePresentation());
        EXPECT_EQ(refreshCalls, 0);
    }
    attempt.releaseAndJoin();
    ASSERT_TRUE(attemptReturned) << "owner admission did not return before the watchdog";
    ASSERT_EQ(attempt.failure, nullptr);
    ASSERT_TRUE(attempt.accepted);

    EXPECT_NO_THROW(_guiDocument->catchUpIdleLivePresentation());
    QApplication::processEvents();
    EXPECT_EQ(refreshCalls, 1);
    EXPECT_EQ(provider->getObject()->Label.getStrValue(), "Refreshed after owner release");
    EXPECT_FALSE(App::atomicPresentationMutationAdmissionActive());
}

TEST_F(CollaborationResponsivenessTest,
       presentationCallbackWaitsForIdleLaneEvenWhenModelIsReady)
{
    QApplication::processEvents();
    auto* lane = _document->executionLane();
    ASSERT_NE(lane, nullptr);
    PreparationGate gate;
    auto work = lane->postToOwner([&] {
        lane->beginRecoverySnapshotOwnerWork();
        const auto endWork = qScopeGuard([&] { lane->endRecoverySnapshotOwnerWork(); });
        return gate.enter({});
    });
    const auto release = qScopeGuard([&] {
        gate.release();
        work.wait();
    });
    ASSERT_TRUE(gate.waitUntilEntered());
    ASSERT_TRUE(_document->getMutationReadiness().ready);
    ASSERT_FALSE(lane->isIdle());
    ASSERT_TRUE(_guiDocument->deferLivePresentationUpdates());

    Base::PyGILStateLocker gil;
    PyObject* globals = PyDict_New();
    ASSERT_NE(globals, nullptr);
    const auto releaseGlobals = qScopeGuard([&] { Py_DECREF(globals); });
    ASSERT_EQ(PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()), 0);
    PyObject* calls = PyList_New(0);
    ASSERT_NE(calls, nullptr);
    const auto releaseCalls = qScopeGuard([&] { Py_DECREF(calls); });
    ASSERT_EQ(PyDict_SetItemString(globals, "calls", calls), 0);
    PyObject* callback = PyRun_String("lambda: calls.append(1)", Py_eval_input, globals, globals);
    ASSERT_NE(callback, nullptr);
    const auto releaseCallback = qScopeGuard([&] { Py_DECREF(callback); });
    PyObject* document = _document->getPyObject();
    const auto releaseDocument = qScopeGuard([&] { Py_DECREF(document); });

    auto invoke = [&](bool targeted) {
        PyObject* args = targeted ? PyTuple_Pack(2, callback, document)
                                  : PyTuple_Pack(1, callback);
        PyObject* result = Gui::ApplicationPy::sTryRunPresentationCallback(nullptr, args);
        Py_DECREF(args);
        if (!result) {
            PyErr_Print();
            return -1;
        }
        const int truth = PyObject_IsTrue(result);
        Py_DECREF(result);
        return truth;
    };

    // Snapshot owner work holds no mutation admission, so the old global-only
    // reservation admits it. The target lane must still reject presentation.
    EXPECT_EQ(invoke(false), 1);
    EXPECT_EQ(PyList_Size(calls), 1);
    EXPECT_EQ(invoke(true), 0);
    EXPECT_EQ(PyList_Size(calls), 1);

    gate.release();
    ASSERT_EQ(work.wait_for(5s), std::future_status::ready);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!lane->isIdle() && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::yield();
    }
    ASSERT_TRUE(lane->isIdle());
    EXPECT_EQ(invoke(true), 1);
    EXPECT_EQ(PyList_Size(calls), 2);
}
