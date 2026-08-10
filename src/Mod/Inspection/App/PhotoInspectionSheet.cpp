// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionSheet.h"

#include "OpenCVPhotoInspectionCompat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace Inspection::Photo
{
namespace
{

constexpr double markerSizeMm = 12.0;
constexpr double markerQuietZoneMm = 2.0;
constexpr double qrBoxMm = 24.0;

struct Bounds
{
    double minimumX {std::numeric_limits<double>::infinity()};
    double minimumY {std::numeric_limits<double>::infinity()};
    double maximumX {-std::numeric_limits<double>::infinity()};
    double maximumY {-std::numeric_limits<double>::infinity()};

    void include(const Vector2d& point)
    {
        minimumX = std::min(minimumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumX = std::max(maximumX, point.x);
        maximumY = std::max(maximumY, point.y);
    }

    double width() const
    {
        return maximumX - minimumX;
    }

    double height() const
    {
        return maximumY - minimumY;
    }
};

bool finite(const double value)
{
    return std::isfinite(value);
}

std::pair<double, double> pageDimensions(const PageMedia media, const PageOrientation orientation)
{
    double width = media == PageMedia::A4 ? 210.0 : 297.0;
    double height = media == PageMedia::A4 ? 297.0 : 420.0;
    if (orientation == PageOrientation::Landscape) {
        std::swap(width, height);
    }
    return {width, height};
}

Vector2d rotate(const Vector2d& point, const double radians)
{
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);
    return {
        cosine * point.x - sine * point.y,
        sine * point.x + cosine * point.y,
    };
}

std::string number(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed << std::setprecision(6) << value;
    std::string result = stream.str();
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    if (result == "-0") {
        result = "0";
    }
    return result;
}

std::string escapeXml(const std::string& source)
{
    std::string result;
    result.reserve(source.size());
    for (const char character : source) {
        switch (character) {
            case '&':
                result += "&amp;";
                break;
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '"':
                result += "&quot;";
                break;
            case '\'':
                result += "&apos;";
                break;
            default:
                result.push_back(character);
                break;
        }
    }
    return result;
}

bool validIdentityToken(const std::string& token, const std::size_t maximumLength)
{
    if (token.empty() || token.size() > maximumLength) {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](const unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '_';
    });
}

std::vector<std::uint8_t> bytes(const std::string& value)
{
    return {value.begin(), value.end()};
}

std::string buildQrPayload(
    const SheetIdentity& identity,
    const std::string& projectionHash,
    const std::string& sheetHash,
    const PageMedia media,
    const PageOrientation orientation
)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "{\"v\":[1,0],\"sid\":\"" << identity.revisionUuid << "\",\"r\":" << identity.revision
           << ",\"so\":\"" << identity.sourceToken << "\",\"ph\":\"" << projectionHash
           << "\",\"sh\":\"" << sheetHash << "\",\"pg\":\"" << toString(media)
           << (orientation == PageOrientation::Landscape ? "L" : "P") << "\"}";
    return stream.str();
}

Vector2d compensate(const AffineTransform2d& commandFromPhysical, const Vector2d& physicalPoint)
{
    const Point2d command = commandFromPhysical.apply({physicalPoint.x, physicalPoint.y});
    return {command.x, command.y};
}

std::vector<Vector2d> rectangle(
    const AffineTransform2d& commandFromPhysical,
    const double x,
    const double y,
    const double width,
    const double height
)
{
    return {
        compensate(commandFromPhysical, {x, y}),
        compensate(commandFromPhysical, {x + width, y}),
        compensate(commandFromPhysical, {x + width, y + height}),
        compensate(commandFromPhysical, {x, y + height}),
    };
}

void addBinaryGrid(
    VectorScene& scene,
    const BinaryGrid& grid,
    const AffineTransform2d& commandFromPhysical,
    const std::string& idPrefix,
    const std::string& layer,
    const double physicalX,
    const double physicalY,
    const double physicalSize
)
{
    const double cellWidth = physicalSize / grid.columns;
    const double cellHeight = physicalSize / grid.rows;
    for (int row = 0; row < grid.rows; ++row) {
        for (int column = 0; column < grid.columns; ++column) {
            if (!grid.black(row, column)) {
                continue;
            }
            ScenePrimitive cell;
            cell.id = idPrefix + "-r" + std::to_string(row) + "-c" + std::to_string(column);
            cell.layer = layer;
            cell.kind = ScenePrimitiveKind::Polygon;
            cell.points = rectangle(
                commandFromPhysical,
                physicalX + column * cellWidth,
                physicalY + row * cellHeight,
                cellWidth,
                cellHeight
            );
            cell.closed = true;
            cell.filled = true;
            scene.primitives.push_back(std::move(cell));
        }
    }
}

