// SPDX-License-Identifier: LGPL-2.1-or-later

// Lane and handle recomputes publish the same terminal notifications.
// The lane cases are the unskipped FIX-LANE-TERMINAL-FINALIZE contracts from
// tests/src/App/RecomputePublicationContracts.cpp on feat/parallel-recompute-m0.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentObject.h>
#include <App/DocumentPresentationBoundary.h>
#include <App/DocumentRecomputeCoordinator.h>
#include <App/FeatureTest.h>
#include <App/RecomputeHandle.h>
#include <Base/Parameter.h>
#include <fastsignals/connection.h>

#include <src/App/InitApplication.h>

using namespace App;

namespace {

constexpr auto kTimeout = std::chrono::seconds(30);
// Cleanup waits use their own budget. When it expires the guard reports and
// returns while the lane may still be inside a callback, so that callback's
// state has to outlive this stack frame.
constexpr auto kCleanupTimeout = kTimeout;

struct FineGrainedPreference
{
    Base::Reference<ParameterGrp> group;
    bool previous {false};

    explicit FineGrainedPreference(bool enabled)
        : group(GetApplication().GetParameterGroupByPath(
            "User parameter:BaseApp/Preferences/Document"))
        , previous(group->GetBool("FineGrainedRecompute", true))
    {
        group->SetBool("FineGrainedRecompute", enabled);
    }

    ~FineGrainedPreference()
    {
        group->SetBool("FineGrainedRecompute", previous);
    }
};

std::string objectName(const DocumentObject& object)
{
    const char* name = object.getNameInDocument();
    return name ? name : std::string(object.getTypeId().getName());
}

std::string joinTrace(const std::vector<std::string>& trace)
{
    std::string text;
    for (const auto& event : trace) {
        text += event;
        text += '\n';
    }
    return text;
}

std::vector<std::size_t> positions(const std::vector<std::string>& trace, const std::string& event)
{
    std::vector<std::size_t> found;
    for (std::size_t index = 0; index < trace.size(); ++index) {
        if (trace[index] == event) {
            found.push_back(index);
        }
    }
    return found;
}

void expectBefore(const std::vector<std::string>& trace,
                  const std::string& earlier,
                  const std::string& later)
{
    const auto earlierHits = positions(trace, earlier);
    const auto laterHits = positions(trace, later);
    ASSERT_FALSE(earlierHits.empty()) << earlier << "\n" << joinTrace(trace);
    ASSERT_FALSE(laterHits.empty()) << later << "\n" << joinTrace(trace);
    EXPECT_LT(earlierHits.front(), laterHits.front()) << joinTrace(trace);
}

class SignalTrace
{
public:
    // Slots capture this, not the SignalTrace. A cleanup timeout destroys the
    // trace while an in-flight slot may still append.
    std::shared_ptr<std::vector<std::string>> storage {
        std::make_shared<std::vector<std::string>>()};
    std::vector<std::string>& events {*storage};

