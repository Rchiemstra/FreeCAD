// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PhotoInspectionImage.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
# include <opencv2/calib3d.hpp>
# include <opencv2/core/version.hpp>
# include <opencv2/imgcodecs.hpp>
# include <opencv2/imgproc.hpp>
# include <opencv2/objdetect.hpp>
# if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 7
#  include <opencv2/aruco.hpp>
# else
#  include <opencv2/objdetect/aruco_detector.hpp>
# endif
#endif

namespace Inspection::Photo
{
namespace
{

Diagnostic error(const DiagnosticCode code, std::string message)
{
    return {code, DiagnosticSeverity::Error, std::move(message)};
}

std::uint32_t bigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24U)
        | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

bool checkedRasterSize(
    const int width,
    const int height,
    const int channels,
    const ImageLimits& limits,
    std::size_t& bytes
)
{
    if (width <= 0 || height <= 0 || channels <= 0 || width > limits.maximumDimension
        || height > limits.maximumDimension) {
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixelCount > limits.maximumDecodedPixels
        || pixelCount > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(channels)) {
        return false;
    }
    bytes = pixelCount * static_cast<std::size_t>(channels);
    return true;
}

ValidationResult preflightPng(
    const std::vector<std::uint8_t>& encoded,
    const ImageLimits& limits,
    EncodedImageInfo& output
)
{
    static constexpr std::uint8_t signature[8] {137, 80, 78, 71, 13, 10, 26, 10};
    if (encoded.size() < 33 || std::memcmp(encoded.data(), signature, sizeof(signature)) != 0
        || bigEndian32(encoded.data() + 8) != 13 || std::memcmp(encoded.data() + 12, "IHDR", 4) != 0) {
        return ValidationResult::failure(DiagnosticCode::InvalidSchema, "invalid PNG header");
    }
    const std::uint32_t width = bigEndian32(encoded.data() + 16);
    const std::uint32_t height = bigEndian32(encoded.data() + 20);
    const int depth = encoded[24];
    const int colorType = encoded[25];
    int channels = 0;
    switch (colorType) {
        case 0:
            channels = 1;
            break;
        case 2:
            channels = 3;
            break;
        case 3:
            channels = 1;
            break;
        case 4:
            channels = 2;
            break;
        case 6:
            channels = 4;
            break;
        default:
            return ValidationResult::failure(DiagnosticCode::InvalidSchema, "unsupported PNG color type");
    }
    if (depth != 8 && depth != 16) {
        return ValidationResult::failure(
            DiagnosticCode::InvalidSchema,
            "only 8-bit and 16-bit PNG are accepted"
        );
    }
    std::size_t decodedBytes = 0;
    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || !checkedRasterSize(
            static_cast<int>(width),
            static_cast<int>(height),
            channels * (depth / 8),
            limits,
            decodedBytes
        )) {
        return ValidationResult::failure(
            DiagnosticCode::ResourceLimit,
            "PNG dimensions exceed configured limits"
        );
    }
    output = {
        EncodedImageFormat::Png,
        static_cast<int>(width),
        static_cast<int>(height),
        channels,
        depth,
        encoded.size(),
        decodedBytes,
    };
    return ValidationResult::success();
}