Diagnostic error(const DiagnosticCode code, std::string message)
{
    return {code, DiagnosticSeverity::Error, std::move(message)};
}

}  // namespace

SheetDraft buildPhotoInspectionSheet(
    const CanonicalProjection& projection,
    const SheetIdentity& identity,
    const SheetOptions& options
)
{
    SheetDraft result;
    result.identity = identity;
    result.options = options;
    result.projection = projection;
    result.projectionGeometrySha256 = projection.sha256;

    if (projection.bytes.empty() || projection.sha256.size() != 64
        || projection.snapshot.cycles.empty()) {
        result.diagnostic
            = error(DiagnosticCode::InvalidGeometry, "canonical projection is incomplete");
        return result;
    }
    if (!validIdentityToken(identity.seriesUuid, 64) || !validIdentityToken(identity.revisionUuid, 64)
        || !validIdentityToken(identity.sourceToken, 96) || identity.revision < 1) {
        result.diagnostic = error(DiagnosticCode::InvalidSchema, "sheet identity is invalid");
        return result;
    }
    if (!finite(options.marginMm) || options.marginMm < 5.0 || options.marginMm > 50.0
        || !finite(options.clearanceMm) || options.clearanceMm < 5.0 || options.clearanceMm > 50.0
        || !finite(options.userRotationDegrees) || std::abs(options.userRotationDegrees) > 360.0) {
        result.diagnostic
            = error(DiagnosticCode::InvalidSchema, "sheet layout options are outside safe limits");
        return result;
    }

    const auto commandFromPhysical = printerCommandFromPhysical(options.physicalFromCommand);
    if (!commandFromPhysical) {
        result.diagnostic
            = error(DiagnosticCode::NumericalFailure, "printer transform is not invertible");
        return result;
    }

    const auto [pageWidth, pageHeight] = pageDimensions(options.media, options.orientation);
    result.scene.widthMm = pageWidth;
    result.scene.heightMm = pageHeight;

    const double furnitureInset = options.marginMm + markerQuietZoneMm + markerSizeMm
        + options.clearanceMm;
    const double qrCornerGapMm = markerSizeMm + 2.0 * markerQuietZoneMm;
    const double estimatedQrTopY = pageHeight - options.marginMm - qrBoxMm;
    const double estimatedQrLeftX = pageWidth - options.marginMm - qrBoxMm - qrCornerGapMm;
    const double roiLeft = furnitureInset;
    const double roiTop = furnitureInset;
    const double roiRight = std::min(pageWidth - furnitureInset, estimatedQrLeftX - options.clearanceMm);
    const double roiBottom = std::min(pageHeight - furnitureInset, estimatedQrTopY - options.clearanceMm);
    if (roiRight <= roiLeft || roiBottom <= roiTop) {
        result.diagnostic
            = error(DiagnosticCode::InvalidGeometry, "page has no usable measurement region");
        return result;
    }

    const double radians = options.userRotationDegrees * std::acos(-1.0) / 180.0;
    std::vector<PolylineCycle> rotatedCycles = projection.snapshot.cycles;
    Bounds projectionBounds;
    for (PolylineCycle& cycle : rotatedCycles) {
        for (Vector2d& point : cycle.points) {
            point = rotate(point, radians);
            projectionBounds.include(point);
        }
    }

    const double availableWidth = roiRight - roiLeft - 2.0 * options.clearanceMm;
    const double availableHeight = roiBottom - roiTop - 2.0 * options.clearanceMm;
    if (projectionBounds.width() > availableWidth || projectionBounds.height() > availableHeight) {
        result.diagnostic
            = error(DiagnosticCode::InvalidGeometry, "projection does not fit the selected page");
        return result;
    }

    const double targetCenterX = (roiLeft + roiRight) * 0.5;
    const double targetCenterY = (roiTop + roiBottom) * 0.5;
    const double sourceCenterX = (projectionBounds.minimumX + projectionBounds.maximumX) * 0.5;
    const double sourceCenterY = (projectionBounds.minimumY + projectionBounds.maximumY) * 0.5;
    for (std::size_t index = 0; index < rotatedCycles.size(); ++index) {
        ScenePrimitive boundary;
        boundary.id = "cad-cycle-" + std::to_string(index);
        boundary.layer = "cad-boundary";
        boundary.kind = ScenePrimitiveKind::Polyline;
        boundary.closed = true;
        boundary.strokeWidthMm = 0.15;
        for (const Vector2d& point : rotatedCycles[index].points) {
            const Vector2d physical {
                point.x - sourceCenterX + targetCenterX,
                point.y - sourceCenterY + targetCenterY,
            };
            boundary.points.push_back(compensate(*commandFromPhysical, physical));
        }
        result.scene.primitives.push_back(std::move(boundary));
    }

    const double markerInset = options.marginMm + markerQuietZoneMm;
    const double middleX = (pageWidth - markerSizeMm) * 0.5;
    const double middleY = (pageHeight - markerSizeMm) * 0.5;
    const std::array<Vector2d, 8> markerOrigins {
        Vector2d {markerInset, markerInset},
        Vector2d {middleX, markerInset},
        Vector2d {pageWidth - markerInset - markerSizeMm, markerInset},
        Vector2d {pageWidth - markerInset - markerSizeMm, middleY},
        Vector2d {
            pageWidth - markerInset - markerSizeMm,
            pageHeight - markerInset - markerSizeMm,
        },
        Vector2d {middleX, pageHeight - markerInset - markerSizeMm},
        Vector2d {markerInset, pageHeight - markerInset - markerSizeMm},
        Vector2d {markerInset, middleY},
    };

    std::string generationReason;
    for (std::size_t index = 0; index < markerOrigins.size(); ++index) {
        BinaryGrid marker;
        if (!OpenCVPhotoInspectionCompat::markerGrid(
                7,
                920 + static_cast<int>(index),
                marker,
                generationReason
            )) {
            result.status = OperationStatus::Unavailable;
            result.diagnostic = error(DiagnosticCode::OpenCVUnavailable, generationReason);
            return result;
        }
        addBinaryGrid(
            result.scene,
            marker,
            *commandFromPhysical,
            "aruco-" + std::to_string(920 + index),
            "markers",
            markerOrigins[index].x,
            markerOrigins[index].y,
            markerSizeMm
        );
    }

    const auto buildSemanticRecipe = [&](const VectorScene& scene) {
        std::ostringstream semanticRecipe;
        semanticRecipe.imbue(std::locale::classic());
        semanticRecipe << "photo-inspection-sheet-v1\n"
                       << projection.sha256 << '\n'
                       << identity.seriesUuid << '\n'
                       << identity.revisionUuid << '\n'
                       << identity.sourceToken << '\n'
                       << identity.revision << '\n'
                       << toString(options.media) << '\n'
                       << toString(options.orientation) << '\n'
                       << number(options.marginMm) << '\n'
                       << number(options.clearanceMm) << '\n'
                       << number(options.userRotationDegrees) << '\n'
                       << renderPhotoInspectionSvg(scene);
        return semanticRecipe.str();
    };

    // QR embeds the pre-identity hash because the identity graphic cannot include itself.
    result.qrContentSha256 = sha256Hex(bytes(buildSemanticRecipe(result.scene)));
    result.qrPayload = buildQrPayload(
        identity,
        result.projectionGeometrySha256,
        result.qrContentSha256,
        options.media,
        options.orientation
    );
    if (result.qrPayload.size() > 320) {
        result.diagnostic = error(DiagnosticCode::ResourceLimit, "canonical QR payload is too large");
        return result;
    }

    BinaryGrid qr;
    if (!OpenCVPhotoInspectionCompat::qrGrid(result.qrPayload, qr, generationReason)) {
        result.status = OperationStatus::Unavailable;
        result.diagnostic = error(DiagnosticCode::OpenCVUnavailable, generationReason);
        return result;
    }
    const double qrModule = qrBoxMm / static_cast<double>(qr.rows + 8);
    const double encodedQrSize = qr.rows * qrModule;
    const double qrX = pageWidth - options.marginMm - qrBoxMm - qrCornerGapMm + 4.0 * qrModule;
    const double qrY = pageHeight - options.marginMm - qrBoxMm + 4.0 * qrModule;
    addBinaryGrid(result.scene, qr, *commandFromPhysical, "identity-qr", "identity", qrX, qrY, encodedQrSize);

    ScenePrimitive referenceHorizontal;
    referenceHorizontal.id = "reference-horizontal-100mm";
    referenceHorizontal.layer = "references";
    referenceHorizontal.kind = ScenePrimitiveKind::Polyline;
    referenceHorizontal.strokeWidthMm = 0.20;
    const double referenceLineY = pageHeight - markerInset - markerSizeMm - 4.0;
    referenceHorizontal.points = {
        compensate(*commandFromPhysical, {targetCenterX - 50.0, referenceLineY}),
        compensate(*commandFromPhysical, {targetCenterX + 50.0, referenceLineY}),
    };
    result.scene.primitives.push_back(std::move(referenceHorizontal));

    ScenePrimitive label;
    label.id = "sheet-identity-label";
    label.layer = "annotations";
    label.kind = ScenePrimitiveKind::Text;
    const double labelBaselineY = referenceLineY - 4.0;
    label.points = {
        compensate(*commandFromPhysical, {options.marginMm, labelBaselineY}),
    };
    label.text = "PHOTO INSPECTION " + identity.revisionUuid;
    result.scene.primitives.push_back(std::move(label));

    result.sheetContentSha256 = sha256Hex(bytes(buildSemanticRecipe(result.scene)));

    result.status = OperationStatus::Complete;
    return result;
}

