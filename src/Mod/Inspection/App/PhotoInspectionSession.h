// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionEngine.h"

namespace Inspection::Photo
{

enum class SessionState
{
    Idle,
    Running,
    Complete,
    Cancelled,
    Closed
};

struct InspectionExport SessionSnapshot
{
    std::uint64_t generation {0};
    SessionState state {SessionState::Idle};
    bool cancellationRequested {false};
    bool hasResult {false};
};

class InspectionExport AnalysisSession
{
public:
    std::uint64_t begin();
    bool requestCancellation(std::uint64_t generation);
    bool cancellationRequested(std::uint64_t generation) const;
    bool publish(AnalysisResult result);
    std::optional<AnalysisResult> result(std::uint64_t generation) const;
    SessionSnapshot snapshot() const;
    void close();

private:
    mutable std::mutex mutex;
    std::uint64_t currentGeneration {0};
    SessionState currentState {SessionState::Idle};
    bool cancelRequested {false};
    std::optional<AnalysisResult> acceptedResult;
};

}  // namespace Inspection::Photo
