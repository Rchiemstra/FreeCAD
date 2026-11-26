cmake_minimum_required(VERSION 3.21)
project(PivyStageFixture NONE)
if(NOT HELPER_DIR)
    message(FATAL_ERROR "HELPER_DIR is required")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shared" "${CMAKE_BINARY_DIR}/input/pivy/gui")
file(WRITE "${CMAKE_BINARY_DIR}/shared/coin.py" "# generated wrapper\n")
file(WRITE "${CMAKE_BINARY_DIR}/input/pivy/__init__.py" "# package\n")
file(WRITE "${CMAKE_BINARY_DIR}/input/pivy/gui/__init__.py" "# gui\n")
add_custom_target(pivy_coin
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/$<CONFIG>/Mod/pivy"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/extension-$<CONFIG>.txt"
        "${CMAKE_BINARY_DIR}/$<CONFIG>/Mod/pivy/_coin.fixture"
    VERBATIM)
add_custom_target(pivy)
include("${HELPER_DIR}/StageBundledPivy.cmake")
freecad_stage_bundled_pivy("${CMAKE_BINARY_DIR}/input/pivy" "${CMAKE_BINARY_DIR}/shared" "${CMAKE_BINARY_DIR}")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    foreach(kind LIBRARY RUNTIME)
        get_target_property(path pivy_coin "${kind}_OUTPUT_DIRECTORY_${upper}")
        if(NOT path STREQUAL "${CMAKE_BINARY_DIR}/${config}/Mod/pivy")
            message(FATAL_ERROR "Wrong ${kind} directory for ${config}: ${path}")
        endif()
    endforeach()
endforeach()