std::string renderPhotoInspectionSvg(const VectorScene& scene)
{
    if (!finite(scene.widthMm) || !finite(scene.heightMm) || scene.widthMm <= 0.0
        || scene.heightMm <= 0.0) {
        return {};
    }

    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<svg xmlns=\"http://www.w3.org/2000/svg\"" << " width=\"" << number(scene.widthMm)
           << "mm\"" << " height=\"" << number(scene.heightMm) << "mm\"" << " viewBox=\"0 0 "
           << number(scene.widthMm) << ' ' << number(scene.heightMm)
           << "\" data-schema=\"freecad-photo-inspection-scene-1.0\">\n";

    std::string activeLayer;
    for (const ScenePrimitive& primitive : scene.primitives) {
        if (primitive.layer != activeLayer) {
            if (!activeLayer.empty()) {
                stream << "  </g>\n";
            }
            activeLayer = primitive.layer;
            stream << "  <g id=\"layer-" << escapeXml(activeLayer) << "\" data-layer=\""
                   << escapeXml(activeLayer) << "\">\n";
        }

        if (primitive.kind == ScenePrimitiveKind::Text) {
            if (!primitive.points.empty()) {
                stream << "    <text id=\"" << escapeXml(primitive.id) << "\" x=\""
                       << number(primitive.points.front().x) << "\" y=\""
                       << number(primitive.points.front().y)
                       << "\" font-family=\"DejaVu Sans\" font-size=\"3\">"
                       << escapeXml(primitive.text) << "</text>\n";
            }
            continue;
        }
        if (primitive.points.empty()) {
            continue;
        }

        stream << "    <path id=\"" << escapeXml(primitive.id) << "\" d=\"M "
               << number(primitive.points.front().x) << ' ' << number(primitive.points.front().y);
        for (std::size_t index = 1; index < primitive.points.size(); ++index) {
            stream << " L " << number(primitive.points[index].x) << ' '
                   << number(primitive.points[index].y);
        }
        if (primitive.closed) {
            stream << " Z";
        }
        stream << "\" fill=\"" << (primitive.filled ? "#000000" : "none") << "\"";
        if (primitive.strokeWidthMm > 0.0) {
            stream << " stroke=\"#000000\" stroke-width=\"" << number(primitive.strokeWidthMm)
                   << "\" vector-effect=\"none\"";
        }
        stream << "/>\n";
    }
    if (!activeLayer.empty()) {
        stream << "  </g>\n";
    }
    stream << "</svg>\n";
    return stream.str();
}

const char* toString(const PageMedia media)
{
    return media == PageMedia::A4 ? "A4" : "A3";
}

const char* toString(const PageOrientation orientation)
{
    return orientation == PageOrientation::Portrait ? "Portrait" : "Landscape";
}

}  // namespace Inspection::Photo
