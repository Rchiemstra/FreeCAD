// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentRecomputeCoordinator.h"

#include "DocumentCollaborationService.h"

#include <Base/Exception.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{

constexpr std::size_t MaxFeatures = 10'000;
constexpr std::size_t MaxDependencies = 100'000;
constexpr std::size_t MaxArgumentsPerFeature = 256;
constexpr std::size_t MaxFieldBytes = 1U << 20;
constexpr std::size_t MaxPlanBytes = 16U << 20;
constexpr std::size_t MaxRetainedJobs = 256;

class OperationAdmission
{
public:
    explicit OperationAdmission(bool& active)
        : _active(active)
    {
        if (_active) {
            throw std::runtime_error("reentrant document recompute mutation is not supported");
        }
        _active = true;
    }

    ~OperationAdmission()
    {
        _active = false;
    }

    OperationAdmission(const OperationAdmission&) = delete;
    OperationAdmission& operator=(const OperationAdmission&) = delete;

private:
    bool& _active;
};

bool featureTerminal(const App::DocumentRecomputeFeatureState state)
{
    using State = App::DocumentRecomputeFeatureState;
    return state == State::Committed || state == State::Stale || state == State::Failed
        || state == State::Blocked || state == State::Cancelled;
}

bool featureFailed(const App::DocumentRecomputeFeatureState state)
{
    using State = App::DocumentRecomputeFeatureState;
    return state == State::Stale || state == State::Failed || state == State::Blocked;
}

bool jobTerminal(const App::DocumentRecomputeState state)
{
    using State = App::DocumentRecomputeState;
    return state == State::Completed || state == State::PartialFailure
        || state == State::Cancelled;
}

void requireBoundedField(const std::string& value, const char* name, std::size_t& bytes)
{
    if (value.empty()) {
        throw std::invalid_argument(std::string(name) + " must not be empty");
    }
    if (value.size() > MaxFieldBytes || bytes > MaxPlanBytes - value.size()) {
        throw std::invalid_argument(std::string(name) + " exceeds the recompute plan limit");
    }
    bytes += value.size();
}

void appendField(std::string& signature, std::string_view field)
{
    signature += std::to_string(field.size());
    signature.push_back(':');
    signature.append(field);
    signature.push_back(';');
}

