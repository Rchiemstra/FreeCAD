// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#include "EventLoopLatencyStats.h"

#include <algorithm>
#include <vector>

namespace Gui
{

EventLoopLatencyStats::EventLoopLatencyStats(std::size_t capacity)
    : _capacity(capacity)
{}

bool EventLoopLatencyStats::record(Duration sample)
{
    if (sample < Duration::zero() || _capacity == 0) {
        return false;
    }

    std::lock_guard lock(_mutex);
    if (_samples.size() == _capacity) {
        _samples.pop_front();
    }
    _samples.push_back(sample);
    return true;
}

EventLoopLatencyStats::Snapshot EventLoopLatencyStats::snapshot() const
{
    std::vector<Duration> samples;
    {
        std::lock_guard lock(_mutex);
        samples.assign(_samples.begin(), _samples.end());
    }

    if (samples.empty()) {
        return {0, Duration::zero(), Duration::zero()};
    }

    std::sort(samples.begin(), samples.end());
    const auto maximum = samples.back();
    // Nearest-rank percentile: rank is ceil(0.99 * count), one-based.
    const auto rank = samples.size() - samples.size() / 100;
    return {samples.size(), maximum, samples[rank - 1]};
}

std::size_t EventLoopLatencyStats::capacity() const noexcept
{
    return _capacity;
}

} // namespace Gui
