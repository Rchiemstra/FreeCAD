// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentRevisionIndex.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{

using KeySet =
    std::unordered_set<App::DocumentRevisionKey, App::DocumentRevisionKeyHash>;

void validateKey(const App::DocumentRevisionKey& key)
{
    if (!key.valid()) {
        throw std::invalid_argument("invalid document revision key scope");
    }
}

bool isContinuationByte(unsigned char byte) noexcept
{
    return (byte & 0xc0U) == 0x80U;
}

void validateUtf8(std::string_view value)
{
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= value.size()
                || !isContinuationByte(static_cast<unsigned char>(value[index + 1]))) {
                throw std::invalid_argument("JSON string is not valid UTF-8");
            }
            index += 2;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= value.size()) {
                throw std::invalid_argument("JSON string is not valid UTF-8");
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            const bool validSecond =
                (first == 0xe0U && second >= 0xa0U && second <= 0xbfU)
                || (first == 0xedU && second >= 0x80U && second <= 0x9fU)
                || ((first >= 0xe1U && first <= 0xecU) && isContinuationByte(second))
                || ((first >= 0xeeU && first <= 0xefU) && isContinuationByte(second));
            if (!validSecond || !isContinuationByte(third)) {
                throw std::invalid_argument("JSON string is not valid UTF-8");
            }
            index += 3;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= value.size()) {
                throw std::invalid_argument("JSON string is not valid UTF-8");
            }
            const auto second = static_cast<unsigned char>(value[index + 1]);
            const auto third = static_cast<unsigned char>(value[index + 2]);
            const auto fourth = static_cast<unsigned char>(value[index + 3]);
            const bool validSecond =
                (first == 0xf0U && second >= 0x90U && second <= 0xbfU)
                || (first == 0xf4U && second >= 0x80U && second <= 0x8fU)
                || ((first >= 0xf1U && first <= 0xf3U) && isContinuationByte(second));
            if (!validSecond || !isContinuationByte(third) || !isContinuationByte(fourth)) {
                throw std::invalid_argument("JSON string is not valid UTF-8");
            }
            index += 4;
            continue;
        }

        throw std::invalid_argument("JSON string is not valid UTF-8");
    }
}

bool isObjectScoped(App::DocumentRevisionKind kind) noexcept
{
    return kind == App::DocumentRevisionKind::ObjectExistence
        || kind == App::DocumentRevisionKind::ObjectModel
        || kind == App::DocumentRevisionKind::ObjectStructure;
}

std::vector<App::DocumentRevisionKey>
distinctKeys(const std::vector<App::DocumentRevisionKey>& keys)
{
    std::vector<App::DocumentRevisionKey> result;
    result.reserve(keys.size());

    KeySet seen;
    seen.reserve(keys.size());
    for (const auto& key : keys) {
        validateKey(key);
        if (seen.insert(key).second) {
            result.push_back(key);
        }
    }
    return result;
}

std::vector<App::DocumentRevisionPublicationRequest> distinctPublicationRequests(
    const std::vector<App::DocumentRevisionPublicationRequest>& changes)
{
    using IdentityByKey = std::unordered_map<App::DocumentRevisionKey,
                                             std::optional<std::string>,
                                             App::DocumentRevisionKeyHash>;

    std::vector<App::DocumentRevisionPublicationRequest> result;
    result.reserve(changes.size());
    IdentityByKey identities;
    identities.reserve(changes.size());

    for (const auto& change : changes) {
        validateKey(change.key);
        validateUtf8(change.key.subject);
        if (isObjectScoped(change.key.kind)) {
            if (!change.stableObjectIdentity || change.stableObjectIdentity->empty()) {
                throw std::invalid_argument(
                    "object-scoped revision publication requires a stable object identity");
            }
        }
        else if (change.stableObjectIdentity) {
            throw std::invalid_argument(
                "document-scoped revision publication cannot carry an object identity");
        }
        if (change.stableObjectIdentity) {
            validateUtf8(*change.stableObjectIdentity);
        }

        auto [identity, inserted] =
            identities.emplace(change.key, change.stableObjectIdentity);
        if (!inserted && identity->second != change.stableObjectIdentity) {
            throw std::invalid_argument(
                "duplicate revision keys cannot carry inconsistent object identities");
        }
        if (inserted) {
            result.push_back(change);
        }
    }
    return result;
}

