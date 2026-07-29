// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <App/DocumentObject.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionEngine.h"
#include "PhotoInspectionSheet.h"

namespace Inspection::Photo
{

class InspectionExport PhotoInspectionSheet: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Inspection::Photo::PhotoInspectionSheet);

public:
    PhotoInspectionSheet();
    ~PhotoInspectionSheet() override;

    App::PropertyString SheetSeriesUUID;
    App::PropertyString SheetRevisionUUID;
    App::PropertyInteger Revision;
    App::PropertyString SourceToken;
    App::PropertyLinkSub Source;
    App::PropertyString ProjectionGeometrySha256;
    App::PropertyString SheetContentSha256;
    App::PropertyString CanonicalProjectionHex;
    App::PropertyString SceneSvg;
    App::PropertyString QrPayload;
    App::PropertyString SchemaVersion;
    App::PropertyBool Sealed;

    bool initializeFromDraft(const SheetDraft& draft, std::string& reason);

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;

    const char* getViewProviderName() const override
    {
        return "InspectionGui::ViewProviderPhotoInspectionSheet";
    }
};

class InspectionExport PhotoInspectionResult: public App::DocumentObject
{
    PROPERTY_HEADER_WITH_OVERRIDE(Inspection::Photo::PhotoInspectionResult);

public:
    PhotoInspectionResult();
    ~PhotoInspectionResult() override;

    App::PropertyString ResultUUID;
    App::PropertyString SheetRevisionUUID;
    App::PropertyString ProjectionGeometrySha256;
    App::PropertyString OperationStatus;
    App::PropertyString Decision;
    App::PropertyString ReportJson;
    App::PropertyString ReportSha256;
    App::PropertyString SchemaVersion;
    App::PropertyBool Sealed;

    bool initializeFromResult(
        const AnalysisResult& result,
        const std::string& resultUuid,
        const std::string& sheetRevisionUuid,
        std::string& reason
    );

    App::DocumentObjectExecReturn* execute() override;
    short mustExecute() const override;

    const char* getViewProviderName() const override
    {
        return "InspectionGui::ViewProviderPhotoInspectionResult";
    }
};

}  // namespace Inspection::Photo
