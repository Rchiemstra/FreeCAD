// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionWaitTestHelpers.h"

#include <App/Document.h>
#include <App/DocumentCollaborationService.h>
#include <App/DocumentCommandHandle.h>
#include <App/DocumentHandle.h>
#include <App/RecoverySnapshot.h>
#include <Base/Console.h>
#include <Base/Exception.h>
#include <Gui/Application.h>
#include <Gui/Document.h>
#include <Gui/DocumentExecutionIngress.h>

#include <QApplication>

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <thread>

using namespace std::chrono_literals;

namespace Gui::Test
{

class RecoveryWaitTestAccess
{
public:
    using LifecyclePin = App::DocumentCollaborationService::LifecyclePin;
};

bool writeRecoverySnapshotAwaitingOwnerThread(
    App::Document& document,
    const App::RecoverySnapshotSaveOptions& options,
    const std::chrono::milliseconds timeout)
{
    if (!document.executionLane()) {
        // No retained owner lane exists to protect a legacy document after a
        // timeout. Complete on the caller instead of detaching a raw reference.
        return App::writeRecoverySnapshotToTransientDir(document, options);
    }

    struct Completion
    {
        explicit Completion(const App::DocumentCollaborationService& service)
            : lifecycle(service)
        {}

        RecoveryWaitTestAccess::LifecyclePin lifecycle;
        std::optional<bool> result;
        std::exception_ptr failure;
        std::atomic<bool> finished {false};
    };
    auto completion = std::make_shared<Completion>(document.collaborationService());
    if (!completion->lifecycle) {
        throw Base::RuntimeError("recovery snapshot cannot start while document close is sealed");
    }
    // Pin before dispatch, including the queued interval before the writer's
    // own lifecycle pin. LifecyclePin must be destroyed on its creating thread:
    // the scheduler delivers and releases this shared state on the GUI thread.
    // Reuse production's lane retention, recovery-work flag and admission guard.
    // Its worker owns this callback (and thus Completion), with no references
    // to our stack or options. Completion is delivered on the Qt GUI thread.
    scheduleRecoverySnapshotWrite(document, options,
        [completion](bool written, std::exception_ptr failure) {
            completion->result = written;
            completion->failure = failure;
            completion->finished.store(true, std::memory_order_release);
        });

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Nested event processing is deliberately confined to test support.
    while (!completion->finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        std::this_thread::sleep_for(1ms);
    }
    if (!completion->finished.load(std::memory_order_acquire)) {
        // Safe detach: the scheduler keeps the lane and recovery-work flag
        // until owner work ends; its queued callback retains Completion even
        // after this function throws. Timeout does not cancel the write.
        throw Base::RuntimeError("recovery snapshot write did not finish before timeout");
    }
    if (completion->failure) {
        std::rethrow_exception(completion->failure);
    }
    return completion->result.value_or(false);
}

// This nested wait is test-only, including close/save scenarios. Interactive
// production saves continue to use the nonblocking ingress and passive timer.
bool submitDocumentSaveAwaitingCompletion(
    App::Document& document,
    std::string* failureDiagnostic)
{
    const auto outcome = submitDocumentKindCommand(document, App::DocumentCommandKind::Save);
    if (!outcome.accepted()) {
        reportDocumentCommandSubmitBlocked(document, outcome);
        if (failureDiagnostic) {
            failureDiagnostic->clear();
        }
        return false;
    }

    App::DocumentCommandHandle commandHandle(
        outcome.commandId,
        document.executionHandle().identity());
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        const auto snapshot = commandHandle.status();
        if (!snapshot.terminal()) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (snapshot.state == App::DocumentCommandState::Completed) {
            if (auto* guiDocument = Application::Instance
                    ? Application::Instance->getDocument(&document) : nullptr) {
                guiDocument->finishExecutionLaneSave(snapshot.state);
            }
            if (failureDiagnostic) {
                failureDiagnostic->clear();
            }
            return true;
        }
        if (failureDiagnostic) {
            *failureDiagnostic = snapshot.diagnostic;
        }
        Base::Console().error("Document Save {}: {}\n",
            App::documentCommandStateName(snapshot.state),
            snapshot.diagnostic.empty() ? "no diagnostic was provided" : snapshot.diagnostic);
        return false;
    }

    if (failureDiagnostic) {
        *failureDiagnostic = "document save did not finish before timeout";
    }
    Base::Console().error("Document Save timed out waiting for completion\n");
    return false;
}

}  // namespace Gui::Test
