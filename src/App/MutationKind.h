// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <cstdint>

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
    /** add/change/rename/remove of dynamic properties */
    StructuralProperty = 1u << 14,
};

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
        case MutationKind::StructuralProperty:
            return "StructuralProperty";
    }
    return "Unknown";
}

}  // namespace App
