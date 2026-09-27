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

}  // namespace

TEST_F(GuiPythonGateTest, NonModelCallbackQueuesWhenGilIsHeldElsewhere)
{
    std::atomic<bool> holdGil {true};
    std::atomic<bool> ran {false};
    std::thread holder([&holdGil] {
        Base::PyGILStateLocker lock;
        while (holdGil.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(1ms);
        }
    });

    const auto outcome = Gui::GuiPythonGate::tryAdmit(
        Gui::GuiPythonGateCallbackKind::NonModel,
        [&ran] { ran.store(true, std::memory_order_relaxed); });
    EXPECT_TRUE(outcome.queued());

    holdGil.store(false, std::memory_order_relaxed);
    holder.join();

    EXPECT_GE(Gui::GuiPythonGate::pumpQueuedCallbacks(), 1U);
    EXPECT_TRUE(ran.load(std::memory_order_relaxed));
}

TEST_F(GuiPythonGateTest, ModelTouchingRejectsWhenGilIsBusy)
{
    std::atomic<bool> holdGil {true};
    std::thread holder([&holdGil] {
        Base::PyGILStateLocker lock;
        while (holdGil.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(1ms);
        }
    });

    const auto outcome = Gui::GuiPythonGate::tryAdmit(
        Gui::GuiPythonGateCallbackKind::ModelTouching,
        [] {},
        _document);
    EXPECT_EQ(outcome.result, Gui::GuiPythonGateAdmissionResult::RejectedGilBusy);

    holdGil.store(false, std::memory_order_relaxed);
    holder.join();
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
