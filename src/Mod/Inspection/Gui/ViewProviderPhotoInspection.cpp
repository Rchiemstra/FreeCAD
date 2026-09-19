// SPDX-License-Identifier: LGPL-2.1-or-later

#include "ViewProviderPhotoInspection.h"

namespace InspectionGui
{

PROPERTY_SOURCE(InspectionGui::ViewProviderPhotoInspectionSheet, Gui::ViewProviderDocumentObject)

ViewProviderPhotoInspectionSheet::ViewProviderPhotoInspectionSheet() = default;
ViewProviderPhotoInspectionSheet::~ViewProviderPhotoInspectionSheet() = default;

PROPERTY_SOURCE(InspectionGui::ViewProviderPhotoInspectionResult, Gui::ViewProviderDocumentObject)

ViewProviderPhotoInspectionResult::ViewProviderPhotoInspectionResult() = default;
ViewProviderPhotoInspectionResult::~ViewProviderPhotoInspectionResult() = default;

}  // namespace InspectionGui
