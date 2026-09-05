// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Mod/Assembly/AssemblyGlobal.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace Assembly
{

/**
 * Owns scan generation / cancellation so late workers cannot mutate a newer scan.
 * One active generation at a time; superseded generations are ignored on finish.
 */
class AssemblyExport InterferenceScanSession
{
public:
    struct Handle
    {
        std::uint64_t generation = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
    };

    /** Cancel any prior generation, start a new one, mark busy. */
    Handle beginScan();

    /** Mark results stale and cancel the active generation without clearing busy. */
    void markStale();

    /** Cooperative cancel of the active generation. */
    void requestCancel();

    /**
     * Called when a worker finishes.
     * @return true if this generation still owns the session and results may be applied.
     * Late/superseded finishes return false and must not mutate shared UI/results.
     */
    bool finishScan(std::uint64_t generation);

    bool isBusy() const
    {
        return busy;
    }
    bool isStale() const
    {
        return stale;
    }
    std::uint64_t activeGeneration() const
    {
        return generation;
    }

    const std::shared_ptr<std::atomic<bool>>& activeCancelFlag() const
    {
        return activeCancel;
    }

private:
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> activeCancel;
    bool busy = false;
    bool stale = false;
};

}  // namespace Assembly
