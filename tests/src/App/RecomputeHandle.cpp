// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/FeatureTest.h>
#include <App/RecomputeHandle.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <src/App/InitApplication.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>

namespace
{

using namespace std::chrono_literals;

constexpr std::string_view BlockingRecomputeOperationType =
    "FreeCAD.Tests.BlockingRecomputeHandle";

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

void ensureBlockingRecomputeAdapterRegistered()
{
    static std::once_flag registered;
    std::call_once(registered, [] {
        static_cast<void>(App::Internal::CollaborativeOperationRegistrar::registerAdapter(
            std::string(BlockingRecomputeOperationType),
            [](const App::Document&,
               const App::CollaborativeOperationIntent& intent) {
                if (intent.arguments.size() != 1 || !intent.arguments.contains("token")) {
                    throw std::invalid_argument("invalid blocking recompute intent");
                }
                auto state = BlockingRecomputeStore::get(intent.arguments.at("token"));
                if (!state) {
                    throw std::invalid_argument("unknown blocking recompute state");
                }
                App::CollaborativeOperationPreparation::DetachedTask task =
                    [state = std::move(state)](const std::stop_token stopToken) {
                        if (!state->block(stopToken)) {
                            throw std::runtime_error("blocking recompute was cancelled");
                        }
                        return std::make_unique<const NoopRecomputeOperation>();
                    };
                return App::CollaborativeOperationPreparation {
                    {},
                    {},
                    {},
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
        if (!_savePath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(_savePath, ignored);
        }
    }

    std::unique_ptr<App::RecomputeHandle> blockingHandle()
    {
        App::DocumentRecomputeFeatureRequest feature;
        feature.featureId = "blocking-probe";
        feature.operationId = "blocking-recompute-handle";
        feature.intent.operationType = std::string(BlockingRecomputeOperationType);
        feature.intent.arguments = {{"token", _blockingToken}};
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
    std::filesystem::path _savePath;
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