ValidationResult preflightJpeg(
    const std::vector<std::uint8_t>& encoded,
    const ImageLimits& limits,
    EncodedImageInfo& output
)
{
    if (encoded.size() < 4 || encoded[0] != 0xff || encoded[1] != 0xd8) {
        return ValidationResult::failure(DiagnosticCode::InvalidSchema, "invalid JPEG header");
    }
    std::size_t offset = 2;
    while (offset + 3 < encoded.size()) {
        while (offset < encoded.size() && encoded[offset] == 0xff) {
            ++offset;
        }
        if (offset >= encoded.size()) {
            break;
        }
        const std::uint8_t marker = encoded[offset++];
        if (marker == 0xd9 || marker == 0xda) {
            break;
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (offset + 2 > encoded.size()) {
            break;
        }
        const std::size_t length = (static_cast<std::size_t>(encoded[offset]) << 8U)
            | encoded[offset + 1];
        if (length < 2 || length > encoded.size() - offset) {
            return ValidationResult::failure(DiagnosticCode::InvalidSchema, "truncated JPEG segment");
        }
        const bool startOfFrame = (marker >= 0xc0 && marker <= 0xc3)
            || (marker >= 0xc5 && marker <= 0xc7) || (marker >= 0xc9 && marker <= 0xcb)
            || (marker >= 0xcd && marker <= 0xcf);
        if (startOfFrame) {
            if (length < 8) {
                return ValidationResult::failure(DiagnosticCode::InvalidSchema, "invalid JPEG frame");
            }
            const int depth = encoded[offset + 2];
            const int height = (static_cast<int>(encoded[offset + 3]) << 8) | encoded[offset + 4];
            const int width = (static_cast<int>(encoded[offset + 5]) << 8) | encoded[offset + 6];
            const int channels = encoded[offset + 7];
            if (depth != 8 || (channels != 1 && channels != 3 && channels != 4)) {
                return ValidationResult::failure(
                    DiagnosticCode::InvalidSchema,
                    "unsupported JPEG precision or channel count"
                );
            }
            std::size_t decodedBytes = 0;
            if (!checkedRasterSize(width, height, channels, limits, decodedBytes)) {
                return ValidationResult::failure(
                    DiagnosticCode::ResourceLimit,
                    "JPEG dimensions exceed configured limits"
                );
            }
            output = {
                EncodedImageFormat::Jpeg,
                width,
                height,
                channels,
                depth,
                encoded.size(),
                decodedBytes,
            };
            return ValidationResult::success();
        }
        offset += length;
    }
    return ValidationResult::failure(DiagnosticCode::InvalidSchema, "JPEG has no image frame");
}

bool finite(const Vector2d& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

}  // namespace

bool GrayRaster::valid() const
{
    return width > 0 && height > 0
        && pixels.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

ValidationResult preflightEncodedImage(
    const std::vector<std::uint8_t>& encoded,
    const ImageLimits& limits,
    EncodedImageInfo& output
)
{
    output = {};
    if (encoded.empty() || encoded.size() > limits.maximumEncodedBytes) {
        return ValidationResult::failure(
            DiagnosticCode::ResourceLimit,
            "encoded image is empty or exceeds configured byte limits"
        );
    }
    if (encoded.size() >= 8 && encoded[0] == 137 && encoded[1] == 80) {
        return preflightPng(encoded, limits, output);
    }
    if (encoded.size() >= 2 && encoded[0] == 0xff && encoded[1] == 0xd8) {
        return preflightJpeg(encoded, limits, output);
    }
    return ValidationResult::failure(
        DiagnosticCode::InvalidSchema,
        "only local JPEG and PNG bytes are accepted"
    );
}

ImageDecodeResult decodePhotoImage(const std::vector<std::uint8_t>& encoded, const ImageLimits& limits)
{
    ImageDecodeResult result;
    const ValidationResult preflight = preflightEncodedImage(encoded, limits, result.info);
    if (!preflight.valid) {
        result.status = preflight.diagnostic.code == DiagnosticCode::ResourceLimit
            ? OperationStatus::ResourceLimit
            : OperationStatus::InvalidInput;
        result.diagnostic = preflight.diagnostic;
        return result;
    }
#if !FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.status = OperationStatus::Unavailable;
    result.diagnostic = error(
        DiagnosticCode::OpenCVUnavailable,
        "photo decoding is unavailable because OpenCV support is disabled"
    );
    return result;
#else
    try {
        const cv::Mat encodedView(
            1,
            static_cast<int>(encoded.size()),
            CV_8UC1,
            const_cast<std::uint8_t*>(encoded.data())
        );
        const cv::Mat decoded = cv::imdecode(encodedView, cv::IMREAD_GRAYSCALE);
        if (decoded.empty() || decoded.cols != result.info.width || decoded.rows != result.info.height
            || decoded.type() != CV_8UC1 || !decoded.isContinuous()) {
            result.status = OperationStatus::InvalidInput;
            result.diagnostic
                = error(DiagnosticCode::InvalidSchema, "decoder output disagrees with preflight");
            return result;
        }
        result.raster.width = decoded.cols;
        result.raster.height = decoded.rows;
        result.raster.sourceSha256 = sha256Hex(encoded);
        result.raster.pixels.assign(decoded.datastart, decoded.dataend);

        cv::Scalar mean;
        cv::Scalar deviation;
        cv::meanStdDev(decoded, mean, deviation);
        cv::Mat laplacian;
        cv::Laplacian(decoded, laplacian, CV_64F);
        cv::Scalar laplacianMean;
        cv::Scalar laplacianDeviation;
        cv::meanStdDev(laplacian, laplacianMean, laplacianDeviation);
        result.quality.mean = mean[0];
        result.quality.standardDeviation = deviation[0];
        result.quality.laplacianVariance = laplacianDeviation[0] * laplacianDeviation[0];
        result.quality.darkClippedFraction = static_cast<double>(cv::countNonZero(decoded <= 2))
            / static_cast<double>(decoded.total());
        result.quality.brightClippedFraction = static_cast<double>(cv::countNonZero(decoded >= 253))
            / static_cast<double>(decoded.total());
        result.status = OperationStatus::Complete;
        return result;
    }
    catch (const cv::Exception& exception) {
        result.status = OperationStatus::InvalidInput;
        result.diagnostic = error(
            DiagnosticCode::InvalidSchema,
            std::string("image decode failed: ") + exception.what()
        );
        result.raster = {};
        return result;
    }
    catch (...) {
        result.status = OperationStatus::NumericalFailure;
        result.diagnostic = error(DiagnosticCode::NumericalFailure, "unknown image decode failure");
        result.raster = {};
        return result;
    }
#endif
}

MarkerDetectionResult detectPhotoMarkers(
    const GrayRaster& raster,
    const int dictionaryId,
    const std::vector<int>& allowedMarkerIds
)
{
    MarkerDetectionResult result;
    if (!raster.valid() || dictionaryId < 0 || dictionaryId > 20 || allowedMarkerIds.empty()
        || allowedMarkerIds.size() > 1024) {
        result.diagnostic = error(DiagnosticCode::InvalidSchema, "marker detection input is invalid");
        return result;
    }
    std::vector<int> sortedAllowed = allowedMarkerIds;
    std::sort(sortedAllowed.begin(), sortedAllowed.end());
    if (sortedAllowed.front() < 0
        || std::adjacent_find(sortedAllowed.begin(), sortedAllowed.end()) != sortedAllowed.end()) {
        result.diagnostic
            = error(DiagnosticCode::InvalidSchema, "allowed marker IDs are invalid or duplicated");
        return result;
    }
#if !FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.status = OperationStatus::Unavailable;
    result.diagnostic
        = error(DiagnosticCode::OpenCVUnavailable, "marker detection requires OpenCV support");
    return result;
#else
    try {
        const cv::Mat image(
            raster.height,
            raster.width,
            CV_8UC1,
            const_cast<std::uint8_t*>(raster.pixels.data())
        );
# if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 7
        const auto dictionary = cv::aruco::getPredefinedDictionary(
            static_cast<cv::aruco::PREDEFINED_DICTIONARY_NAME>(dictionaryId)
        );
# else
        const auto dictionary = cv::aruco::getPredefinedDictionary(
            static_cast<cv::aruco::PredefinedDictionaryType>(dictionaryId)
        );
# endif
        std::vector<int> ids;
        std::vector<std::vector<cv::Point2f>> corners;
        std::vector<std::vector<cv::Point2f>> rejectedCandidates;
# if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 7
        const auto parameters = cv::aruco::DetectorParameters::create();
        cv::aruco::detectMarkers(image, dictionary, corners, ids, parameters, rejectedCandidates);
# else
        const cv::aruco::ArucoDetector detector(dictionary);
        detector.detectMarkers(image, corners, ids, rejectedCandidates);
# endif
        std::vector<int> observedIds;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (corners[index].size() != 4) {
                continue;
            }
            MarkerObservation observation;
            observation.id = ids[index];
            observation.minimumSidePixels = std::numeric_limits<double>::infinity();
            for (std::size_t corner = 0; corner < 4; ++corner) {
                observation.corners[corner] = {
                    corners[index][corner].x,
                    corners[index][corner].y,
                };
                const cv::Point2f delta = corners[index][corner] - corners[index][(corner + 1) % 4];
                observation.minimumSidePixels
                    = std::min(observation.minimumSidePixels, static_cast<double>(cv::norm(delta)));
            }
            const bool allowed
                = std::binary_search(sortedAllowed.begin(), sortedAllowed.end(), observation.id);
            const bool duplicate = std::find(observedIds.begin(), observedIds.end(), observation.id)
                != observedIds.end();
            if (allowed && !duplicate) {
                observedIds.push_back(observation.id);
                result.accepted.push_back(std::move(observation));
            }
            else {
                result.rejected.push_back(std::move(observation));
            }
        }
        if (result.accepted.empty()) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic
                = error(DiagnosticCode::LowImageQuality, "no allowed marker was detected");
            return result;
        }
        if (!result.rejected.empty()) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic
                = error(DiagnosticCode::IdentityMismatch, "foreign or duplicate marker detected");
            return result;
        }
        result.status = OperationStatus::Complete;
        return result;
    }
    catch (const cv::Exception& exception) {
        result.status = OperationStatus::NumericalFailure;
        result.diagnostic = error(
            DiagnosticCode::NumericalFailure,
            std::string("marker detection failed: ") + exception.what()
        );
        result.accepted.clear();
        result.rejected.clear();
        return result;
    }
