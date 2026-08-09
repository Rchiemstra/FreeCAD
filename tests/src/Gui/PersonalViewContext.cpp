// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <Gui/PersonalViewContext.h>

namespace
{

Gui::PersonalViewContext context(std::string marker)
{
    Gui::PersonalViewContext result;
    result.camera = "camera-" + marker;
    result.projection = "projection-" + marker;
    result.selectionPaths = {"doc/" + marker + "/one", "doc/" + marker + "/two"};
    result.preselectionPath = "doc/" + marker + "/preselection";
    result.expandedTreePaths = {"doc/" + marker, "doc/" + marker + "/group"};
    result.treeHorizontalScroll = 11;
    result.treeVerticalScroll = 29;
    result.activeDocument = "document-" + marker;
    result.activeView = "view-" + marker;
    result.activeWorkbench = "workbench-" + marker;
    result.editFocus = "doc/" + marker + "/edit";
    result.temporaryOverlays = {{"overlay-" + marker, "outline", "payload-" + marker}};
    return result;
}

class Gate
{
public:
    explicit Gate(int participants)
        : _participants(participants)
    {}

    void arriveAndWait()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_arrived;
        if (_arrived == _participants) {
            _released = true;
            _condition.notify_all();
            return;
        }
        _condition.wait(lock, [this] { return _released; });
    }

private:
    const int _participants;
    int _arrived {0};
    bool _released {false};
    std::mutex _mutex;
    std::condition_variable _condition;
};

struct PotentiallyThrowingRestore
{
    void operator()(const Gui::PersonalViewContext&) const {}
};

struct ReliableRestore
{
    void operator()(const Gui::PersonalViewContext&) const noexcept {}
};

static_assert(
    !std::is_constructible_v<Gui::PersonalViewRestoreCallback, PotentiallyThrowingRestore>
);
static_assert(std::is_constructible_v<Gui::PersonalViewRestoreCallback, ReliableRestore>);

}  // namespace

TEST(PersonalViewContextTest, actorValuesAreDeepCopiedAndIsolated)
{
    Gui::PersonalViewContextStore store;
    auto alice = context("alice");
    const auto bob = context("bob");
    const auto expectedAlice = alice;
    store.store("alice", alice);
    store.store("bob", bob);

    alice.camera = "caller-mutated";
    alice.selectionPaths.front() = "caller/mutated";
    alice.temporaryOverlays.front().payload = "caller-mutated";

    auto aliceSnapshot = store.snapshot("alice");
    ASSERT_TRUE(aliceSnapshot);
    EXPECT_EQ(*aliceSnapshot, expectedAlice);
    EXPECT_EQ(store.snapshot("bob"), bob);

    aliceSnapshot->camera = "snapshot-mutated";
    aliceSnapshot->selectionPaths.clear();
    aliceSnapshot->temporaryOverlays.clear();
    EXPECT_EQ(store.snapshot("alice"), expectedAlice);
    EXPECT_EQ(store.snapshot("bob"), bob);

    const auto replacement = context("alice-updated");
    ASSERT_TRUE(store.update("alice", replacement));
    EXPECT_EQ(store.snapshot("alice"), replacement);
    EXPECT_EQ(store.snapshot("bob"), bob);

    EXPECT_TRUE(store.remove("alice"));
    EXPECT_FALSE(store.snapshot("alice"));
    EXPECT_FALSE(store.remove("alice"));
    EXPECT_EQ(store.snapshot("bob"), bob);
}