std::string canonicalizeAndSign(App::DocumentRecomputeRequest& request)
{
    if (request.features.size() > MaxFeatures) {
        throw std::invalid_argument("recompute plan contains too many features");
    }
    if (request.coalescingKey.size() > MaxFieldBytes) {
        throw std::invalid_argument("recompute coalescing key exceeds the plan limit");
    }

    std::size_t bytes = request.coalescingKey.size();
    std::size_t dependencyCount = 0;
    for (auto& feature : request.features) {
        requireBoundedField(feature.featureId, "feature id", bytes);
        requireBoundedField(feature.operationId, "operation id", bytes);
        requireBoundedField(feature.intent.operationType, "operation type", bytes);
        requireBoundedField(feature.provenance, "provenance", bytes);
        if (feature.intent.arguments.size() > MaxArgumentsPerFeature) {
            throw std::invalid_argument("recompute feature contains too many arguments");
        }
        for (const auto& [key, value] : feature.intent.arguments) {
            requireBoundedField(key, "argument name", bytes);
            if (value.size() > MaxFieldBytes || bytes > MaxPlanBytes - value.size()) {
                throw std::invalid_argument("argument value exceeds the recompute plan limit");
            }
            bytes += value.size();
        }
        if (feature.dependencies.size() > MaxDependencies - dependencyCount) {
            throw std::invalid_argument("recompute plan contains too many dependencies");
        }
        dependencyCount += feature.dependencies.size();
        std::ranges::sort(feature.dependencies);
        if (std::ranges::adjacent_find(feature.dependencies) != feature.dependencies.end()) {
            throw std::invalid_argument("recompute feature contains a duplicate dependency");
        }
        for (const auto& dependency : feature.dependencies) {
            requireBoundedField(dependency, "dependency id", bytes);
            if (dependency == feature.featureId) {
                throw std::invalid_argument("recompute feature cannot depend on itself");
            }
        }
    }
    std::ranges::sort(request.features, {}, &App::DocumentRecomputeFeatureRequest::featureId);
    if (std::ranges::adjacent_find(
            request.features,
            {},
            &App::DocumentRecomputeFeatureRequest::featureId)
        != request.features.end()) {
        throw std::invalid_argument("recompute plan contains a duplicate feature id");
    }

    std::map<std::string, std::size_t> indegrees;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& feature : request.features) {
        indegrees.emplace(feature.featureId, feature.dependencies.size());
    }
    for (const auto& feature : request.features) {
        for (const auto& dependency : feature.dependencies) {
            if (!indegrees.contains(dependency)) {
                throw std::invalid_argument("recompute feature names an unknown dependency");
            }
            dependents[dependency].push_back(feature.featureId);
        }
    }

    std::set<std::string> ready;
    for (const auto& [featureId, indegree] : indegrees) {
        if (indegree == 0) {
            ready.insert(featureId);
        }
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        auto current = ready.extract(ready.begin()).value();
        ++visited;
        for (const auto& dependent : dependents[current]) {
            auto& indegree = indegrees.at(dependent);
            if (--indegree == 0) {
                ready.insert(dependent);
            }
        }
    }
    if (visited != request.features.size()) {
        throw std::invalid_argument("recompute plan contains a dependency cycle");
    }

    std::string signature;
    signature.reserve(bytes + request.features.size() * 32);
    appendField(signature, request.coalescingKey);
    for (const auto& feature : request.features) {
        appendField(signature, feature.featureId);
        appendField(signature, feature.operationId);
        appendField(signature, feature.intent.operationType);
        appendField(signature, feature.provenance);
        for (const auto& dependency : feature.dependencies) {
            appendField(signature, dependency);
        }
        signature.push_back('|');
        for (const auto& [key, value] : feature.intent.arguments) {
            appendField(signature, key);
            appendField(signature, value);
        }
        signature.push_back('\n');
    }
    return signature;
}

}  // namespace

namespace App
{

struct DocumentRecomputeCoordinator::Job
{
    struct Node
    {
        DocumentRecomputeFeatureRequest request;
        DocumentRecomputeFeatureState state {DocumentRecomputeFeatureState::Waiting};
        std::optional<PreparedEditExecutionId> executionId;
        std::string diagnostic;
    };

