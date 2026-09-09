// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentRecomputeCoordinator.h"

#include "DocumentCollaborationService.h"
#include "Document.h"
#include "DocumentObject.h"
#include "GenericIsolatedRecompute.h"
#include "GeometryJobManager.h"

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
        if (!feature.stableObjectIdentity.empty()) {
            requireBoundedField(
                feature.stableObjectIdentity, "stable object identity", bytes);
        }
        if (feature.stableObjectIdentity.empty()
            != !feature.presentationObjectModelRevision.has_value()) {
            throw std::invalid_argument(
                "recompute live-object identity and presentation revision must be paired");
        }
        std::ranges::sort(
            feature.presentationRevisionFence,
            {},
            &App::DocumentRevisionObservation::key);
        if (std::ranges::adjacent_find(
                feature.presentationRevisionFence,
                {},
                &App::DocumentRevisionObservation::key)
            != feature.presentationRevisionFence.end()) {
            throw std::invalid_argument(
                "recompute presentation fence contains a duplicate key");
        }
        for (const auto& observation : feature.presentationRevisionFence) {
            if (!observation.key.valid()) {
                throw std::invalid_argument(
                    "recompute presentation fence contains an invalid key");
            }
            if (observation.key.subject.size() > MaxFieldBytes
                || observation.key.propertyName.size() > MaxFieldBytes
                || bytes > MaxPlanBytes - observation.key.subject.size()
                || bytes + observation.key.subject.size()
                    > MaxPlanBytes - observation.key.propertyName.size()) {
                throw std::invalid_argument(
                    "recompute presentation fence exceeds the plan limit");
            }
            bytes += observation.key.subject.size()
                + observation.key.propertyName.size();
        }
        if (feature.intent.operationType
            == App::GenericIsolatedRecomputeOperationType) {
            const auto target = feature.intent.arguments.find("feature");
            const auto identity =
                feature.intent.arguments.find("stable_object_identity");
            if (feature.stableObjectIdentity.empty()
                || target == feature.intent.arguments.end()
                || identity == feature.intent.arguments.end()
                || target->second != feature.featureId
                || identity->second != feature.stableObjectIdentity) {
                throw std::invalid_argument(
                    "generic recompute presentation selector must match its operation target and stable identity");
            }
        }
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
    signature.push_back(request.refreshRevisionFenceAfterEachCommit ? '1' : '0');
    signature.push_back(request.publishTerminalPresentation ? '1' : '0');
    signature.push_back('\n');
    for (const auto& feature : request.features) {
        appendField(signature, feature.featureId);
        appendField(signature, feature.operationId);
        appendField(signature, feature.intent.operationType);
        appendField(signature, feature.provenance);
        appendField(signature, feature.stableObjectIdentity);
        signature.push_back(
            feature.presentationObjectModelRevision.has_value() ? '1' : '0');
        if (feature.presentationObjectModelRevision) {
            appendField(
                signature, std::to_string(*feature.presentationObjectModelRevision));
        }
        signature.push_back('F');
        appendField(
            signature,
            std::to_string(feature.presentationRevisionFence.size()));
        for (const auto& observation : feature.presentationRevisionFence) {
            appendField(
                signature,
                std::to_string(static_cast<int>(observation.key.kind)));
            appendField(signature, observation.key.subject);
            appendField(signature, observation.key.propertyName);
            appendField(signature, std::to_string(observation.revision));
        }
        signature.push_back(
            feature.presentationRevisionFenceComplete ? '1' : '0');
        signature.push_back('D');
        appendField(signature, std::to_string(feature.dependencies.size()));
        for (const auto& dependency : feature.dependencies) {
            appendField(signature, dependency);
        }
        signature.push_back('A');
        appendField(
            signature, std::to_string(feature.intent.arguments.size()));
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
    enum class PresentationFinalizationState
    {
        Unclaimed,
        InProgress,
        Finalized
    };

    struct Node
    {
        DocumentRecomputeFeatureRequest request;
        DocumentRecomputeFeatureState state {DocumentRecomputeFeatureState::Waiting};
        std::optional<PreparedEditExecutionId> executionId;
        std::string diagnostic;
        bool executed {false};
        std::optional<std::uint64_t> presentationObjectModelRevision;
        std::vector<DocumentRevisionObservation> presentationRevisionFence;
        bool presentationRevisionFenceComplete {false};
        bool outcomeApplied {false};
        bool targetPublicationConfirmed {false};
    };

    DocumentRecomputeId id {0};
    DocumentRecomputeState state {DocumentRecomputeState::Running};
    std::string coalescingKey;
    std::string signature;
    std::string sessionId;
    std::map<std::string, Node> nodes;
    bool refreshRevisionFenceAfterEachCommit {false};
    bool publishTerminalPresentation {true};
    bool activationPending {false};
    bool cancelRequested {false};
    bool sessionFinalized {false};
    PresentationFinalizationState presentationFinalization {
        PresentationFinalizationState::Unclaimed};
    std::string diagnostic;
};

