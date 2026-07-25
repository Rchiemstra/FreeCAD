// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/GeometryJob.h>
#include <App/GeometryJobManager.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/GuiResponsivenessProbe.h>
#include <App/MainThreadSignal.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <algorithm>
#include <atomic>
#include <memory>

TEST(GeometryJobTest, StateMachineAndExactOnceCallback)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 100;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 42;
    spec.target.internalName = "Box001";
    spec.key.documentIncarnation = 100;
    spec.key.targetObjectId = 42;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    App::GeometryJobHandle handle = App::GeometryJobManager::instance().submit(spec);
    EXPECT_TRUE(handle.isValid());

    int callbackCount = 0;
    App::GeometryJobState lastState = App::GeometryJobState::Queued;

    App::GeometryJobManager::instance().registerCallback(
        handle.id(),
        [&callbackCount, &lastState](App::GeometryJobId,
                                     App::GeometryJobState state,
                                     const App::DetachedGeometryResult&) {
            callbackCount++;
            lastState = state;
        });

    App::DetachedGeometryResult dummyResult;
    dummyResult.success = true;
    dummyResult.resultArchivePath = "/tmp/test.fcg";

    App::GeometryJobManager::instance().setJobState(
        handle.id(), App::GeometryJobState::Completed, dummyResult);

    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(lastState, App::GeometryJobState::Completed);

    App::GeometryJobManager::instance().setJobState(
        handle.id(), App::GeometryJobState::Completed, dummyResult);
    EXPECT_EQ(callbackCount, 1);
}

TEST(GeometryJobTest, CoalescingSameGeneration)
{
    App::GeometryJobSpec spec1;
    spec1.document.runtimeIncarnation = 200;
    spec1.document.modelGeneration = 5;
    spec1.target.objectId = 99;
    spec1.key.documentIncarnation = 200;
    spec1.key.targetObjectId = 99;
    spec1.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec1.coalescing = App::CoalesceMode::SingleInstance;

    App::GeometryJobHandle handle1 = App::GeometryJobManager::instance().submit(spec1);

    App::GeometryJobSpec spec2 = spec1;
    App::GeometryJobHandle handle2 = App::GeometryJobManager::instance().submit(spec2);

    EXPECT_EQ(handle1.id(), handle2.id());
}

TEST(GeometryJobTest, PreviewLatestWinsReplacesPending)
{
    App::GeometryJobSpec spec1;
    spec1.document.runtimeIncarnation = 250;
    spec1.document.modelGeneration = 3;
    spec1.target.objectId = 7;
    spec1.key.documentIncarnation = 250;
    spec1.key.targetObjectId = 7;
    spec1.key.purpose = App::GeometryJobPurpose::Preview;
    spec1.key.previewChannel = 1;
    spec1.purpose = App::GeometryJobPurpose::Preview;
    spec1.coalescing = App::CoalesceMode::LatestWins;

    int cancelledCount = 0;
    App::GeometryJobHandle handle1 = App::GeometryJobManager::instance().submit(spec1);
    App::GeometryJobManager::instance().registerCallback(
        handle1.id(),
        [&cancelledCount](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                cancelledCount++;
            }
        });

    App::GeometryJobSpec spec2 = spec1;
    App::GeometryJobHandle handle2 = App::GeometryJobManager::instance().submit(spec2);

    EXPECT_NE(handle1.id(), handle2.id());
    EXPECT_EQ(cancelledCount, 1);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle1.id()),
              App::GeometryJobState::Cancelled);
}

TEST(GeometryJobTest, InvalidationOnDocumentClose)
{
    App::DocumentRevisionToken docToken;
    docToken.runtimeIncarnation = 300;
    docToken.modelGeneration = 1;

    App::GeometryJobSpec spec;
    spec.document = docToken;
    spec.target.objectId = 10;
    spec.key.documentIncarnation = 300;
    spec.key.targetObjectId = 10;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    App::GeometryJobHandle handle = App::GeometryJobManager::instance().submit(spec);

    bool documentClosedCalled = false;
    App::GeometryJobManager::instance().registerCallback(
        handle.id(),
        [&documentClosedCalled](App::GeometryJobId,
                                App::GeometryJobState state,
                                const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::DocumentClosed) {
                documentClosedCalled = true;
            }
        });

    App::GeometryJobManager::instance().invalidateDocument(docToken, App::CancelReason::DocumentClosed);
    EXPECT_TRUE(documentClosedCalled);
}