    DocumentRecomputeId id {0};
    DocumentRecomputeState state {DocumentRecomputeState::Running};
    std::string coalescingKey;
    std::string signature;
    std::string sessionId;
    std::map<std::string, Node> nodes;
    bool cancelRequested {false};
    bool sessionFinalized {false};
    bool presentationFinalized {false};
    std::string diagnostic;
};

DocumentRecomputeCoordinator::DocumentRecomputeCoordinator(
    DocumentCollaborationService& service)
    : _service(service)
{}

DocumentRecomputeCoordinator::~DocumentRecomputeCoordinator()
{
    std::lock_guard operationLock(_operationMutex);
    std::vector<PreparedEditExecutionId> executions;
    std::vector<std::string> sessions;
    {
        std::lock_guard stateLock(_stateMutex);
        for (auto& [id, job] : _jobs) {
            static_cast<void>(id);
            if (jobTerminal(job->state)) {
                continue;
            }
            job->cancelRequested = true;
            job->state = DocumentRecomputeState::Cancelling;
            if (!job->sessionId.empty()) {
                sessions.push_back(job->sessionId);
            }
            for (auto& [featureId, node] : job->nodes) {
                static_cast<void>(featureId);
                if (node.executionId && !featureTerminal(node.state)) {
                    executions.push_back(*node.executionId);
                }
            }
        }
    }
    for (const auto executionId : executions) {
        try {
            static_cast<void>(_service.cancelPreparedEdit(executionId));
        }
        catch (...) {
        }
    }
    for (const auto& sessionId : sessions) {
        try {
            static_cast<void>(_service.cancelEdit(sessionId, "document recompute coordinator destroyed"));
        }
        catch (...) {
        }
    }
}

DocumentRecomputeId DocumentRecomputeCoordinator::submit(DocumentRecomputeRequest request)
{
    std::lock_guard operationLock(_operationMutex);
    OperationAdmission operationAdmission(_operationActive);
    const std::string signature = canonicalizeAndSign(request);

    {
        std::lock_guard stateLock(_stateMutex);
        if (!request.coalescingKey.empty()) {
            for (const auto& [id, job] : _jobs) {
                if (job->coalescingKey != request.coalescingKey || jobTerminal(job->state)) {
                    continue;
                }
                if (job->signature != signature) {
                    throw std::invalid_argument(
                        "active recompute coalescing key names a different plan");
                }
                return id;
            }
        }

        while (_jobs.size() >= MaxRetainedJobs) {
            const auto terminal = std::ranges::find_if(_jobs, [](const auto& entry) {
                return jobTerminal(entry.second->state);
            });
            if (terminal == _jobs.end()) {
                throw std::runtime_error("too many active document recompute plans");
            }
            _jobs.erase(terminal);
        }
    }

    if (_nextId == 0 || _nextId == std::numeric_limits<DocumentRecomputeId>::max()) {
        throw std::overflow_error("document recompute id space exhausted");
    }
    const auto id = _nextId++;

    std::string sessionId;
    if (!request.features.empty()) {
        sessionId = _service.beginEditSession("document-recompute").sessionId();
    }

    auto job = std::make_unique<Job>();
    job->id = id;
    job->coalescingKey = std::move(request.coalescingKey);
    job->signature = signature;
    job->sessionId = std::move(sessionId);
    for (auto& feature : request.features) {
        const std::string featureId = feature.featureId;
        job->nodes.emplace(featureId, Job::Node {std::move(feature)});
    }
    if (job->nodes.empty()) {
        job->state = DocumentRecomputeState::Completed;
    }
    {
        std::lock_guard stateLock(_stateMutex);
        _jobs.emplace(id, std::move(job));
    }

    scheduleReady(id);
    return id;
}

void DocumentRecomputeCoordinator::scheduleReady(const DocumentRecomputeId id)
{
    while (true) {
        std::optional<DocumentRecomputeFeatureRequest> request;
        std::string sessionId;
        std::string selectedFeature;
        {
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob == _jobs.end() || jobTerminal(foundJob->second->state)) {
                return;
            }
            auto& job = *foundJob->second;
            bool changed = false;
            for (auto& [featureId, node] : job.nodes) {
                if (node.state != DocumentRecomputeFeatureState::Waiting) {
                    continue;
                }
                if (job.cancelRequested) {
                    node.state = DocumentRecomputeFeatureState::Cancelled;
                    node.diagnostic = "recompute cancelled before preparation";
                    changed = true;
                    continue;
                }
                const auto failedDependency = std::ranges::find_if(
                    node.request.dependencies,
                    [&job](const std::string& dependency) {
                        return featureFailed(job.nodes.at(dependency).state)
                            || job.nodes.at(dependency).state
                                == DocumentRecomputeFeatureState::Cancelled;
                    });
                if (failedDependency != node.request.dependencies.end()) {
                    node.state = DocumentRecomputeFeatureState::Blocked;
                    node.diagnostic = "dependency did not commit: " + *failedDependency;
                    changed = true;
                }
            }

            const auto ready = std::ranges::find_if(job.nodes, [&job](const auto& entry) {
                const auto& node = entry.second;
                return node.state == DocumentRecomputeFeatureState::Waiting
                    && std::ranges::all_of(node.request.dependencies,
                                           [&job](const std::string& dependency) {
                                               return job.nodes.at(dependency).state
                                                   == DocumentRecomputeFeatureState::Committed;
                                           });
            });
            if (ready != job.nodes.end()) {
                ready->second.state = DocumentRecomputeFeatureState::Preparing;
                selectedFeature = ready->first;
                request = ready->second.request;
                sessionId = job.sessionId;
            }
            else if (!changed) {
                break;
            }
        }

        if (!request) {
            continue;
        }

        try {
            const auto executionId = _service.prepareEditAsync(sessionId,
                                                               request->operationId,
                                                               request->intent,
                                                               request->provenance);
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob == _jobs.end()) {
                static_cast<void>(_service.cancelPreparedEdit(executionId));
                return;
            }
            auto& node = foundJob->second->nodes.at(selectedFeature);
            if (foundJob->second->cancelRequested) {
                node.state = DocumentRecomputeFeatureState::Cancelling;
                node.executionId = executionId;
                static_cast<void>(_service.cancelPreparedEdit(executionId));
            }
            else {
                node.executionId = executionId;
            }
        }
        catch (const Base::Exception& error) {
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob != _jobs.end()) {
                auto& node = foundJob->second->nodes.at(selectedFeature);
                node.state = DocumentRecomputeFeatureState::Failed;
                node.diagnostic = std::string("detached preparation submission failed: ")
                    + error.what();
                if (foundJob->second->diagnostic.empty()) {
                    foundJob->second->diagnostic = node.diagnostic;
                }
            }
        }
        catch (const std::exception& error) {
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob != _jobs.end()) {
                auto& node = foundJob->second->nodes.at(selectedFeature);
                node.state = DocumentRecomputeFeatureState::Failed;
                node.diagnostic = std::string("detached preparation submission failed: ")
                    + error.what();
                if (foundJob->second->diagnostic.empty()) {
                    foundJob->second->diagnostic = node.diagnostic;
                }
            }
        }
        catch (...) {
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob != _jobs.end()) {
                auto& node = foundJob->second->nodes.at(selectedFeature);
                node.state = DocumentRecomputeFeatureState::Failed;
                node.diagnostic = "detached preparation submission failed with unknown exception";
                if (foundJob->second->diagnostic.empty()) {
                    foundJob->second->diagnostic = node.diagnostic;
                }
            }
        }
    }
    finalizeIfTerminal(id);
}