    void watch(Document& document)
    {
        auto trace = storage;
        connections.emplace_back(document.signalRecomputedObject.connect(
            [trace](const DocumentObject& object) {
                trace->push_back("recomputedObject:" + objectName(object));
            }));
        connections.emplace_back(document.signalRecomputed.connect(
            [trace](const Document&, const std::vector<DocumentObject*>& objects) {
                std::string line = "recomputed:";
                for (std::size_t index = 0; index < objects.size(); ++index) {
                    if (index != 0) {
                        line += ",";
                    }
                    line += objects[index] ? objectName(*objects[index]) : "?";
                }
                trace->push_back(std::move(line));
            }));
        connections.emplace_back(document.signalBecameStable.connect(
            [trace](const Document&) { trace->emplace_back("becameStable"); }));
    }

private:
    // Disconnects in this destructor. The slot's shared_ptr keeps storage alive
    // for a callback that has already been copied onto the lane thread.
    std::vector<::fastsignals::scoped_connection> connections;
};

const DocumentRecomputeFeatureSnapshot& featureNode(const DocumentRecomputeSnapshot& snapshot,
                                                     const char* name)
{
    const auto found = std::find_if(
        snapshot.features.begin(), snapshot.features.end(), [&](const auto& feature) {
            return feature.featureId == name;
        });
    if (found == snapshot.features.end()) {
        throw std::runtime_error(std::string("recompute snapshot omitted ") + name);
    }
    return *found;
}

struct PresentationState
{
    std::atomic<int> calls {0};
    std::shared_ptr<std::vector<std::string>> trace;
    std::shared_ptr<std::atomic<int>> publishedCalls;
};

struct PresentationSlot
{
    std::mutex mutex;
    std::shared_ptr<PresentationState> state;
};

PresentationSlot& presentationSlot()
{
    // Process lifetime: the installed hook must not be destroyed while the
    // lane thread is inside it. The slot's shared state is what comes and goes.
    static auto* slot = new PresentationSlot();
    return *slot;
}

void presentationTrampoline(Document&)
{
    std::shared_ptr<PresentationState> state;
    {
        std::lock_guard<std::mutex> lock(presentationSlot().mutex);
        state = presentationSlot().state;
    }
    if (!state) {
        return;
    }
    state->calls.fetch_add(1, std::memory_order_relaxed);
    if (state->publishedCalls) {
        state->publishedCalls->fetch_add(1, std::memory_order_release);
    }
    if (state->trace) {
        state->trace->emplace_back("presentation");
    }
}

void clearPresentationHook()
{
    setDocumentPresentationBoundaryCallback(nullptr);
    std::lock_guard<std::mutex> lock(presentationSlot().mutex);
    presentationSlot().state.reset();
}

class PresentationProbe
{
public:
    explicit PresentationProbe(std::shared_ptr<std::vector<std::string>> trace,
                               std::shared_ptr<std::atomic<int>> publishedCalls = nullptr)
        : _state(std::make_shared<PresentationState>())
    {
        _state->trace = std::move(trace);
        _state->publishedCalls = std::move(publishedCalls);
        {
            std::lock_guard<std::mutex> lock(presentationSlot().mutex);
            presentationSlot().state = _state;
        }
        setDocumentPresentationBoundaryCallback(&presentationTrampoline);
    }

    ~PresentationProbe()
    {
        // Drop the shared state. An in-flight trampoline already holds its own
        // copy. The function object stays installed until the lane thread has
        // been joined; replacing it from this destructor races that call.
        std::lock_guard<std::mutex> lock(presentationSlot().mutex);
        if (presentationSlot().state == _state) {
            presentationSlot().state.reset();
        }
    }

    PresentationProbe(const PresentationProbe&) = delete;
    PresentationProbe& operator=(const PresentationProbe&) = delete;

    int calls() const
    {
        return _state->calls.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<PresentationState> _state;
};

struct CancelRendezvous
{
    std::mutex mutex;
    std::condition_variable ready;
    bool observed {false};
    bool released {false};
    bool abort {false};
    std::string timeout;

    void waitForPublication(const DocumentObject& object, const Property& property)
    {
        const char* name = object.getNameInDocument();
        if (!name || std::string(name) != "A" || std::string(property.getName()) != "ExecCount") {
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (observed) {
            return;
        }
        observed = true;
        ready.notify_all();
        if (!ready.wait_for(lock, kTimeout, [&] { return released || abort; })) {
            if (timeout.empty()) {
                timeout = "cancel handshake timed out after 30s waiting for cancel() to release "
                          "the ExecCount observer";
            }
        }
    }
};

struct JoinCanceller
{
    std::thread thread;
    std::shared_ptr<CancelRendezvous> gate;

    JoinCanceller(std::thread threadIn, std::shared_ptr<CancelRendezvous> gateIn)
        : thread(std::move(threadIn))
        , gate(std::move(gateIn))
    {}