TEST(GeometryJobTest, NewerGenerationCancelsOlderModelJob)
{
    App::GeometryJobSpec spec1;
    spec1.document.runtimeIncarnation = 400;
    spec1.document.modelGeneration = 1;
    spec1.target.objectId = 11;
    spec1.key.documentIncarnation = 400;
    spec1.key.targetObjectId = 11;
    spec1.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    bool cancelled = false;
    auto handle1 = App::GeometryJobManager::instance().submit(spec1);
    App::GeometryJobManager::instance().registerCallback(
        handle1.id(),
        [&cancelled](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                cancelled = true;
            }
        });

    App::GeometryJobSpec spec2 = spec1;
    spec2.document.modelGeneration = 2;
    auto handle2 = App::GeometryJobManager::instance().submit(spec2);

    EXPECT_NE(handle1.id(), handle2.id());
    EXPECT_TRUE(cancelled);
}

TEST(GeometryJobTest, InProcessDeniedByDefault)
{
    App::GeometryJobManager::instance().setAllowInProcess(false);

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 500;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 12;
    spec.key.documentIncarnation = 500;
    spec.key.targetObjectId = 12;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::VerifiedInProcess;

    auto handle = App::GeometryJobManager::instance().submit(spec);
    EXPECT_TRUE(handle.isValid());
    // Without a task, backend downgrade is still applied for VerifiedInProcess requests.
    EXPECT_FALSE(App::GeometryJobManager::instance().isAllowInProcess());
}

TEST(GeometryJobTest, TerminalStateIsMonotonic)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 600;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 13;
    spec.key.documentIncarnation = 600;
    spec.key.targetObjectId = 13;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    auto handle = App::GeometryJobManager::instance().submit(spec);
    App::DetachedGeometryResult ok;
    ok.success = true;
    App::GeometryJobManager::instance().setJobState(handle.id(), App::GeometryJobState::Completed, ok);

    App::DetachedGeometryResult fail;
    fail.success = false;
    fail.errorCode = "LateFail";
    App::GeometryJobManager::instance().setJobState(handle.id(), App::GeometryJobState::Failed, fail);

    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Completed);
    EXPECT_TRUE(App::GeometryJobManager::instance().getJobResult(handle.id()).success);
}

TEST(GeometryJobTest, CancelFromCallbackDoesNotReenterCorrupt)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 700;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 14;
    spec.key.documentIncarnation = 700;
    spec.key.targetObjectId = 14;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;

    auto handle = App::GeometryJobManager::instance().submit(spec);
    int calls = 0;
    App::GeometryJobManager::instance().registerCallback(
        handle.id(),
        [&calls](App::GeometryJobId id, App::GeometryJobState, const App::DetachedGeometryResult&) {
            ++calls;
            // Re-entrant cancel of the same id must be safe once already terminal.
            App::GeometryJobManager::instance().cancel(id, App::CancelReason::UserRequested);
        });

    App::DetachedGeometryResult ok;
    ok.success = true;
    App::GeometryJobManager::instance().setJobState(handle.id(), App::GeometryJobState::Completed, ok);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Completed);
}

TEST(GeometryJobTest, ObjectInvalidationIsDocumentScoped)
{
    App::GeometryJobSpec specA;
    specA.document.runtimeIncarnation = 801;
    specA.document.modelGeneration = 1;
    specA.target.objectId = 42;
    specA.target.internalName = "SharedName";
    specA.key.documentIncarnation = 801;
    specA.key.targetObjectId = 42;

    App::GeometryJobSpec specB = specA;
    specB.document.runtimeIncarnation = 802;
    specB.key.documentIncarnation = 802;

    auto handleA = App::GeometryJobManager::instance().submit(specA);
    auto handleB = App::GeometryJobManager::instance().submit(specB);

    App::ObjectRevisionToken token;
    token.objectId = 42;
    token.internalName = "SharedName";
    token.documentIncarnation = 801;
    App::GeometryJobManager::instance().invalidateObject(token, App::CancelReason::ObjectRemoved);

    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handleA.id()),
              App::GeometryJobState::Stale);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handleB.id()),
              App::GeometryJobState::Queued);
}