DocumentRecomputeCoordinator::DocumentRecomputeCoordinator(
    DocumentCollaborationService& service)
    : _service(service)
{}

DocumentRecomputeCoordinator::SubmissionReservation::SubmissionReservation(
    DocumentRecomputeCoordinator& coordinator,
    std::unique_lock<std::recursive_mutex> operationLock,
    const DocumentRecomputeId id,
    const bool created) noexcept
    : _coordinator(&coordinator)
    , _operationLock(std::move(operationLock))
    , _id(id)
    , _created(created)
{}

DocumentRecomputeCoordinator::SubmissionReservation::SubmissionReservation(
    SubmissionReservation&& other) noexcept
    : _coordinator(std::exchange(other._coordinator, nullptr))
    , _operationLock(std::move(other._operationLock))
    , _id(std::exchange(other._id, 0))
    , _created(std::exchange(other._created, false))
{}

DocumentRecomputeCoordinator::SubmissionReservation::~SubmissionReservation()
{
    if (_coordinator) {
        _coordinator->_operationActive = false;
    }
}

DocumentRecomputeId
DocumentRecomputeCoordinator::SubmissionReservation::id() const noexcept
{
    return _id;
}

bool DocumentRecomputeCoordinator::SubmissionReservation::created() const noexcept
{
    return _created;
}

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
    auto reservation = admitSubmission(std::move(request));
    const auto id = reservation.id();
    if (reservation.created()) {
        activateSubmission(std::move(reservation));
    }
    return id;
}

DocumentRecomputeCoordinator::SubmissionReservation
DocumentRecomputeCoordinator::admitSubmission(DocumentRecomputeRequest request)
{
    std::unique_lock operationLock(_operationMutex);
    if (_operationActive) {
        throw std::runtime_error(
            "reentrant document recompute mutation is not supported");
    }
    _operationActive = true;
    try {
        const std::string signature = canonicalizeAndSign(request);

        {
            std::lock_guard stateLock(_stateMutex);
            if (!request.coalescingKey.empty()) {
                for (const auto& [id, job] : _jobs) {
                    if (job->coalescingKey != request.coalescingKey
                        || jobTerminal(job->state)) {
                        continue;
                    }
                    if (job->signature != signature) {
                        throw std::invalid_argument(
                            "active recompute coalescing key names a different plan");
                    }
                    return SubmissionReservation(
                        *this, std::move(operationLock), id, false);
                }
            }

            while (_jobs.size() >= MaxRetainedJobs) {
                const auto terminal = std::ranges::find_if(_jobs, [](const auto& entry) {
                    return jobTerminal(entry.second->state);
                });
                if (terminal == _jobs.end()) {
                    throw std::runtime_error(
                        "too many active document recompute plans");
                }
                const auto evictedId = terminal->first;
                _jobs.erase(terminal);
                std::erase_if(
                    _unresolvedSyntheticFeatures,
                    [evictedId](const auto& entry) {
                        return entry.first.first == evictedId;
                    });
            }
        }

        if (_nextId == 0
            || _nextId == std::numeric_limits<DocumentRecomputeId>::max()) {
            throw std::overflow_error("document recompute id space exhausted");
        }
        const auto id = _nextId++;

        auto job = std::make_unique<Job>();
        job->id = id;
        job->coalescingKey = std::move(request.coalescingKey);
        job->signature = signature;
        job->activationPending = true;
        job->refreshRevisionFenceAfterEachCommit =
            request.refreshRevisionFenceAfterEachCommit;
        job->publishTerminalPresentation =
            request.publishTerminalPresentation;
        for (auto& feature : request.features) {
            const std::string featureId = feature.featureId;
            const auto presentationObjectRevision =
                feature.presentationObjectModelRevision;
            auto presentationRevisionFence =
                feature.presentationRevisionFence;
            const bool presentationRevisionFenceComplete =
                feature.presentationRevisionFenceComplete;
            job->nodes.emplace(
                featureId,
                Job::Node {
                    .request = std::move(feature),
                    .executionId = std::nullopt,
                    .diagnostic = {},
                    .presentationObjectModelRevision = presentationObjectRevision,
                    .presentationRevisionFence =
                        std::move(presentationRevisionFence),
                    .presentationRevisionFenceComplete =
                        presentationRevisionFenceComplete});
        }
        {
            std::lock_guard stateLock(_stateMutex);
            _jobs.emplace(id, std::move(job));
        }

        return SubmissionReservation(
            *this, std::move(operationLock), id, true);
    }
    catch (...) {
        _operationActive = false;
        throw;
    }
}

