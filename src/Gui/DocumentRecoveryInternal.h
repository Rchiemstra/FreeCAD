// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <FCGlobal.h>
#include <QString>

namespace Gui::Dialog::DocumentRecoveryInternal
{

/// Distinguishes open/read failures (e.g. EMFILE / locked file) from malformed content.
enum class ProjectValidationResult
{
    Ok,
    OpenFailed,
    InvalidContent
};

/// Rough ZIP integrity check. Opens a stream per entry; streams are owned via unique_ptr.
ProjectValidationResult GuiExport checkZipData(const QString& fcstdFile);

/// Validates Document.xml (required) and GuiDocument.xml (optional if present).
ProjectValidationResult GuiExport checkXmlFiles(const QString& fcstdFile);

/// Recovery pre-check: checkZipData then checkXmlFiles; returns the first non-Ok result.
ProjectValidationResult GuiExport validateProjectArchive(const QString& fcstdFile);

}  // namespace Gui::Dialog::DocumentRecoveryInternal