namespace
{

class SleepTask : public App::DetachedGeometryTask
{
public:
    explicit SleepTask(std::chrono::milliseconds duration, std::string paramDigest = {})
        : _duration(duration)
        , _paramDigest(std::move(paramDigest))
    {
    }

    std::string operationType() const override
    {
        return "test.sleep";
    }

    uint32_t codecVersion() const override
    {
        return 1;
    }

    std::string parameterDigest() const override
    {
        return _paramDigest;
    }

    App::GeometryOperationTraits traits() const override
    {
        App::GeometryOperationTraits t;
        t.allowInProcess = true;
        t.supportsInProcess = true;
        t.supportsCooperativeCancel = true;
        t.operationName = "test.sleep";
        return t;
    }

    App::DetachedGeometryResult run(App::GeometryWorkerContext& ctx) const override
    {
        App::DetachedGeometryResult result;
        const auto end = std::chrono::steady_clock::now() + _duration;
        double fraction = 0.0;
        while (std::chrono::steady_clock::now() < end) {
            if (ctx.isCancelled()) {
                result.success = false;
                result.errorCode = "Cancelled";
                return result;
            }
            fraction = std::min(0.99, fraction + 0.05);
            ctx.reportProgress(fraction, "test.sleep");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ctx.reportProgress(1.0, "test.sleep.done");
        result.success = true;
        result.resultArchivePath = ctx.tempDir() + "/synthetic.ok";
        {
            std::ofstream ofs(result.resultArchivePath, std::ios::binary);
            ofs << "synthetic-ok";
        }
        return result;
    }

    void writeArchive(App::GeometryArchiveWriter&) const override {}

private:
    std::chrono::milliseconds _duration;
    std::string _paramDigest;
};

struct QueuedMainThreadHooks
{
    static inline std::thread::id mainId {};
    static inline std::mutex mutex;
    static inline std::deque<std::function<void()>> pending;

    static bool isMainThread()
    {
        return std::this_thread::get_id() == mainId;
    }

    static void invoke(std::function<void()>&& fn, bool blocking)
    {
        if (blocking || isMainThread()) {
            fn();
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        pending.push_back(std::move(fn));
    }

    static void install()
    {
        mainId = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.clear();
        }
        App::MainThreadSignalConfig::setHooks(&isMainThread, &invoke);
    }

    static void uninstall()
    {
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear();
    }

    static size_t flush()
    {
        size_t count = 0;
        for (;;) {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (pending.empty()) {
                    break;
                }
                fn = std::move(pending.front());
                pending.pop_front();
            }
            fn();
            ++count;
        }
        return count;
    }
};

} // namespace

TEST(GeometryJobTest, ResultWorkspaceSurvivesQueuedGuiCallback)
{
    QueuedMainThreadHooks::install();
    App::GeometryJobManager::instance().setAllowInProcess(true);

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 910;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 101;
    spec.key.documentIncarnation = 910;
    spec.key.targetObjectId = 101;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::VerifiedInProcess;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(20));
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    bool artifactReadableInCallback = false;
    std::string observedPath;

    auto handle = App::GeometryJobManager::instance().submit(spec);
    ASSERT_TRUE(handle.isValid());
    App::GeometryJobManager::instance().registerCallback(
        handle.id(),
        [&](App::GeometryJobId,
            App::GeometryJobState state,
            const App::DetachedGeometryResult& result) {
            if (state != App::GeometryJobState::Completed) {
                return;
            }
            observedPath = result.resultArchivePath;
            artifactReadableInCallback =
                !observedPath.empty() && std::filesystem::exists(observedPath);
        });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        QueuedMainThreadHooks::flush();
        const auto state = App::GeometryJobManager::instance().getJobState(handle.id());
        if (state == App::GeometryJobState::Completed
            || state == App::GeometryJobState::Failed
            || state == App::GeometryJobState::Cancelled
            || state == App::GeometryJobState::TimedOut) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QueuedMainThreadHooks::flush();

    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Completed);
    EXPECT_TRUE(artifactReadableInCallback) << "missing artifact path=" << observedPath;
    EXPECT_TRUE(std::filesystem::exists(observedPath));

    App::GeometryJobManager::instance().releaseJobArtifacts(handle.id());
    EXPECT_FALSE(std::filesystem::exists(observedPath));

    App::GeometryJobManager::instance().setAllowInProcess(false);
    QueuedMainThreadHooks::uninstall();
}

