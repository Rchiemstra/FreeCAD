// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "App/Application.h"
#include "App/Document.h"
#include "App/DocumentCollaborationService.h"
#include "App/FeatureTest.h"
#include "App/MergeDocuments.h"
#include "App/StringHasher.h"
#include "App/Transactions.h"
#include "Base/Writer.h"
#include <src/App/InitApplication.h>

using ::testing::Eq;
using ::testing::Ne;

// NOLINTBEGIN(readability-magic-numbers)

class FakeWriter: public Base::Writer
{
    void writeFiles() override
    {}
    std::ostream& Stream() override
    {
        return std::cout;
    }
};

class DocumentTest: public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
        _keepDocumentAliveOnTimeout = false;
    }

    void TearDown() override
    {
        if (!_keepDocumentAliveOnTimeout) {
            App::GetApplication().closeDocument(_docName.c_str());
        }
    }

    App::Document* doc()
    {
        return _doc;
    }

    void keepDocumentAliveOnWorkerTimeout()
    {
        _keepDocumentAliveOnTimeout = true;
    }

private:
    std::string _docName;
    App::Document* _doc {};
    bool _keepDocumentAliveOnTimeout {false};
};


TEST_F(DocumentTest, addStringHasherIndicatesUnwrittenWhenNew)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);

    // Act
    auto addResult = doc()->addStringHasher(hasher);

    // Assert
    EXPECT_TRUE(addResult.first);
    EXPECT_THAT(addResult.second, Ne(-1));
}

TEST_F(DocumentTest, addStringHasherIndicatesAlreadyWritten)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);
    doc()->addStringHasher(hasher);

    // Act
    auto addResult = doc()->addStringHasher(hasher);

    // Assert
    EXPECT_FALSE(addResult.first);
}

TEST_F(DocumentTest, getStringHasherGivesExpectedHasher)
{
    // Arrange
    App::StringHasherRef hasher(new App::StringHasher);
    auto pair = doc()->addStringHasher(hasher);
    int index = pair.second;

    // Act
    auto foundHasher = doc()->getStringHasher(index);

    // Assert
    EXPECT_EQ(hasher, foundHasher);
}

TEST_F(DocumentTest, mergeIntoCombinesDifferentPropertiesOfTheSameObject)
{
    // Arrange
    auto* object = doc()->addObject<App::FeatureTest>("MergeDifferentProps");
    ASSERT_NE(object, nullptr);
    object->Integer.setValue(1);
    object->Float.setValue(1.0);

    App::Transaction parent;
    App::Transaction nested;
    // TransactionObject only snapshots a property the first time it sees it,
    // so each Transaction's snapshot is whatever the property held when that
    // Transaction first recorded it.
    parent.addObjectChange(object, &object->Integer);
    object->Integer.setValue(2);
    nested.addObjectChange(object, &object->Float);
    object->Float.setValue(2.0);

    // Act
    nested.mergeInto(parent);
    parent.apply(*doc(), false);

    // Assert: undoing the merged parent restores both properties, proving
    // the nested transaction's Float snapshot was not dropped when its
    // record for `object` was merged into the parent's.
    EXPECT_EQ(object->Integer.getValue(), 1);
    EXPECT_DOUBLE_EQ(object->Float.getValue(), 1.0);
}

TEST_F(DocumentTest, mergeIntoKeepsTheParentsSnapshotForADuplicateProperty)
{
    // Arrange
    auto* object = doc()->addObject<App::FeatureTest>("MergeDuplicateProp");
    ASSERT_NE(object, nullptr);
    object->Integer.setValue(1);

    App::Transaction parent;
    App::Transaction nested;
    parent.addObjectChange(object, &object->Integer);
    object->Integer.setValue(2);
    nested.addObjectChange(object, &object->Integer);
    object->Integer.setValue(3);

    // Act
    nested.mergeInto(parent);
    parent.apply(*doc(), false);

    // Assert: the parent's older snapshot (1) is the one undo restores, not
    // the nested transaction's newer duplicate (2).
    EXPECT_EQ(object->Integer.getValue(), 1);
}