void DocumentRecomputeCoordinator::replaceSubmission(
    SubmissionReservation& reservation,
    DocumentRecomputeRequest request)
{
    if (reservation._coordinator != this || !reservation._created) {
        throw std::logic_error("invalid document recompute submission reservation");
    }
    const std::string signature = canonicalizeAndSign(request);
    std::lock_guard stateLock(_stateMutex);
    auto& job = *_jobs.at(reservation._id);
    if (!job.activationPending || jobTerminal(job.state)) {
        throw std::logic_error(
            "document recompute submission reservation is no longer replaceable");
    }
    if (job.coalescingKey != request.coalescingKey) {
        throw std::invalid_argument(
            "deferred recompute capture changed its coalescing identity");
    }

    std::map<std::string, Job::Node> nodes;
    for (auto& feature : request.features) {
        const std::string featureId = feature.featureId;
        const auto presentationObjectRevision =
            feature.presentationObjectModelRevision;
        auto presentationRevisionFence =
            feature.presentationRevisionFence;
        const bool presentationRevisionFenceComplete =
            feature.presentationRevisionFenceComplete;
        nodes.emplace(
            featureId,
            Job::Node {
                .request = std::move(feature),
                .executionId = std::nullopt,
                .diagnostic = {},
                .presentationObjectModelRevision = presentationObjectRevision,
                .presentationRevisionFence =
                    std::move(presentationRevisionFence),
                .presentationRevisionFenceComplete =
                    presentationRevisionFenceComplete});
    }
    job.signature = signature;
    job.refreshRevisionFenceAfterEachCommit =
        request.refreshRevisionFenceAfterEachCommit;
    job.publishTerminalPresentation = request.publishTerminalPresentation;
    job.nodes = std::move(nodes);
    job.state = DocumentRecomputeState::Running;
}

void DocumentRecomputeCoordinator::failSubmission(
    SubmissionReservation& reservation,
    std::string diagnostic) noexcept
{
    bool finalizeNodes = false;
    try {
        if (reservation._coordinator != this || !reservation._created) {
            return;
        }
        {
            std::lock_guard stateLock(_stateMutex);
            const auto found = _jobs.find(reservation._id);
            if (found == _jobs.end() || jobTerminal(found->second->state)) {
                return;
            }
            auto& job = *found->second;
            if (job.nodes.empty()) {
                job.state = DocumentRecomputeState::PartialFailure;
            }
            else {
                // Publish every terminal node state before copying diagnostics.
                // A diagnostic allocation failure must not leave part of an
                // unactivated plan permanently Waiting.
                for (auto& [featureId, node] : job.nodes) {
                    static_cast<void>(featureId);
                    node.state = DocumentRecomputeFeatureState::Failed;
                }
                finalizeNodes = true;
            }
            job.activationPending = false;
            job.diagnostic = std::move(diagnostic);
            for (auto& [featureId, node] : job.nodes) {
                static_cast<void>(featureId);
                try {
                    node.diagnostic = job.diagnostic;
                }
                catch (...) {
                    // State is authoritative; diagnostics are best effort in
                    // this noexcept rejection path.
                }
            }
        }
    }
    catch (...) {
    }
    if (finalizeNodes) {
        try {
            finalizeIfTerminal(reservation._id);
        }
        catch (...) {
            // Ledger allocation can be retried by the next poll. Do not expose
            // a terminal job until finalizeIfTerminal succeeds.
        }
    }
}

void DocumentRecomputeCoordinator::activateSubmission(
    SubmissionReservation reservation)
{
    if (reservation._coordinator != this || !reservation._created) {
        return;
    }
    const auto id = reservation._id;

    bool needsSession = false;
    {
        std::lock_guard stateLock(_stateMutex);
        const auto found = _jobs.find(id);
        if (found == _jobs.end() || jobTerminal(found->second->state)
            || !found->second->activationPending) {
            return;
        }
        needsSession = !found->second->nodes.empty();
        // Presentation fences are coordinator-owned observations. Discard the
        // caller's provisional values before any fallible activation step;
        // DCS recaptures them under the document lock for each live selector.
        for (auto& [featureId, node] : found->second->nodes) {
            static_cast<void>(featureId);
            if (!node.request.stableObjectIdentity.empty()) {
                node.presentationObjectModelRevision.reset();
                node.presentationRevisionFence.clear();
                node.presentationRevisionFenceComplete = false;
            }
        }
    }

    std::string sessionId;
    const auto failActivation = [&](const char* detail) noexcept {
        try {
            std::lock_guard stateLock(_stateMutex);
            const auto found = _jobs.find(id);
            if (found == _jobs.end() || jobTerminal(found->second->state)) {
                return;
            }
            auto& job = *found->second;
            // Make the plan terminal at feature granularity before any
            // diagnostic allocation. activationPending is cleared only after
            // no Waiting node remains.
            for (auto& [featureId, node] : job.nodes) {
                static_cast<void>(featureId);
                if (!featureTerminal(node.state)) {
                    node.state = DocumentRecomputeFeatureState::Failed;
                }
            }
            job.activationPending = false;
            try {
                job.diagnostic = "recompute activation failed";
                if (detail && *detail) {
                    job.diagnostic += ": ";
                    job.diagnostic += detail;
                }
            }
            catch (...) {
                job.diagnostic.clear();
            }
            for (auto& [featureId, node] : job.nodes) {
                static_cast<void>(featureId);
                try {
                    node.diagnostic = job.diagnostic;
                }
                catch (...) {
                    // State remains terminal even if diagnostics cannot be
                    // retained under memory pressure.
                }
            }
        }
        catch (...) {
        }
    };
    try {
        if (needsSession) {
            sessionId =
                _service.beginEditSession("document-recompute").sessionId();
        }
    }
    catch (const std::exception& error) {
        failActivation(error.what());
    }
    catch (...) {
        failActivation("unknown exception");
    }

    {
        std::lock_guard stateLock(_stateMutex);
        const auto found = _jobs.find(id);
        if (found == _jobs.end() || jobTerminal(found->second->state)) {
            return;
        }
        auto& job = *found->second;
        if (!job.activationPending) {
            // A session-admission failure converted every node to a terminal
            // failure so the asynchronous caller still receives a handle.
        }
        else {
            job.sessionId = std::move(sessionId);
            job.activationPending = false;
        }
    }
    finalizeIfTerminal(id);
    {
        std::lock_guard stateLock(_stateMutex);
        const auto found = _jobs.find(id);
        if (found == _jobs.end() || jobTerminal(found->second->state)) {
            return;
        }
    }
    scheduleReady(id);
}

