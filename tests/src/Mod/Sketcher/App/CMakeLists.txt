# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(Sketcher_tests_run
        BSplineOffset.cpp
        Constraint.cpp
        SketcherTestHelpers.cpp
        SketchObject.cpp
        SketchObjectChanges.cpp
        SketchObjectSymmetric.cpp
)

add_subdirectory(planegcs)

add_executable(OffsetChain_tests_run OffsetChain.cpp)
target_link_libraries(OffsetChain_tests_run
    GTest::gtest_main
    ${Python3_LIBRARIES}
    Sketcher
)