std::string_view revisionKindName(App::DocumentRevisionKind kind) noexcept
{
    switch (kind) {
        case App::DocumentRevisionKind::ObjectExistence:
            return "ObjectExistence";
        case App::DocumentRevisionKind::ObjectModel:
            return "ObjectModel";
        case App::DocumentRevisionKind::ObjectStructure:
            return "ObjectStructure";
        case App::DocumentRevisionKind::DocumentStructure:
            return "DocumentStructure";
        case App::DocumentRevisionKind::UnknownModelMutation:
            return "UnknownModelMutation";
    }
    return "Invalid";
}

void appendJsonString(std::string& output, std::string_view value)
{
    validateUtf8(value);
    constexpr char hexDigits[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20U) {
                    output += "\\u00";
                    output.push_back(hexDigits[(character >> 4U) & 0x0fU]);
                    output.push_back(hexDigits[character & 0x0fU]);
                }
                else {
                    output.push_back(static_cast<char>(character));
                }
        }
    }
    output.push_back('"');
}

std::string_view cursorStatusName(App::DocumentRevisionCursorStatus status) noexcept
{
    switch (status) {
        case App::DocumentRevisionCursorStatus::Valid:
            return "Valid";
        case App::DocumentRevisionCursorStatus::ForeignDocument:
            return "ForeignDocument";
        case App::DocumentRevisionCursorStatus::StaleEpoch:
            return "StaleEpoch";
        case App::DocumentRevisionCursorStatus::FutureSequence:
            return "FutureSequence";
    }
    return "Invalid";
}

}  // namespace

using namespace App;

DocumentRevisionKey DocumentRevisionKey::objectExistence(std::string subject)
{
    return {DocumentRevisionKind::ObjectExistence, std::move(subject)};
}

DocumentRevisionKey DocumentRevisionKey::objectModel(std::string subject)
{
    return {DocumentRevisionKind::ObjectModel, std::move(subject)};
}

DocumentRevisionKey DocumentRevisionKey::objectStructure(std::string subject)
{
    return {DocumentRevisionKind::ObjectStructure, std::move(subject)};
}

DocumentRevisionKey DocumentRevisionKey::documentStructure()
{
    return {DocumentRevisionKind::DocumentStructure, {}};
}

DocumentRevisionKey DocumentRevisionKey::unknownModelMutation()
{
    return {DocumentRevisionKind::UnknownModelMutation, {}};
}

bool DocumentRevisionKey::valid() const noexcept
{
    switch (kind) {
        case DocumentRevisionKind::ObjectExistence:
        case DocumentRevisionKind::ObjectModel:
        case DocumentRevisionKind::ObjectStructure:
            return !subject.empty();
        case DocumentRevisionKind::DocumentStructure:
        case DocumentRevisionKind::UnknownModelMutation:
            return subject.empty();
    }
    return false;
}

std::size_t DocumentRevisionKeyHash::operator()(const DocumentRevisionKey& key) const noexcept
{
    const auto kindHash = std::hash<DocumentRevisionKind> {}(key.kind);
    const auto subjectHash = std::hash<std::string> {}(key.subject);
    return kindHash ^ (subjectHash + 0x9e3779b9U + (kindHash << 6U) + (kindHash >> 2U));
}

DocumentRevisionObservation::DocumentRevisionObservation(DocumentRevisionKey key,
                                                         DocumentRevision revision)
    : key(std::move(key))
    , revision(revision)
{}

DocumentRevisionConflict::DocumentRevisionConflict(DocumentRevisionKey key,
                                                   DocumentRevision expected,
                                                   DocumentRevision current)
    : key(std::move(key))
    , expected(expected)
    , current(current)
{}

std::string DocumentRevisionPublicationEvent::toJson() const
{
    std::string output = "{\"document_instance_id\":";
    output += std::to_string(documentInstanceId);
    output += ",\"lifecycle_epoch\":";
    output += std::to_string(lifecycleEpoch);
    output += ",\"publication_sequence\":";
    output += std::to_string(publicationSequence);
    output += ",\"changes\":[";
    for (std::size_t index = 0; index < changes.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        const auto& change = changes[index];
        output += "{\"kind\":";
        appendJsonString(output, revisionKindName(change.key.kind));
        output += ",\"subject\":";
        appendJsonString(output, change.key.subject);
        output += ",\"revision\":";
        output += std::to_string(change.revision);
        output += ",\"stable_object_identity\":";
        if (change.stableObjectIdentity) {
            appendJsonString(output, *change.stableObjectIdentity);
        }
        else {
            output += "null";
        }
        output.push_back('}');
    }
    output += "]}";
    return output;
}

