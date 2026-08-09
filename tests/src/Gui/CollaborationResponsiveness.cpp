// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <QApplication>
#include <QTimer>

#include <App/Application.h>
#include <App/CollaborativeOperationRegistry.h>
#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentObject.h>
#include <App/DocumentRevisionIndex.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <Gui/Application.h>
#include <Gui/Camera.h>
#include <Gui/Document.h>
#include <Gui/MainWindow.h>
#include <Gui/View3DInventor.h>
#include <Gui/View3DInventorViewer.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
        _document->recompute();
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

    App::Document* _document {nullptr};
    App::DocumentObject* _source {nullptr};
    App::DocumentObject* _target {nullptr};
    Gui::Document* _guiDocument {nullptr};
    App::EditSession _session {"placeholder", "placeholder", 1};
    std::string _documentName;
};

}  // namespace

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
    const auto executionId = _document->collaborationService().prepareEditAsync(
        _session.sessionId(),
        "gui-responsive-preparation",
        intent,
        "phase-3-gui-responsiveness-acceptance");
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
    auto prepared = _document->collaborationService().takePreparedEdit(
        _session.sessionId(), executionId);
    ASSERT_TRUE(prepared.has_value());
    ASSERT_EQ(prepared->status, App::PreparedEditExecutionStatus::Completed);
    ASSERT_NE(prepared->preparedEdit, nullptr);

    const auto commit = _document->collaborationService().commitEdit(
        _session.sessionId(), *prepared->preparedEdit);
    EXPECT_TRUE(commit.committed());
    EXPECT_EQ(_target->Label.getStrValue(), "Source-before/detached");
    EXPECT_TRUE(Gui::Camera::rotationsMatch(viewer->getCameraOrientation(), targetOrientation));
}