TEST_F(DocumentTest, checkedRollbackRemovesAddedLockedProperty)
{
    auto* object = doc()->addObject<App::FeatureTest>("LockedProperty");
    auto* property = object->addDynamicProperty("App::PropertyLength", "DummyLength");
    ASSERT_NE(property, nullptr);
    property->setStatus(App::Property::LockDynamic, true);
    App::Transaction transaction;
    transaction.addOrRemoveProperty(object, property, true);
    transaction.addObjectChange(object, &object->Integer);
    const auto original = object->Integer.getValue();
    object->Integer.setValue(15);

    EXPECT_NO_THROW(transaction.applyChecked(*doc(), false));
    EXPECT_EQ(object->getDynamicPropertyByName("DummyLength"), nullptr);
    EXPECT_EQ(object->Integer.getValue(), original);
}

TEST_F(DocumentTest, checkedPropertyRemovalFailureRestoresDynamicLock)
{
    auto* object = doc()->addObject<App::FeatureTest>("LockedProperty");
    auto* property = object->addDynamicProperty("App::PropertyLength", "DummyLength");
    ASSERT_NE(property, nullptr);
    property->setStatus(App::Property::LockDynamic, true);
    App::Transaction transaction;
    transaction.addOrRemoveProperty(object, property, true);

    bool removalAttempted = false;
    auto connection = App::GetApplication().signalRemoveDynamicProperty.connect(
        [&](const App::Property& removed) {
            if (&removed == property) {
                removalAttempted = true;
                throw std::runtime_error("injected removal failure");
            }
        }
    );
    EXPECT_THROW(transaction.applyChecked(*doc(), false), std::runtime_error);
    connection.disconnect();

    EXPECT_TRUE(removalAttempted);
    ASSERT_EQ(object->getDynamicPropertyByName("DummyLength"), property);
    EXPECT_TRUE(property->testStatus(App::Property::LockDynamic));
    EXPECT_FALSE(object->removeDynamicProperty("DummyLength"));
}

TEST_F(DocumentTest, importObjectsRestoresSourceStringHasher)
{
    // Arrange
    auto& app = App::GetApplication();
    const std::string sourceName = app.getUniqueDocumentName("MergeSource");
    App::Document* source = app.newDocument(sourceName.c_str(), "testUser");
    App::StringHasherRef sourceHasher = source->getStringHasher();
    ASSERT_TRUE(sourceHasher);
    sourceHasher->setSaveAll(true);
    sourceHasher->getID("persisted element name");

    const std::string path = App::Application::getTempFileName();
    ASSERT_TRUE(source->saveAs(path.c_str()));
    const std::string savedPath = source->getFileName();
    app.closeDocument(sourceName.c_str());

    std::size_t restoredStringCount = 0;
    auto connection = doc()->signalFinishImportObjects.connect(
        [this, &restoredStringCount](const std::vector<App::DocumentObject*>&) {
            App::StringHasherRef importedHasher = doc()->getStringHasher(0);
            restoredStringCount = importedHasher ? importedHasher->size() : 0;
        }
    );

    // Act
    std::ifstream stream(savedPath, std::ios::in | std::ios::binary);
    ASSERT_TRUE(stream.is_open());
    App::MergeDocuments merge(doc());
    merge.importObjects(stream);

    // Assert
    EXPECT_GT(restoredStringCount, 0);
    connection.disconnect();
    std::remove(savedPath.c_str());
}