    ~JoinCanceller()
    {
        if (gate) {
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->abort = true;
            gate->released = true;
            gate->ready.notify_all();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    JoinCanceller(const JoinCanceller&) = delete;
    JoinCanceller& operator=(const JoinCanceller&) = delete;
};

template<typename WaitFn, typename CancelFn>
void rendezvousCancel(Document& document, WaitFn&& waitFn, CancelFn&& cancelFn)
{
    auto gate = std::make_shared<CancelRendezvous>();
    ::fastsignals::scoped_connection connection = document.signalChangedObject.connect(
        [gate](const DocumentObject& object, const Property& property) {
            gate->waitForPublication(object, property);
        });
    JoinCanceller joiner {
        std::thread([gate, cancelFn = std::forward<CancelFn>(cancelFn)]() mutable {
            bool shouldCancel = false;
            {
                std::unique_lock<std::mutex> lock(gate->mutex);
                if (!gate->ready.wait_for(lock, kTimeout, [&] {
                        return gate->observed || gate->abort;
                    })) {
                    if (gate->timeout.empty()) {
                        gate->timeout =
                            "cancel handshake timed out after 30s waiting for A's ExecCount "
                            "publication";
                    }
                }
                else if (gate->observed && !gate->abort) {
                    shouldCancel = true;
                }
            }
            if (shouldCancel) {
                try {
                    std::forward<CancelFn>(cancelFn)();
                }
                catch (...) {
                    gate->timeout = "cancel() raised";
                }
            }
            std::lock_guard<std::mutex> lock(gate->mutex);
            gate->released = true;
            gate->ready.notify_all();
        }),
        gate};
    std::forward<WaitFn>(waitFn)();
    ASSERT_TRUE(gate->timeout.empty()) << gate->timeout;
    static_cast<void>(connection);
    static_cast<void>(joiner);
}

DocumentCommandHandle submitLaneRecompute(Document& document, const char* key)
{
    auto execution = document.executionHandle();
    DocumentCommand command;
    command.kind = DocumentCommandKind::Recompute;
    command.document = execution.identity();
    command.recompute = DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = key;
    const auto outcome = execution.trySubmit(std::move(command));
    if (outcome.result != DocumentCommandSubmitResult::Accepted) {
        throw std::runtime_error(std::string("lane recompute was not accepted: ")
                                 + outcome.diagnostic);
    }
    return DocumentCommandHandle(outcome.commandId, execution.identity());
}

DocumentCommandSnapshot waitForLaneCommand(DocumentCommandHandle& handle)
{
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    auto snapshot = handle.status();
    while (!snapshot.terminal()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string("lane recompute timed out in state ")
                                     + documentCommandStateName(snapshot.state) + ": "
                                     + snapshot.diagnostic);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        snapshot = handle.status();
    }
    return snapshot;
}

// Samples command status from another thread and from inside signalRecomputed.
// The signal check is the deterministic ordering probe: it runs before
// completeActiveCommand, so a terminal status there means publication raced
// ahead of finalization.
//
// The signal slot captures state, not the TerminalWitness. Cleanup can destroy
// the witness while that slot is still on the lane thread.
struct WitnessState
{
    std::shared_ptr<std::atomic<int>> presentationCallsShared {
        std::make_shared<std::atomic<int>>(0)};
    std::atomic<int>& presentationCalls {*presentationCallsShared};
    std::atomic<int> recomputedCount {0};
    std::atomic<bool> commandTerminalDuringSignal {false};
    bool resubmit {false};
    bool resubmitAttempted {false};
    DocumentCommandSubmitResult resubmitRecompute {DocumentCommandSubmitResult::Closed};
    DocumentCommandSubmitResult resubmitClose {DocumentCommandSubmitResult::Closed};
    int callsAtTerminal {-1};
    int recomputedAtTerminal {-1};
    bool sawTerminal {false};
    std::mutex mutex;
    std::condition_variable published;
    bool ready {false};
    DocumentCommandHandle command;
    std::atomic<bool> stop {false};

    void bind(DocumentCommandHandle bound)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            command = std::move(bound);
            ready = true;
        }
        published.notify_all();
    }

    DocumentCommandHandle waitForCommand()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!published.wait_for(lock, kTimeout, [&] { return ready; })) {
            return {};
        }
        return command;
    }

    void onRecomputed(Document& document)
    {
        DocumentCommandHandle local = waitForCommand();
        if (resubmit && !resubmitAttempted) {
            resubmitAttempted = true;
            auto execution = document.executionHandle();
            DocumentCommand again;
            again.kind = DocumentCommandKind::Recompute;
            again.document = execution.identity();
            again.recompute = DocumentCommandRecomputePayload {};
            again.recompute->coalescingKey = "lane-reenter-from-signal";
            resubmitRecompute = execution.trySubmit(std::move(again)).result;

            DocumentCommand closeCommand;
            closeCommand.kind = DocumentCommandKind::Close;
            closeCommand.document = execution.identity();
            resubmitClose = execution.trySubmit(std::move(closeCommand)).result;
        }
        if (local.valid() && local.status().terminal()) {
            commandTerminalDuringSignal.store(true, std::memory_order_release);
        }
        recomputedCount.fetch_add(1, std::memory_order_release);
    }