bool DocumentRecomputeCoordinator::poll(const DocumentRecomputeId id)
{
    std::lock_guard operationLock(_operationMutex);
    OperationAdmission operationAdmission(_operationActive);
    std::vector<std::pair<std::string, PreparedEditExecutionId>> active;
    {
        std::lock_guard stateLock(_stateMutex);
        const auto foundJob = _jobs.find(id);
        if (foundJob == _jobs.end() || jobTerminal(foundJob->second->state)) {
            return false;
        }
        for (const auto& [featureId, node] : foundJob->second->nodes) {
            if (node.executionId
                && (node.state == DocumentRecomputeFeatureState::Preparing
                    || node.state == DocumentRecomputeFeatureState::Cancelling)) {
                active.emplace_back(featureId, *node.executionId);
            }
        }
    }

    bool changed = false;
    for (const auto& [featureId, executionId] : active) {
        std::optional<PreparedEditExecutionSnapshot> execution;
        try {
            execution = _service.preparedEditStatus(executionId);
        }
        catch (const Base::Exception& error) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = std::string("preparation status failed: ") + error.what();
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }
        catch (const std::exception& error) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = std::string("preparation status failed: ") + error.what();
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }
        catch (...) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = "preparation status failed with unknown exception";
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }

        if (!execution) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Stale;
            node.diagnostic = "detached preparation is no longer available for this document lifecycle";
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }
        if (execution->status == PreparedEditExecutionStatus::Queued
            || execution->status == PreparedEditExecutionStatus::Running) {
            continue;
        }

        std::optional<CollaborationPreparedEditResult> terminal;
        try {
            terminal = _service.takeRecomputePreparedEdit(
                [&] {
                    std::lock_guard stateLock(_stateMutex);
                    return _jobs.at(id)->sessionId;
                }(),
                executionId);
        }
        catch (const Base::Exception& error) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = std::string("preparation collection failed: ") + error.what();
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }
        catch (const std::exception& error) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = std::string("preparation collection failed: ") + error.what();
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }
        catch (...) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = "preparation collection failed with unknown exception";
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }

        if (!terminal) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = "terminal detached preparation could not be collected";
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }

        bool cancelled = false;
        {
            std::lock_guard stateLock(_stateMutex);
            cancelled = _jobs.at(id)->cancelRequested;
        }
        if (cancelled || terminal->status == PreparedEditExecutionStatus::Cancelled) {
            std::lock_guard stateLock(_stateMutex);
            auto& node = _jobs.at(id)->nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Cancelled;
            node.diagnostic = terminal->diagnostic.empty() ? "recompute preparation cancelled"
                                                           : terminal->diagnostic;
            changed = true;
            continue;
        }
        if (terminal->status != PreparedEditExecutionStatus::Completed
            || !terminal->preparedEdit) {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.state = DocumentRecomputeFeatureState::Failed;
            node.diagnostic = terminal->diagnostic.empty() ? "detached preparation failed"
                                                           : terminal->diagnostic;
            if (job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
            changed = true;
            continue;
        }

        std::string sessionId;
        {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            job.nodes.at(featureId).state = DocumentRecomputeFeatureState::Committing;
            sessionId = job.sessionId;
        }
        DocumentCommitResult commit;
        const bool recomputeSucceeded =
            terminal->preparedEdit->operation().recomputeOutcomeSucceeded();
        const std::string recomputeDiagnostic(
            terminal->preparedEdit->operation().recomputeOutcomeDiagnostic());
        try {
            commit = _service.commitRecomputeEdit(sessionId, *terminal->preparedEdit);
        }
        catch (const Base::Exception& error) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            commit.message = std::string("recompute commit failed: ") + error.what();
        }
        catch (const std::exception& error) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            commit.message = std::string("recompute commit failed: ") + error.what();
        }
        catch (...) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            commit.message = "recompute commit failed with unknown exception";
        }
        {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.diagnostic = commit.message;
            switch (commit.status) {
                case DocumentCommitStatus::Committed:
                    node.state = recomputeSucceeded
                        ? DocumentRecomputeFeatureState::Committed
                        : DocumentRecomputeFeatureState::Failed;
                    if (!recomputeSucceeded) {
                        node.diagnostic = recomputeDiagnostic.empty()
                            ? "detached feature recompute failed"
                            : recomputeDiagnostic;
                    }
                    break;
                case DocumentCommitStatus::Conflict:
                case DocumentCommitStatus::StaleDocument:
                    node.state = DocumentRecomputeFeatureState::Stale;
                    break;
                case DocumentCommitStatus::Cancelled:
                    node.state = DocumentRecomputeFeatureState::Cancelled;
                    break;
                default:
                    node.state = DocumentRecomputeFeatureState::Failed;
                    break;
            }
            if (node.state != DocumentRecomputeFeatureState::Committed
                && job.diagnostic.empty()) {
                job.diagnostic = node.diagnostic;
            }
        }
        changed = true;
    }

    scheduleReady(id);
    finalizeIfTerminal(id);
    return changed;
}

