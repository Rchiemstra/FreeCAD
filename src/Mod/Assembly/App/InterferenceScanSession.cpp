// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreCompiled.h"
#include "InterferenceScanSession.h"

namespace Assembly
{

InterferenceScanSession::Handle InterferenceScanSession::beginScan()
{
    if (activeCancel) {
        activeCancel->store(true, std::memory_order_relaxed);
    }
    ++generation;
    activeCancel = std::make_shared<std::atomic<bool>>(false);
    busy = true;
    stale = false;
    return Handle {generation, activeCancel};
}

void InterferenceScanSession::markStale()
{
    stale = true;
    if (activeCancel) {
        activeCancel->store(true, std::memory_order_relaxed);
    }
}

void InterferenceScanSession::requestCancel()
{
    if (activeCancel) {
        activeCancel->store(true, std::memory_order_relaxed);
    }
}

bool InterferenceScanSession::finishScan(std::uint64_t finishedGeneration)
{
    // Superseded worker: never touch busy/stale owned by a newer generation.
    if (finishedGeneration != generation) {
        return false;
    }

    busy = false;
    if (stale) {
        return false;
    }
    if (activeCancel && activeCancel->load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

}  // namespace Assembly
