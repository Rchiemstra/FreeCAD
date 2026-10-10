// SPDX-License-Identifier: LGPL-2.1-or-later

#include <gtest/gtest.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentExecutionStall.h>
#include <src/App/InitApplication.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;

namespace App::Internal
{

class DocumentExecutionLaneTestAccess
{
public:
    static bool isCollaborationOwnerThread(const Document& document) noexcept
    {
        return document.isCollaborationOwnerThread();
    }
};

}  // namespace App::Internal

namespace
{

class DocumentExecutionStallTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        tests::initApplication();
    }

    void SetUp() override
    {
        _docName = App::GetApplication().getUniqueDocumentName("stall-test");
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

private:
    std::string _docName;
    App::Document* _doc {};
};

TEST_F(DocumentExecutionStallTest, RunRecordsDocumentOwnerThread)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    EXPECT_FALSE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
        *doc()));

    std::stop_source stopSource;
    std::optional<App::DocumentExecutionStall::Result> result;
    std::thread::id ownerThread {};

    std::jthread canceller([&](const std::stop_token stopToken) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!stopToken.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            if (App::DocumentExecutionStall::isActive()) {
                ownerThread = App::DocumentExecutionStall::activeThread();
                stopSource.request_stop();
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        ADD_FAILURE() << "document-owner stall did not become active";
        stopSource.request_stop();
    });

    lane->dispatchToOwner([&] {
        EXPECT_TRUE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
            *doc()));
        result = App::DocumentExecutionStall::run(stopSource.get_token(), 30s);
    });

    ASSERT_TRUE(result.has_value());
    ASSERT_NE(ownerThread, std::thread::id {});
    EXPECT_EQ(result->executingThread, ownerThread);
    EXPECT_EQ(result->executingThread, lane->ownerThreadId());
    EXPECT_FALSE(result->completed);
    EXPECT_LT(result->elapsed, 30s);
    EXPECT_FALSE(App::DocumentExecutionStall::isActive());
}

TEST_F(DocumentExecutionStallTest, RunOnNonOwnerThreadRecordsThatThread)
{
    auto* lane = doc()->executionLane();
    ASSERT_NE(lane, nullptr);
    const auto ownerThread = lane->ownerThreadId();
    EXPECT_FALSE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
        *doc()));

    std::optional<App::DocumentExecutionStall::Result> workerResult;
    std::stop_source stopSource;

    std::jthread worker([&] {
        EXPECT_NE(std::this_thread::get_id(), ownerThread);
        EXPECT_FALSE(App::Internal::DocumentExecutionLaneTestAccess::isCollaborationOwnerThread(
            *doc()));
        workerResult = App::DocumentExecutionStall::run(stopSource.get_token(), 30s);
    });

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::thread::id workerThread {};
    while (std::chrono::steady_clock::now() < deadline) {
        if (App::DocumentExecutionStall::isActive()) {
            workerThread = App::DocumentExecutionStall::activeThread();
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(App::DocumentExecutionStall::isActive())
        << "non-owner stall did not become active";
    EXPECT_NE(workerThread, ownerThread);
    EXPECT_EQ(App::DocumentExecutionStall::activeThread(), workerThread);
    stopSource.request_stop();
    worker.join();

    ASSERT_TRUE(workerResult.has_value());
    EXPECT_EQ(workerResult->executingThread, workerThread);
    EXPECT_NE(workerResult->executingThread, ownerThread);
    EXPECT_FALSE(workerResult->completed);
}

TEST_F(DocumentExecutionStallTest, DefaultDurationIsThirtySeconds)
{
    EXPECT_EQ(App::DocumentExecutionStall::DefaultDuration, 30s);
}

}  // namespace
