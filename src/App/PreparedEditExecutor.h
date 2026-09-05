// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborativeOperationRegistry.h"

#include <FCGlobal.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace App
{

class DocumentCollaborationService;

namespace Internal
{
class PreparedEditExecutorTestAccess;
}

using PreparedEditExecutionId = std::uint64_t;

enum class PreparedEditExecutionStatus
{
    Queued,
    Running,
    Completed,
    Cancelled,
    Failed
};

/** Copyable observation of one submitted preparation. */
struct AppExport PreparedEditExecutionSnapshot
{
    PreparedEditExecutionId id {0};
    PreparedEditExecutionStatus status {PreparedEditExecutionStatus::Queued};
    std::string diagnostic;
};

/** Move-only terminal outcome of one submitted preparation. */
struct AppExport PreparedEditExecutionResult
{
    PreparedEditExecutionResult() = default;
    PreparedEditExecutionResult(PreparedEditExecutionResult&&) noexcept = default;
    PreparedEditExecutionResult& operator=(PreparedEditExecutionResult&&) noexcept = default;
    PreparedEditExecutionResult(const PreparedEditExecutionResult&) = delete;
    PreparedEditExecutionResult& operator=(const PreparedEditExecutionResult&) = delete;

    PreparedEditExecutionId id {0};
    PreparedEditExecutionStatus status {PreparedEditExecutionStatus::Failed};
    std::unique_ptr<const CollaborativeOperation> operation;
    std::string diagnostic;
};

/**
 * Bounded worker pool for tasks that operate exclusively on detached values.
 *
 * This pool is independent of the application's recompute worker. Submitted
 * tasks must capture all inputs by value and cooperate with cancellation via
 * the supplied stop token. A stop request runs registered std::stop_callback
 * functions synchronously on the requesting thread. Trusted tasks must keep
 * those callbacks bounded and must not wait for executor progress from them.
 */
class AppExport PreparedEditExecutor
{
public:
    explicit PreparedEditExecutor(std::size_t workerCount = 0,
                                  std::size_t queueCapacity = 64);
    ~PreparedEditExecutor();

    PreparedEditExecutor(const PreparedEditExecutor&) = delete;
    PreparedEditExecutor& operator=(const PreparedEditExecutor&) = delete;
    PreparedEditExecutor(PreparedEditExecutor&&) = delete;
    PreparedEditExecutor& operator=(PreparedEditExecutor&&) = delete;

    [[nodiscard]] bool cancel(PreparedEditExecutionId id);
    [[nodiscard]] std::optional<PreparedEditExecutionSnapshot>
    status(PreparedEditExecutionId id) const;
    [[nodiscard]] std::optional<PreparedEditExecutionResult>
    takeResult(PreparedEditExecutionId id);

    [[nodiscard]] std::size_t workerCount() const noexcept;
    [[nodiscard]] std::size_t queueCapacity() const noexcept;

private:
    friend class DocumentCollaborationService;
    friend class Internal::PreparedEditExecutorTestAccess;

    [[nodiscard]] PreparedEditExecutionId
    submit(CollaborativeOperationPreparation::DetachedTask task);
    /** Disown a job without joining its task; a running job reaps itself. */
    [[nodiscard]] bool abandon(PreparedEditExecutionId id);

    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace App
