// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <App/Document.h>
#include <App/DocumentExecutionLane.h>
#include <App/MutationClassification.h>
#include <Gui/CollaborationCompatibilityAdapter.h>
#include <Gui/SharedPresentationCoordinator.h>

#include <App/DocumentCollaborationService.h>

#include <QApplication>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace App
{
class Document;
}

namespace Gui
{
class Document;
}

namespace Gui::Test
{

// Retry only typed Busy admission refusals, before any work starts. The caller
// must hold no document locks and must recheck prerequisites on each attempt.
// The observer and clock coordinate deterministic overlap/deadline probes.
[[nodiscard]] App::DocumentCommitResult serializeWithPresentationAdmissionRetry(
    const std::function<App::DocumentCommitResult()>& attempt,
    const std::function<void()>& onDeferred = {},
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
    const std::function<std::chrono::steady_clock::time_point()>& now =
        std::chrono::steady_clock::now);

/**
 * Admit on the owner thread without document locks, retrying only the same
 * pre-admission deferral as production. The deadline bounds admission retries;
 * the App wait has no timed overload, so refresh probes must release under a
 * watchdog. The optional observer coordinates those probes before the wait.
 * Tests may inject a clock to control retry deadlines independently of scheduling.
 * Callers must pair successful admission with an owner-thread scope guard.
 */
inline void beginAtomicPresentationMutationTargetWithRetry(
    App::Document& document,
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
    const std::function<void()>& onDeferred = {},
    const std::function<std::chrono::steady_clock::time_point()>& now =
        std::chrono::steady_clock::now)
{
    for (;;) {
        if (now() >= deadline) {
            throw std::runtime_error("owner presentation admission timed out");
        }
        try {
            App::beginAtomicPresentationMutationTarget(document);
            return;
        }
        catch (const App::LivePresentationRefreshInProgress& error) {
            if (!error.deferrable()) {
                throw;
            }
            if (onDeferred) {
                onDeferred();
            }
            if (!App::waitForLivePresentationRefresh()) {
                throw;
            }
        }
    }
}

/**
 * Run \p fn on the document execution lane owner thread while pumping Qt on
 * the GUI thread (for blocking MainThreadSignal marshalling during the call).
 */
template<typename Fn>
auto runOnDocumentOwnerWhilePumpingGui(App::Document& document, Fn&& fn)
    -> std::invoke_result_t<std::decay_t<Fn>>
{
    return invokeOnOwnerWorkerWhilePumpingGui([&] {
        if (auto* lane = document.executionLane()) {
            return lane->dispatchToOwner(std::forward<Fn>(fn));
        }
        return std::forward<Fn>(fn)();
    });
}

/**
 * Document-owner collaboration APIs throw DocumentWouldBlock on the GUI thread.
 * Run \p fn on a worker while pumping Qt so detached preparation and the lane
 * keep making progress.
 */
template<typename Fn>
auto invokeOnOwnerWorkerWhilePumpingGui(Fn&& fn)
    -> std::invoke_result_t<std::decay_t<Fn>>
{
    using Result = std::invoke_result_t<std::decay_t<Fn>>;
    std::exception_ptr failure;
    std::atomic<bool> finished {false};
    if constexpr (std::is_void_v<Result>) {
        std::thread worker([&] {
            try {
                std::forward<Fn>(fn)();
            }
            catch (...) {
                failure = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!finished.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "owner-thread collaboration call did not finish before timeout");
        }
        return;
    }
    else {
        std::optional<Result> result;
        std::thread worker([&] {
            try {
                result.emplace(std::forward<Fn>(fn)());
            }
            catch (...) {
                failure = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!finished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            QApplication::processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        worker.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (!finished.load(std::memory_order_acquire)) {
            throw std::runtime_error(
                "owner-thread collaboration call did not finish before timeout");
        }
        return std::move(*result);
    }
}

/// Submits a recompute through the document execution handle and pumps the GUI
/// event loop until it completes, avoiding synchronous recompute() on the GUI thread.
void recomputeWithoutBlockingGui(App::Document& document,
                                 const char* coalescingKey = "gui-test-setup-recompute");

/// Owner-thread recompute passes until mustExecute clears (matches commit coordinator).
void settlePendingRecomputeWithoutBlockingGui(App::Document& document);

/// Submits save-as through the document execution handle and pumps the GUI event
/// loop until it completes, avoiding synchronous saveAs() on the GUI thread.
void saveAsWithoutBlockingGui(App::Document& document, const char* path);

App::DocumentSaveOutcome saveWithOutcomeWithoutBlockingGui(App::Document& document);

App::DocumentSaveOutcome saveAsWithOutcomeWithoutBlockingGui(App::Document& document,
                                                               const char* path,
                                                               bool overwrite = false);

/**
 * commitSharedPresentation on the GUI thread with App serialization admitted on
 * the document owner thread (worker hop + Qt pump), not production GUI wait.
 */
[[nodiscard]] SharedPresentationCommitResult commitSharedPresentationWithoutBlockingGui(
    Document& guiDocument,
    SharedPresentationCommitRequest request,
    SharedPresentationCommitCallbacks callbacks,
    const std::function<void()>& onAdmissionDeferred = {});

[[nodiscard]] CollaborationCompatibilityMutationOutcome
executeCompatibilityMutationWithoutBlockingGui(
    Document& guiDocument,
    CollaborationCompatibilityMutationDeclaration declaration,
    CollaborationCompatibilityMutationCallback callback);

[[nodiscard]] App::DocumentCommitResult commitCompatibilityMutationWithoutBlockingGui(
    App::Document& document,
    App::CollaborationCompatibilityMutation mutation,
    std::function<void()> callback);

/// Runs AutoSaver recovery synchronously on the lane owner (test pump helper).
void flushAutoSaverWithoutBlockingGui(App::Document& document);

/**
 * Gui::Document::canClose() with lane saves pumped to completion in tests only
 * (uses submitDocumentSaveAwaitingCompletion, not production Document::save() wait).
 */
bool canCloseWithoutBlockingGui(Gui::Document& guiDocument,
                                bool checkModify = true,
                                bool checkLink = true);

/// MainWindow::closeAllDocuments() with lane saves pumped to completion in tests only.
bool closeAllDocumentsWithoutBlockingGui(bool close);

}  // namespace Gui::Test

namespace Gui::Test {
bool beginAutoSaveAttempt(App::Document& document);
void deliverRecoveryCompletion(const std::string& name, std::uint64_t instance, std::uint64_t epoch);
void deliverRecoveryRetry(const std::string& name, std::uint64_t instance, std::uint64_t epoch);
}
