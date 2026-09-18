// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/CollaborativeOperation.h>
#include <App/Document.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/FeatureTest.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>
#include <App/RecomputeHandle.h>
#include <App/Transactions.h>
#include <App/private/CollaborativeOperationRegistryInternal.h>
#include <App/private/DocumentP.h>
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
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace App::Internal
{

/** Exposes the id of the document's currently open (but not yet committed)
 * undo transaction, purely so tests can assert fix 1's invariant that a live
 * transaction always carries the caller's booked id. Returns 0 when no
 * transaction is currently open. */
class NestedCommitBookingTestAccess
{
public:
    static int activeTransactionId(const App::Document& document) noexcept
    {
        return document.d->activeUndoTransaction ? document.d->activeUndoTransaction->getID()
                                                  : 0;
    }
};

}  // namespace App::Internal

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
        if (!_dependencyPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(_dependencyPath, ignored);
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
       ownerThreadVenueDeclaresItselfWhileTheIsolatedVenueKeepsAShortWaitBounded)
{
    auto* feature = _document->addObject<App::FeatureTest>("VenueProbe");
    ASSERT_NE(feature, nullptr);
    feature->touch();

    // Default venue. The plan says up front that this node's execute() runs
    // live on whichever thread polls the handle, and nothing runs until one
    // does: the count staying at zero across the submit is what distinguishes
    // owner-thread execution from work already dispatched elsewhere.
    auto owner = _document->recomputeAsync({feature});
    EXPECT_EQ(feature->ExecCount.getValue(), 0)
        << "owner-thread work must not begin before the handle is polled";
    const auto declared = owner->wait(120s);
    ASSERT_TRUE(declared.terminal()) << declared.diagnostic;
    ASSERT_EQ(declared.features.size(), 1U);
    EXPECT_TRUE(declared.features.front().ownerThreadExecution);
    EXPECT_EQ(declared.ownerThreadFeatures, 1U);
    EXPECT_EQ(feature->ExecCount.getValue(), 1);  // ran on the polling thread

    // Isolated venue. The same worker-capable feature does not run here: a
    // zero timeout comes back with the node still running and ExecCount
    // untouched, which is only possible if execute() happened elsewhere.
    feature->touch();
    auto isolated = _document->recomputeAsync(
        {feature}, false, 0, App::RecomputeVenue::Isolated);
    const auto started = std::chrono::steady_clock::now();
    const auto pending = isolated->wait(0ms);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    ASSERT_EQ(pending.features.size(), 1U);
    EXPECT_FALSE(pending.features.front().ownerThreadExecution);
    EXPECT_EQ(pending.ownerThreadFeatures, 0U);
    EXPECT_FALSE(pending.terminal());
    EXPECT_EQ(feature->ExecCount.getValue(), 1);

    const auto done = isolated->wait(120s);
    ASSERT_TRUE(done.terminal()) << done.diagnostic;
    EXPECT_EQ(done.state, App::DocumentRecomputeState::Completed) << done.diagnostic;
    EXPECT_EQ(feature->ExecCount.getValue(), 2);  // came back from the worker
}