void DocumentRecomputeCoordinator::scheduleReady(const DocumentRecomputeId id)
{
    struct BlockedPresentationCandidate
    {
        std::string featureId;
        std::string stableObjectIdentity;
        std::string operationType;
    };
    const auto refreshBlockedPresentation =
        [&](const std::vector<BlockedPresentationCandidate>& candidates) noexcept {
            if (candidates.empty()) {
                return;
            }
            try {
                auto lifecyclePin = _service.pinDocumentAccess();
                if (!lifecyclePin) {
                    return;
                }
                auto& document = _service.document();
                if (!document.isCollaborationOwnerThread()) {
                    return;
                }
                std::lock_guard<std::recursive_mutex> serialized(
                    document.collaborationCommitMutex());
                if (document.collaborationIdentity().state
                    != DocumentLifecycleState::Live) {
                    return;
                }
                for (const auto& candidate : candidates) {
                    std::optional<DocumentRevision> objectRevision;
                    std::vector<DocumentRevisionObservation> fence;
                    bool fenceComplete = false;
                    auto* object = document.getObject(candidate.featureId.c_str());
                    if (object
                        && document.collaborationObjectIdentity(*object)
                            == candidate.stableObjectIdentity) {
                        objectRevision = document.collaborationRevisions().current(
                            DocumentRevisionKey::objectModel(
                                candidate.featureId));
                        if (candidate.operationType
                            == GenericIsolatedRecomputeOperationType) {
                            if (auto captured = Internal::
                                    captureGenericIsolatedRecomputePresentationFence(
                                        document, *object)) {
                                fence = std::move(*captured);
                                fenceComplete = true;
                            }
                        }
                    }
                    std::lock_guard stateLock(_stateMutex);
                    const auto foundJob = _jobs.find(id);
                    if (foundJob == _jobs.end()) {
                        return;
                    }
                    const auto foundNode =
                        foundJob->second->nodes.find(candidate.featureId);
                    if (foundNode == foundJob->second->nodes.end()
                        || foundNode->second.state
                            != DocumentRecomputeFeatureState::Blocked) {
                        continue;
                    }
                    foundNode->second.presentationObjectModelRevision =
                        objectRevision;
                    foundNode->second.presentationRevisionFence =
                        std::move(fence);
                    foundNode->second.presentationRevisionFenceComplete =
                        fenceComplete;
                }
            }
            catch (...) {
                // An incomplete fence suppresses presentation. Never turn a
                // failed observation capture into authority over the object.
            }
        };

    while (true) {
        std::optional<DocumentRecomputeFeatureRequest> request;
        std::string sessionId;
        std::string selectedFeature;
        std::vector<BlockedPresentationCandidate> newlyBlocked;
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
                    std::string blockedDiagnostic =
                        "dependency did not commit: " + *failedDependency;
                    if (!node.request.stableObjectIdentity.empty()) {
                        newlyBlocked.push_back(
                            {featureId,
                             node.request.stableObjectIdentity,
                             node.request.intent.operationType});
                    }
                    node.diagnostic = std::move(blockedDiagnostic);
                    node.state = DocumentRecomputeFeatureState::Blocked;
                    changed = true;
                }
            }

            if (job.refreshRevisionFenceAfterEachCommit
                && std::ranges::any_of(job.nodes, [](const auto& entry) {
                       const auto state = entry.second.state;
                       return state == DocumentRecomputeFeatureState::Preparing
                           || state == DocumentRecomputeFeatureState::Committing
                           || state == DocumentRecomputeFeatureState::Cancelling;
                   })) {
                break;
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
                // Copy every fallible launch input before publishing Preparing.
                // Otherwise allocation failure leaves a node with no execution
                // identity that neither poll nor scheduleReady can advance.
                selectedFeature = ready->first;
                request = ready->second.request;
                sessionId = job.sessionId;
                ready->second.state = DocumentRecomputeFeatureState::Preparing;
            }
            else if (!changed) {
                break;
            }
        }

        refreshBlockedPresentation(newlyBlocked);

        if (!request) {
            continue;
        }

        auto presentationObjectRevision =
            request->presentationObjectModelRevision;
        auto presentationRevisionFence =
            request->presentationRevisionFence;
        bool presentationRevisionFenceComplete =
            request->presentationRevisionFenceComplete;
        if (!request->stableObjectIdentity.empty()) {
            // A retry must not reuse a fence captured by a preceding attempt.
            // prepareRecomputeEditAsync() replaces all three values atomically
            // once it owns a stable document boundary.
            presentationObjectRevision.reset();
            presentationRevisionFence.clear();
            presentationRevisionFenceComplete = false;
        }
        std::optional<PreparedEditExecutionId> unclaimedExecution;
        BOOST_SCOPE_EXIT_ALL(&) {
            if (unclaimedExecution) {
                try {
                    static_cast<void>(
                        _service.cancelPreparedEdit(*unclaimedExecution));
                }
                catch (...) {
                }
            }
        };
        const auto failPreparationSubmission =
            [&](const char* detail) noexcept {
                try {
                    std::lock_guard stateLock(_stateMutex);
                    const auto foundJob = _jobs.find(id);
                    if (foundJob == _jobs.end()) {
                        return;
                    }
                    const auto foundNode =
                        foundJob->second->nodes.find(selectedFeature);
                    if (foundNode == foundJob->second->nodes.end()) {
                        return;
                    }
                    auto& node = foundNode->second;
                    // Terminal state first; fence/diagnostic retention is
                    // useful evidence but cannot be allowed to strand the
                    // state machine if allocation fails.
                    node.state = DocumentRecomputeFeatureState::Failed;
                    node.presentationObjectModelRevision =
                        presentationObjectRevision;
                    node.presentationRevisionFence =
                        std::move(presentationRevisionFence);
                    node.presentationRevisionFenceComplete =
                        presentationRevisionFenceComplete;
                    try {
                        node.diagnostic =
                            "detached preparation submission failed";
                        if (detail && *detail) {
                            node.diagnostic += ": ";
                            node.diagnostic += detail;
                        }
                    }
                    catch (...) {
                        node.diagnostic.clear();
                    }
                    if (foundJob->second->diagnostic.empty()) {
                        try {
                            foundJob->second->diagnostic = node.diagnostic;
                        }
                        catch (...) {
                        }
                    }
                }
                catch (...) {
                }
            };
        try {
            const auto executionId = request->stableObjectIdentity.empty()
                ? _service.prepareEditAsync(sessionId,
                                            request->operationId,
                                            request->intent,
                                            request->provenance)
                : _service.prepareRecomputeEditAsync(
                      sessionId,
                      request->operationId,
                      request->intent,
                      request->provenance,
                      request->featureId,
                      request->stableObjectIdentity,
                      presentationObjectRevision,
                      presentationRevisionFence,
                      presentationRevisionFenceComplete);
            unclaimedExecution = executionId;
            bool cancelExecution = false;
            bool jobDisappeared = false;
            {
                std::lock_guard stateLock(_stateMutex);
                const auto foundJob = _jobs.find(id);
                if (foundJob == _jobs.end()) {
                    jobDisappeared = true;
                    cancelExecution = true;
                }
                else {
                    const auto foundNode =
                        foundJob->second->nodes.find(selectedFeature);
                    if (foundNode == foundJob->second->nodes.end()) {
                        jobDisappeared = true;
                        cancelExecution = true;
                        continue;
                    }
                    auto& node = foundNode->second;
                    node.presentationObjectModelRevision =
                        presentationObjectRevision;
                    node.presentationRevisionFence =
                        std::move(presentationRevisionFence);
                    node.presentationRevisionFenceComplete =
                        presentationRevisionFenceComplete;
                    node.executionId = executionId;
                    unclaimedExecution.reset();
                    if (foundJob->second->cancelRequested) {
                        node.state = DocumentRecomputeFeatureState::Cancelling;
                        cancelExecution = true;
                    }
                }
            }
            if (cancelExecution) {
                static_cast<void>(_service.cancelPreparedEdit(executionId));
            }
            if (jobDisappeared) {
                return;
            }
        }
        catch (const GeometryJobQueueFull&) {
            // This is capacity backpressure, not a feature failure. Leave the
            // node waiting so the next owner-thread poll retries after queued
            // preparations have completed and released slots.
            std::lock_guard stateLock(_stateMutex);
            const auto foundJob = _jobs.find(id);
            if (foundJob != _jobs.end()) {
                const auto foundNode =
                    foundJob->second->nodes.find(selectedFeature);
                if (foundNode == foundJob->second->nodes.end()) {
                    return;
                }
                auto& node = foundNode->second;
                node.presentationObjectModelRevision = presentationObjectRevision;
                node.presentationRevisionFence =
                    std::move(presentationRevisionFence);
                node.presentationRevisionFenceComplete =
                    presentationRevisionFenceComplete;
                node.state = foundJob->second->cancelRequested
                    ? DocumentRecomputeFeatureState::Cancelled
                    : DocumentRecomputeFeatureState::Waiting;
                if (foundJob->second->cancelRequested) {
                    try {
                        node.diagnostic =
                            "recompute cancelled while awaiting preparation capacity";
                    }
                    catch (...) {
                    }
                }
            }
            return;
        }
        catch (const Base::Exception& error) {
            failPreparationSubmission(error.what());
        }
        catch (const std::exception& error) {
            failPreparationSubmission(error.what());
        }
        catch (...) {
            failPreparationSubmission("unknown exception");
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
        std::string targetStableIdentity;
        {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            // Copy every potentially allocating value before publishing the
            // Committing state. A throw after that state transition would
            // strand the node because only preparation/execution states are
            // advanced by a later poll.
            targetStableIdentity = node.request.stableObjectIdentity;
            sessionId = job.sessionId;
            node.state = DocumentRecomputeFeatureState::Committing;
        }
        DocumentCommitResult commit;
        try {
            commit = _service.commitRecomputeEdit(sessionId, *terminal->preparedEdit);
        }
        catch (const Base::Exception& error) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            try {
                commit.message = std::string("recompute commit failed: ") + error.what();
            }
            catch (...) {
            }
        }
        catch (const std::exception& error) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            try {
                commit.message = std::string("recompute commit failed: ") + error.what();
            }
            catch (...) {
            }
        }
        catch (...) {
            commit.status = DocumentCommitStatus::ApplyFailed;
            try {
                commit.message = "recompute commit failed with unknown exception";
            }
            catch (...) {
            }
        }
        const auto failPostCommitClassification = [&](const char* detail) noexcept {
            try {
                std::lock_guard stateLock(_stateMutex);
                const auto foundJob = _jobs.find(id);
                if (foundJob == _jobs.end()) {
                    return;
                }
                auto& job = *foundJob->second;
                auto& node = job.nodes.at(featureId);
                // Set the fail-closed state before formatting diagnostics: a
                // second allocation failure must not strand Committing.
                node.state = DocumentRecomputeFeatureState::Failed;
                node.outcomeApplied = false;
                node.targetPublicationConfirmed = false;
                try {
                    node.diagnostic = "recompute post-commit classification failed";
                    if (detail && *detail) {
                        node.diagnostic += ": ";
                        node.diagnostic += detail;
                    }
                    if (job.diagnostic.empty()) {
                        job.diagnostic = node.diagnostic;
                    }
                }
                catch (...) {
                }
            }
            catch (...) {
            }
        };
        try {
        // Read the operation's outcome only after commit. Some trusted result
        // operations finalize their failure state while applying, so querying
        // them earlier could misclassify a cleanly applied failure as success.
        const bool recomputeSucceeded =
            terminal->preparedEdit->operation().recomputeOutcomeSucceeded();
        const bool recomputeExecuted =
            terminal->preparedEdit->operation().recomputeCountedFeature();
        const std::string recomputeDiagnostic(
            terminal->preparedEdit->operation().recomputeOutcomeDiagnostic());
        std::optional<DocumentRevision> publishedTargetModelRevision;
        bool targetPublicationConfirmed = targetStableIdentity.empty();
        if (commit.status == DocumentCommitStatus::Committed
            && !targetStableIdentity.empty()) {
            const auto modelKey = DocumentRevisionKey::objectModel(featureId);
            const auto existenceKey =
                DocumentRevisionKey::objectExistence(featureId);
            const auto hasBoundEffect = [&](const DocumentRevisionKey& key) {
                return std::ranges::any_of(
                    terminal->preparedEdit->publicationEffects(),
                    [&](const auto& effect) {
                        return effect.key == key
                            && effect.stableObjectIdentity
                            && *effect.stableObjectIdentity
                                == targetStableIdentity;
                    });
            };
            const auto publishedModel = std::ranges::find(
                commit.publishedRevisions,
                modelKey,
                &DocumentRevisionObservation::key);
            const auto publishedExistence = std::ranges::find(
                commit.publishedRevisions,
                existenceKey,
                &DocumentRevisionObservation::key);
            const bool modelEvidence = hasBoundEffect(modelKey)
                && publishedModel != commit.publishedRevisions.end();
            const bool removalEvidence = hasBoundEffect(existenceKey)
                && publishedExistence != commit.publishedRevisions.end();
            if (modelEvidence || removalEvidence) {
                auto lifecyclePin = _service.pinDocumentAccess();
                if (lifecyclePin) {
                    auto& document = _service.document();
                    if (document.isCollaborationOwnerThread()) {
                        std::lock_guard<std::recursive_mutex> serialized(
                            document.collaborationCommitMutex());
                        if (document.collaborationIdentity().state
                            == DocumentLifecycleState::Live) {
                            auto* object = document.getObject(featureId.c_str());
                            if (modelEvidence && object
                                && document.collaborationObjectIdentity(*object)
                                    == targetStableIdentity) {
                                publishedTargetModelRevision =
                                    publishedModel->revision;
                                targetPublicationConfirmed = true;
                            }
                            else if (removalEvidence && !object) {
                                targetPublicationConfirmed = true;
                            }
                        }
                    }
                }
            }
        }
        {
            std::lock_guard stateLock(_stateMutex);
            auto& job = *_jobs.at(id);
            auto& node = job.nodes.at(featureId);
            node.diagnostic = commit.message;
            node.executed = recomputeExecuted;
            node.targetPublicationConfirmed = targetPublicationConfirmed;
            if (publishedTargetModelRevision) {
                node.presentationObjectModelRevision =
                    *publishedTargetModelRevision;
            }
            switch (commit.status) {
                case DocumentCommitStatus::Committed:
                    if (!targetPublicationConfirmed) {
                        node.state = DocumentRecomputeFeatureState::Failed;
                        node.outcomeApplied = false;
                        node.diagnostic =
                            "recompute commit did not publish its bound target";
                    }
                    else if (!recomputeSucceeded) {
                        node.state = DocumentRecomputeFeatureState::Failed;
                        node.outcomeApplied = true;
                        node.diagnostic = recomputeDiagnostic.empty()
                            ? "detached feature recompute failed"
                            : recomputeDiagnostic;
                    }
                    else {
                        node.state = DocumentRecomputeFeatureState::Committed;
                        node.outcomeApplied = true;
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
        }
        catch (const Base::Exception& error) {
            failPostCommitClassification(error.what());
            changed = true;
            continue;
        }
        catch (const std::exception& error) {
            failPostCommitClassification(error.what());
            changed = true;
            continue;
        }
        catch (...) {
            failPostCommitClassification("unknown exception");
            changed = true;
            continue;
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
    struct LiveFailureCandidate
    {
        std::string featureId;
        std::string stableObjectIdentity;
    };

    std::string sessionId;
    std::string reason;
    DocumentRecomputeState terminalState {DocumentRecomputeState::Running};
    std::vector<LiveFailureCandidate> liveFailures;
    std::vector<std::string> syntheticFailures;
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
            terminalState = DocumentRecomputeState::Cancelled;
            reason = job.diagnostic.empty() ? "recompute plan cancelled" : job.diagnostic;
        }
        else if (std::ranges::any_of(job.nodes, [](const auto& entry) {
                     return featureFailed(entry.second.state)
                         || entry.second.state == DocumentRecomputeFeatureState::Cancelled;
                 })) {
            terminalState = DocumentRecomputeState::PartialFailure;
            reason = "recompute plan reached a partial failure";
        }
        else {
            terminalState = DocumentRecomputeState::Completed;
            reason = "recompute plan completed";
        }
        for (const auto& [featureId, node] : job.nodes) {
            if (node.state != DocumentRecomputeFeatureState::Committed
                && node.request.stableObjectIdentity.empty()) {
                syntheticFailures.push_back(featureId);
            }
            else if (node.state != DocumentRecomputeFeatureState::Committed) {
                liveFailures.push_back(
                    {featureId,
                     node.request.stableObjectIdentity});
            }
        }
    }

    auto lifecyclePin = _service.pinDocumentAccess();
    auto finalizeState = [&] {
        std::lock_guard stateLock(_stateMutex);
        const auto foundJob = _jobs.find(id);
        if (foundJob == _jobs.end() || jobTerminal(foundJob->second->state)
            || !std::ranges::all_of(foundJob->second->nodes, [](const auto& entry) {
                   return featureTerminal(entry.second.state);
               })) {
            return;
        }

        // Populate every save-blocking record before publishing the terminal
        // job state. If allocation fails, the job remains nonterminal and a
        // later poll can retry instead of exposing a terminal result with a
        // missing unresolved ledger entry.
        for (const auto& featureId : syntheticFailures) {
            _unresolvedSyntheticFeatures.insert_or_assign(
                std::pair {id, featureId}, true);
        }
        for (const auto& candidate : liveFailures) {
            const auto unresolvedKey = std::pair {
                candidate.featureId, candidate.stableObjectIdentity};
            const auto found = _unresolvedLiveFeatures.find(unresolvedKey);
            if (found == _unresolvedLiveFeatures.end()
                || found->second.generation <= id) {
                _unresolvedLiveFeatures.insert_or_assign(
                    unresolvedKey, UnresolvedLiveFeature {id});
            }
        }
        for (const auto& [featureId, node] : foundJob->second->nodes) {
            if (node.state == DocumentRecomputeFeatureState::Committed) {
                _unresolvedSyntheticFeatures.erase({id, featureId});
            }
        }
        if (!foundJob->second->sessionFinalized
            && !foundJob->second->sessionId.empty()) {
            sessionId = foundJob->second->sessionId;
            foundJob->second->sessionFinalized = true;
        }
        // Publish terminal last: every ledger entry and the session hand-off
        // must already be complete when a status reader can observe it.
        foundJob->second->state = terminalState;
    };

    if (lifecyclePin) {
        auto& document = _service.document();
        if (document.isCollaborationOwnerThread()) {
            std::lock_guard<std::recursive_mutex> serialized(
                document.collaborationCommitMutex());
            if (document.collaborationIdentity().state
                == DocumentLifecycleState::Live) {
                std::erase_if(liveFailures, [&document](const auto& candidate) {
                    auto* object = document.getObject(candidate.featureId.c_str());
                    return !object
                        || document.collaborationObjectIdentity(*object)
                            != candidate.stableObjectIdentity;
                });
            }
            else {
                liveFailures.clear();
            }
            finalizeState();
        }
        else {
            // Public mutation is expected on the owner thread. Retain every
            // candidate fail-closed when its live identity cannot be checked.
            finalizeState();
        }
    }
    else {
        // Teardown owns the document from here; no future canonical save can
        // observe this coordinator. It is still safe to close the job state.
        liveFailures.clear();
        finalizeState();
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
    snapshot.publishTerminalPresentation = job.publishTerminalPresentation;
    snapshot.features.reserve(job.nodes.size());
    std::size_t terminalCount = 0;
    for (const auto& [featureId, node] : job.nodes) {
        snapshot.features.push_back(
            {featureId,
             node.state,
             node.diagnostic,
             node.executed,
             node.request.stableObjectIdentity,
             node.presentationObjectModelRevision,
             node.presentationRevisionFence,
             node.presentationRevisionFenceComplete,
             node.outcomeApplied,
             node.targetPublicationConfirmed});
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
    return !_unresolvedSyntheticFeatures.empty()
        || !_unresolvedLiveFeatures.empty()
        || std::ranges::any_of(_jobs, [](const auto& entry) {
               return !jobTerminal(entry.second->state);
           });
}

bool DocumentRecomputeCoordinator::hasUnresolvedExecutableWork() const
{
    std::vector<std::pair<std::string, std::string>> unresolved;
    {
        std::lock_guard stateLock(_stateMutex);
        unresolved.reserve(_unresolvedLiveFeatures.size());
        for (const auto& [key, feature] : _unresolvedLiveFeatures) {
            static_cast<void>(feature);
            unresolved.push_back(key);
        }
    }
    if (unresolved.empty()) {
        return false;
    }

    auto lifecyclePin = _service.pinDocumentAccess();
    if (!lifecyclePin) {
        return true;
    }
    auto& document = _service.document();
    if (!document.isCollaborationOwnerThread()) {
        return true;
    }
    std::lock_guard<std::recursive_mutex> serialized(
        document.collaborationCommitMutex());
    if (document.collaborationIdentity().state != DocumentLifecycleState::Live) {
        return true;
    }
    return std::ranges::any_of(unresolved, [&](const auto& key) {
        const auto& [featureId, stableObjectIdentity] = key;
        if (stableObjectIdentity.empty()) {
            return false;
        }
        auto* object = document.getObject(featureId.c_str());
        if (!object) {
            return false;
        }
        if (!stableObjectIdentity.empty()
            && document.collaborationObjectIdentity(*object)
                != stableObjectIdentity) {
            return false;
        }
        return object->mustRecompute() != 0;
    });
}

void DocumentRecomputeCoordinator::forgetUnresolvedFeature(
    const DocumentRecomputeId id,
    const std::string& featureId,
    const std::string& stableObjectIdentity)
{
    std::lock_guard stateLock(_stateMutex);
    if (stableObjectIdentity.empty()) {
        _unresolvedSyntheticFeatures.erase({id, featureId});
        return;
    }
    const auto found = _unresolvedLiveFeatures.find(
        {featureId, stableObjectIdentity});
    if (found == _unresolvedLiveFeatures.end()) {
        return;
    }
    if (found->second.generation <= id) {
        _unresolvedLiveFeatures.erase(found);
    }
}

void DocumentRecomputeCoordinator::forgetAllUnresolvedFeature(
    const std::string& featureId,
    const std::string& stableObjectIdentity)
{
    std::lock_guard stateLock(_stateMutex);
    _unresolvedLiveFeatures.erase({featureId, stableObjectIdentity});
}

bool DocumentRecomputeCoordinator::claimPresentationFinalization(
    const DocumentRecomputeId id)
{
    std::lock_guard stateLock(_stateMutex);
    const auto found = _jobs.find(id);
    if (found == _jobs.end() || !jobTerminal(found->second->state)
        || found->second->presentationFinalization
            != Job::PresentationFinalizationState::Unclaimed) {
        return false;
    }
    found->second->presentationFinalization =
        Job::PresentationFinalizationState::InProgress;
    return true;
}

void DocumentRecomputeCoordinator::finishPresentationFinalization(
    const DocumentRecomputeId id,
    const bool completed) noexcept
{
    try {
        std::lock_guard stateLock(_stateMutex);
        const auto found = _jobs.find(id);
        if (found == _jobs.end()
            || found->second->presentationFinalization
                != Job::PresentationFinalizationState::InProgress) {
            return;
        }
        found->second->presentationFinalization = completed
            ? Job::PresentationFinalizationState::Finalized
            : Job::PresentationFinalizationState::Unclaimed;
    }
    catch (...) {
    }
}

}  // namespace App
