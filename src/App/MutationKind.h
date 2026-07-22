// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>
#include <string>

namespace App
{

/** Classifies a document mutation for authority checks. */
enum class MutationKind : std::uint32_t
{
    PropertyWrite = 1u << 0,
    AddObject = 1u << 1,
    RemoveObject = 1u << 2,
    Recompute = 1u << 3,
    Undo = 1u << 4,
    Redo = 1u << 5,
    Save = 1u << 6,
    SaveAs = 1u << 7,
    Close = 1u << 8,
    TransactionOpen = 1u << 9,
    TransactionCommit = 1u << 10,
    TransactionAbort = 1u << 11,
    ImportExport = 1u << 12,
    BulkCopy = 1u << 13,
};

using MutationKindMask = std::uint32_t;

constexpr MutationKindMask mutationKindBit(MutationKind kind)
{
    return static_cast<MutationKindMask>(kind);
}

constexpr MutationKindMask MutationKindAll =
    mutationKindBit(MutationKind::PropertyWrite) | mutationKindBit(MutationKind::AddObject)
    | mutationKindBit(MutationKind::RemoveObject) | mutationKindBit(MutationKind::Recompute)
    | mutationKindBit(MutationKind::Undo) | mutationKindBit(MutationKind::Redo)
    | mutationKindBit(MutationKind::Save) | mutationKindBit(MutationKind::SaveAs)
    | mutationKindBit(MutationKind::Close) | mutationKindBit(MutationKind::TransactionOpen)
    | mutationKindBit(MutationKind::TransactionCommit)
    | mutationKindBit(MutationKind::TransactionAbort)
    | mutationKindBit(MutationKind::ImportExport) | mutationKindBit(MutationKind::BulkCopy);

inline const char* mutationKindName(MutationKind kind)
{
    switch (kind) {
        case MutationKind::PropertyWrite:
            return "PropertyWrite";
        case MutationKind::AddObject:
            return "AddObject";
        case MutationKind::RemoveObject:
            return "RemoveObject";
        case MutationKind::Recompute:
            return "Recompute";
        case MutationKind::Undo:
            return "Undo";
        case MutationKind::Redo:
            return "Redo";
        case MutationKind::Save:
            return "Save";
        case MutationKind::SaveAs:
            return "SaveAs";
        case MutationKind::Close:
            return "Close";
        case MutationKind::TransactionOpen:
            return "TransactionOpen";
        case MutationKind::TransactionCommit:
            return "TransactionCommit";
        case MutationKind::TransactionAbort:
            return "TransactionAbort";
        case MutationKind::ImportExport:
            return "ImportExport";
        case MutationKind::BulkCopy:
            return "BulkCopy";
    }
    return "Unknown";
}

enum class MutationOwner : std::uint8_t
{
    Unrestricted = 0,
    McpOwned = 1,
    UserOwned = 2,
};

enum class MutationOrigin : std::uint8_t
{
    Gui = 0,
    Python = 1,
    Mcp = 2,
    Cpp = 3,
    Internal = 4,
};

enum class MutationDecision : std::uint8_t
{
    Allow = 0,
    DenyNoCapability = 1,
    DenyWrongOwner = 2,
    DenyStaleGeneration = 3,
    DenyRecoveryMode = 4,
    RequireTakeover = 5,
};

inline const char* mutationDecisionName(MutationDecision decision)
{
    switch (decision) {
        case MutationDecision::Allow:
            return "ALLOW";
        case MutationDecision::DenyNoCapability:
            return "DENY_NO_CAPABILITY";
        case MutationDecision::DenyWrongOwner:
            return "DENY_WRONG_OWNER";
        case MutationDecision::DenyStaleGeneration:
            return "DENY_STALE_GENERATION";
        case MutationDecision::DenyRecoveryMode:
            return "DENY_RECOVERY_MODE";
        case MutationDecision::RequireTakeover:
            return "REQUIRE_TAKEOVER";
    }
    return "UNKNOWN";
}

inline const char* mutationOwnerName(MutationOwner owner)
{
    switch (owner) {
        case MutationOwner::Unrestricted:
            return "unrestricted";
        case MutationOwner::McpOwned:
            return "mcp";
        case MutationOwner::UserOwned:
            return "user";
    }
    return "unknown";
}

struct MutationContext
{
    MutationKind kind {MutationKind::PropertyWrite};
    MutationOrigin origin {MutationOrigin::Cpp};
    std::uint64_t fencingGeneration {0};
    std::uint64_t capabilityId {0};
    std::string documentName;
    std::string objectName;
    std::string propertyName;
    std::string transactionName;
    bool multiDocument {false};
};

}  // namespace App