TEST(GeometryJobTest, JoinedObserversAllReceiveTerminal)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 920;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 55;
    spec.key.documentIncarnation = 920;
    spec.key.targetObjectId = 55;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.coalescing = App::CoalesceMode::SingleInstance;

    auto handle = App::GeometryJobManager::instance().submit(spec);
    ASSERT_TRUE(handle.isValid());

    // Same key + generation joins; both observers must be retained.
    auto joined = App::GeometryJobManager::instance().submit(spec);
    EXPECT_EQ(handle.id(), joined.id());

    int observerA = 0;
    int observerB = 0;
    App::GeometryJobState stateA = App::GeometryJobState::Queued;
    App::GeometryJobState stateB = App::GeometryJobState::Queued;

    App::GeometryJobManager::instance().registerCallback(
        handle.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            ++observerA;
            stateA = state;
        });
    App::GeometryJobManager::instance().registerCallback(
        joined.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            ++observerB;
            stateB = state;
        });

    App::DetachedGeometryResult ok;
    ok.success = true;
    App::GeometryJobManager::instance().setJobState(
        handle.id(), App::GeometryJobState::Completed, ok);

    EXPECT_EQ(observerA, 1);
    EXPECT_EQ(observerB, 1);
    EXPECT_EQ(stateA, App::GeometryJobState::Completed);
    EXPECT_EQ(stateB, App::GeometryJobState::Completed);
}

TEST(GeometryJobTest, SupersedeSetsCancelFlagForRunningInProcessJob)
{
    App::GeometryJobManager::instance().setAllowInProcess(true);

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 930;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 77;
    spec.key.documentIncarnation = 930;
    spec.key.targetObjectId = 77;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::VerifiedInProcess;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(800));
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    const auto started = std::chrono::steady_clock::now();
    auto handle = App::GeometryJobManager::instance().submit(spec);
    ASSERT_TRUE(handle.isValid());

    const auto runDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < runDeadline) {
        if (App::GeometryJobManager::instance().getJobState(handle.id())
            == App::GeometryJobState::Running) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Running);

    App::GeometryJobSpec newer = spec;
    newer.document.modelGeneration = 2;
    newer.task = std::make_shared<SleepTask>(std::chrono::milliseconds(20));
    auto replacement = App::GeometryJobManager::instance().submit(newer);
    EXPECT_NE(replacement.id(), handle.id());
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Cancelled);

    const auto terminalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < terminalDeadline) {
        const auto state = App::GeometryJobManager::instance().getJobState(replacement.id());
        if (state == App::GeometryJobState::Completed
            || state == App::GeometryJobState::Failed
            || state == App::GeometryJobState::Cancelled
            || state == App::GeometryJobState::TimedOut) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Cooperative cancel must stop the superseded 800 ms sleep well before it finishes.
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::milliseconds(500));
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(replacement.id()),
              App::GeometryJobState::Completed);

    App::GeometryJobManager::instance().releaseJobArtifacts(handle.id());
    App::GeometryJobManager::instance().releaseJobArtifacts(replacement.id());
    App::GeometryJobManager::instance().setAllowInProcess(false);
}