std::string DocumentRevisionPollResult::toJson() const
{
    std::string output = "{\"status\":";
    appendJsonString(output, cursorStatusName(status));
    output += ",\"requested_cursor\":{\"document_instance_id\":";
    output += std::to_string(requestedCursor.documentInstanceId);
    output += ",\"lifecycle_epoch\":";
    output += std::to_string(requestedCursor.lifecycleEpoch);
    output += ",\"after_sequence\":";
    output += std::to_string(requestedCursor.afterSequence);
    output += "},\"current_identity\":{\"document_instance_id\":";
    output += std::to_string(currentIdentity.documentInstanceId);
    output += ",\"lifecycle_epoch\":";
    output += std::to_string(currentIdentity.lifecycleEpoch);
    output += "},\"next_cursor\":{\"document_instance_id\":";
    output += std::to_string(nextCursor.documentInstanceId);
    output += ",\"lifecycle_epoch\":";
    output += std::to_string(nextCursor.lifecycleEpoch);
    output += ",\"after_sequence\":";
    output += std::to_string(nextCursor.afterSequence);
    output.push_back('}');
    output += ",\"oldest_available_sequence\":";
    output += std::to_string(oldestAvailableSequence);
    output += ",\"latest_sequence\":";
    output += std::to_string(latestSequence);
    output += ",\"gap\":";
    output += gap ? "true" : "false";
    output += ",\"events\":[";
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (index != 0) {
            output.push_back(',');
        }
        output += events[index].toJson();
    }
    output += "]}";
    return output;
}

DocumentRevisionIndex::DocumentRevisionIndex()
    : DocumentRevisionIndex(std::numeric_limits<DocumentRevision>::max(),
                            std::numeric_limits<DocumentPublicationSequence>::max(),
                            DefaultJournalCapacity)
{}

DocumentRevisionIndex::DocumentRevisionIndex(DocumentRevision maximumRevision)
    : DocumentRevisionIndex(maximumRevision,
                            std::numeric_limits<DocumentPublicationSequence>::max(),
                            DefaultJournalCapacity)
{}

DocumentRevisionIndex::DocumentRevisionIndex(DocumentRevision maximumRevision,
                                             std::size_t journalCapacity)
    : DocumentRevisionIndex(maximumRevision,
                            std::numeric_limits<DocumentPublicationSequence>::max(),
                            journalCapacity)
{}

DocumentRevisionIndex::DocumentRevisionIndex(
    DocumentRevision maximumRevision,
    DocumentPublicationSequence maximumPublicationSequence,
    std::size_t journalCapacity)
    : _maximumRevision(maximumRevision)
    , _maximumPublicationSequence(maximumPublicationSequence)
    , _journalCapacity(journalCapacity)
{
    if (_journalCapacity == 0) {
        throw std::invalid_argument("document revision journal capacity must be nonzero");
    }
}

void DocumentRevisionIndex::bindDocumentIdentity(DocumentInstanceId documentInstanceId,
                                                 DocumentLifecycleEpoch lifecycleEpoch)
{
    if (documentInstanceId == 0 || lifecycleEpoch == 0) {
        throw std::invalid_argument("document revision identity values must be nonzero");
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        _documentIdentity = DocumentRevisionIdentityBinding {documentInstanceId, lifecycleEpoch};
        return;
    }
    if (_documentIdentity->documentInstanceId != documentInstanceId) {
        throw std::logic_error("document revision index cannot be rebound to another instance");
    }
    if (lifecycleEpoch < _documentIdentity->lifecycleEpoch) {
        throw std::invalid_argument("document lifecycle epoch cannot rewind");
    }
    _documentIdentity->lifecycleEpoch = lifecycleEpoch;
}

std::optional<DocumentRevisionIdentityBinding> DocumentRevisionIndex::documentIdentity() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _documentIdentity;
}

DocumentRevision DocumentRevisionIndex::current(const DocumentRevisionKey& key) const
{
    validateKey(key);
    std::lock_guard<std::mutex> lock(_mutex);
    return currentLocked(key);
}

std::vector<DocumentRevisionObservation>
DocumentRevisionIndex::capture(const std::vector<DocumentRevisionKey>& keys) const
{
    const auto uniqueKeys = distinctKeys(keys);
    std::vector<DocumentRevisionObservation> result;
    result.reserve(uniqueKeys.size());

    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& key : uniqueKeys) {
        result.emplace_back(key, currentLocked(key));
    }
    return result;
}

std::vector<DocumentRevisionConflict> DocumentRevisionIndex::validate(
    const std::vector<DocumentRevisionObservation>& observations) const
{
    for (const auto& observation : observations) {
        validateKey(observation.key);
    }

    std::vector<DocumentRevisionConflict> conflicts;
    conflicts.reserve(observations.size());

    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& observation : observations) {
        const auto revision = currentLocked(observation.key);
        if (revision != observation.revision) {
            conflicts.emplace_back(observation.key, observation.revision, revision);
        }
    }
    return conflicts;
}

