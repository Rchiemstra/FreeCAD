// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentExecutionStall.h"

#include <mutex>

namespace App
{

namespace
{

std::mutex g_stateMutex;
std::thread::id g_activeThread {};
std::atomic<bool> g_active {false};

}  // namespace

DocumentExecutionStall::Result DocumentExecutionStall::run(
    std::stop_token stopToken,
    std::chrono::milliseconds duration)
{
    Result result;
    result.executingThread = std::this_thread::get_id();

    {
        std::lock_guard lock(g_stateMutex);
        g_activeThread = result.executingThread;
        g_active.store(true, std::memory_order_release);
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + duration;
    constexpr auto pollInterval = std::chrono::milliseconds(10);

    while (std::chrono::steady_clock::now() < deadline) {
        if (stopToken.stop_requested()) {
            break;
        }
        std::this_thread::sleep_for(pollInterval);
    }

    const auto finished = std::chrono::steady_clock::now();
    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
    result.completed = !stopToken.stop_requested() && finished >= deadline;

    {
        std::lock_guard lock(g_stateMutex);
        g_active.store(false, std::memory_order_release);
        g_activeThread = {};
    }

    return result;
}

bool DocumentExecutionStall::isActive() noexcept
{
    return g_active.load(std::memory_order_acquire);
}

std::thread::id DocumentExecutionStall::activeThread() noexcept
{
    std::lock_guard lock(g_stateMutex);
    return g_active ? g_activeThread : std::thread::id {};
}

}  // namespace App
