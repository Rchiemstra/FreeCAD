# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(
    InspectionGui_tests_run
    PhotoInspectionRenderer.cpp
)

target_link_libraries(
    InspectionGui_tests_run
    GTest::gtest_main
    ${Python3_LIBRARIES}
    Inspection
    InspectionGui
    FreeCADApp
    FreeCADGui
)