TEST_F(DocumentTest, getMutationReadinessSkipsGraphWalkWhileCommitMutexHeld)
{
    constexpr auto waitTimeout = std::chrono::seconds(30);
    auto* object = doc()->addObject<App::FeatureTest>("TouchedFeature");
    ASSERT_NE(object, nullptr);
    doc()->recompute();
    ASSERT_FALSE(doc()->mustExecute());

    struct SharedState
    {
        explicit SharedState(App::Document* document)
            : document(document)
        {}

        App::Document* document;
        std::mutex mutex;
        std::condition_variable cv;
        bool enteredCommit {false};
        bool releaseCommit {false};
        bool cancelled {false};
        bool readerFinished {false};
        bool readerTimedOut {false};
        bool commitTimedOut {false};
        bool observedWhileLocked {false};
        bool readerThrew {false};
        App::DocumentMutationReadiness whileLocked;
    };

    auto state = std::make_shared<SharedState>(doc());
    std::thread reader;
    bool readerCleanupDone = false;

    const auto finishReader = [this, state, &reader, &readerCleanupDone, waitTimeout]() -> bool {
        if (readerCleanupDone) {
            return !reader.joinable();
        }
        readerCleanupDone = true;
        bool completed = true;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cancelled = true;
            state->releaseCommit = true;
            state->cv.notify_all();
            if (reader.joinable()) {
                completed = state->cv.wait_for(lock, waitTimeout, [&] {
                    return state->readerFinished;
                });
            }
        }
        if (reader.joinable()) {
            if (completed) {
                reader.join();
            }
            else {
                keepDocumentAliveOnWorkerTimeout();
                reader.detach();
                ADD_FAILURE() << "Reader thread did not finish within timeout";
            }
        }
        return completed;
    };

    struct CleanupGuard
    {
        const decltype(finishReader)& finish;
        ~CleanupGuard()
        {
            finish();
        }
    } cleanupGuard {finishReader};

    reader = std::thread([state, waitTimeout] {
        struct ExitSignal
        {
            std::shared_ptr<SharedState> shared;
            ~ExitSignal()
            {
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->releaseCommit = true;
                shared->readerFinished = true;
                shared->cv.notify_all();
            }
        } exitSignal {state};

        try {
            bool shouldRead = false;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                const bool ready = state->cv.wait_for(lock, waitTimeout, [&] {
                    return state->enteredCommit || state->cancelled;
                });
                if (!ready) {
                    state->readerTimedOut = true;
                    return;
                }
                shouldRead = state->enteredCommit && !state->cancelled;
            }
            if (shouldRead) {
                const auto readiness = state->document->getMutationReadiness();
                std::lock_guard<std::mutex> lock(state->mutex);
                state->whileLocked = readiness;
                state->observedWhileLocked = true;
            }
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->readerThrew = true;
        }
    });

    App::CollaborationCompatibilityMutation mutation;
    mutation.scope = App::CollaborationCompatibilityScope::UnknownModel;
    const auto commitResult = doc()->collaborationService().commitCompatibilityMutationWithPolicy(
        std::move(mutation),
        [object, state, waitTimeout] {
            object->touch();
            std::unique_lock<std::mutex> lock(state->mutex);
            state->enteredCommit = true;
            state->cv.notify_all();
            const bool released = state->cv.wait_for(lock, waitTimeout, [&] {
                return state->releaseCommit || state->cancelled;
            });
            if (!released) {
                state->commitTimedOut = true;
            }
        },
        App::CollaborationCompatibilityRecomputePolicy::Deferred
    );

    const bool readerCompleted = finishReader();
    ASSERT_TRUE(readerCompleted);
    EXPECT_FALSE(state->readerThrew);
    EXPECT_FALSE(state->readerTimedOut);
    EXPECT_FALSE(state->commitTimedOut);
    ASSERT_EQ(commitResult.status, App::DocumentCommitStatus::Committed);
    ASSERT_TRUE(state->observedWhileLocked);
    EXPECT_FALSE(state->whileLocked.ready);
    EXPECT_TRUE(state->whileLocked.commitBarrier);
    EXPECT_FALSE(state->whileLocked.mustExecute);

    const auto afterUnlock = doc()->getMutationReadiness();
    EXPECT_FALSE(afterUnlock.ready);
    EXPECT_FALSE(afterUnlock.commitBarrier);
    EXPECT_TRUE(afterUnlock.mustExecute);
}