    void watch()
    {
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        const auto local = waitForCommand();
        while (!stop.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            if (local.valid() && local.status().terminal()) {
                callsAtTerminal = presentationCalls.load(std::memory_order_acquire);
                recomputedAtTerminal = recomputedCount.load(std::memory_order_acquire);
                sawTerminal = true;
                return;
            }
            std::this_thread::yield();
        }
    }
};

class TerminalWitness
{
public:
    std::shared_ptr<WitnessState> state {std::make_shared<WitnessState>()};
    std::atomic<int>& presentationCalls {state->presentationCalls};
    std::atomic<int>& recomputedCount {state->recomputedCount};
    std::atomic<bool>& commandTerminalDuringSignal {state->commandTerminalDuringSignal};
    bool& resubmit {state->resubmit};
    bool& resubmitAttempted {state->resubmitAttempted};
    DocumentCommandSubmitResult& resubmitRecompute {state->resubmitRecompute};
    DocumentCommandSubmitResult& resubmitClose {state->resubmitClose};
    int& callsAtTerminal {state->callsAtTerminal};
    int& recomputedAtTerminal {state->recomputedAtTerminal};
    bool& sawTerminal {state->sawTerminal};

    void start()
    {
        auto watched = state;
        watcher = std::thread([watched] { watched->watch(); });
    }

    void bind(DocumentCommandHandle bound)
    {
        state->bind(std::move(bound));
    }

    void join()
    {
        if (watcher.joinable()) {
            watcher.join();
        }
    }

    ~TerminalWitness()
    {
        state->stop.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->ready = true;
        }
        state->published.notify_all();
        join();
    }

    TerminalWitness(const TerminalWitness&) = delete;
    TerminalWitness& operator=(const TerminalWitness&) = delete;
    TerminalWitness() = default;

private:
    std::thread watcher;
};

// Reports and returns when the cleanup budget expires. Callers must already
// have given every callback its own shared state: this does not keep waiting
// until those callbacks have left the stack frame.
void drainLaneCommand(DocumentCommandHandle command, const char* what)
{
    if (!command.valid()) {
        return;
    }
    try {
        const auto deadline = std::chrono::steady_clock::now() + kCleanupTimeout;
        auto snapshot = command.status();
        while (!snapshot.terminal()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                ADD_FAILURE() << what << " timed out in state "
                              << documentCommandStateName(snapshot.state) << ": "
                              << snapshot.diagnostic;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            snapshot = command.status();
        }
    }
    catch (const std::exception& exception) {
        ADD_FAILURE() << what << " failed: " << exception.what();
    }
    catch (...) {
        ADD_FAILURE() << what << " failed with an unknown exception";
    }
}

struct CommandDrain
{
    DocumentCommandHandle command;

    ~CommandDrain()
    {
        drainLaneCommand(command, "lane command cleanup");
    }
};

void expectSynchronizedTerminal(const TerminalWitness& witness)
{
    EXPECT_FALSE(witness.commandTerminalDuringSignal.load(std::memory_order_acquire))
        << "command status was already terminal inside signalRecomputed";
    EXPECT_TRUE(witness.sawTerminal);
    EXPECT_EQ(witness.callsAtTerminal, 1);
    EXPECT_EQ(witness.recomputedAtTerminal, 1);
}

void expectFailureFinalization(const SignalTrace& trace, int presentationCalls)
{
    EXPECT_EQ(presentationCalls, 1);
    EXPECT_EQ(positions(trace.events, "presentation").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Keep").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Other").size(), 1U)
        << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Bad").size(), 0U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Down").size(), 0U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputed:Bad,Down,Keep,Other").size(), 1U)
        << joinTrace(trace.events);
    EXPECT_TRUE(positions(trace.events, "becameStable").empty()) << joinTrace(trace.events);
    expectBefore(trace.events, "recomputedObject:Keep", "recomputedObject:Other");
    expectBefore(trace.events, "recomputedObject:Other", "presentation");
    expectBefore(trace.events, "presentation", "recomputed:Bad,Down,Keep,Other");
}

