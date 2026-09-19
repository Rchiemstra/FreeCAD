// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <string>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

struct InspectionExport AtomicWriteOptions
{
    std::string allowedRoot;
    bool replaceExisting {false};
    std::size_t maximumBytes {4U * 1024U * 1024U};
};

InspectionExport ValidationResult writePhotoInspectionFileAtomically(
    const std::string& target,
    const std::string& content,
    const AtomicWriteOptions& options
);

}  // namespace Inspection::Photo