TEST_F(DocumentTest, concurrentTransactionHistoryReadsDuringUndoRedo)
{
    constexpr auto waitTimeout = std::chrono::seconds(30);
    auto* object = doc()->addObject<App::FeatureTest>("HistoryFeature");
    ASSERT_NE(object, nullptr);
    const std::string expectedName = "Update object label references";
    doc()->openTransaction(expectedName);
    object->Integer.setValue(42);
    doc()->commitTransaction();
    ASSERT_EQ(doc()->getAvailableUndos(), 1);

    struct SharedState
    {
        SharedState(App::Document* document, std::string expected)
            : document(document)
            , expectedName(std::move(expected))
        {}

        App::Document* document;
        std::string expectedName;
        std::atomic<bool> stop {false};
        std::atomic<bool> corrupted {false};
        std::atomic<bool> readerThrew {false};
        std::atomic<unsigned int> reads {0};
        std::mutex completionMutex;
        std::condition_variable completionCv;
        bool finished {false};
    };

    auto state = std::make_shared<SharedState>(doc(), expectedName);
    std::thread reader;
    bool readerCleanupDone = false;

    const auto finishReader = [this, state, &reader, &readerCleanupDone, waitTimeout]() -> bool {
        if (readerCleanupDone) {
            return !reader.joinable();
        }
        readerCleanupDone = true;
        state->stop.store(true, std::memory_order_release);
        bool completed = true;
        if (reader.joinable()) {
            std::unique_lock<std::mutex> lock(state->completionMutex);
            completed = state->completionCv.wait_for(lock, waitTimeout, [&] {
                return state->finished;
            });
        }
        if (reader.joinable()) {
            if (completed) {
                reader.join();
            }
            else {
                keepDocumentAliveOnWorkerTimeout();
                reader.detach();
                ADD_FAILURE() << "Transaction history reader thread did not finish within timeout";
            }
        }
        return completed;
    };

    struct CleanupGuard
    {
        const decltype(finishReader)& finish;
        ~CleanupGuard()
        {
            finish();
        }
    } cleanupGuard {finishReader};

    reader = std::thread([state] {
        struct ExitSignal
        {
            std::shared_ptr<SharedState> shared;
            ~ExitSignal()
            {
                std::lock_guard<std::mutex> lock(shared->completionMutex);
                shared->finished = true;
                shared->completionCv.notify_all();
            }
        } exitSignal {state};

        try {
            while (!state->stop.load(std::memory_order_acquire)) {
                const auto undos = state->document->getAvailableUndoNames();
                for (const auto& name : undos) {
                    if (name != state->expectedName) {
                        state->corrupted.store(true, std::memory_order_release);
                        return;
                    }
                }
                const auto redos = state->document->getAvailableRedoNames();
                for (const auto& name : redos) {
                    if (name != state->expectedName) {
                        state->corrupted.store(true, std::memory_order_release);
                        return;
                    }
                }
                static_cast<void>(state->document->getAvailableUndos());
                static_cast<void>(state->document->getAvailableRedos());
                static_cast<void>(state->document->getTransactionID(true, 0));
                static_cast<void>(state->document->getTransactionID(false, 0));
                state->reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
        catch (...) {
            state->readerThrew.store(true, std::memory_order_release);
        }
    });

    for (int i = 0; i < 2000 && !state->corrupted.load(std::memory_order_acquire)
         && !state->readerThrew.load(std::memory_order_acquire);
         ++i) {
        ASSERT_TRUE(doc()->undo());
        ASSERT_TRUE(doc()->redo());
    }

    const bool readerCompleted = finishReader();
    ASSERT_TRUE(readerCompleted);
    EXPECT_FALSE(state->readerThrew.load(std::memory_order_acquire));
    EXPECT_FALSE(state->corrupted.load(std::memory_order_acquire));
    EXPECT_GT(state->reads.load(std::memory_order_relaxed), 0u);
}

TEST_F(DocumentTest, concurrentGetMutationReadinessReadersNeverReportCommitBarrier)
{
    constexpr auto waitTimeout = std::chrono::seconds(30);
    constexpr int objectCount = 500;
    for (int i = 0; i < objectCount; ++i) {
        auto* object = doc()->addObject<App::FeatureTest>("ReadinessObj");
        ASSERT_NE(object, nullptr);
    }
    doc()->recompute();
    ASSERT_FALSE(doc()->mustExecute());
    ASSERT_TRUE(doc()->getMutationReadiness().ready);

    constexpr int threadCount = 4;
    constexpr int iterations = 2000;

    struct SharedState
    {
        explicit SharedState(App::Document* document)
            : document(document)
            , finished(threadCount, false)
        {}

        App::Document* document;
        std::mutex mutex;
        std::condition_variable cv;
        int waitingThreads {0};
        bool startFlag {false};
        bool cancelled {false};
        std::vector<bool> finished;
        std::atomic<bool> stop {false};
        std::atomic<bool> timedOut {false};
        std::atomic<bool> readerThrew {false};
        std::atomic<int> falseBarriers {0};
        std::atomic<int> notReady {0};
    };

    auto state = std::make_shared<SharedState>(doc());
    std::vector<std::thread> readers;
    readers.reserve(threadCount);
    bool readersCleanupDone = false;
    bool readersAllCompleted = true;

    const auto finishReaders =
        [this, state, &readers, &readersCleanupDone, &readersAllCompleted, waitTimeout](
            bool cancel
        ) -> bool {
        if (readersCleanupDone) {
            for (const auto& thread : readers) {
                if (thread.joinable()) {
                    return false;
                }
            }
            return readersAllCompleted;
        }
        readersCleanupDone = true;
        const std::size_t spawned = readers.size();
        if (cancel) {
            state->stop.store(true, std::memory_order_release);
        }
        bool allCompleted = true;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (cancel) {
                state->cancelled = true;
                state->cv.notify_all();
            }
            allCompleted = state->cv.wait_for(lock, waitTimeout, [&] {
                for (std::size_t idx = 0; idx < spawned; ++idx) {
                    if (!state->finished[idx]) {
                        return false;
                    }
                }
                return true;
            });
            if (!allCompleted) {
                state->stop.store(true, std::memory_order_release);
                state->cancelled = true;
                state->cv.notify_all();
            }
        }
        bool anyStillRunning = false;
        for (std::size_t idx = 0; idx < spawned; ++idx) {
            if (!readers[idx].joinable()) {
                continue;
            }
            bool threadDone = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                threadDone = state->finished[idx];
            }
            if (threadDone) {
                readers[idx].join();
            }
            else {
                anyStillRunning = true;
                keepDocumentAliveOnWorkerTimeout();
                readers[idx].detach();
            }
        }
        const bool timedOut = !allCompleted || anyStillRunning;
        if (timedOut) {
            readersAllCompleted = false;
            ADD_FAILURE() << "One or more readiness reader threads did not finish within timeout";
        }
        return !timedOut;
    };

    struct CleanupGuard
    {
        const decltype(finishReaders)& finish;
        ~CleanupGuard()
        {
            finish(true);
        }
    } cleanupGuard {finishReaders};

    for (int t = 0; t < threadCount; ++t) {
        readers.emplace_back([state, waitTimeout, t] {
            struct ExitSignal
            {
                std::shared_ptr<SharedState> shared;
                int index;
                ~ExitSignal()
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    shared->finished[index] = true;
                    shared->cv.notify_all();
                }
            } exitSignal {state, t};

            try {
                {
                    std::unique_lock<std::mutex> lock(state->mutex);
                    ++state->waitingThreads;
                    state->cv.notify_all();
                    const bool started = state->cv.wait_for(lock, waitTimeout, [&] {
                        return state->startFlag || state->cancelled;
                    });
                    if (!started) {
                        state->timedOut.store(true, std::memory_order_release);
                        return;
                    }
                    if (state->cancelled) {
                        return;
                    }
                }
                for (int i = 0; i < iterations && !state->stop.load(std::memory_order_relaxed); ++i) {
                    const auto readiness = state->document->getMutationReadiness();
                    if (readiness.commitBarrier) {
                        state->falseBarriers.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!readiness.ready) {
                        state->notReady.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            catch (...) {
                state->readerThrew.store(true, std::memory_order_release);
            }
        });
    }

    bool allReady = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        allReady = state->cv.wait_for(lock, waitTimeout, [&] {
            return state->waitingThreads == threadCount;
        });
        if (allReady) {
            state->startFlag = true;
        }
        else {
            state->cancelled = true;
            state->stop.store(true, std::memory_order_release);
        }
        state->cv.notify_all();
    }

    const bool readersCompleted = finishReaders(!allReady);
    ASSERT_TRUE(allReady);
    ASSERT_TRUE(readersCompleted);
    EXPECT_FALSE(state->readerThrew.load(std::memory_order_acquire));
    EXPECT_FALSE(state->timedOut.load(std::memory_order_acquire));
    EXPECT_EQ(state->falseBarriers.load(std::memory_order_acquire), 0);
    EXPECT_EQ(state->notReady.load(std::memory_order_acquire), 0);
}

TEST_F(DocumentTest, getMutationReadinessNotReadyWhilePerformingTransaction)
{
    auto* object = doc()->addObject<App::FeatureTest>("AbortFeature");
    ASSERT_NE(object, nullptr);
    doc()->recompute();
    ASSERT_TRUE(doc()->getMutationReadiness().ready);

    bool observedAbort = false;
    App::DocumentMutationReadiness readinessDuringAbort;
    auto connection = doc()->signalAbortTransaction.connect([&](const App::Document& document) {
        observedAbort = true;
        readinessDuringAbort = document.getMutationReadiness();
    });
    struct DisconnectGuard
    {
        decltype(connection)& conn;
        ~DisconnectGuard()
        {
            conn.disconnect();
        }
    } disconnectGuard {connection};

    doc()->openTransaction("AbortReadinessCheck");
    object->Integer.setValue(7);
    doc()->abortTransaction();
    connection.disconnect();

    ASSERT_TRUE(observedAbort);
    EXPECT_FALSE(readinessDuringAbort.ready);
    EXPECT_FALSE(readinessDuringAbort.mustExecute);
    EXPECT_EQ(readinessDuringAbort.diagnostic, "A document transaction is active");
}

// NOLINTEND(readability-magic-numbers)