void expectCancelFinalization(const SignalTrace& trace, int presentationCalls)
{
    EXPECT_EQ(presentationCalls, 1);
    EXPECT_EQ(positions(trace.events, "presentation").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:A").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:B").size(), 0U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputed:A,B").size(), 1U) << joinTrace(trace.events);
    EXPECT_TRUE(positions(trace.events, "becameStable").empty()) << joinTrace(trace.events);
    expectBefore(trace.events, "recomputedObject:A", "presentation");
    expectBefore(trace.events, "presentation", "recomputed:A,B");
}

DocumentCommandSnapshot runLaneCancel(Document& document,
                                       const char* key,
                                       TerminalWitness* witness)
{
    auto gate = std::make_shared<CancelRendezvous>();
    ::fastsignals::scoped_connection connection = document.signalChangedObject.connect(
        [gate](const DocumentObject& object, const Property& property) {
            gate->waitForPublication(object, property);
        });
    // The handle is declared first so ~Release runs while it is still alive.
    // Release owns a shared gate and a command copy. A cleanup timeout reports
    // and returns; the slot captured the gate shared_ptr, so the frame can go.
    DocumentCommandHandle command;
    struct Release
    {
        std::shared_ptr<CancelRendezvous> gate;
        DocumentCommandHandle command;

        ~Release()
        {
            if (gate) {
                std::lock_guard<std::mutex> lock(gate->mutex);
                gate->released = true;
                gate->abort = true;
                gate->ready.notify_all();
            }
            drainLaneCommand(command, "lane cancel cleanup");
        }
    } release {gate, {}};
    command = submitLaneRecompute(document, key);
    release.command = command;
    if (witness) {
        witness->bind(command);
    }
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        if (!gate->ready.wait_for(lock, kTimeout, [&] { return gate->observed || gate->abort; })) {
            if (gate->timeout.empty()) {
                gate->timeout =
                    "cancel handshake timed out after 30s waiting for A's ExecCount publication";
            }
        }
    }
    if (!gate->timeout.empty()) {
        throw std::runtime_error(gate->timeout);
    }
    if (!command.cancel("cancelled mid-way")) {
        throw std::runtime_error("lane cancel() returned false");
    }
    {
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->released = true;
        gate->ready.notify_all();
    }
    auto snapshot = waitForLaneCommand(command);
    if (!gate->timeout.empty()) {
        throw std::runtime_error(gate->timeout);
    }
    static_cast<void>(connection);
    return snapshot;
}

}  // namespace

class LaneTerminalFinalize: public ::testing::Test
{
protected:
    void SetUp() override
    {
        tests::initApplication();
        fineGrained.emplace(false);
        documentName = GetApplication().getUniqueDocumentName("LaneTerminalFinalize");
        document = GetApplication().newDocument(documentName.c_str(), "test");
        ASSERT_NE(document, nullptr);
        ASSERT_NE(document->executionLane(), nullptr);
    }

    void TearDown() override
    {
        if (document && GetApplication().getDocument(documentName.c_str()) == document) {
            GetApplication().closeDocument(documentName.c_str());
            document = nullptr;
        }
        // closeDocument joins the lane thread, so the presentation hook is no
        // longer running and can be cleared.
        clearPresentationHook();
        fineGrained.reset();
    }

    FeatureTest& feature(const char* name)
    {
        auto* created = document->addObject<FeatureTest>(name);
        if (!created) {
            throw std::runtime_error(std::string("failed to add FeatureTest ") + name);
        }
        return *created;
    }

    void settle()
    {
        const auto snapshot = document->recomputeAsync()->wait(kTimeout);
        if (snapshot.state != DocumentRecomputeState::Completed) {
            throw std::runtime_error(snapshot.diagnostic.empty()
                                         ? "initial recompute did not complete"
                                         : snapshot.diagnostic);
        }
    }

    std::optional<FineGrainedPreference> fineGrained;
    Document* document {nullptr};
    std::string documentName;
};

