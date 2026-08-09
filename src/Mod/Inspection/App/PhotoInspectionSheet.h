// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTransform.h"
#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

enum class PageMedia
{
    A4,
    A3
};

enum class PageOrientation
{
    Portrait,
    Landscape
};

enum class ScenePrimitiveKind
{
    Polyline,
    Polygon,
    Text
};

struct InspectionExport ScenePrimitive
{
    std::string id;
    std::string layer;
    ScenePrimitiveKind kind {ScenePrimitiveKind::Polyline};
    std::vector<Vector2d> points;
    std::string text;
    double strokeWidthMm {0.0};
    bool closed {false};
    bool filled {false};
};

struct InspectionExport VectorScene
{
    double widthMm {0.0};
    double heightMm {0.0};
    std::vector<ScenePrimitive> primitives;
};

struct InspectionExport SheetIdentity
{
    std::string seriesUuid;
    std::string revisionUuid;
    std::string sourceToken;
    int revision {1};
};

struct InspectionExport SheetOptions
{
    PageMedia media {PageMedia::A4};
    PageOrientation orientation {PageOrientation::Portrait};
    double marginMm {5.0};
    double clearanceMm {5.0};
    double userRotationDegrees {0.0};
    bool includeCircleCenterMarks {false};
    AffineTransform2d physicalFromCommand;
};

struct InspectionExport SheetDraft
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    SheetIdentity identity;
    SheetOptions options;
    CanonicalProjection projection;
    VectorScene scene;
    std::string qrPayload;
    std::string projectionGeometrySha256;
    std::string qrContentSha256;
    std::string sheetContentSha256;
};

InspectionExport SheetDraft buildPhotoInspectionSheet(
    const CanonicalProjection& projection,
    const SheetIdentity& identity,
    const SheetOptions& options
);

InspectionExport std::string renderPhotoInspectionSvg(const VectorScene& scene);

InspectionExport const char* toString(PageMedia media);
InspectionExport const char* toString(PageOrientation orientation);

}  // namespace Inspection::Photo
