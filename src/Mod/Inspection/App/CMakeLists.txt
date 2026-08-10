# SPDX-License-Identifier: LGPL-2.1-or-later

set(Inspection_LIBS
    FreeCADApp
    Mesh
    Points
    Part
)

set(Inspection_SRCS
    AppInspection.cpp
    InspectionFeature.cpp
    InspectionFeature.h
    OpenCVPhotoInspectionCompat.h
    OpenCVPhotoInspectionCompat.cpp
    PhotoInspectionTransform.h
    PhotoInspectionTransform.cpp
    PhotoInspectionSheet.h
    PhotoInspectionSheet.cpp
    PhotoInspectionProfiles.h
    PhotoInspectionProfiles.cpp
    PhotoInspectionEngine.h
    PhotoInspectionEngine.cpp
    PhotoInspectionImage.h
    PhotoInspectionImage.cpp
    PhotoInspectionMeasurement.h
    PhotoInspectionMeasurement.cpp
    PhotoInspectionReport.h
    PhotoInspectionReport.cpp
    PhotoInspectionSession.h
    PhotoInspectionSession.cpp
    PhotoInspectionStorage.h
    PhotoInspectionStorage.cpp
    PhotoInspectionObject.h
    PhotoInspectionObject.cpp
    PhotoInspectionProjection.h
    PhotoInspectionProjection.cpp
    PhotoInspectionTypes.h
    PhotoInspectionTypes.cpp
    PreCompiled.h
)

set(Inspection_Scripts
    ../Init.py
    ../PhotoInspectionMcpContract.json
)

add_library(Inspection SHARED ${Inspection_SRCS} ${Inspection_Scripts})

if(FREECAD_USE_PCH)
    target_precompile_headers(Inspection PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:"${CMAKE_CURRENT_LIST_DIR}/PreCompiled.h">
    )
endif(FREECAD_USE_PCH)

target_include_directories(
    Inspection
    PRIVATE
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Inspection PRIVATE ${Inspection_LIBS})

if (PHOTO_INSPECTION_OPENCV_AVAILABLE)
    target_include_directories(Inspection SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(Inspection PRIVATE ${OpenCV_LIBS})
    set(
        PhotoInspectionOpenCVDefinitions
        FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE=1
        FREECAD_PHOTO_INSPECTION_OPENCV_VERSION="${PHOTO_INSPECTION_OPENCV_VERSION}"
        FREECAD_PHOTO_INSPECTION_OPENCV_COMPONENTS="${PHOTO_INSPECTION_OPENCV_COMPONENTS}"
        FREECAD_PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH="${PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH}"
    )
else()
    set(
        PhotoInspectionOpenCVDefinitions
        FREECAD_PHOTO_INSPECTION_OPENCV_AVAILABLE=0
        FREECAD_PHOTO_INSPECTION_OPENCV_VERSION=""
        FREECAD_PHOTO_INSPECTION_OPENCV_COMPONENTS=""
        FREECAD_PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH="${PHOTO_INSPECTION_OPENCV_COMPAT_BRANCH}"
        FREECAD_PHOTO_INSPECTION_OPENCV_REASON="${PHOTO_INSPECTION_OPENCV_REASON}"
    )
endif()
target_compile_definitions(Inspection PRIVATE ${PhotoInspectionOpenCVDefinitions})

if (MSVC)
    target_compile_options(Inspection PRIVATE /wd4251)
endif()

if (FREECAD_WARN_ERROR)
    target_compile_warn_error(Inspection)
endif()

fc_target_copy_resource_flat(Inspection
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/Inspection
    ${Inspection_Scripts}
)

SET_BIN_DIR(Inspection Inspection /Mod/Inspection)
SET_PYTHON_PREFIX_SUFFIX(Inspection)

INSTALL(TARGETS Inspection DESTINATION ${CMAKE_INSTALL_LIBDIR})