bool DocumentRecomputeCoordinator::cancel(const DocumentRecomputeId id, std::string reason)
{
    if (reason.empty()) {
        reason = "recompute cancelled by caller";
    }
    std::lock_guard operationLock(_operationMutex);
    OperationAdmission operationAdmission(_operationActive);
    std::vector<PreparedEditExecutionId> executions;
    {
        std::lock_guard stateLock(_stateMutex);
        const auto foundJob = _jobs.find(id);
        if (foundJob == _jobs.end() || jobTerminal(foundJob->second->state)) {
            return false;
        }
        auto& job = *foundJob->second;
        job.cancelRequested = true;
        job.state = DocumentRecomputeState::Cancelling;
        job.diagnostic = reason;
        for (auto& [featureId, node] : job.nodes) {
            static_cast<void>(featureId);
            if (node.state == DocumentRecomputeFeatureState::Waiting) {
                node.state = DocumentRecomputeFeatureState::Cancelled;
                node.diagnostic = reason;
            }
            else if (node.state == DocumentRecomputeFeatureState::Preparing && node.executionId) {
                node.state = DocumentRecomputeFeatureState::Cancelling;
                executions.push_back(*node.executionId);
            }
        }
    }
    for (const auto executionId : executions) {
        try {
            static_cast<void>(_service.cancelPreparedEdit(executionId));
        }
        catch (...) {
        }
    }
    finalizeIfTerminal(id);
    return true;
}

