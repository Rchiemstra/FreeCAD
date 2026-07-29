// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionSession.h"

#include <utility>

namespace Inspection::Photo
{

std::uint64_t AnalysisSession::begin()
{
    std::scoped_lock lock(mutex);
    ++currentGeneration;
    currentState = SessionState::Running;
    cancelRequested = false;
    acceptedResult.reset();
    return currentGeneration;
}

bool AnalysisSession::requestCancellation(const std::uint64_t generation)
{
    std::scoped_lock lock(mutex);
    if (currentState != SessionState::Running || generation != currentGeneration) {
        return false;
    }
    cancelRequested = true;
    return true;
}

bool AnalysisSession::cancellationRequested(const std::uint64_t generation) const
{
    std::scoped_lock lock(mutex);
    return currentState != SessionState::Running || generation != currentGeneration
        || cancelRequested;
}

bool AnalysisSession::publish(AnalysisResult result)
{
    std::scoped_lock lock(mutex);
    if (currentState != SessionState::Running || result.generation != currentGeneration
        || cancelRequested) {
        return false;
    }
    currentState = result.status == OperationStatus::Cancelled ? SessionState::Cancelled
                                                               : SessionState::Complete;
    acceptedResult = std::move(result);
    return true;
}

std::optional<AnalysisResult> AnalysisSession::result(const std::uint64_t generation) const
{
    std::scoped_lock lock(mutex);
    if (generation != currentGeneration || currentState != SessionState::Complete || !acceptedResult) {
        return std::nullopt;
    }
    return acceptedResult;
}

SessionSnapshot AnalysisSession::snapshot() const
{
    std::scoped_lock lock(mutex);
    return {
        currentGeneration,
        currentState,
        cancelRequested,
        acceptedResult.has_value(),
    };
}

void AnalysisSession::close()
{
    std::scoped_lock lock(mutex);
    ++currentGeneration;
    currentState = SessionState::Closed;
    cancelRequested = true;
    acceptedResult.reset();
}

}  // namespace Inspection::Photo
