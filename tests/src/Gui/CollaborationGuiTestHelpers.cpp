// SPDX-License-Identifier: LGPL-2.1-or-later

#include "CollaborationGuiTestHelpers.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommand.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentExecutionLane.h>
#include <App/DocumentHandle.h>
#include <Gui/Document.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace Gui::Test
{

class SharedPresentationSerializeTestAccess
{
public:
    static App::DocumentCommitResult serializeAtomic(
        App::DocumentCollaborationService& service,
        std::vector<App::CollaborationAtomicPresentationWrite> allowedWrites,
        App::CollaborationAtomicCompatibilityCallback callback)
    {
        return service.serializeAtomicCompatibilityCallback(
            std::move(allowedWrites), std::move(callback));
    }
};

namespace
{

void waitForExecutionLaneIdle(App::Document& document)
{
    const auto* lane = document.executionLane();
    if (!lane) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        if (lane->isIdle()) {
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    FAIL() << "document execution lane did not become idle before timeout";
}

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
    waitForExecutionLaneIdle(document);
    auto handle = document.executionHandle();
    command.document = handle.identity();
    const auto outcome = handle.trySubmit(std::move(command));
    if (outcome.result != App::DocumentCommandSubmitResult::Accepted) {
        FAIL() << "document command trySubmit failed: "
               << App::documentCommandSubmitResultName(outcome.result);
    }

    App::DocumentCommandHandle commandHandle(outcome.commandId, handle.identity());
    waitForCompletedCommand(commandHandle);
    waitForExecutionLaneIdle(document);
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

App::DocumentSaveOutcome saveWithOutcomeWithoutBlockingGui(App::Document& document)
{
    waitForExecutionLaneIdle(document);
    return invokeOnOwnerWorkerWhilePumpingGui(
        [&] { return document.saveWithOutcome(); });
}

App::DocumentSaveOutcome saveAsWithOutcomeWithoutBlockingGui(App::Document& document,
                                                             const char* path,
                                                             const bool overwrite)
{
    waitForExecutionLaneIdle(document);
    return invokeOnOwnerWorkerWhilePumpingGui(
        [&] { return document.saveAsWithOutcome(path, overwrite); });
}

SharedPresentationCommitResult commitSharedPresentationWithoutBlockingGui(
    Document& guiDocument,
    SharedPresentationCommitRequest request,
    SharedPresentationCommitCallbacks callbacks)
{
    App::Document* const appDocument = guiDocument.getDocument();
    auto allowedGuiWrites = request.presentationWrites;
    std::sort(allowedGuiWrites.begin(), allowedGuiWrites.end());
    allowedGuiWrites.erase(
        std::unique(allowedGuiWrites.begin(), allowedGuiWrites.end()),
        allowedGuiWrites.end());
    std::vector<App::CollaborationAtomicPresentationWrite> allowedAppWrites;
    allowedAppWrites.reserve(allowedGuiWrites.size());
    for (const auto& write : allowedGuiWrites) {
        allowedAppWrites.push_back({write.stableObjectIdentity, write.propertyName});
    }
    callbacks.serialize =
        [appDocument, allowedAppWrites = std::move(allowedAppWrites)](
            SharedPresentationCommitWork&& work,
            SharedPresentationCommitCompletion&& complete) mutable {
            invokeOnOwnerWorkerWhilePumpingGui([&] {
                return SharedPresentationSerializeTestAccess::serializeAtomic(
                    appDocument->collaborationService(),
                    allowedAppWrites,
                    [&] {
                        work();
                        complete({true, {}});
                    });
            });
        };
    return guiDocument.commitSharedPresentation(std::move(request), std::move(callbacks));
}

}  // namespace Gui::Test