#endif
}

QrDetectionResult detectPhotoQr(const GrayRaster& raster)
{
    QrDetectionResult result;
    if (!raster.valid()) {
        result.diagnostic = error(DiagnosticCode::InvalidSchema, "QR raster is invalid");
        return result;
    }
#if !FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.status = OperationStatus::Unavailable;
    result.diagnostic
        = error(DiagnosticCode::OpenCVUnavailable, "QR detection requires OpenCV support");
    return result;
#else
    try {
        const cv::Mat image(
            raster.height,
            raster.width,
            CV_8UC1,
            const_cast<std::uint8_t*>(raster.pixels.data())
        );
        cv::QRCodeDetector detector;
        result.payload = detector.detectAndDecode(image);
        if (result.payload.empty()) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic = error(DiagnosticCode::LowImageQuality, "no QR payload detected");
            return result;
        }
        if (result.payload.size() > 512 || result.payload.find('\0') != std::string::npos) {
            result.status = OperationStatus::InvalidInput;
            result.diagnostic
                = error(DiagnosticCode::InvalidSchema, "QR payload exceeds identity limits");
            result.payload.clear();
            return result;
        }
        result.status = OperationStatus::Complete;
        return result;
    }
    catch (const cv::Exception& exception) {
        result.status = OperationStatus::NumericalFailure;
        result.diagnostic = error(
            DiagnosticCode::NumericalFailure,
            std::string("QR detection failed: ") + exception.what()
        );
        result.payload.clear();
        return result;
    }
