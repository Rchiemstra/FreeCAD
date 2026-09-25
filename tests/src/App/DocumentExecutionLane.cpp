// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentHandle.h>
#include <App/FeatureTest.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace
{

class DocumentExecutionLaneTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("lane-test");
        _doc = App::GetApplication().newDocument(_docName.c_str(), "testUser");
    }

    void TearDown() override
    {
        App::GetApplication().closeDocument(_docName.c_str());
    }

    App::Document* doc() const noexcept
    {
        return _doc;
    }

    App::DocumentCommand makeRecomputeCommand(const std::string& coalescingKey) const
    {
        App::DocumentCommand command;
        command.kind = App::DocumentCommandKind::Recompute;
        command.document = doc()->executionHandle().identity();
        command.recompute = App::DocumentCommandRecomputePayload {};
        command.recompute->coalescingKey = coalescingKey;
        return command;
    }

private:
    std::string _docName;
    App::Document* _doc {};
};

TEST_F(DocumentExecutionLaneTest, OwnerThreadIsLaneThread)
{
    const auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_FALSE(doc()->isCollaborationOwnerThread());
    lane->dispatchToOwner([&] {
        EXPECT_TRUE(doc()->isCollaborationOwnerThread());
        EXPECT_EQ(std::this_thread::get_id(), lane->ownerThreadId());
    });
}

TEST_F(DocumentExecutionLaneTest, TrySubmitAcceptsRecompute)
{
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("lane-test-recompute"));
    EXPECT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_NE(outcome.commandId, 0U);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    App::DocumentCommandSnapshot snapshot;
    do {
        snapshot = commandHandle.status();
        if (snapshot.terminal()) {
            break;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);

    EXPECT_TRUE(snapshot.terminal());
}

TEST_F(DocumentExecutionLaneTest, IdenticalRecomputeSharesHandle)
{
    auto handle = doc()->executionHandle();
    const auto first = handle.trySubmit(makeRecomputeCommand("shared-recompute"));
    ASSERT_EQ(first.result, App::DocumentCommandSubmitResult::Accepted);

    const auto second = handle.trySubmit(makeRecomputeCommand("shared-recompute"));
    EXPECT_EQ(second.result, App::DocumentCommandSubmitResult::Accepted);
    EXPECT_EQ(second.commandId, first.commandId);
}

TEST_F(DocumentExecutionLaneTest, EditReturnsBusyWhileRecomputeActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto accepted = handle.trySubmit(makeRecomputeCommand("busy-edit-test"));
    ASSERT_EQ(accepted.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommand edit;
    edit.kind = App::DocumentCommandKind::Edit;
    edit.document = handle.identity();
    const auto busy = handle.trySubmit(std::move(edit));
    EXPECT_EQ(busy.result, App::DocumentCommandSubmitResult::Busy);
}

TEST_F(DocumentExecutionLaneTest, StatusObservationReturnsStableIdentity)
{
    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("status-observation"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    const auto first = commandHandle.status();
    const auto second = commandHandle.status();
    EXPECT_EQ(first.id, second.id);
    EXPECT_EQ(first.document, second.document);
    EXPECT_NE(first.state, App::DocumentCommandState::Failed);
}

TEST_F(DocumentExecutionLaneTest, CancelRemainsCallableWhileActive)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneCancel"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("cancel-test"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("test cancellation"));
    const auto snapshot = commandHandle.status();
    EXPECT_TRUE(snapshot.state == App::DocumentCommandState::Cancelling
                || snapshot.state == App::DocumentCommandState::Cancelled
                || snapshot.state == App::DocumentCommandState::Failed);
}

TEST_F(DocumentExecutionLaneTest, CloseDocumentReturnsFalseWhileBusy)
{
    auto* feature = dynamic_cast<App::FeatureTest*>(
        doc()->addObject("App::FeatureTest", "LaneCloseBusy"));
    ASSERT_NE(feature, nullptr);
    feature->touch();

    auto handle = doc()->executionHandle();
    const auto outcome = handle.trySubmit(makeRecomputeCommand("close-busy"));
    ASSERT_EQ(outcome.result, App::DocumentCommandSubmitResult::Accepted);

    EXPECT_FALSE(App::GetApplication().closeDocument(_docName.c_str()));

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    EXPECT_TRUE(commandHandle.cancel("allow teardown"));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!doc()->executionLane()->isIdle()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(doc()->executionLane()->isIdle());
}

}  // namespace