TEST(GeometryJobTest, DifferentParametersDoNotJoinUnderSingleInstance)
{
    App::GeometryJobSpec specA;
    specA.document.runtimeIncarnation = 940;
    specA.document.modelGeneration = 1;
    specA.target.objectId = 88;
    specA.key.documentIncarnation = 940;
    specA.key.targetObjectId = 88;
    specA.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    specA.coalescing = App::CoalesceMode::SingleInstance;
    specA.task = std::make_shared<SleepTask>(std::chrono::milliseconds(0), "radius=1");

    App::GeometryJobSpec specB = specA;
    specB.task = std::make_shared<SleepTask>(std::chrono::milliseconds(0), "radius=2");

    auto handleA = App::GeometryJobManager::instance().submit(specA);
    int cancelled = 0;
    App::GeometryJobManager::instance().registerCallback(
        handleA.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                ++cancelled;
            }
        });

    auto handleB = App::GeometryJobManager::instance().submit(specB);
    EXPECT_NE(handleA.id(), handleB.id());
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handleA.id()),
              App::GeometryJobState::Cancelled);
}

TEST(GeometryJobTest, CoalesceNoneDoesNotJoinOrCancel)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 941;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 89;
    spec.key.documentIncarnation = 941;
    spec.key.targetObjectId = 89;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.coalescing = App::CoalesceMode::None;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(0), "same");

    auto handle1 = App::GeometryJobManager::instance().submit(spec);
    int cancelled = 0;
    App::GeometryJobManager::instance().registerCallback(
        handle1.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                ++cancelled;
            }
        });

    auto handle2 = App::GeometryJobManager::instance().submit(spec);
    EXPECT_NE(handle1.id(), handle2.id());
    EXPECT_EQ(cancelled, 0);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle1.id()),
              App::GeometryJobState::Queued);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle2.id()),
              App::GeometryJobState::Queued);
}

TEST(GeometryJobTest, CoalesceLatestWinsCancelsEvenIdenticalModelJob)
{
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 942;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 90;
    spec.key.documentIncarnation = 942;
    spec.key.targetObjectId = 90;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.coalescing = App::CoalesceMode::LatestWins;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(0), "identical");

    auto handle1 = App::GeometryJobManager::instance().submit(spec);
    int cancelled = 0;
    App::GeometryJobManager::instance().registerCallback(
        handle1.id(),
        [&](App::GeometryJobId, App::GeometryJobState state, const App::DetachedGeometryResult&) {
            if (state == App::GeometryJobState::Cancelled) {
                ++cancelled;
            }
        });

    auto handle2 = App::GeometryJobManager::instance().submit(spec);
    EXPECT_NE(handle1.id(), handle2.id());
    EXPECT_EQ(cancelled, 1);
    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle1.id()),
              App::GeometryJobState::Cancelled);
}

TEST(GeometryJobTest, AllowlistedInProcessExecutorCompletes)
{
    App::GeometryJobManager::instance().setAllowInProcess(true);

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 900;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 99;
    spec.key.documentIncarnation = 900;
    spec.key.targetObjectId = 99;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::VerifiedInProcess;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(30));
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    auto handle = App::GeometryJobManager::instance().submit(spec);
    ASSERT_TRUE(handle.isValid());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = App::GeometryJobManager::instance().getJobState(handle.id());
        if (state == App::GeometryJobState::Completed
            || state == App::GeometryJobState::Failed
            || state == App::GeometryJobState::Cancelled
            || state == App::GeometryJobState::TimedOut) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Completed);
    EXPECT_TRUE(App::GeometryJobManager::instance().getJobResult(handle.id()).success);

    const auto path = App::GeometryJobManager::instance().getJobResult(handle.id()).resultArchivePath;
    EXPECT_TRUE(std::filesystem::exists(path));
    App::GeometryJobManager::instance().releaseJobArtifacts(handle.id());
    EXPECT_FALSE(std::filesystem::exists(path));

    App::GeometryJobManager::instance().setAllowInProcess(false);
}

