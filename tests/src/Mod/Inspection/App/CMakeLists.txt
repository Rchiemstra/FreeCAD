# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(
    Inspection_tests_run
    PhotoInspectionPhase0.cpp
    PhotoInspectionImage.cpp
    PhotoInspectionMeasurement.cpp
    PhotoInspectionProjection.cpp
    PhotoInspectionPhase1.cpp
    PhotoInspectionPhase2.cpp
    PhotoInspectionPhase3.cpp
    PhotoInspectionPhase5.cpp
    PhotoInspectionPhase6.cpp
    PhotoInspectionFutureGates.cpp
    PhotoInspectionSession.cpp
    PhotoInspectionStorage.cpp
    PhotoInspectionObjects.cpp
)

target_include_directories(
    Inspection_tests_run
    PUBLIC
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src/3rdParty/FastSignals/libfastsignals/include
)