TEST(PersonalViewContextTest, missingActorsAndEmptyActorIdsAreSafe)
{
    Gui::PersonalViewContextStore store;
    const auto value = context("value");

    EXPECT_FALSE(store.snapshot("missing"));
    EXPECT_FALSE(store.contains("missing"));
    EXPECT_FALSE(store.update("missing", value));
    EXPECT_FALSE(store.remove("missing"));
    EXPECT_EQ(store.actorCount(), 0U);

    int callbackCalls = 0;
    const Gui::PersonalViewRendererCallbacks renderer {
        [&] {
            ++callbackCalls;
            return value;
        },
        [&](const auto&) { ++callbackCalls; },
        [&] { ++callbackCalls; },
        [](const auto&) noexcept {},
    };
    EXPECT_EQ(
        store.applyAndRender("missing", renderer),
        Gui::PersonalViewRenderStatus::ActorNotFound
    );
    EXPECT_EQ(callbackCalls, 0);

    EXPECT_THROW(store.store("", value), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(store.update("", value)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(store.snapshot("")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(store.remove("")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(store.contains("")), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(store.applyAndRender("", renderer)), std::invalid_argument);
}

TEST(PersonalViewContextTest, concurrentStoreAccessKeepsExactActorValues)
{
    Gui::PersonalViewContextStore store;
    constexpr int threadCount = 8;
    constexpr int updatesPerThread = 200;
    Gate start(threadCount);
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        threads.emplace_back([&, threadIndex] {
            const std::string actor = "actor-" + std::to_string(threadIndex);
            start.arriveAndWait();
            for (int update = 0; update < updatesPerThread; ++update) {
                auto value = context(actor + "-" + std::to_string(update));
                value.treeVerticalScroll = update;
                store.store(actor, value);
                const auto copied = store.snapshot(actor);
                ASSERT_TRUE(copied);
                ASSERT_EQ(copied->activeDocument, value.activeDocument);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(store.actorCount(), static_cast<std::size_t>(threadCount));
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex) {
        const std::string actor = "actor-" + std::to_string(threadIndex);
        auto expected = context(actor + "-" + std::to_string(updatesPerThread - 1));
        expected.treeVerticalScroll = updatesPerThread - 1;
        EXPECT_EQ(store.snapshot(actor), expected);
    }
}

TEST(PersonalViewContextTest, rendererActionIsSerializedAndRestoresOnSuccess)
{
    Gui::PersonalViewContextStore store;
    const auto alice = context("alice");
    const auto bob = context("bob");
    const auto prior = context("renderer-prior");
    store.store("alice", alice);
    store.store("bob", bob);

    std::atomic<int> actionsInside {0};
    std::atomic<int> maximumInside {0};
    std::mutex rendererStateMutex;
    auto rendererState = prior;
    auto renderer = Gui::PersonalViewRendererCallbacks {
        [&] {
            const int inside = actionsInside.fetch_add(1) + 1;
            int maximum = maximumInside.load();
            while (inside > maximum
                   && !maximumInside.compare_exchange_weak(maximum, inside)) {}
            std::lock_guard<std::mutex> guard(rendererStateMutex);
            return rendererState;
        },
        [&](const Gui::PersonalViewContext& value) {
            std::lock_guard<std::mutex> guard(rendererStateMutex);
            rendererState = value;
        },
        [] { std::this_thread::yield(); },
        [&](const Gui::PersonalViewContext& value) noexcept {
            std::lock_guard<std::mutex> guard(rendererStateMutex);
            rendererState = value;
            actionsInside.fetch_sub(1);
        },
    };

    Gate start(2);
    std::thread first([&] {
        start.arriveAndWait();
        EXPECT_EQ(
            store.applyAndRender("alice", renderer),
            Gui::PersonalViewRenderStatus::Rendered
        );
    });
    std::thread second([&] {
        start.arriveAndWait();
        EXPECT_EQ(store.applyAndRender("bob", renderer), Gui::PersonalViewRenderStatus::Rendered);
    });
    first.join();
    second.join();

    EXPECT_EQ(maximumInside.load(), 1);
    EXPECT_EQ(actionsInside.load(), 0);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(store.snapshot("alice"), alice);
    EXPECT_EQ(store.snapshot("bob"), bob);
}

TEST(PersonalViewContextTest, rendererSerializationIsProcessWideAcrossDistinctStores)
{
    Gui::PersonalViewContextStore firstStore;
    Gui::PersonalViewContextStore secondStore;
    const auto firstContext = context("first-store");
    const auto secondContext = context("second-store");
    const auto prior = context("shared-renderer-prior");
    firstStore.store("first", firstContext);
    secondStore.store("second", secondContext);

    std::atomic<int> actionsInside {0};
    std::atomic<int> maximumInside {0};
    std::atomic<int> rendererEntries {0};
    std::mutex overlapMutex;
    std::condition_variable overlapCondition;
    int callAttempts = 0;
    std::mutex rendererStateMutex;
    auto rendererState = prior;
    const Gui::PersonalViewRendererCallbacks renderer {
        [&] {
            const int inside = actionsInside.fetch_add(1) + 1;
            int maximum = maximumInside.load();
            while (inside > maximum
                   && !maximumInside.compare_exchange_weak(maximum, inside)) {}

            const int entry = rendererEntries.fetch_add(1) + 1;
            overlapCondition.notify_all();
            if (entry == 1) {
                std::unique_lock<std::mutex> overlapLock(overlapMutex);
                EXPECT_TRUE(overlapCondition.wait_for(
                    overlapLock,
                    std::chrono::seconds(2),
                    [&] { return callAttempts == 2; }
                ));
                const bool competingRendererEntered = overlapCondition.wait_for(
                    overlapLock,
                    std::chrono::milliseconds(250),
                    [&] { return rendererEntries.load() == 2; }
                );
                EXPECT_FALSE(competingRendererEntered)
                    << "a second store entered while the first renderer action was in flight";
            }

            std::lock_guard<std::mutex> guard(rendererStateMutex);
            return rendererState;
        },
        [&](const Gui::PersonalViewContext& value) {
            std::lock_guard<std::mutex> guard(rendererStateMutex);
            rendererState = value;
        },
        [] {},
        [&](const Gui::PersonalViewContext& value) noexcept {
            std::lock_guard<std::mutex> guard(rendererStateMutex);
            rendererState = value;
            actionsInside.fetch_sub(1);
        },
    };

    Gate start(2);
    std::thread first([&] {
        start.arriveAndWait();
        {
            std::lock_guard<std::mutex> guard(overlapMutex);
            ++callAttempts;
        }
        overlapCondition.notify_all();
        EXPECT_EQ(
            firstStore.applyAndRender("first", renderer),
            Gui::PersonalViewRenderStatus::Rendered
        );
    });
    std::thread second([&] {
        start.arriveAndWait();
        {
            std::lock_guard<std::mutex> guard(overlapMutex);
            ++callAttempts;
        }
        overlapCondition.notify_all();
        EXPECT_EQ(
            secondStore.applyAndRender("second", renderer),
            Gui::PersonalViewRenderStatus::Rendered
        );
    });
    first.join();
    second.join();

    EXPECT_EQ(maximumInside.load(), 1);
    EXPECT_EQ(actionsInside.load(), 0);
    EXPECT_EQ(rendererEntries.load(), 2);
    EXPECT_EQ(callAttempts, 2);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(firstStore.snapshot("first"), firstContext);
    EXPECT_EQ(secondStore.snapshot("second"), secondContext);
}

TEST(PersonalViewContextTest, applyExceptionRestoresAndPreservesPrimaryException)
{
    Gui::PersonalViewContextStore store;
    const auto requested = context("requested");
    const auto prior = context("prior");
    store.store("actor", requested);
    int applyCalls = 0;
    bool restored = false;
    auto rendererState = prior;

    const Gui::PersonalViewRendererCallbacks renderer {
        [&] { return prior; },
        [&](const Gui::PersonalViewContext& value) {
            ++applyCalls;
            if (value == requested) {
                rendererState = context("partially-applied");
                throw std::runtime_error("primary apply failure");
            }
        },
        [] { FAIL() << "render must not run after apply failure"; },
        [&](const Gui::PersonalViewContext& value) noexcept {
            // A reliable restore boundary absorbs adversity from a secondary
            // renderer cleanup step and still restores the captured value.
            try {
                throw std::logic_error("secondary renderer cleanup failure");
            }
            catch (...) {
                rendererState = value;
                restored = true;
            }
        },
    };

    try {
        (void)store.applyAndRender("actor", renderer);
        FAIL() << "apply failure must escape";
    }
    catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "primary apply failure");
    }
    EXPECT_EQ(applyCalls, 1);
    EXPECT_TRUE(restored);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(store.snapshot("actor"), requested);
}

TEST(PersonalViewContextTest, renderExceptionRestoresAndPreservesPrimaryException)
{
    Gui::PersonalViewContextStore store;
    const auto requested = context("requested");
    const auto prior = context("prior");
    store.store("actor", requested);
    std::vector<Gui::PersonalViewContext> applied;
    auto rendererState = prior;
    bool restored = false;

    const Gui::PersonalViewRendererCallbacks renderer {
        [&] { return prior; },
        [&](const Gui::PersonalViewContext& value) {
            applied.push_back(value);
            rendererState = value;
        },
        [] { throw std::runtime_error("primary render failure"); },
        [&](const Gui::PersonalViewContext& value) noexcept {
            rendererState = value;
            restored = true;
        },
    };

    try {
        (void)store.applyAndRender("actor", renderer);
        FAIL() << "render failure must escape";
    }
    catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "primary render failure");
    }
    ASSERT_EQ(applied.size(), 1U);
    EXPECT_EQ(applied[0], requested);
    EXPECT_TRUE(restored);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(store.snapshot("actor"), requested);
}

TEST(PersonalViewContextTest, reliableRestoreRestoresPriorStateAfterSuccessfulRender)
{
    Gui::PersonalViewContextStore store;
    const auto requested = context("requested");
    const auto prior = context("prior");
    store.store("actor", requested);
    bool rendered = false;
    auto rendererState = prior;
    int restoreCalls = 0;

    const Gui::PersonalViewRendererCallbacks renderer {
        [&] { return prior; },
        [&](const Gui::PersonalViewContext& value) {
            rendererState = value;
        },
        [&] { rendered = true; },
        [&](const Gui::PersonalViewContext& value) noexcept {
            ++restoreCalls;
            rendererState = value;
        },
    };

    EXPECT_EQ(
        store.applyAndRender("actor", renderer),
        Gui::PersonalViewRenderStatus::Rendered
    );
    EXPECT_TRUE(rendered);
    EXPECT_EQ(restoreCalls, 1);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(store.snapshot("actor"), requested);
}

TEST(PersonalViewContextTest, recursiveRenderIsRejectedAndOuterStateIsExactlyRestored)
{
    Gui::PersonalViewContextStore store;
    const auto outer = context("outer");
    const auto inner = context("inner");
    const auto prior = context("prior");
    store.store("outer", outer);
    store.store("inner", inner);

    auto rendererState = prior;
    int captureCalls = 0;
    int applyCalls = 0;
    int renderCalls = 0;
    int restoreCalls = 0;
    Gui::PersonalViewRendererCallbacks renderer;
    renderer = {
        [&] {
            ++captureCalls;
            return rendererState;
        },
        [&](const Gui::PersonalViewContext& value) {
            ++applyCalls;
            rendererState = value;
        },
        [&] {
            ++renderCalls;
            (void)store.applyAndRender("inner", renderer);
        },
        [&](const Gui::PersonalViewContext& value) noexcept {
            ++restoreCalls;
            rendererState = value;
        },
    };

    try {
        (void)store.applyAndRender("outer", renderer);
        FAIL() << "recursive renderer entry must be rejected";
    }
    catch (const std::logic_error& error) {
        EXPECT_STREQ(error.what(), "personal view rendering must not be invoked recursively");
    }

    // The nested invocation is rejected before capture/apply/render.  The
    // counts therefore describe only the outer invocation and its rollback.
    EXPECT_EQ(captureCalls, 1);
    EXPECT_EQ(applyCalls, 1);
    EXPECT_EQ(renderCalls, 1);
    EXPECT_EQ(restoreCalls, 1);
    EXPECT_EQ(rendererState, prior);
    EXPECT_EQ(store.snapshot("outer"), outer);
    EXPECT_EQ(store.snapshot("inner"), inner);
}

TEST(PersonalViewContextTest, renderingUsesImmutableSnapshotWhileStoreCanUpdate)
{
    Gui::PersonalViewContextStore store;
    const auto initial = context("initial");
    const auto updated = context("updated");
    const auto other = context("other");
    const auto prior = context("prior");
    store.store("actor", initial);
    store.store("other", other);
    Gui::PersonalViewContext applied;

    const Gui::PersonalViewRendererCallbacks renderer {
        [&] { return prior; },
        [&](const Gui::PersonalViewContext& value) {
            if (value == initial) {
                applied = value;
            }
        },
        [&] {
            ASSERT_TRUE(store.update("actor", updated));
            EXPECT_EQ(applied, initial);
            EXPECT_EQ(store.snapshot("other"), other);
        },
        [](const Gui::PersonalViewContext&) noexcept {},
    };

    EXPECT_EQ(
        store.applyAndRender("actor", renderer),
        Gui::PersonalViewRenderStatus::Rendered
    );
    EXPECT_EQ(applied, initial);
    EXPECT_EQ(store.snapshot("actor"), updated);
    EXPECT_EQ(store.snapshot("other"), other);
}