TEST_F(LaneTerminalFinalize, handlePartialFailureFinalizesCommittedSiblingsAndPresentation)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    auto& other = feature("Other");
    blocked.Source1.setValue(&bad);
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    blocked.touch();
    other.touch();
    const auto keptCount = kept.ExecCount.getValue();
    SignalTrace trace;
    trace.watch(*document);
    PresentationProbe probe(trace.storage);
    const auto snapshot = document->recomputeAsync()->wait(kTimeout);
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::PartialFailure) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "Keep").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "Other").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "Bad").state, DocumentRecomputeFeatureState::Failed);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_TRUE(bad.isError());
    const char* diagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("Test Exception"), std::string::npos);
    expectFailureFinalization(trace, probe.calls());
}

TEST_F(LaneTerminalFinalize, lanePartialFailureFinalizesCommittedSiblingsAndPresentation)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    auto& blocked = feature("Down");
    auto& other = feature("Other");
    blocked.Source1.setValue(&bad);
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    blocked.touch();
    other.touch();
    const auto keptCount = kept.ExecCount.getValue();
    TerminalWitness witness;
    witness.start();
    SignalTrace trace;
    trace.watch(*document);
    Document* const observed = document;
    ::fastsignals::scoped_connection recomputed = document->signalRecomputed.connect(
        [state = witness.state, observed](const Document&, const std::vector<DocumentObject*>&) {
            state->onRecomputed(*observed);
        });
    PresentationProbe probe(trace.storage, witness.state->presentationCallsShared);
    auto command = submitLaneRecompute(*document, "lane-partial-failure");
    CommandDrain drain {command};
    witness.bind(command);
    const auto snapshot = waitForLaneCommand(command);
    witness.join();
    expectSynchronizedTerminal(witness);
    static_cast<void>(recomputed);
    static_cast<void>(drain);
    EXPECT_EQ(snapshot.state, DocumentCommandState::Failed) << snapshot.diagnostic;
    ASSERT_TRUE(snapshot.recompute);
    EXPECT_EQ(snapshot.recompute->state, DocumentCommandRecomputeState::PartialFailure);
    EXPECT_EQ(kept.ExecCount.getValue(), keptCount + 1);
    EXPECT_TRUE(bad.isError());
    const char* diagnostic = document->getErrorDescription(&bad);
    ASSERT_NE(diagnostic, nullptr);
    EXPECT_NE(std::string(diagnostic).find("Test Exception"), std::string::npos);
    expectFailureFinalization(trace, probe.calls());
}

TEST_F(LaneTerminalFinalize, handleCancelFinalizesCommittedPrefixAndPresentation)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    const auto firstCount = first.ExecCount.getValue();
    const auto secondCount = second.ExecCount.getValue();
    SignalTrace trace;
    trace.watch(*document);
    PresentationProbe probe(trace.storage);
    DocumentRecomputeSnapshot snapshot;
    auto handle = document->recomputeAsync();
    rendezvousCancel(
        *document,
        [&] { snapshot = handle->wait(kTimeout); },
        [&] {
            if (!handle->cancel("cancelled mid-way")) {
                throw std::runtime_error("recompute cancel() returned false");
            }
        });
    ASSERT_EQ(snapshot.state, DocumentRecomputeState::Cancelled) << snapshot.diagnostic;
    EXPECT_EQ(featureNode(snapshot, "A").state, DocumentRecomputeFeatureState::Committed);
    EXPECT_EQ(featureNode(snapshot, "B").state, DocumentRecomputeFeatureState::Cancelled);
    EXPECT_EQ(first.ExecCount.getValue(), firstCount + 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    expectCancelFinalization(trace, probe.calls());
}

