// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <Mod/Inspection/InspectionGlobal.h>

#include "PhotoInspectionTypes.h"

namespace Inspection::Photo
{

struct InspectionExport ImageLimits
{
    std::size_t maximumEncodedBytes {64U * 1024U * 1024U};
    std::size_t maximumDecodedPixels {50U * 1000U * 1000U};
    std::size_t maximumRectifiedPixels {32U * 1000U * 1000U};
    int maximumDimension {20000};
};

enum class EncodedImageFormat
{
    Unknown,
    Jpeg,
    Png
};

struct InspectionExport EncodedImageInfo
{
    EncodedImageFormat format {EncodedImageFormat::Unknown};
    int width {0};
    int height {0};
    int channels {0};
    int bitsPerChannel {0};
    std::size_t encodedBytes {0};
    std::size_t decodedBytes {0};
};

struct InspectionExport GrayRaster
{
    int width {0};
    int height {0};
    std::vector<std::uint8_t> pixels;
    std::string sourceSha256;

    bool valid() const;
};

struct InspectionExport ImageQuality
{
    double mean {0.0};
    double standardDeviation {0.0};
    double laplacianVariance {0.0};
    double darkClippedFraction {0.0};
    double brightClippedFraction {0.0};
};

struct InspectionExport ImageDecodeResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    EncodedImageInfo info;
    GrayRaster raster;
    ImageQuality quality;
};

struct InspectionExport PointCorrespondence
{
    Vector2d imagePixel;
    Vector2d sheetMm;
};

struct InspectionExport MarkerObservation
{
    int id {-1};
    std::array<Vector2d, 4> corners;
    double minimumSidePixels {0.0};
};

struct InspectionExport MarkerDetectionResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    std::vector<MarkerObservation> accepted;
    std::vector<MarkerObservation> rejected;
};

struct InspectionExport QrDetectionResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    std::string payload;
};

struct InspectionExport RectificationOptions
{
    double pixelsPerMm {15.0};
    double sheetWidthMm {210.0};
    double sheetHeightMm {297.0};
    double ransacThresholdPixels {3.0};
};

struct InspectionExport RectificationResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    GrayRaster raster;
    std::array<double, 9> imageToSheet {
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        1.0,
    };
    std::vector<std::uint8_t> inliers;
    double rmsResidualMm {0.0};
};

struct InspectionExport SegmentationOptions
{
    std::uint8_t foregroundThreshold {24};
    int morphologyRadiusPixels {1};
    double pixelsPerMm {15.0};
    std::size_t maximumContourPoints {maximumPointCount};
};

struct InspectionExport SegmentationResult
{
    OperationStatus status {OperationStatus::InvalidInput};
    Diagnostic diagnostic;
    std::vector<std::uint8_t> mask;
    std::vector<PolylineCycle> cycles;
};

InspectionExport ValidationResult preflightEncodedImage(
    const std::vector<std::uint8_t>& encoded,
    const ImageLimits& limits,
    EncodedImageInfo& output
);

InspectionExport ImageDecodeResult
decodePhotoImage(const std::vector<std::uint8_t>& encoded, const ImageLimits& limits = {});

InspectionExport MarkerDetectionResult detectPhotoMarkers(
    const GrayRaster& raster,
    int dictionaryId,
    const std::vector<int>& allowedMarkerIds
);

InspectionExport QrDetectionResult detectPhotoQr(const GrayRaster& raster);

InspectionExport RectificationResult rectifyPhotoImage(
    const GrayRaster& source,
    const std::vector<PointCorrespondence>& correspondences,
    const RectificationOptions& options,
    const ImageLimits& limits = {}
);

InspectionExport SegmentationResult segmentPhotoContour(
    const GrayRaster& rectified,
    const std::vector<std::uint8_t>& inkMask,
    const GrayRaster* emptyReference,
    const SegmentationOptions& options
);

}  // namespace Inspection::Photo
