// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PreparedEdit.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace
{

using KeySet =
    std::unordered_set<App::DocumentRevisionKey, App::DocumentRevisionKeyHash>;

bool isObjectScoped(App::DocumentRevisionKind kind) noexcept
{
    return kind == App::DocumentRevisionKind::ObjectExistence
        || kind == App::DocumentRevisionKind::ObjectModel
        || kind == App::DocumentRevisionKind::ObjectStructure;
}

void validateAndSortKeySet(std::vector<App::DocumentRevisionKey>& keys, const char* name)
{
    KeySet seen;
    seen.reserve(keys.size());
    for (const auto& key : keys) {
        if (!key.valid()) {
            throw std::invalid_argument(std::string("invalid revision key in ") + name);
        }
        if (!seen.insert(key).second) {
            throw std::invalid_argument(std::string("duplicate revision key in ") + name);
        }
    }
    std::sort(keys.begin(), keys.end());
}

std::vector<App::DocumentRevisionObservation> validateAndSortExpectedRevisions(
    const std::vector<App::DocumentRevisionObservation>& expectedRevisions,
    const std::vector<App::DocumentRevisionKey>& readSet,
    const std::vector<App::DocumentRevisionKey>& writeSet)
{
    KeySet required(readSet.begin(), readSet.end());
    required.insert(writeSet.begin(), writeSet.end());

    std::map<App::DocumentRevisionKey, App::DocumentRevision> canonical;
    for (const auto& observation : expectedRevisions) {
        if (!observation.key.valid()) {
            throw std::invalid_argument("invalid revision key in expected observations");
        }
        if (!canonical.emplace(observation.key, observation.revision).second) {
            throw std::invalid_argument("duplicate revision key in expected observations");
        }
        if (required.find(observation.key) == required.end()) {
            throw std::invalid_argument("expected observations contain an undeclared key");
        }
    }
    if (canonical.size() != required.size()) {
        throw std::invalid_argument("expected observations do not cover every dependency key");
    }

    std::vector<App::DocumentRevisionObservation> result;
    result.reserve(canonical.size());
    for (const auto& [key, revision] : canonical) {
        result.emplace_back(key, revision);
    }
    return result;
}

void validateAndSortPublicationEffects(
    std::vector<App::DocumentRevisionPublicationRequest>& effects,
    const std::vector<App::DocumentRevisionKey>& writeSet)
{
    KeySet writeKeys(writeSet.begin(), writeSet.end());
    KeySet seen;
    seen.reserve(effects.size());
    for (const auto& effect : effects) {
        if (!effect.key.valid()) {
            throw std::invalid_argument("invalid revision key in publication effects");
        }
        if (!seen.insert(effect.key).second) {
            throw std::invalid_argument("duplicate revision key in publication effects");
        }
        if (writeKeys.find(effect.key) == writeKeys.end()) {
            throw std::invalid_argument("publication effects contain an undeclared write key");
        }
        if (isObjectScoped(effect.key.kind)) {
            if (!effect.stableObjectIdentity || effect.stableObjectIdentity->empty()) {
                throw std::invalid_argument(
                    "object-scoped publication effect requires a stable object identity");
            }
        }
        else if (effect.stableObjectIdentity) {
            throw std::invalid_argument(
                "document-scoped publication effect cannot carry an object identity");
        }
    }
    if (seen.size() != writeKeys.size()) {
        throw std::invalid_argument("publication effects do not cover every write key");
    }
    std::sort(effects.begin(), effects.end(), [](const auto& left, const auto& right) {
        return left.key < right.key;
    });
}

}  // namespace

using namespace App;

PreparedEditCanonicalContract App::validatePreparedEditMetadata(
    std::string_view operationId,
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::string_view operationType,
    std::vector<DocumentRevisionObservation> expectedRevisions,
    std::vector<DocumentRevisionKey> readSet,
    std::vector<DocumentRevisionKey> writeSet,
    std::vector<DocumentRevisionPublicationRequest> publicationEffects,
    std::string_view provenance)
{
    if (operationId.empty()) {
        throw std::invalid_argument("prepared edit operation id must be nonempty");
    }
    if (documentInstanceId == 0 || lifecycleEpoch == 0) {
        throw std::invalid_argument("prepared edit document identity values must be nonzero");
    }
    if (operationType.empty()) {
        throw std::invalid_argument("prepared edit operation type must be nonempty");
    }
    if (provenance.empty()) {
        throw std::invalid_argument("prepared edit provenance must be nonempty");
    }
    validateAndSortKeySet(readSet, "prepared edit read set");
    validateAndSortKeySet(writeSet, "prepared edit write set");
    auto canonicalExpected =
        validateAndSortExpectedRevisions(expectedRevisions, readSet, writeSet);
    validateAndSortPublicationEffects(publicationEffects, writeSet);
    return {std::move(canonicalExpected),
            std::move(readSet),
            std::move(writeSet),
            std::move(publicationEffects)};
}

