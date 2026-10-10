// SPDX-License-Identifier: LGPL-2.1-or-later

#include "DocumentCrossDocumentSnapshot.h"

#include "Document.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace App
{
namespace
{

struct CrossDocumentReservationSlot
{
    std::mutex mutex;
    std::uint64_t holderToken {0};
};

std::mutex g_reservationRegistryMutex;
std::unordered_map<DocumentInstanceId, std::shared_ptr<CrossDocumentReservationSlot>>
    g_reservationRegistry;
std::atomic<std::uint64_t> g_nextReservationHolderToken {1};

std::shared_ptr<CrossDocumentReservationSlot>
reservationSlotFor(DocumentInstanceId instanceId)
{
    std::lock_guard lock(g_reservationRegistryMutex);
    auto& slot = g_reservationRegistry[instanceId];
    if (!slot) {
        slot = std::make_shared<CrossDocumentReservationSlot>();
    }
    return slot;
}

}  // namespace

std::vector<DocumentRevisionConflict> DocumentCrossDocumentSnapshot::validateAgainstCurrent(
    const Document& document) const
{
    if (!valid()) {
        return {};
    }
    const auto currentIdentity = document.collaborationRevisions().documentIdentity();
    if (!currentIdentity || *currentIdentity != identity) {
        return {DocumentRevisionConflict(DocumentRevisionKey::documentStructure(), 0, 0)};
    }
    return document.collaborationRevisions().validate(revisionsAtCapture);
}

DocumentCrossDocumentSnapshot captureCrossDocumentSnapshot(
    const Document& document,
    const std::vector<DocumentRevisionKey>& keys)
{
    DocumentCrossDocumentSnapshot snapshot;
    const auto identity = document.collaborationRevisions().documentIdentity();
    if (!identity) {
        return snapshot;
    }
    snapshot.identity = *identity;
    snapshot.revisionsAtCapture = document.collaborationRevisions().capture(keys);
    return snapshot;
}

DocumentCrossDocumentReservationOutcome tryReserveDocumentsForCrossDocumentCommand(
    std::vector<DocumentRevisionIdentityBinding> documents,
    const DocumentCrossDocumentDependencyKind dependencyKind)
{
    DocumentCrossDocumentReservationOutcome outcome;
    if (dependencyKind == DocumentCrossDocumentDependencyKind::UndeclaredLiveReference) {
        outcome.result = DocumentCrossDocumentReservationResult::Unsupported;
        outcome.diagnostic =
            "undeclared live cross-document dependencies are not supported on the execution lane";
        return outcome;
    }

    std::sort(documents.begin(),
              documents.end(),
              [](const DocumentRevisionIdentityBinding& left,
                 const DocumentRevisionIdentityBinding& right) noexcept {
                  return left.documentInstanceId < right.documentInstanceId;
              });

    const std::uint64_t holderToken =
        g_nextReservationHolderToken.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::pair<std::shared_ptr<CrossDocumentReservationSlot>,
                          std::unique_lock<std::mutex>>>
        acquired;
    acquired.reserve(documents.size());

    for (const auto& identity : documents) {
        if (identity.documentInstanceId == 0) {
            for (auto& entry : acquired) {
                entry.first->holderToken = 0;
                entry.second.unlock();
            }
            outcome.result = DocumentCrossDocumentReservationResult::Conflict;
            outcome.diagnostic = "cross-document reservation requires a bound document identity";
            return outcome;
        }

        auto slot = reservationSlotFor(identity.documentInstanceId);
        std::unique_lock lock(slot->mutex, std::try_to_lock);
        if (!lock.owns_lock() || slot->holderToken != 0) {
            for (auto& entry : acquired) {
                entry.first->holderToken = 0;
                entry.second.unlock();
            }
            outcome.result = DocumentCrossDocumentReservationResult::Busy;
            outcome.diagnostic =
                "another cross-document command holds a required document reservation";
            return outcome;
        }
        slot->holderToken = holderToken;
        acquired.emplace_back(slot, std::move(lock));
        outcome.reservedInOrder.push_back(identity);
    }

    outcome.result = DocumentCrossDocumentReservationResult::Reserved;
    return outcome;
}

void releaseCrossDocumentReservations(
    const std::vector<DocumentRevisionIdentityBinding>& documents) noexcept
{
    for (const auto& identity : documents) {
        if (identity.documentInstanceId == 0) {
            continue;
        }
        const auto slot = reservationSlotFor(identity.documentInstanceId);
        std::lock_guard lock(slot->mutex);
        slot->holderToken = 0;
    }
}

}  // namespace App