#endif
}

RectificationResult rectifyPhotoImage(
    const GrayRaster& source,
    const std::vector<PointCorrespondence>& correspondences,
    const RectificationOptions& options,
    const ImageLimits& limits
)
{
    RectificationResult result;
    if (!source.valid() || correspondences.size() < 4 || correspondences.size() > 4096
        || !std::isfinite(options.pixelsPerMm) || options.pixelsPerMm < 10.0
        || options.pixelsPerMm > 25.0 || !std::isfinite(options.sheetWidthMm)
        || !std::isfinite(options.sheetHeightMm) || options.sheetWidthMm <= 0.0
        || options.sheetHeightMm <= 0.0 || !std::isfinite(options.ransacThresholdPixels)
        || options.ransacThresholdPixels <= 0.0) {
        result.diagnostic
            = error(DiagnosticCode::InvalidSchema, "rectification input or sampling is invalid");
        return result;
    }
    for (const PointCorrespondence& point : correspondences) {
        if (!finite(point.imagePixel) || !finite(point.sheetMm)) {
            result.diagnostic
                = error(DiagnosticCode::NonFiniteValue, "correspondence contains non-finite values");
            return result;
        }
    }
    const int width = static_cast<int>(std::ceil(options.sheetWidthMm * options.pixelsPerMm));
    const int height = static_cast<int>(std::ceil(options.sheetHeightMm * options.pixelsPerMm));
    std::size_t rectifiedBytes = 0;
    if (!checkedRasterSize(width, height, 1, limits, rectifiedBytes)
        || rectifiedBytes > limits.maximumRectifiedPixels) {
        result.status = OperationStatus::ResourceLimit;
        result.diagnostic
            = error(DiagnosticCode::ResourceLimit, "rectified raster exceeds configured limits");
        return result;
    }
#if !FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.status = OperationStatus::Unavailable;
    result.diagnostic
        = error(DiagnosticCode::OpenCVUnavailable, "rectification requires OpenCV support");
    return result;
#else
    try {
        std::vector<cv::Point2d> imagePoints;
        std::vector<cv::Point2d> sheetPoints;
        imagePoints.reserve(correspondences.size());
        sheetPoints.reserve(correspondences.size());
        for (const PointCorrespondence& point : correspondences) {
            imagePoints.emplace_back(point.imagePixel.x, point.imagePixel.y);
            sheetPoints.emplace_back(point.sheetMm.x, point.sheetMm.y);
        }
        cv::Mat inlierMask;
        cv::Mat homography = cv::findHomography(
            imagePoints,
            sheetPoints,
            cv::RANSAC,
            options.ransacThresholdPixels / options.pixelsPerMm,
            inlierMask,
            2000,
            0.995
        );
        if (homography.empty() || !cv::checkRange(homography)) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic
                = error(DiagnosticCode::NumericalFailure, "homography fit is singular or unstable");
            return result;
        }
        homography.convertTo(homography, CV_64F);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                result.imageToSheet[static_cast<std::size_t>(row * 3 + column)]
                    = homography.at<double>(row, column);
            }
        }
        result.inliers.assign(inlierMask.datastart, inlierMask.dataend);
        if (cv::countNonZero(inlierMask) < 4) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic
                = error(DiagnosticCode::LowImageQuality, "fewer than four inlier references");
            return result;
        }

        std::vector<cv::Point2d> projected;
        cv::perspectiveTransform(imagePoints, projected, homography);
        double residualSum = 0.0;
        std::size_t residualCount = 0;
        for (std::size_t index = 0; index < projected.size(); ++index) {
            if (result.inliers[index] != 0) {
                const double dx = projected[index].x - sheetPoints[index].x;
                const double dy = projected[index].y - sheetPoints[index].y;
                residualSum += dx * dx + dy * dy;
                ++residualCount;
            }
        }
        result.rmsResidualMm = std::sqrt(residualSum / static_cast<double>(residualCount));

        cv::Mat imageToRectified = homography.clone();
        imageToRectified.row(0) *= options.pixelsPerMm;
        imageToRectified.row(1) *= options.pixelsPerMm;
        const cv::Mat sourceView(
            source.height,
            source.width,
            CV_8UC1,
            const_cast<std::uint8_t*>(source.pixels.data())
        );
        cv::Mat rectified;
        cv::warpPerspective(
            sourceView,
            rectified,
            imageToRectified,
            cv::Size(width, height),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT,
            cv::Scalar(255)
        );
        result.raster.width = width;
        result.raster.height = height;
        result.raster.sourceSha256 = source.sourceSha256;
        result.raster.pixels.assign(rectified.datastart, rectified.dataend);
        result.status = OperationStatus::Complete;
        return result;
    }
    catch (const cv::Exception& exception) {
        result.status = OperationStatus::NumericalFailure;
        result.diagnostic = error(
            DiagnosticCode::NumericalFailure,
            std::string("rectification failed: ") + exception.what()
        );
        result.raster = {};
        return result;
    }
