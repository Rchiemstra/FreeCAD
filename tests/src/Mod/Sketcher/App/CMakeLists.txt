# SPDX-License-Identifier: LGPL-2.1-or-later

add_executable(Sketcher_tests_run
        BSplineOffset.cpp
        Constraint.cpp
        LinkedOffset.cpp
        OffsetChain.cpp
        SketcherTestHelpers.cpp
        SketchObject.cpp
        SketchObjectChanges.cpp
        SketchObjectSymmetric.cpp
)

add_subdirectory(planegcs)