TEST_F(LaneTerminalFinalize, laneCancelFinalizesCommittedPrefixAndPresentation)
{
    auto& first = feature("A");
    auto& second = feature("B");
    settle();
    first.touch();
    second.touch();
    const auto firstCount = first.ExecCount.getValue();
    const auto secondCount = second.ExecCount.getValue();
    TerminalWitness witness;
    witness.start();
    SignalTrace trace;
    trace.watch(*document);
    Document* const observed = document;
    ::fastsignals::scoped_connection recomputed = document->signalRecomputed.connect(
        [state = witness.state, observed](const Document&, const std::vector<DocumentObject*>&) {
            state->onRecomputed(*observed);
        });
    PresentationProbe probe(trace.storage, witness.state->presentationCallsShared);
    const auto snapshot = runLaneCancel(*document, "lane-cancel-finalization", &witness);
    witness.join();
    expectSynchronizedTerminal(witness);
    static_cast<void>(recomputed);
    EXPECT_EQ(snapshot.state, DocumentCommandState::Cancelled) << snapshot.diagnostic;
    ASSERT_TRUE(snapshot.recompute);
    EXPECT_EQ(snapshot.recompute->state, DocumentCommandRecomputeState::Cancelled);
    EXPECT_EQ(first.ExecCount.getValue(), firstCount + 1);
    EXPECT_EQ(second.ExecCount.getValue(), secondCount);
    EXPECT_TRUE(second.isTouched());
    expectCancelFinalization(trace, probe.calls());
}

TEST_F(LaneTerminalFinalize, laneAndHandleWaitPublishPartialFailureOnce)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    SignalTrace trace;
    trace.watch(*document);
    PresentationProbe probe(trace.storage);
    auto command = submitLaneRecompute(*document, "lane-and-handle-once");
    CommandDrain drain {command};
    const auto laneSnapshot = waitForLaneCommand(command);
    static_cast<void>(drain);
    EXPECT_EQ(laneSnapshot.state, DocumentCommandState::Failed) << laneSnapshot.diagnostic;
    ASSERT_TRUE(laneSnapshot.recompute);
    ASSERT_NE(laneSnapshot.recompute->id, 0U);
    // The lane must already have published. A following handle wait shares the
    // one-shot claim and must not publish again. Waiting while the lane command
    // is still active nests inside the owner pump and observes a stale snapshot.
    const int callsAfterLane = probe.calls();
    RecomputeHandle handle(
        *document, static_cast<DocumentRecomputeId>(laneSnapshot.recompute->id));
    const auto handleSnapshot = handle.wait(kTimeout);
    EXPECT_EQ(handleSnapshot.state, DocumentRecomputeState::PartialFailure)
        << handleSnapshot.diagnostic;
    EXPECT_EQ(callsAfterLane, 1);
    EXPECT_EQ(probe.calls(), 1);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Keep").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Bad").size(), 0U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputed:Bad,Keep").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "presentation").size(), 1U) << joinTrace(trace.events);
}

TEST_F(LaneTerminalFinalize, lanePartialFailureObserverResubmitDoesNotRepublish)
{
    auto& kept = feature("Keep");
    auto& bad = feature("Bad");
    settle();
    bad.ExceptionType.setValue(1);
    kept.touch();
    bad.touch();
    TerminalWitness witness;
    witness.resubmit = true;
    witness.start();
    SignalTrace trace;
    trace.watch(*document);
    Document* const observed = document;
    ::fastsignals::scoped_connection recomputed = document->signalRecomputed.connect(
        [state = witness.state, observed](const Document&, const std::vector<DocumentObject*>&) {
            state->onRecomputed(*observed);
        });
    PresentationProbe probe(trace.storage, witness.state->presentationCallsShared);
    auto command = submitLaneRecompute(*document, "lane-reenter-outer");
    CommandDrain drain {command};
    witness.bind(command);
    const auto snapshot = waitForLaneCommand(command);
    witness.join();
    EXPECT_EQ(snapshot.state, DocumentCommandState::Failed) << snapshot.diagnostic;
    ASSERT_TRUE(snapshot.recompute);
    EXPECT_EQ(snapshot.recompute->state, DocumentCommandRecomputeState::PartialFailure);
    EXPECT_TRUE(witness.resubmitAttempted);
    EXPECT_EQ(witness.resubmitRecompute, DocumentCommandSubmitResult::Busy);
    EXPECT_EQ(witness.resubmitClose, DocumentCommandSubmitResult::Busy);
    expectSynchronizedTerminal(witness);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Keep").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputedObject:Bad").size(), 0U) << joinTrace(trace.events);
    EXPECT_EQ(positions(trace.events, "recomputed:Bad,Keep").size(), 1U) << joinTrace(trace.events);
    EXPECT_EQ(probe.calls(), 1);
    static_cast<void>(recomputed);
    static_cast<void>(drain);
}