#endif
}

SegmentationResult segmentPhotoContour(
    const GrayRaster& rectified,
    const std::vector<std::uint8_t>& inkMask,
    const GrayRaster* emptyReference,
    const SegmentationOptions& options
)
{
    SegmentationResult result;
    if (!rectified.valid() || inkMask.size() != rectified.pixels.size()
        || (emptyReference
            && (!emptyReference->valid() || emptyReference->width != rectified.width
                || emptyReference->height != rectified.height))
        || options.morphologyRadiusPixels < 0 || options.morphologyRadiusPixels > 25
        || !std::isfinite(options.pixelsPerMm) || options.pixelsPerMm < 10.0
        || options.pixelsPerMm > 25.0 || options.maximumContourPoints == 0
        || options.maximumContourPoints > maximumPointCount) {
        result.diagnostic
            = error(DiagnosticCode::InvalidSchema, "segmentation input or limits are invalid");
        return result;
    }
#if !FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.status = OperationStatus::Unavailable;
    result.diagnostic
        = error(DiagnosticCode::OpenCVUnavailable, "contour segmentation requires OpenCV support");
    return result;
#else
    try {
        const cv::Mat current(
            rectified.height,
            rectified.width,
            CV_8UC1,
            const_cast<std::uint8_t*>(rectified.pixels.data())
        );
        cv::Mat foreground;
        if (emptyReference) {
            const cv::Mat empty(
                emptyReference->height,
                emptyReference->width,
                CV_8UC1,
                const_cast<std::uint8_t*>(emptyReference->pixels.data())
            );
            cv::Mat difference;
            cv::absdiff(current, empty, difference);
            cv::threshold(difference, foreground, options.foregroundThreshold, 255, cv::THRESH_BINARY);
        }
        else {
            cv::threshold(current, foreground, options.foregroundThreshold, 255, cv::THRESH_BINARY_INV);
        }
        const cv::Mat ink(
            rectified.height,
            rectified.width,
            CV_8UC1,
            const_cast<std::uint8_t*>(inkMask.data())
        );
        foreground.setTo(0, ink != 0);
        if (options.morphologyRadiusPixels > 0) {
            const int size = options.morphologyRadiusPixels * 2 + 1;
            const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(size, size));
            cv::morphologyEx(foreground, foreground, cv::MORPH_OPEN, kernel);
            cv::morphologyEx(foreground, foreground, cv::MORPH_CLOSE, kernel);
        }
        result.mask.assign(foreground.datastart, foreground.dataend);

        std::vector<std::vector<cv::Point>> contours;
        std::vector<cv::Vec4i> hierarchy;
        cv::findContours(foreground.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE);
        std::size_t pointCount = 0;
        for (const auto& contour : contours) {
            pointCount += contour.size();
            if (pointCount > options.maximumContourPoints) {
                result.status = OperationStatus::ResourceLimit;
                result.diagnostic
                    = error(DiagnosticCode::ResourceLimit, "contours exceed configured point limit");
                result.mask.clear();
                result.cycles.clear();
                return result;
            }
        }
        result.cycles.reserve(contours.size());
        for (std::size_t index = 0; index < contours.size(); ++index) {
            if (contours[index].size() < 3) {
                continue;
            }
            PolylineCycle cycle;
            cycle.hole = hierarchy[index][3] >= 0;
            cycle.points.reserve(contours[index].size());
            for (const cv::Point& point : contours[index]) {
                cycle.points.push_back({point.x / options.pixelsPerMm, point.y / options.pixelsPerMm});
            }
            result.cycles.push_back(std::move(cycle));
        }
        if (result.cycles.empty()) {
            result.status = OperationStatus::Inconclusive;
            result.diagnostic
                = error(DiagnosticCode::LowImageQuality, "no supported foreground contour found");
            return result;
        }
        result.status = OperationStatus::Complete;
        return result;
    }
    catch (const cv::Exception& exception) {
        result.status = OperationStatus::NumericalFailure;
        result.diagnostic = error(
            DiagnosticCode::NumericalFailure,
            std::string("segmentation failed: ") + exception.what()
        );
        result.mask.clear();
        result.cycles.clear();
        return result;
    }
#endif
}

}  // namespace Inspection::Photo
