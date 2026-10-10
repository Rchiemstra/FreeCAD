// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include "CollaborationRegistry.h"
#include "DocumentRevisionIndex.h"

#include <FCGlobal.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace App
{

class Document;
class DocumentObject;
class Property;

using DocumentCommandId = std::uint64_t;

enum class DocumentCommandKind
{
    Recompute,
    Edit,
    Undo,
    Redo,
    Save,
    Close
};

enum class DocumentCommandSubmitResult
{
    Accepted,
    Busy,
    Closed,
    Conflict,
    Unsupported
};

/** One copied object target referenced by stable identity, never by live pointer. */
struct AppExport DocumentCommandObjectTarget
{
    std::string stableObjectIdentity;
    std::string objectName;
};

/** One copied property value admitted by a command. */
struct AppExport DocumentCommandPropertyValue
{
    std::string stableObjectIdentity;
    std::string propertyName;
    std::string copiedValue;
};

/**
 * Pointer-free recompute payload carried by one admitted command.
 *
 * The coalescing key follows DocumentRecomputeRequest so an identical active
 * recompute may share the current handle instead of returning Busy.
 */
struct AppExport DocumentCommandRecomputePayload
{
    std::vector<std::string> featureIds;
    std::string coalescingKey;
    int options {0};
    /** When true, runs a forced recompute regardless of touched state. L5: use this field;
     *  do not encode force via coalescingKey. */
    bool force {false};
    bool refreshRevisionFenceAfterEachCommit {false};
    /** When true, foreign-document out-list dependencies use DeclaredSnapshot. */
    bool declaresCrossDocumentSnapshots {false};
};

/** Pointer-free edit payload carried by one admitted command. */
struct AppExport DocumentCommandEditPayload
{
    std::string operationId;
    std::string provenance;
    std::vector<DocumentCommandObjectTarget> targets;
    std::vector<DocumentCommandPropertyValue> propertyValues;
};

/** Pointer-free undo/redo payload carried by one admitted command. */
struct AppExport DocumentCommandTransactionPayload
{
    int steps {1};
};

/** Pointer-free save / save-as payload carried by one admitted Save command. */
struct AppExport DocumentCommandSavePayload
{
    /** When empty, the lane runs canonical Document::save(). */
    std::string targetPath;
    bool overwrite {false};
    std::string expectedDestinationSha256;
    bool saveAs {false};
};

/**
 * Immutable, pointer-free document command submitted through DocumentHandle.
 *
 * Commands copy every document/object identity, expected revision, and value
 * they need. They never retain live Document, DocumentObject, or Property
 * pointers.
 */
struct AppExport DocumentCommand
{
    DocumentCommandKind kind {DocumentCommandKind::Recompute};
    DocumentRevisionIdentityBinding document;
    std::vector<DocumentRevisionObservation> expectedRevisions;
    std::optional<DocumentCommandRecomputePayload> recompute;
    std::optional<DocumentCommandEditPayload> edit;
    std::optional<DocumentCommandTransactionPayload> transaction;
    std::optional<DocumentCommandSavePayload> save;
    std::string diagnostic;

    [[nodiscard]] bool pointerFree() const noexcept
    {
        return true;
    }
};

/** Complete non-blocking admission result for one trySubmit call. */
struct AppExport DocumentCommandSubmitOutcome
{
    DocumentCommandSubmitResult result {DocumentCommandSubmitResult::Unsupported};
    DocumentCommandId commandId {0};
    std::string diagnostic;

    [[nodiscard]] bool accepted() const noexcept
    {
        return result == DocumentCommandSubmitResult::Accepted;
    }
};

AppExport const char* documentCommandKindName(DocumentCommandKind kind) noexcept;
AppExport const char* documentCommandSubmitResultName(
    DocumentCommandSubmitResult result) noexcept;

/** True when an active command of this kind must reject owner-thread dispatch hops. */
[[nodiscard]] constexpr bool documentCommandKindBlocksOwnerDispatch(
    DocumentCommandKind kind) noexcept
{
    switch (kind) {
        case DocumentCommandKind::Recompute:
            return true;
        case DocumentCommandKind::Edit:
        case DocumentCommandKind::Undo:
        case DocumentCommandKind::Redo:
        case DocumentCommandKind::Save:
        case DocumentCommandKind::Close:
            return true;
    }
    return true;
}

static_assert(!std::is_pointer_v<DocumentCommandId>);
static_assert(std::is_trivially_copyable_v<DocumentRevisionIdentityBinding>);

}  // namespace App
