// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <App/Document.h>
#include <Gui/SharedPresentationCoordinator.h>

#include <QApplication>

#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
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
    SharedPresentationCommitCallbacks callbacks);

}  // namespace Gui::Test
