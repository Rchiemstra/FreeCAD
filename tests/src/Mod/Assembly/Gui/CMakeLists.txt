# SPDX-License-Identifier: LGPL-2.1-or-later

# AssemblyGui TaskInterferenceCheck lifecycle tests (default QT_QPA_PLATFORM=offscreen;
# CI also runs one case under Xvfb with QT_QPA_PLATFORM=xcb).
# These prove TaskInterferenceCheck generation policy without a live 3D view.
add_executable(AssemblyGui_tests_run
    TaskInterferenceCheck.cpp
)

if (NOT FREECAD_USE_EXTERNAL_ONDSELSOLVER)
    target_include_directories(AssemblyGui_tests_run PUBLIC
        ${CMAKE_SOURCE_DIR}/src/3rdParty/OndselSolver
    )
endif ()

target_link_libraries(AssemblyGui_tests_run
    GTest::gtest
    ${Python3_LIBRARIES}
    Assembly
    AssemblyGui
    PartGui
    FreeCADGui
    ${QtConcurrent_LIBRARIES}
)

if(WIN32)
    set_target_properties(AssemblyGui_tests_run PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
else()
    set_target_properties(AssemblyGui_tests_run PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
    )
endif()