TEST(GeometryJobTest, InProcessWorkersAreBoundedAndReclaimed)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.setAllowInProcess(true);
    mgr.resetPeakWorkerThreads();

    constexpr int JobCount = 32;
    std::vector<App::GeometryJobHandle> handles;
    handles.reserve(JobCount);

    for (int i = 0; i < JobCount; ++i) {
        App::GeometryJobSpec spec;
        spec.document.runtimeIncarnation = 970;
        spec.document.modelGeneration = 1;
        spec.target.objectId = 1000 + i;
        spec.key.documentIncarnation = 970;
        spec.key.targetObjectId = 1000 + i;
        spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
        spec.coalescing = App::CoalesceMode::None;
        spec.backend = App::GeometryBackend::VerifiedInProcess;
        spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(15));
        spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        handles.push_back(mgr.submit(spec));
        ASSERT_TRUE(handles.back().isValid());
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        size_t done = 0;
        for (const auto& h : handles) {
            const auto state = mgr.getJobState(h.id());
            if (state == App::GeometryJobState::Completed
                || state == App::GeometryJobState::Failed
                || state == App::GeometryJobState::Cancelled
                || state == App::GeometryJobState::TimedOut) {
                ++done;
            }
        }
        if (done == handles.size()) {
            break;
        }
        // Drive reclaim/dispatch for finished workers between polls.
        mgr.reclaimFinishedWorkers();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    for (const auto& h : handles) {
        EXPECT_EQ(mgr.getJobState(h.id()), App::GeometryJobState::Completed);
        mgr.releaseJobArtifacts(h.id());
    }

    mgr.reclaimFinishedWorkers();
    EXPECT_EQ(mgr.retainedWorkerThreadCount(), 0u)
        << "finished in-process workers must be joined/reclaimed";
    EXPECT_LE(mgr.peakUnfinishedWorkerThreadCount(),
              App::GeometryJobManager::MaxConcurrentInProcessWorkers)
        << "in-process concurrency must stay within the fixed pool bound";
    EXPECT_GE(mgr.peakUnfinishedWorkerThreadCount(), 1u);

    mgr.setAllowInProcess(false);
}

TEST(GeometryJobTest, FreeCADCmdLauncherIsInvokedAndLeavesQueued)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();

    std::atomic<int> launchCount {0};
    std::string seenWorkspace;
    mgr.setProcessBackend(
        [&](const App::GeometryProcessLaunchRequest& req) {
            ++launchCount;
            seenWorkspace = req.workspaceDir;
            EXPECT_NE(req.id, 0u);
            EXPECT_EQ(req.spec.backend, App::GeometryBackend::FreeCADCmd);
            EXPECT_FALSE(req.workspaceDir.empty());
            EXPECT_TRUE(std::filesystem::exists(req.workspaceDir))
                << "manager must create a random workspace before launch";
            // Simulate FreeCADCmd start failure (no real process in App tests).
            return false;
        },
        [](App::GeometryJobId, App::CancelReason) {});

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1500;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 77;
    spec.key.documentIncarnation = 1500;
    spec.key.targetObjectId = 77;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::FreeCADCmd;
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    auto handle = mgr.submit(spec);
    ASSERT_TRUE(handle.isValid());

    EXPECT_GE(launchCount.load(), 1)
        << "manager must invoke the FreeCADCmd process backend (must not stay Queued forever)";
    EXPECT_NE(mgr.getJobState(handle.id()), App::GeometryJobState::Queued);
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Failed);
    EXPECT_EQ(mgr.getJobResult(handle.id()).errorCode, "ProcessStartFailed");
    EXPECT_FALSE(seenWorkspace.empty());
    // Start-failure cleans the owned workspace.
    EXPECT_FALSE(std::filesystem::exists(seenWorkspace));

    mgr.clearProcessBackend();
}

TEST(GeometryJobTest, FreeCADCmdStartSuccessLeavesRunningUntilTerminal)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();

    std::atomic<bool> cancelSeen {false};
    App::GeometryJobId launchedId = 0;
    mgr.setProcessBackend(
        [&](const App::GeometryProcessLaunchRequest& req) {
            launchedId = req.id;
            EXPECT_TRUE(std::filesystem::exists(req.workspaceDir));
            return true;
        },
        [&](App::GeometryJobId id, App::CancelReason) {
            if (id == launchedId) {
                cancelSeen = true;
            }
        });

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1501;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 78;
    spec.key.documentIncarnation = 1501;
    spec.key.targetObjectId = 78;
    spec.key.purpose = App::GeometryJobPurpose::ModelRecompute;
    spec.backend = App::GeometryBackend::FreeCADCmd;

    auto handle = mgr.submit(spec);
    ASSERT_TRUE(handle.isValid());
    EXPECT_EQ(launchedId, handle.id());
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Running);

    mgr.cancel(handle.id(), App::CancelReason::UserRequested);
    EXPECT_TRUE(cancelSeen.load());
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Cancelling);

    App::DetachedGeometryResult cancelled;
    cancelled.success = false;
    cancelled.errorCode = "Cancelled";
    mgr.setJobState(handle.id(), App::GeometryJobState::Cancelled, cancelled);
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Cancelled);

    mgr.clearProcessBackend();
}

