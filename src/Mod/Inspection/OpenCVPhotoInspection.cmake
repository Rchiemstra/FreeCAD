# SPDX-License-Identifier: LGPL-2.1-or-later

include(CheckCXXSourceCompiles)

set(PHOTO_INSPECTION_OPENCV_AVAILABLE OFF)
set(PHOTO_INSPECTION_OPENCV_VERSION "")
set(PHOTO_INSPECTION_OPENCV_COMPONENTS "")
set(PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH "disabled")
set(
    PHOTO_INSPECTION_OPENCV_REASON
    "disabled by FREECAD_USE_OPENCV_PHOTO_INSPECTION=OFF"
)

if(FREECAD_USE_OPENCV_PHOTO_INSPECTION)
    set(PHOTO_INSPECTION_OPENCV_REASON "OpenCV >=4.6,<5 core component was not found")

    # Resolve the version before choosing the pre/post-4.7 ArUco component set.
    find_package(OpenCV 4.6 QUIET COMPONENTS core)

    if(OpenCV_FOUND AND OpenCV_VERSION VERSION_LESS 5.0)
        set(_photo_inspection_opencv_components core imgproc imgcodecs calib3d objdetect)
        if(OpenCV_VERSION VERSION_LESS 4.7)
            list(APPEND _photo_inspection_opencv_components aruco)
            set(_photo_inspection_opencv_compat_branch "opencv-4.6")
        else()
            set(_photo_inspection_opencv_compat_branch "opencv-4.7+")
        endif()

        find_package(
            OpenCV 4.6
            QUIET
            COMPONENTS ${_photo_inspection_opencv_components}
        )

        if(OpenCV_FOUND)
            if(OpenCV_VERSION VERSION_LESS 4.7)
                set(
                    _photo_inspection_opencv_probe
                    [[
#include <opencv2/aruco.hpp>
#include <opencv2/aruco/charuco.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <vector>
int main()
{
    auto dictionary =
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_1000);
    cv::Mat marker;
    cv::aruco::drawMarker(dictionary, 920, 32, marker, 1);
    auto board = cv::aruco::CharucoBoard::create(5, 7, 0.04F, 0.02F, dictionary);
    cv::QRCodeDetector qr;
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".png", marker, encoded)) {
        return 1;
    }
    return board.empty() || encoded.empty() || marker.empty();
}
]]
                )
            else()
                set(
                    _photo_inspection_opencv_probe
                    [[
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/objdetect/aruco_detector.hpp>
#include <opencv2/objdetect/charuco_detector.hpp>
#include <vector>
int main()
{
    const cv::aruco::Dictionary dictionary =
        cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_1000);
    cv::Mat marker;
    cv::aruco::generateImageMarker(dictionary, 920, 32, marker, 1);
    const cv::aruco::CharucoBoard board(
        cv::Size(5, 7),
        0.04F,
        0.02F,
        dictionary
    );
    const cv::aruco::ArucoDetector markerDetector(dictionary);
    const cv::aruco::CharucoDetector charucoDetector(board);
    cv::QRCodeDetector qr;
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".png", marker, encoded)) {
        return 1;
    }
    return encoded.empty() || marker.empty();
}
]]
                )
            endif()

            set(_photo_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
            set(_photo_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
            set(_photo_saved_required_quiet "${CMAKE_REQUIRED_QUIET}")
            set(CMAKE_REQUIRED_INCLUDES "${OpenCV_INCLUDE_DIRS}")
            set(CMAKE_REQUIRED_LIBRARIES "${OpenCV_LIBS}")
            set(CMAKE_REQUIRED_QUIET TRUE)
            unset(PHOTO_INSPECTION_OPENCV_SYMBOLS_OK CACHE)
            check_cxx_source_compiles(
                "${_photo_inspection_opencv_probe}"
                PHOTO_INSPECTION_OPENCV_SYMBOLS_OK
            )
            set(CMAKE_REQUIRED_INCLUDES "${_photo_saved_required_includes}")
            set(CMAKE_REQUIRED_LIBRARIES "${_photo_saved_required_libraries}")
            set(CMAKE_REQUIRED_QUIET "${_photo_saved_required_quiet}")

            if(PHOTO_INSPECTION_OPENCV_SYMBOLS_OK)
                set(PHOTO_INSPECTION_OPENCV_AVAILABLE ON)
                set(PHOTO_INSPECTION_OPENCV_VERSION "${OpenCV_VERSION}")
                list(
                    JOIN
                    _photo_inspection_opencv_components
                    ","
                    PHOTO_INSPECTION_OPENCV_COMPONENTS
                )
                set(
                    PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH
                    "${_photo_inspection_opencv_compat_branch}"
                )
                set(PHOTO_INSPECTION_OPENCV_REASON "")
            else()
                set(
                    PHOTO_INSPECTION_OPENCV_REASON
                    "OpenCV ${OpenCV_VERSION} is missing required ArUco, ChArUco, QR, calibration, or PNG symbols"
                )
            endif()
        else()
            set(
                PHOTO_INSPECTION_OPENCV_REASON
                "OpenCV ${OpenCV_VERSION} is missing one or more required components: ${_photo_inspection_opencv_components}"
            )
        endif()
    elseif(OpenCV_FOUND)
        set(
            PHOTO_INSPECTION_OPENCV_REASON
            "OpenCV ${OpenCV_VERSION} is outside the supported range >=4.6,<5"
        )
    endif()
endif()

string(REPLACE ";" "," PHOTO_INSPECTION_OPENCV_REASON "${PHOTO_INSPECTION_OPENCV_REASON}")

set(
    PHOTO_INSPECTION_OPENCV_AVAILABLE
    "${PHOTO_INSPECTION_OPENCV_AVAILABLE}"
    CACHE INTERNAL "Photo-inspection OpenCV capability"
    FORCE
)
set(
    PHOTO_INSPECTION_OPENCV_VERSION
    "${PHOTO_INSPECTION_OPENCV_VERSION}"
    CACHE INTERNAL "Photo-inspection OpenCV version"
    FORCE
)
set(
    PHOTO_INSPECTION_OPENCV_COMPONENTS
    "${PHOTO_INSPECTION_OPENCV_COMPONENTS}"
    CACHE INTERNAL "Photo-inspection OpenCV components"
    FORCE
)
set(
    PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH
    "${PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH}"
    CACHE INTERNAL "Photo-inspection OpenCV compatibility branch"
    FORCE
)
set(
    PHOTO_INSPECTION_OPENCV_REASON
    "${PHOTO_INSPECTION_OPENCV_REASON}"
    CACHE INTERNAL "Photo-inspection OpenCV disabled reason"
    FORCE
)

if(FREECAD_USE_OPENCV_PHOTO_INSPECTION AND NOT PHOTO_INSPECTION_OPENCV_AVAILABLE)
    message(
        WARNING
        "Photo inspection OpenCV capability is disabled: ${PHOTO_INSPECTION_OPENCV_REASON}"
    )
endif()