std::vector<DocumentRevisionObservation>
DocumentRevisionIndex::publish(const std::vector<DocumentRevisionKey>& documentScopedKeys)
{
    std::vector<DocumentRevisionPublicationRequest> changes;
    changes.reserve(documentScopedKeys.size());
    for (const auto& key : documentScopedKeys) {
        changes.push_back({key, std::nullopt});
    }
    return publish(changes);
}

std::vector<DocumentRevisionObservation>
DocumentRevisionIndex::publish(const std::vector<DocumentRevisionPublicationRequest>& changes)
{
    const auto uniqueChanges = distinctPublicationRequests(changes);

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        throw std::logic_error("document revision identity must be bound before publication");
    }
    if (uniqueChanges.empty()) {
        return {};
    }
    if (_publicationSequence == _maximumPublicationSequence) {
        throw std::overflow_error("document publication sequence overflow");
    }

    for (const auto& change : uniqueChanges) {
        if (currentLocked(change.key) == _maximumRevision) {
            throw std::overflow_error("document revision counter overflow");
        }
    }

    std::vector<DocumentRevisionObservation> result;
    result.reserve(uniqueChanges.size());
    for (const auto& change : uniqueChanges) {
        result.emplace_back(change.key, currentLocked(change.key) + 1);
    }

    DocumentRevisionPublicationEvent event;
    event.documentInstanceId = _documentIdentity->documentInstanceId;
    event.lifecycleEpoch = _documentIdentity->lifecycleEpoch;
    event.publicationSequence = _publicationSequence + 1;
    event.changes.reserve(uniqueChanges.size());
    for (const auto& change : uniqueChanges) {
        event.changes.push_back({change.key,
                                 currentLocked(change.key) + 1,
                                 change.stableObjectIdentity});
    }

    std::vector<DocumentRevision*> revisionSlots;
    revisionSlots.reserve(uniqueChanges.size());
    for (const auto& change : uniqueChanges) {
        auto [revision, inserted] = _revisions.try_emplace(change.key, 0);
        static_cast<void>(inserted);
        revisionSlots.push_back(&revision->second);
    }

    _journal.push_back(std::move(event));
    for (auto* revision : revisionSlots) {
        ++(*revision);
    }
    ++_publicationSequence;
    if (_journal.size() > _journalCapacity) {
        _journal.pop_front();
    }
    return result;
}

DocumentRevisionObservation DocumentRevisionIndex::publishUnknownModelMutation()
{
    auto revisions = publish(std::vector<DocumentRevisionPublicationRequest> {
        {DocumentRevisionKey::unknownModelMutation(), std::nullopt}});
    return revisions.front();
}

DocumentRevisionPollResult DocumentRevisionIndex::pollPublications(
    const DocumentRevisionCursor& cursor,
    std::size_t maxEvents) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_documentIdentity) {
        throw std::logic_error("document revision identity must be bound before polling");
    }

    DocumentRevisionPollResult result;
    result.requestedCursor = cursor;
    result.currentIdentity = *_documentIdentity;
    result.nextCursor = cursor;
    result.latestSequence = _publicationSequence;
    if (!_journal.empty()) {
        result.oldestAvailableSequence = _journal.front().publicationSequence;
    }

    if (cursor.documentInstanceId != _documentIdentity->documentInstanceId) {
        result.status = DocumentRevisionCursorStatus::ForeignDocument;
        return result;
    }
    if (cursor.lifecycleEpoch != _documentIdentity->lifecycleEpoch) {
        result.status = DocumentRevisionCursorStatus::StaleEpoch;
        return result;
    }
    if (cursor.afterSequence > _publicationSequence) {
        result.status = DocumentRevisionCursorStatus::FutureSequence;
        return result;
    }
    result.gap = result.oldestAvailableSequence != 0
        && cursor.afterSequence < result.oldestAvailableSequence - 1;

    if (maxEvents == 0) {
        return result;
    }
    result.events.reserve(std::min(maxEvents, _journal.size()));
    for (const auto& event : _journal) {
        if (event.publicationSequence <= cursor.afterSequence) {
            continue;
        }
        if (result.events.size() == maxEvents) {
            break;
        }
        result.events.push_back(event);
        result.nextCursor.afterSequence = event.publicationSequence;
    }
    return result;
}

DocumentRevision DocumentRevisionIndex::currentLocked(const DocumentRevisionKey& key) const noexcept
{
    const auto revision = _revisions.find(key);
    return revision == _revisions.end() ? 0 : revision->second;
}
