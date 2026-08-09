// SPDX-License-Identifier: LGPL-2.1-or-later
// SPDX-FileCopyrightText: 2026 Joao Matos
// SPDX-FileNotice: Part of the FreeCAD project.

/******************************************************************************
 *                                                                            *
 *   FreeCAD is free software: you can redistribute it and/or modify          *
 *   it under the terms of the GNU Lesser General Public License as           *
 *   published by the Free Software Foundation, either version 2.1            *
 *   of the License, or (at your option) any later version.                   *
 *                                                                            *
 *   FreeCAD is distributed in the hope that it will be useful,               *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty              *
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                  *
 *   See the GNU Lesser General Public License for more details.              *
 *                                                                            *
 *   You should have received a copy of the GNU Lesser General Public         *
 *   License along with FreeCAD. If not, see https://www.gnu.org/licenses     *
 *                                                                            *
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "CollaborationRegistry.h"
#include "DocumentRevisionIndex.h"
#include "ExportInfo.h"

namespace App
{
class Document;

/**
 * Pointer-free provenance for one stable recovery snapshot.
 *
 * These values describe the source runtime boundary for diagnostics only.
 * They must never be restored as a live document identity, lifecycle epoch,
 * revision counter, publication cursor, or mutation authority.
 */
struct AppExport RecoverySnapshotMetadata
{
    static constexpr std::uint32_t CurrentSchemaVersion = 1;

    std::uint32_t schemaVersion {CurrentSchemaVersion};
    DocumentInstanceId sourceDocumentInstanceId {0};
    DocumentLifecycleEpoch sourceLifecycleEpoch {0};
    DocumentPublicationSequence latestPublicationSequence {0};

    [[nodiscard]] bool valid() const noexcept;
};

inline bool operator==(const RecoverySnapshotMetadata& left,
                       const RecoverySnapshotMetadata& right) noexcept
{
    return left.schemaVersion == right.schemaVersion
        && left.sourceDocumentInstanceId == right.sourceDocumentInstanceId
        && left.sourceLifecycleEpoch == right.sourceLifecycleEpoch
        && left.latestPublicationSequence == right.latestPublicationSequence;
}

inline bool operator!=(const RecoverySnapshotMetadata& left,
                       const RecoverySnapshotMetadata& right) noexcept
{
    return !(left == right);
}

/**
 * Capture provenance after the caller has established a stable committed boundary.
 *
 * The function verifies that the registry and revision stream describe the same
 * instance and epoch. It does not acquire the document commit/owner-thread
 * serialization boundary itself.
 */
AppExport RecoverySnapshotMetadata captureRecoverySnapshotMetadata(const Document& doc);

/** Build the complete auto-recovery metadata XML, escaping user-controlled text. */
AppExport std::string serializeRecoverySnapshotMetadata(
    std::string_view label,
    std::string_view fileName,
    const std::optional<RecoverySnapshotMetadata>& collaborationMetadata
);

/**
 * Parse optional collaboration provenance from complete auto-recovery metadata XML.
 *
 * Legacy XML without CollaborationProvenance and malformed/unsupported optional
 * provenance both return std::nullopt. Parsed values remain diagnostic only.
 */
AppExport std::optional<RecoverySnapshotMetadata>
parseRecoverySnapshotMetadata(std::string_view metadataXml) noexcept;

struct AppExport RecoverySnapshotSaveOptions
{
    bool compressed {true};
    bool saveBinaryBrep {true};
    bool saveThumbnail {false};
};

AppExport bool writeRecoverySnapshotToTransientDir(
    const Document& doc,
    const RecoverySnapshotSaveOptions& options
);
}  // namespace App