TEST(GeometryJobTest, FreeCADCmdWithoutBackendRemainsQueued)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();
    ASSERT_FALSE(mgr.hasProcessBackend());

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1502;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 79;
    spec.key.documentIncarnation = 1502;
    spec.key.targetObjectId = 79;
    spec.backend = App::GeometryBackend::FreeCADCmd;

    auto handle = mgr.submit(spec);
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Queued);
}

TEST(GeometryJobTest, InvalidateDocumentCancelsRunningFreeCADCmdProcess)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();

    std::atomic<int> cancelCount {0};
    App::CancelReason seenReason = App::CancelReason::UserRequested;
    mgr.setProcessBackend(
        [&](const App::GeometryProcessLaunchRequest&) { return true; },
        [&](App::GeometryJobId, App::CancelReason reason) {
            ++cancelCount;
            seenReason = reason;
        });

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1600;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 81;
    spec.key.documentIncarnation = 1600;
    spec.key.targetObjectId = 81;
    spec.backend = App::GeometryBackend::FreeCADCmd;

    auto handle = mgr.submit(spec);
    ASSERT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Running);

    App::DocumentRevisionToken token;
    token.runtimeIncarnation = 1600;
    token.modelGeneration = 1;
    mgr.invalidateDocument(token, App::CancelReason::DocumentClosed);

    EXPECT_EQ(cancelCount.load(), 1)
        << "document close must cancel running FreeCADCmd processes (non-blocking)";
    EXPECT_EQ(seenReason, App::CancelReason::DocumentClosed);
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::DocumentClosed);

    mgr.clearProcessBackend();
}

TEST(GeometryJobTest, InvalidateObjectCancelsRunningFreeCADCmdProcess)
{
    auto& mgr = App::GeometryJobManager::instance();
    mgr.clearProcessBackend();

    std::atomic<int> cancelCount {0};
    mgr.setProcessBackend(
        [&](const App::GeometryProcessLaunchRequest&) { return true; },
        [&](App::GeometryJobId, App::CancelReason) { ++cancelCount; });

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1601;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 82;
    spec.target.internalName = "Box";
    spec.key.documentIncarnation = 1601;
    spec.key.targetObjectId = 82;
    spec.backend = App::GeometryBackend::FreeCADCmd;

    auto handle = mgr.submit(spec);
    ASSERT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Running);

    App::ObjectRevisionToken objToken;
    objToken.objectId = 82;
    objToken.internalName = "Box";
    objToken.documentIncarnation = 1601;
    mgr.invalidateObject(objToken, App::CancelReason::ObjectRemoved);

    EXPECT_EQ(cancelCount.load(), 1);
    EXPECT_EQ(mgr.getJobState(handle.id()), App::GeometryJobState::Stale);

    mgr.clearProcessBackend();
}

TEST(GuiResponsivenessProbeTest, HeartbeatUnderSyntheticLoad)
{
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char arg0[] = "App_tests_run";
        static char* argv[] = {arg0, nullptr};
        new QCoreApplication(argc, argv);
    }

    auto& probe = App::GuiResponsivenessProbe::instance();
    probe.resetStats();
    probe.start(std::chrono::milliseconds(10));

    App::GeometryJobManager::instance().setAllowInProcess(true);
    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 901;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 100;
    spec.key.documentIncarnation = 901;
    spec.key.targetObjectId = 100;
    spec.backend = App::GeometryBackend::VerifiedInProcess;
    spec.task = std::make_shared<SleepTask>(std::chrono::milliseconds(250));
    spec.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto handle = App::GeometryJobManager::instance().submit(spec);

    QEventLoop loop;
    QTimer ticker;
    ticker.setInterval(5);
    QObject::connect(&ticker, &QTimer::timeout, [&]() {
        App::GuiResponsivenessProbe::ScopedCallback slice("synthetic-gui-slice");
        const auto state = App::GeometryJobManager::instance().getJobState(handle.id());
        if (state == App::GeometryJobState::Completed
            || state == App::GeometryJobState::Failed
            || state == App::GeometryJobState::Cancelled
            || state == App::GeometryJobState::TimedOut) {
            loop.quit();
        }
    });
    ticker.start();
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();
    ticker.stop();
    probe.stop();

    EXPECT_EQ(App::GeometryJobManager::instance().getJobState(handle.id()),
              App::GeometryJobState::Completed);
    EXPECT_GT(probe.heartbeatCount(), 5u);
    EXPECT_LT(probe.maxHeartbeatGapMs(), 100.0);
    EXPECT_LT(probe.maxCallbackDurationMs(), 33.0);

    App::GeometryJobManager::instance().releaseJobArtifacts(handle.id());
    App::GeometryJobManager::instance().setAllowInProcess(false);
}

