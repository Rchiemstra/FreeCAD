// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionObject.h"

#include "PhotoInspectionReport.h"

#include <algorithm>
#include <cctype>

namespace Inspection::Photo
{
namespace
{

std::string hex(const std::vector<std::uint8_t>& bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        result.push_back(digits[(byte >> 4U) & 0x0fU]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

bool safeIdentity(const std::string& value)
{
    return !value.empty() && value.size() <= 64
        && std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return std::isalnum(character) || character == '-' || character == '_';
           });
}

}  // namespace

PROPERTY_SOURCE(Inspection::Photo::PhotoInspectionSheet, App::DocumentObject)

PhotoInspectionSheet::PhotoInspectionSheet()
{
    constexpr auto immutable = static_cast<App::PropertyType>(
        App::Prop_ReadOnly | App::Prop_NoRecompute
    );
    constexpr auto immutableHidden = static_cast<App::PropertyType>(immutable | App::Prop_Hidden);

    ADD_PROPERTY_TYPE(SheetSeriesUUID, (""), "Identity", immutable, "Immutable sheet series UUID");
    ADD_PROPERTY_TYPE(SheetRevisionUUID, (""), "Identity", immutable, "Immutable sheet revision UUID");
    ADD_PROPERTY_TYPE(Revision, (0), "Identity", immutable, "Immutable positive revision number");
    ADD_PROPERTY_TYPE(SourceToken, (""), "Source", immutable, "Frozen opaque source identity");
    ADD_PROPERTY_TYPE(Source, (nullptr), "Source", immutable, "Navigation-only source reference");
    ADD_PROPERTY_TYPE(
        ProjectionGeometrySha256,
        (""),
        "Identity",
        immutable,
        "Canonical projected geometry SHA-256"
    );
    ADD_PROPERTY_TYPE(
        QrContentSha256,
        (""),
        "Identity",
        immutable,
        "SHA-256 of the pre-identity sheet semantic recipe; matches the sh field in the printed QR code"
    );
    ADD_PROPERTY_TYPE(
        SheetContentSha256,
        (""),
        "Identity",
        immutable,
        "SHA-256 of the sealed sheet semantic recipe including identity graphics and the final SVG"
    );
    ADD_PROPERTY_TYPE(
        CanonicalProjectionHex,
        (""),
        "Payload",
        immutableHidden,
        "Canonical projection byte stream encoded as hexadecimal"
    );
    ADD_PROPERTY_TYPE(SceneSvg, (""), "Payload", immutableHidden, "Deterministic vector scene snapshot");
    ADD_PROPERTY_TYPE(
        QrPayload,
        (""),
        "Payload",
        immutableHidden,
        "Bounded canonical local identity payload"
    );
    ADD_PROPERTY_TYPE(SchemaVersion, ("1.0"), "Identity", immutable, "Sheet object schema version");
    ADD_PROPERTY_TYPE(Sealed, (false), "Identity", immutable, "True after atomic initialization");

    SheetSeriesUUID.setStatus(App::Property::ReadOnly, true);
    SheetRevisionUUID.setStatus(App::Property::ReadOnly, true);
    Revision.setStatus(App::Property::ReadOnly, true);
    SourceToken.setStatus(App::Property::ReadOnly, true);
    Source.setStatus(App::Property::ReadOnly, true);
    ProjectionGeometrySha256.setStatus(App::Property::ReadOnly, true);
    QrContentSha256.setStatus(App::Property::ReadOnly, true);
    SheetContentSha256.setStatus(App::Property::ReadOnly, true);
    CanonicalProjectionHex.setStatus(App::Property::ReadOnly, true);
    SceneSvg.setStatus(App::Property::ReadOnly, true);
    QrPayload.setStatus(App::Property::ReadOnly, true);
    SchemaVersion.setStatus(App::Property::ReadOnly, true);
    Sealed.setStatus(App::Property::ReadOnly, true);
}

PhotoInspectionSheet::~PhotoInspectionSheet() = default;

bool PhotoInspectionSheet::initializeFromDraft(const SheetDraft& draft, std::string& reason)
{
    if (Sealed.getValue()) {
        reason = "sheet object is already sealed";
        return false;
    }
    if (draft.status != OperationStatus::Complete || draft.projection.bytes.empty()
        || draft.projectionGeometrySha256.size() != 64 || draft.qrContentSha256.size() != 64
        || draft.sheetContentSha256.size() != 64
        || !safeIdentity(draft.identity.seriesUuid) || !safeIdentity(draft.identity.revisionUuid)
        || draft.identity.revision < 1) {
        reason = "sheet draft is incomplete or invalid";
        return false;
    }
    const std::string svg = renderPhotoInspectionSvg(draft.scene);
    if (svg.empty() || draft.qrPayload.empty() || draft.qrPayload.size() > 320) {
        reason = "sheet vector or identity payload is incomplete";
        return false;
    }

    SheetSeriesUUID.setValue(draft.identity.seriesUuid);
    SheetRevisionUUID.setValue(draft.identity.revisionUuid);
    Revision.setValue(draft.identity.revision);
    SourceToken.setValue(draft.identity.sourceToken);
    ProjectionGeometrySha256.setValue(draft.projectionGeometrySha256);
    QrContentSha256.setValue(draft.qrContentSha256);
    SheetContentSha256.setValue(draft.sheetContentSha256);
    CanonicalProjectionHex.setValue(hex(draft.projection.bytes));
    SceneSvg.setValue(svg);
    QrPayload.setValue(draft.qrPayload);
    SchemaVersion.setValue("1.0");
    Sealed.setValue(true);
    reason.clear();
    return true;
}

App::DocumentObjectExecReturn* PhotoInspectionSheet::execute()
{
    return App::DocumentObject::StdReturn;
}

short PhotoInspectionSheet::mustExecute() const
{
    return 0;
}

PROPERTY_SOURCE(Inspection::Photo::PhotoInspectionResult, App::DocumentObject)

PhotoInspectionResult::PhotoInspectionResult()
{
    constexpr auto immutable = static_cast<App::PropertyType>(
        App::Prop_ReadOnly | App::Prop_NoRecompute
    );
    constexpr auto immutableHidden = static_cast<App::PropertyType>(immutable | App::Prop_Hidden);

    ADD_PROPERTY_TYPE(ResultUUID, (""), "Identity", immutable, "Immutable result UUID");
    ADD_PROPERTY_TYPE(
        SheetRevisionUUID,
        (""),
        "Identity",
        immutable,
        "Associated immutable sheet revision UUID"
    );
    ADD_PROPERTY_TYPE(
        ProjectionGeometrySha256,
        (""),
        "Identity",
        immutable,
        "Analyzed canonical projection SHA-256"
    );
    ADD_PROPERTY_TYPE(
        OperationStatus,
        ("InvalidInput"),
        "Decision",
        immutable,
        "Operation completion status"
    );
    ADD_PROPERTY_TYPE(Decision, ("NotEvaluated"), "Decision", immutable, "Separate conformance decision");
    ADD_PROPERTY_TYPE(ReportJson, (""), "Payload", immutableHidden, "Canonical immutable result report");
    ADD_PROPERTY_TYPE(ReportSha256, (""), "Identity", immutable, "Canonical result report SHA-256");
    ADD_PROPERTY_TYPE(SchemaVersion, ("1.0"), "Identity", immutable, "Result object schema version");
    ADD_PROPERTY_TYPE(Sealed, (false), "Identity", immutable, "True after atomic initialization");

    ResultUUID.setStatus(App::Property::ReadOnly, true);
    SheetRevisionUUID.setStatus(App::Property::ReadOnly, true);
    ProjectionGeometrySha256.setStatus(App::Property::ReadOnly, true);
    OperationStatus.setStatus(App::Property::ReadOnly, true);
    Decision.setStatus(App::Property::ReadOnly, true);
    ReportJson.setStatus(App::Property::ReadOnly, true);
    ReportSha256.setStatus(App::Property::ReadOnly, true);
    SchemaVersion.setStatus(App::Property::ReadOnly, true);
    Sealed.setStatus(App::Property::ReadOnly, true);
}

PhotoInspectionResult::~PhotoInspectionResult() = default;

bool PhotoInspectionResult::initializeFromResult(
    const AnalysisResult& result,
    const std::string& resultUuid,
    const std::string& sheetRevisionUuid,
    std::string& reason
)
{
    if (Sealed.getValue()) {
        reason = "result object is already sealed";
        return false;
    }
    if (result.status != Inspection::Photo::OperationStatus::Complete || !safeIdentity(resultUuid)
        || !safeIdentity(sheetRevisionUuid) || result.projectionGeometrySha256.size() != 64) {
        reason = "analysis result identity or status is invalid";
        return false;
    }
    const ReportSerialization report = toCanonicalJson(result);
    if (!report.valid) {
        reason = report.diagnostic.message;
        return false;
    }

    ResultUUID.setValue(resultUuid);
    SheetRevisionUUID.setValue(sheetRevisionUuid);
    ProjectionGeometrySha256.setValue(result.projectionGeometrySha256);
    OperationStatus.setValue(toString(result.status));
    Decision.setValue(toString(result.decision));
    ReportJson.setValue(report.content);
    ReportSha256.setValue(report.sha256);
    SchemaVersion.setValue("1.0");
    Sealed.setValue(true);
    reason.clear();
    return true;
}

App::DocumentObjectExecReturn* PhotoInspectionResult::execute()
{
    return App::DocumentObject::StdReturn;
}

short PhotoInspectionResult::mustExecute() const
{
    return 0;
}

}  // namespace Inspection::Photo
