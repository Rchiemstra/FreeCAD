// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>

namespace Gui
{

/**
 * Bounded event-loop scheduling latency statistics.
 *
 * The accumulator retains only the most recent samples up to its configured
 * capacity. A snapshot is a value object and is independent of the
 * accumulator after it has been returned.
 */
class GuiExport EventLoopLatencyStats final
{
public:
    using Duration = std::chrono::nanoseconds;

    struct Snapshot final
    {
        const std::size_t count;
        const Duration maximum;
        const Duration p99;
    };

    explicit EventLoopLatencyStats(std::size_t capacity);

    EventLoopLatencyStats(const EventLoopLatencyStats&) = delete;
    EventLoopLatencyStats& operator=(const EventLoopLatencyStats&) = delete;

    /** Record a sample, returning false when it is negative or capacity is zero. */
    bool record(Duration sample);

    /** Return count, maximum, and nearest-rank p99 for the retained samples. */
    [[nodiscard]] Snapshot snapshot() const;

    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    const std::size_t _capacity;
    mutable std::mutex _mutex;
    std::deque<Duration> _samples;
};

} // namespace Gui
