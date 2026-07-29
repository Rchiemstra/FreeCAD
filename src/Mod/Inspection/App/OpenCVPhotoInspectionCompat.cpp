// SPDX-License-Identifier: LGPL-2.1-or-later

#include "OpenCVPhotoInspectionCompat.h"

#include <exception>
#include <vector>

#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
# include <opencv2/core.hpp>
# include <opencv2/imgcodecs.hpp>
# include <opencv2/objdetect.hpp>
# if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 7
#  include <opencv2/aruco.hpp>
# else
#  include <opencv2/objdetect/aruco_detector.hpp>
# endif
#endif

#ifndef FREECAD_PHOTO_INSPECTION_OPENCV_REASON
# define FREECAD_PHOTO_INSPECTION_OPENCV_REASON ""
#endif

namespace Inspection::Photo
{
namespace
{

#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
bool convertBinaryImage(const cv::Mat& image, BinaryGrid& output, std::string& reason)
{
    if (image.empty() || image.type() != CV_8UC1 || image.rows <= 0 || image.cols <= 0
        || image.rows > 4096 || image.cols > 4096) {
        reason = "OpenCV returned an invalid or oversized binary grid";
        return false;
    }

    BinaryGrid converted;
    converted.rows = image.rows;
    converted.columns = image.cols;
    converted.cells.reserve(static_cast<std::size_t>(image.rows * image.cols));
    for (int row = 0; row < image.rows; ++row) {
        for (int column = 0; column < image.cols; ++column) {
            converted.cells.push_back(image.at<unsigned char>(row, column) < 128 ? 1U : 0U);
        }
    }
    output = std::move(converted);
    reason.clear();
    return true;
}
#endif

}  // namespace

bool BinaryGrid::valid() const
{
    if (rows <= 0 || columns <= 0) {
        return false;
    }
    const auto expected = static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns);
    return expected == cells.size();
}

bool BinaryGrid::black(const int row, const int column) const
{
    if (!valid() || row < 0 || row >= rows || column < 0 || column >= columns) {
        return false;
    }
    return cells
               [static_cast<std::size_t>(row) * static_cast<std::size_t>(columns)
                + static_cast<std::size_t>(column)]
        != 0;
}

OpenCVCapability OpenCVPhotoInspectionCompat::capability()
{
    OpenCVCapability result;
#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    result.requested = true;
    result.available = true;
    result.versionMajor = CV_VERSION_MAJOR;
    result.versionMinor = CV_VERSION_MINOR;
    result.versionRevision = CV_VERSION_REVISION;
    result.version = FREECAD_PHOTO_INSPECTION_OPENCV_VERSION;
    result.components = FREECAD_PHOTO_INSPECTION_OPENCV_COMPONENTS;
    result.compatibilityBranch = FREECAD_PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH;
    result.buildInformation = cv::getBuildInformation();
#else
    result.requested = std::string(FREECAD_PHOTO_INSPECTION_OPENCV_REASON).find("disabled by")
        == std::string::npos;
    result.compatibilityBranch = "disabled";
    result.reason = FREECAD_PHOTO_INSPECTION_OPENCV_REASON;
#endif
    return result;
}

bool OpenCVPhotoInspectionCompat::runtimeSmokeTest(std::string& reason)
{
#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    try {
        const cv::Mat source(8, 8, CV_8UC1, cv::Scalar(127));
        std::vector<unsigned char> encoded;
        if (!cv::imencode(".png", source, encoded) || encoded.empty()) {
            reason = "OpenCV PNG encoder returned no data";
            return false;
        }

        const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE);
        if (decoded.rows != source.rows || decoded.cols != source.cols) {
            reason = "OpenCV PNG decoder returned an unexpected image";
            return false;
        }

        const cv::QRCodeDetector detector;
        (void)detector;
        reason.clear();
        return true;
    }
    catch (const cv::Exception& error) {
        reason = std::string("OpenCV exception: ") + error.what();
    }
    catch (const std::exception& error) {
        reason = std::string("runtime exception: ") + error.what();
    }
    catch (...) {
        reason = "unknown OpenCV runtime exception";
    }
    return false;
#else
    reason = FREECAD_PHOTO_INSPECTION_OPENCV_REASON;
    return false;
#endif
}

bool OpenCVPhotoInspectionCompat::markerGrid(
    const int dictionaryId,
    const int markerId,
    BinaryGrid& output,
    std::string& reason
)
{
    output = {};
#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    try {
        if (dictionaryId != 7 || markerId < 0 || markerId >= 1000) {
            reason = "only DICT_5X5_1000 and marker IDs 0-999 are supported";
            return false;
        }
        constexpr int cellCountIncludingBorder = 7;
        cv::Mat marker;
# if CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR < 7
        const auto dictionary = cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_1000);
        cv::aruco::drawMarker(dictionary, markerId, cellCountIncludingBorder, marker, 1);
# else
        const cv::aruco::Dictionary dictionary = cv::aruco::getPredefinedDictionary(
            cv::aruco::DICT_5X5_1000
        );
        cv::aruco::generateImageMarker(dictionary, markerId, cellCountIncludingBorder, marker, 1);
# endif
        return convertBinaryImage(marker, output, reason);
    }
    catch (const cv::Exception& error) {
        reason = std::string("OpenCV marker exception: ") + error.what();
    }
    catch (const std::exception& error) {
        reason = std::string("marker runtime exception: ") + error.what();
    }
    catch (...) {
        reason = "unknown OpenCV marker exception";
    }
    return false;
#else
    (void)dictionaryId;
    (void)markerId;
    reason = FREECAD_PHOTO_INSPECTION_OPENCV_REASON;
    return false;
#endif
}

bool OpenCVPhotoInspectionCompat::qrGrid(const std::string& payload, BinaryGrid& output, std::string& reason)
{
    output = {};
#if FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE
    try {
        if (payload.empty() || payload.size() > 320) {
            reason = "QR payload must contain 1-320 bytes";
            return false;
        }
        cv::QRCodeEncoder::Params parameters;
        parameters.version = 0;
        parameters.correction_level = cv::QRCodeEncoder::CORRECT_LEVEL_M;
        parameters.mode = cv::QRCodeEncoder::MODE_BYTE;
        parameters.structure_number = 1;
        const auto encoder = cv::QRCodeEncoder::create(parameters);
        cv::Mat qr;
        encoder->encode(payload, qr);
        return convertBinaryImage(qr, output, reason);
    }
    catch (const cv::Exception& error) {
        reason = std::string("OpenCV QR exception: ") + error.what();
    }
    catch (const std::exception& error) {
        reason = std::string("QR runtime exception: ") + error.what();
    }
    catch (...) {
        reason = "unknown OpenCV QR exception";
    }
    return false;
#else
    (void)payload;
    reason = FREECAD_PHOTO_INSPECTION_OPENCV_REASON;
    return false;
#endif
}

}  // namespace Inspection::Photo
