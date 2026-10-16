// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <FCGlobal.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>

namespace App
{

/**
 * Deterministic document-owner stall used by responsiveness acceptance tests.
 *
 * When invoked on the document-owner thread, this helper blocks that thread for
 * up to the configured duration (30 seconds by default). Cooperative
 * cancellation is polled every 10 ms so headless tests can end early after
 * verifying thread identity.
 */
class AppExport DocumentExecutionStall
{
public:
    static constexpr std::chrono::seconds DefaultDuration {30};

    struct Result
    {
        /** Thread that executed the stall body. */
        std::thread::id executingThread {};
        /** Wall time spent inside the stall loop. */
        std::chrono::milliseconds elapsed {0};
        /** True when the full duration elapsed without cancellation. */
        bool completed {false};
    };

    /**
     * Block the calling thread for up to \p duration.
     *
     * The stall always runs synchronously on the caller's thread. Production
     * acceptance uses the default 30 s duration; tests should pass a
     * \p stopToken and cancel once thread identity has been verified.
     */
    [[nodiscard]] static Result run(
        std::stop_token stopToken = {},
        std::chrono::milliseconds duration = DefaultDuration);

    /** True while any thread is inside run(). */
    [[nodiscard]] static bool isActive() noexcept;

    /**
     * Thread currently executing run(), or a default-constructed id when idle.
     */
    [[nodiscard]] static std::thread::id activeThread() noexcept;
};

}  // namespace App
