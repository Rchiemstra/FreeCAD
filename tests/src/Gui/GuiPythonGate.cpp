// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentWouldBlock.h>
#include <App/MainThreadSignal.h>
#include <Base/Interpreter.h>
#include <Gui/GuiPythonGate.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace
{

bool testIsMainThread()
{
    return true;
}

void testInvokeOnMain(std::function<void()>&& fn, bool /*blocking*/)
{
    fn();
}

class GuiPythonGateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tests::initApplication();
        App::MainThreadSignalConfig::setHooks(&testIsMainThread, &testInvokeOnMain);
        _document = App::GetApplication().newDocument("GuiPythonGate");
        ASSERT_NE(_document, nullptr);
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument("GuiPythonGate");
        App::MainThreadSignalConfig::setHooks(nullptr, nullptr);
    }

    App::Document* _document {nullptr};
};

// Holds the GIL on a worker thread until released. The constructor waits until
// the worker really owns the GIL; otherwise tryAdmit() could run first, find
// the GIL free and admit the callback (flaky under CI load).
class GilHolderThread
{
public:
    GilHolderThread()
        : _thread([this] {
            Base::PyGILStateLocker lock;
            _held.store(true, std::memory_order_release);
            while (!_release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
        })
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!holding() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
    }

    ~GilHolderThread()
    {
        release();
    }

    GilHolderThread(const GilHolderThread&) = delete;
    GilHolderThread& operator=(const GilHolderThread&) = delete;

    bool holding() const
    {
        return _held.load(std::memory_order_acquire);
    }

    void release()
    {
        _release.store(true, std::memory_order_release);
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    std::atomic<bool> _held {false};
    std::atomic<bool> _release {false};
    std::thread _thread;  // last: the flags must exist before the worker starts
};

}  // namespace

TEST_F(GuiPythonGateTest, NonModelCallbackQueuesWhenGilIsHeldElsewhere)
{
    std::atomic<bool> ran {false};
    GilHolderThread holder;
    ASSERT_TRUE(holder.holding());

    const auto outcome = Gui::GuiPythonGate::tryAdmit(
        Gui::GuiPythonGateCallbackKind::NonModel,
        [&ran] { ran.store(true, std::memory_order_relaxed); });
    EXPECT_TRUE(outcome.queued());

    holder.release();

    EXPECT_GE(Gui::GuiPythonGate::pumpQueuedCallbacks(), 1U);
    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
}

TEST_F(GuiPythonGateTest, ModelTouchingRejectsWhenGilIsBusy)
{
    GilHolderThread holder;
    ASSERT_TRUE(holder.holding());

    const auto outcome = Gui::GuiPythonGate::tryAdmit(
        Gui::GuiPythonGateCallbackKind::ModelTouching,
        [] {},
        _document);
    EXPECT_EQ(outcome.result, Gui::GuiPythonGateAdmissionResult::RejectedGilBusy);
}

TEST_F(GuiPythonGateTest, ClosePolicyNeverTerminatesLaneThread)
{
    EXPECT_TRUE(App::DocumentExecutionClosePolicy::laneThreadTerminationIsForbidden());
    EXPECT_EQ(App::DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(false),
              App::DocumentExecutionClosePolicy::UnresponsiveLaneAction::KeepWaiting);
    EXPECT_EQ(App::DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(true),
              App::DocumentExecutionClosePolicy::UnresponsiveLaneAction::RequestProcessExit);
    EXPECT_NE(App::DocumentExecutionClosePolicy::unresponsiveLaneGuidance(), nullptr);
}

TEST_F(GuiPythonGateTest, ObserverEventsEnqueueWithoutLiveModelReferences)
{
    Gui::GuiPythonObserverValueEvent event;
    event.document = *_document->collaborationRevisions().documentIdentity();
    event.observerToken = "observer-1";
    event.eventKind = "ObjectCreated";
    event.payloadJson = "{\"name\":\"Box\"}";
    Gui::GuiPythonGate::enqueueObserverValueEvent(std::move(event));
    EXPECT_GE(Gui::GuiPythonGate::pumpQueuedCallbacks(), 1U);
}