TEST_F(RecomputeHandleTest, isolatedVenueStillFallsBackForAnUnoptedFeature)
{
    auto* blocker = _document->addObject<App::FeatureTestAsyncBlocker>("Unopted");
    ASSERT_NE(blocker, nullptr);
    ASSERT_FALSE(blocker->canRecomputeOnWorker());
    blocker->touch();
    App::FeatureTestAsyncBlocker::resetBlocker();
    App::FeatureTestAsyncBlocker::releaseBlocker();

    auto handle = _document->recomputeAsync(
        {blocker}, false, 0, App::RecomputeVenue::Isolated);
    const auto snapshot = handle->wait(60s);
    ASSERT_TRUE(snapshot.terminal()) << snapshot.diagnostic;
    // Asked for isolated, got the owner thread, and said so.
    ASSERT_EQ(snapshot.features.size(), 1U);
    EXPECT_TRUE(snapshot.features.front().ownerThreadExecution);
    EXPECT_EQ(snapshot.state, App::DocumentRecomputeState::Completed)
        << snapshot.diagnostic;
    EXPECT_FALSE(blocker->mustRecompute());
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

TEST_F(RecomputeHandleTest, syncRecomputeInsideACallerTransactionUndoesItsDerivedOutput)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("NestedRecomputeColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    ASSERT_EQ(_document->recompute({feature}), 1);
    const int valueForB = feature->Value.getValue();

    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    _document->openTransaction("edit column");
    feature->Column.setValue("E");
    // The recompute below runs a nested commit inside this still-open caller
    // transaction, so its Value write must fold into the caller's undo step
    // rather than land in a transaction of its own that gets discarded.
    ASSERT_EQ(_document->recompute({feature}), 1);
    _document->commitTransaction();
    ASSERT_EQ(_document->getAvailableUndos(), 1);

    ASSERT_TRUE(_document->undo());
    EXPECT_STREQ(feature->Column.getValue(), "B");
    EXPECT_EQ(feature->Value.getValue(), valueForB);

    ASSERT_TRUE(_document->redo());
    EXPECT_STREQ(feature->Column.getValue(), "E");
}

TEST_F(RecomputeHandleTest, bookingOnlyTransactionAdoptsTheRecomputesUndoStepOnAbort)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("BookingOnlyAbortColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    ASSERT_EQ(_document->recompute({feature}), 1);
    const int valueForB = feature->Value.getValue();

    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    // Changing the input before opening a transaction leaves the write
    // untracked, so the transaction below only ever gets a booking, never a
    // live App::Transaction, until the recompute's own nested commit opens
    // one.
    feature->Column.setValue("E");
    const int booked = _document->openTransaction("booking only recompute (abort)");
    ASSERT_GT(booked, 0);
    ASSERT_EQ(_document->getBookedTransactionID(), booked);
    ASSERT_EQ(App::Internal::NestedCommitBookingTestAccess::activeTransactionId(*_document), 0);

    // The recompute below opens its own nested transaction because the
    // caller only has a booking: parkTransactionForNestedCommit() has
    // nothing to park but the booking, so the nested transaction must be
    // minted under `booked` and adopted as the caller's own.
    ASSERT_EQ(_document->recompute({feature}), 1);

    EXPECT_EQ(_document->getBookedTransactionID(), booked);
    EXPECT_EQ(App::Internal::NestedCommitBookingTestAccess::activeTransactionId(*_document),
              booked);

    _document->abortTransaction();
    EXPECT_EQ(feature->Value.getValue(), valueForB);
    EXPECT_EQ(_document->getAvailableUndos(), 0);
}

TEST_F(RecomputeHandleTest, bookingOnlyTransactionAdoptsTheRecomputesUndoStepOnCommit)
{
    auto* feature = _document->addObject<App::FeatureTestColumn>("BookingOnlyCommitColumn");
    ASSERT_NE(feature, nullptr);
    feature->Column.setValue("B");
    ASSERT_EQ(_document->recompute({feature}), 1);
    const int valueForB = feature->Value.getValue();

    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    feature->Column.setValue("E");
    const int booked = _document->openTransaction("booking only recompute (commit)");
    ASSERT_GT(booked, 0);
    ASSERT_EQ(_document->getBookedTransactionID(), booked);

    ASSERT_EQ(_document->recompute({feature}), 1);

    EXPECT_EQ(_document->getBookedTransactionID(), booked);
    EXPECT_EQ(App::Internal::NestedCommitBookingTestAccess::activeTransactionId(*_document),
              booked);

    _document->commitTransaction();
    ASSERT_EQ(_document->getAvailableUndos(), 1);

    ASSERT_TRUE(_document->undo(booked));
    EXPECT_EQ(feature->Value.getValue(), valueForB);
}

TEST_F(RecomputeHandleTest, emptyRecomputeInsideABookingOnlyTransactionRecordsNoUndo)
{
    auto* storage = _document->addObject<App::DocumentObject>("BookingOnlyEmptyStorage");
    ASSERT_NE(storage, nullptr);
    auto* value = dynamic_cast<App::PropertyInteger*>(storage->addDynamicProperty(
        "App::PropertyInteger", "Value", "Data", "", App::Prop_NoRecompute));
    ASSERT_NE(value, nullptr);
    // Untracked: no transaction is open yet, so this write is not undoable
    // and merely leaves `storage` touched without recording anything.
    value->setValue(7);
    ASSERT_TRUE(storage->isTouched());
    ASSERT_EQ(storage->mustRecompute(), 0);

    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    const int booked = _document->openTransaction("booking only, nothing to record");
    ASSERT_GT(booked, 0);
    ASSERT_EQ(_document->getBookedTransactionID(), booked);
    ASSERT_EQ(App::Internal::NestedCommitBookingTestAccess::activeTransactionId(*_document), 0);

    // A Prop_NoRecompute-only change touches an object without ever calling
    // execute() on it (see
    // fullDocumentFacadeDoesNotExecuteTouchedNoRecomputeStorageObject above),
    // so the recompute below settles it through a nested commit that records
    // no properties at all. Fix 1(ii)'s empty-transaction retirement must
    // leave the caller with the bare booking rather than an empty undo entry.
    EXPECT_EQ(_document->recompute(), 0);

    EXPECT_EQ(_document->getBookedTransactionID(), booked);
    EXPECT_EQ(App::Internal::NestedCommitBookingTestAccess::activeTransactionId(*_document), 0);

    _document->commitTransaction();
    EXPECT_EQ(_document->getAvailableUndos(), 0);
}

TEST_F(RecomputeHandleTest, mergingANestedRemovalIntoAChangeRecordDoesNotLeakTheObject)
{
    auto* object = _document->addObject<App::FeatureTest>("NestedRemovalTarget");
    ASSERT_NE(object, nullptr);
    const std::string objectName = object->getNameInDocument();
    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    const int booked = _document->openTransaction("edit then nested removal");
    ASSERT_GT(booked, 0);
    object->Label.setValue("edited before nested removal");
    // The caller's transaction now holds a Chn record for `object`.

    // Simulate a recompute's nested commit removing the object the caller
    // just edited: park the caller's transaction, do the removal in a
    // transaction of its own, and fold it back exactly as
    // DocumentCommitCoordinator's ParkedTransactionGuard does around a real
    // recompute commit.
    ASSERT_TRUE(_document->parkTransactionForNestedCommit());
    _document->openTransaction("nested removal");
    _document->removeObject(objectName.c_str());
    // The nested transaction now holds a New record for `object` (removed
    // objects are tracked as status New; see Transaction::addObjectNew()):
    // mergeInto() has to reconcile it against the parent's Chn record rather
    // than just dropping the nested record, or the detached object leaks.
    _document->commitTransaction();
    _document->restoreParkedTransactionAfterNestedCommit();

    // The caller's own transaction is live again, carrying the merged
    // record; committing it is the caller's own responsibility, same as for
    // an ordinary edit.
    _document->commitTransaction();

    ASSERT_EQ(_document->getAvailableUndos(), 1);
    ASSERT_TRUE(_document->undo(booked));
    EXPECT_EQ(_document->getObject(objectName.c_str()), object);
    // Undoing the caller's step reverts everything it contained -- both the
    // label edit and the nested removal -- so the label returns to what it
    // was before the transaction opened, not to its mid-transaction value.
    EXPECT_STREQ(object->Label.getValue(), "NestedRemovalTarget");
}

TEST_F(RecomputeHandleTest, mergingANestedRemovalOfANewlyAddedObjectDestroysItWithoutLeaking)
{
    _document->setMaxUndoStackSize(20);
    _document->clearUndos();

    const int booked = _document->openTransaction("add then nested removal");
    ASSERT_GT(booked, 0);
    auto* object = _document->addObject<App::FeatureTest>("DelNewCancelTarget");
    ASSERT_NE(object, nullptr);
    const std::string objectName = object->getNameInDocument();
    // The caller's transaction now holds a Del record for `object` (Del
    // means the object was added during this transaction; undo would
    // remove it).

    ASSERT_TRUE(_document->parkTransactionForNestedCommit());
    _document->openTransaction("nested removal of the new object");
    _document->removeObject(objectName.c_str());
    // The nested transaction holds a New record for the same object
    // (removed during this transaction). Added-then-removed cancels out:
    // the merged step must do neither, and since nothing else owns the
    // now-detached object, mergeInto() has to destroy it right there or it
    // leaks. `object` is dangling from this point on.
    _document->commitTransaction();
    _document->restoreParkedTransactionAfterNestedCommit();
    _document->commitTransaction();

    EXPECT_EQ(_document->getObject(objectName.c_str()), nullptr);
    if (_document->getAvailableUndos() > 0) {
        EXPECT_TRUE(_document->undo(booked));
    }
    EXPECT_EQ(_document->getObject(objectName.c_str()), nullptr);
}
