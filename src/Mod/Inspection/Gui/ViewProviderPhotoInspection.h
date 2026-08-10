// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <Gui/ViewProviderDocumentObject.h>
#include <Mod/Inspection/InspectionGlobal.h>

namespace InspectionGui
{

class InspectionGuiExport ViewProviderPhotoInspectionSheet: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(InspectionGui::ViewProviderPhotoInspectionSheet);

public:
    ViewProviderPhotoInspectionSheet();
    ~ViewProviderPhotoInspectionSheet() override;
};

class InspectionGuiExport ViewProviderPhotoInspectionResult: public Gui::ViewProviderDocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(InspectionGui::ViewProviderPhotoInspectionResult);

public:
    ViewProviderPhotoInspectionResult();
    ~ViewProviderPhotoInspectionResult() override;
};

}  // namespace InspectionGui
