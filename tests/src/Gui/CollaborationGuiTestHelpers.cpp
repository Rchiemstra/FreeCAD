// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationGuiTestHelpers.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <App/Document.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentHandle.h>

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace Gui::Test
{

namespace
{

void waitForCompletedCommand(App::DocumentCommandHandle& commandHandle)
{
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    App::DocumentCommandSnapshot snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        snapshot = commandHandle.status();
        if (snapshot.terminal()) {
            ASSERT_EQ(snapshot.state, App::DocumentCommandState::Completed)
                << snapshot.diagnostic;
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL() << "document command did not finish before timeout";
}

void submitAndWait(App::Document& document, App::DocumentCommand&& command)
{
    auto handle = document.executionHandle();
    command.document = handle.identity();
    const auto outcome = handle.trySubmit(std::move(command));
    if (outcome.result != App::DocumentCommandSubmitResult::Accepted) {
        FAIL() << "document command trySubmit failed: "
               << App::documentCommandSubmitResultName(outcome.result);
    }

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    waitForCompletedCommand(commandHandle);
}

}  // namespace

void recomputeWithoutBlockingGui(App::Document& document, const char* coalescingKey)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Recompute;
    command.recompute = App::DocumentCommandRecomputePayload {};
    command.recompute->coalescingKey = coalescingKey;
    command.recompute->options = 0;
    submitAndWait(document, std::move(command));
}

void saveAsWithoutBlockingGui(App::Document& document, const char* path)
{
    App::DocumentCommand command;
    command.kind = App::DocumentCommandKind::Save;
    command.save = App::DocumentCommandSavePayload {};
    command.save->targetPath = path;
    command.save->saveAs = true;
    submitAndWait(document, std::move(command));
}

}  // namespace Gui::Test