TEST(GeometryJobTest, TrustedRelativeResultPathRejectsTraversal)
{
    EXPECT_TRUE(App::isTrustedRelativeResultPath("result.fcg"));
    EXPECT_TRUE(App::isTrustedRelativeResultPath("out/result.fcg"));
    EXPECT_FALSE(App::isTrustedRelativeResultPath(""));
    EXPECT_FALSE(App::isTrustedRelativeResultPath("/tmp/result.fcg"));
    EXPECT_FALSE(App::isTrustedRelativeResultPath("C:\\temp\\result.fcg"));
    EXPECT_FALSE(App::isTrustedRelativeResultPath("../result.fcg"));
    EXPECT_FALSE(App::isTrustedRelativeResultPath("a/../../etc/passwd"));
    EXPECT_FALSE(App::isTrustedRelativeResultPath("..\\result.fcg"));
}

TEST(GeometryJobTest, ProgressListenerReceivesUpdates)
{
    App::GeometryJobManager::instance().setAllowInProcess(false);
    App::GeometryJobManager::instance().clearProgressListeners();

    std::vector<std::pair<App::GeometryJobId, double>> seen;
    App::GeometryJobManager::instance().setProgressListener(
        [&seen](App::GeometryJobId id, double fraction, const std::string&) {
            seen.emplace_back(id, fraction);
        });

    App::GeometryJobSpec spec;
    spec.document.runtimeIncarnation = 1;
    spec.document.modelGeneration = 1;
    spec.target.objectId = 1;
    spec.target.internalName = "ProgressListener";
    auto handle = App::GeometryJobManager::instance().submit(spec);
    ASSERT_TRUE(handle.isValid());

    App::GeometryJobManager::instance().updateProgress(handle.id(), 0.25, "test.phase");
    App::GeometryJobManager::instance().updateProgress(handle.id(), 0.75, "test.phase");

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0].first, handle.id());
    EXPECT_DOUBLE_EQ(seen[0].second, 0.25);
    EXPECT_DOUBLE_EQ(seen[1].second, 0.75);

    App::GeometryJobManager::instance().clearProgressListeners();
    App::GeometryJobManager::instance().setJobState(
        handle.id(), App::GeometryJobState::Cancelled, {});
}

TEST(GuiResponsivenessProbeTest, HeartbeatTracksGaps)
{
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static char arg0[] = "App_tests_run";
        static char* argv[] = {arg0, nullptr};
        new QCoreApplication(argc, argv);
    }

    auto& probe = App::GuiResponsivenessProbe::instance();
    probe.resetStats();
    probe.start(std::chrono::milliseconds(10));

    QEventLoop loop;
    QTimer::singleShot(80, &loop, &QEventLoop::quit);
    loop.exec();

    probe.stop();
    EXPECT_GT(probe.heartbeatCount(), 0u);
    EXPECT_LT(probe.maxHeartbeatGapMs(), 100.0);
}

TEST(GuiResponsivenessProbeTest, ScopedCallbackRecordsDuration)
{
    {
        App::GuiResponsivenessProbe::ScopedCallback scope("test-slice");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(App::GuiResponsivenessProbe::instance().maxCallbackDurationMs(), 0.0);
}
