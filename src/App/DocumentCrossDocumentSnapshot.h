// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "DocumentCommand.h"
#include "DocumentHandle.h"
#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

#include <cstdint>
#include <string>
#include <vector>

namespace App
{

class Document;

/** How a multi-document command declared its cross-document inputs. */
enum class DocumentCrossDocumentDependencyKind
{
    /** Reads use revision-bound snapshots captured before execution. */
    DeclaredSnapshot,
    /** Live Document or DocumentObject pointers across documents (unsupported). */
    UndeclaredLiveReference
};

/** Immutable, revision-bound read view of one foreign document. */
struct AppExport DocumentCrossDocumentSnapshot
{
    DocumentRevisionIdentityBinding identity;
    std::vector<DocumentRevisionObservation> revisionsAtCapture;

    [[nodiscard]] bool valid() const noexcept
    {
        return identity.documentInstanceId != 0;
    }

    [[nodiscard]] std::vector<DocumentRevisionConflict>
    validateAgainstCurrent(const Document& document) const;
};

enum class DocumentCrossDocumentReservationResult
{
    Reserved,
    Busy,
    Conflict,
    Unsupported
};

/** Outcome of reserving multiple documents without blocking. */
struct AppExport DocumentCrossDocumentReservationOutcome
{
    DocumentCrossDocumentReservationResult result {
        DocumentCrossDocumentReservationResult::Unsupported};
    std::string diagnostic;
    /** Documents reserved in ascending \c DocumentInstanceId order. */
    std::vector<DocumentRevisionIdentityBinding> reservedInOrder;
};

/**
 * Capture one revision-bound snapshot for cross-document reads.
 *
 * The snapshot is immutable and safe to retain while model work continues on
 * the source document thread.
 */
[[nodiscard]] AppExport DocumentCrossDocumentSnapshot captureCrossDocumentSnapshot(
    const Document& document,
    const std::vector<DocumentRevisionKey>& keys);

/**
 * Reserve documents for one multi-document command.
 *
 * Identities are sorted by \c DocumentInstanceId. Each reservation is acquired
 * with \c try_lock only; the call never waits while holding another document
 * reservation. Undeclared live cross-document dependencies return Unsupported.
 */
[[nodiscard]] AppExport DocumentCrossDocumentReservationOutcome
tryReserveDocumentsForCrossDocumentCommand(
    std::vector<DocumentRevisionIdentityBinding> documents,
    DocumentCrossDocumentDependencyKind dependencyKind);

/** Release reservations acquired by \c tryReserveDocumentsForCrossDocumentCommand. */
AppExport void releaseCrossDocumentReservations(
    const std::vector<DocumentRevisionIdentityBinding>& documents) noexcept;

}  // namespace App