PreparedEditCanonicalContract App::validatePreparedEditContract(
    std::string_view operationId,
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::string_view operationType,
    std::vector<DocumentRevisionObservation> expectedRevisions,
    std::vector<DocumentRevisionKey> readSet,
    std::vector<DocumentRevisionKey> writeSet,
    std::vector<DocumentRevisionPublicationRequest> publicationEffects,
    std::string_view provenance,
    const CollaborativeOperation& operation)
{
    if (operation.typeId().empty()) {
        throw std::invalid_argument("prepared edit operation payload type must be nonempty");
    }
    if (operationType != operation.typeId()) {
        throw std::invalid_argument("prepared edit operation type does not match its payload");
    }
    return validatePreparedEditMetadata(operationId,
                                        documentInstanceId,
                                        lifecycleEpoch,
                                        operationType,
                                        std::move(expectedRevisions),
                                        std::move(readSet),
                                        std::move(writeSet),
                                        std::move(publicationEffects),
                                        provenance);
}

PreparedEdit::PreparedEdit(
    ConstructionKey,
    std::uint64_t adapterRegistrationId,
    std::string operationId,
    DocumentInstanceId documentInstanceId,
    DocumentLifecycleEpoch lifecycleEpoch,
    std::string operationType,
    std::vector<DocumentRevisionObservation> expectedRevisions,
    std::vector<DocumentRevisionKey> readSet,
    std::vector<DocumentRevisionKey> writeSet,
    std::vector<DocumentRevisionPublicationRequest> publicationEffects,
    std::string provenance,
    std::unique_ptr<const CollaborativeOperation> operation)
    : _adapterRegistrationId(adapterRegistrationId)
    , _operationId(std::move(operationId))
    , _documentInstanceId(documentInstanceId)
    , _lifecycleEpoch(lifecycleEpoch)
    , _operationType(std::move(operationType))
    , _provenance(std::move(provenance))
{
    if (_adapterRegistrationId == 0) {
        throw std::invalid_argument("prepared edit adapter registration must be nonzero");
    }
    if (!operation) {
        throw std::invalid_argument("prepared edit operation payload must be nonnull");
    }
    auto canonical = validatePreparedEditContract(_operationId,
                                                   _documentInstanceId,
                                                   _lifecycleEpoch,
                                                   _operationType,
                                                   std::move(expectedRevisions),
                                                   std::move(readSet),
                                                   std::move(writeSet),
                                                   std::move(publicationEffects),
                                                   _provenance,
                                                   *operation);
    _expectedRevisions = std::move(canonical.expectedRevisions);
    _readSet = std::move(canonical.readSet);
    _writeSet = std::move(canonical.writeSet);
    _publicationEffects = std::move(canonical.publicationEffects);
    _operation = std::move(operation);
}

std::uint64_t PreparedEdit::adapterRegistrationId() const noexcept
{
    return _adapterRegistrationId;
}

const std::string& PreparedEdit::operationId() const noexcept
{
    return _operationId;
}

DocumentInstanceId PreparedEdit::documentInstanceId() const noexcept
{
    return _documentInstanceId;
}

DocumentLifecycleEpoch PreparedEdit::lifecycleEpoch() const noexcept
{
    return _lifecycleEpoch;
}

const std::string& PreparedEdit::operationType() const noexcept
{
    return _operationType;
}

const std::vector<DocumentRevisionObservation>& PreparedEdit::expectedRevisions() const noexcept
{
    return _expectedRevisions;
}

const std::vector<DocumentRevisionKey>& PreparedEdit::readSet() const noexcept
{
    return _readSet;
}

const std::vector<DocumentRevisionKey>& PreparedEdit::writeSet() const noexcept
{
    return _writeSet;
}

const std::vector<DocumentRevisionPublicationRequest>&
PreparedEdit::publicationEffects() const noexcept
{
    return _publicationEffects;
}

const std::string& PreparedEdit::provenance() const noexcept
{
    return _provenance;
}

const CollaborativeOperation& PreparedEdit::operation() const noexcept
{
    return *_operation;
}