void DocumentRecomputeCoordinator::finalizeIfTerminal(const DocumentRecomputeId id)
{
    std::string sessionId;
    std::string reason;
    {
        std::lock_guard stateLock(_stateMutex);
        const auto foundJob = _jobs.find(id);
        if (foundJob == _jobs.end() || jobTerminal(foundJob->second->state)) {
            return;
        }
        auto& job = *foundJob->second;
        if (!std::ranges::all_of(job.nodes, [](const auto& entry) {
                return featureTerminal(entry.second.state);
            })) {
            return;
        }
        if (job.cancelRequested) {
            job.state = DocumentRecomputeState::Cancelled;
            reason = job.diagnostic.empty() ? "recompute plan cancelled" : job.diagnostic;
        }
        else if (std::ranges::any_of(job.nodes, [](const auto& entry) {
                     return featureFailed(entry.second.state)
                         || entry.second.state == DocumentRecomputeFeatureState::Cancelled;
                 })) {
            job.state = DocumentRecomputeState::PartialFailure;
            reason = "recompute plan reached a partial failure";
        }
        else {
            job.state = DocumentRecomputeState::Completed;
            reason = "recompute plan completed";
        }
        for (const auto& [featureId, node] : job.nodes) {
            if (node.state == DocumentRecomputeFeatureState::Committed) {
                _unresolvedFeatures.erase(featureId);
            }
            else {
                _unresolvedFeatures.insert(featureId);
            }
        }
        if (!job.sessionFinalized && !job.sessionId.empty()) {
            job.sessionFinalized = true;
            sessionId = job.sessionId;
        }
    }
    if (!sessionId.empty()) {
        try {
            static_cast<void>(_service.cancelEdit(sessionId, std::move(reason)));
        }
        catch (...) {
        }
    }
}

std::optional<DocumentRecomputeSnapshot> DocumentRecomputeCoordinator::statusLocked(
    const DocumentRecomputeId id) const
{
    const auto foundJob = _jobs.find(id);
    if (foundJob == _jobs.end()) {
        return std::nullopt;
    }
    const auto& job = *foundJob->second;
    DocumentRecomputeSnapshot snapshot;
    snapshot.id = id;
    snapshot.state = job.state;
    snapshot.totalFeatures = job.nodes.size();
    snapshot.diagnostic = job.diagnostic;
    snapshot.features.reserve(job.nodes.size());
    std::size_t terminalCount = 0;
    for (const auto& [featureId, node] : job.nodes) {
        snapshot.features.push_back({featureId, node.state, node.diagnostic});
        if (node.state == DocumentRecomputeFeatureState::Committed) {
            ++snapshot.completedFeatures;
        }
        if (featureFailed(node.state)) {
            ++snapshot.failedFeatures;
        }
        if (featureTerminal(node.state)) {
            ++terminalCount;
        }
    }
    snapshot.progress = snapshot.totalFeatures == 0
        ? 1.0
        : static_cast<double>(terminalCount) / static_cast<double>(snapshot.totalFeatures);
    return snapshot;
}

std::optional<DocumentRecomputeSnapshot> DocumentRecomputeCoordinator::status(
    const DocumentRecomputeId id) const
{
    std::lock_guard stateLock(_stateMutex);
    return statusLocked(id);
}

bool DocumentRecomputeCoordinator::hasPendingWork() const
{
    std::lock_guard stateLock(_stateMutex);
    return std::ranges::any_of(_jobs, [](const auto& entry) {
        return !jobTerminal(entry.second->state);
    });
}

bool DocumentRecomputeCoordinator::hasUnresolvedWork() const
{
    std::lock_guard stateLock(_stateMutex);
    return !_unresolvedFeatures.empty()
        || std::ranges::any_of(_jobs, [](const auto& entry) {
               return !jobTerminal(entry.second->state);
           });
}

bool DocumentRecomputeCoordinator::claimPresentationFinalization(
    const DocumentRecomputeId id)
{
    std::lock_guard stateLock(_stateMutex);
    const auto found = _jobs.find(id);
    if (found == _jobs.end() || !jobTerminal(found->second->state)
        || found->second->presentationFinalized) {
        return false;
    }
    found->second->presentationFinalized = true;
    return true;
}

}  // namespace App
