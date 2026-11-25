# As of February 2025 we require CMake 3.22.0
cmake_minimum_required(VERSION 3.22.0 FATAL_ERROR)


# As of cMake 3.27, find_package() will look for both case-sensitive and all-uppercase versions
# of the package. This seems to affect FLANN as included by Pixi
if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW)
endif()

# The exec_program command was deprecated in cMake 3.0, and policy CMP0153 was added in cMake
# 3.28 to control whether this gives a warning (the OLD behavior) or a fatal error (NEW)
if(POLICY CMP0153)
    cmake_policy(SET CMP0153 NEW)
endif()

# Suppress 'FindBoost module is removed' warning. Will use BoostConfig.cmake instead
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# As of cMake 3.31 add_custom_command() rejects invalid arguments
if(POLICY CMP0175)
    cmake_policy(SET CMP0175 NEW)
endif()

# Added in cMake 3.31, enforce normalization of all DESTINATION paths given to install()
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

# Make CMake output a bit more focused
set(CMAKE_REQUIRED_QUIET TRUE)


option(FREECAD_USE_CCACHE "Auto detect and use ccache during compilation" ON)

# FreeCAD does not use C++20 modules. Keep CMake 3.28+ from enabling module
# dependency scanning on C++20 targets, especially with toolchains that do not
# provide a scanner such as clang-scan-deps.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Version and Package Name are set in the version.json file and propagate to all other
# files with version information in them either via CMake or via the sync_version.py
# script (in src/Tools/)
file(READ "${CMAKE_SOURCE_DIR}/version.json" VERSION_JSON)
string(JSON _PROJECT_NAME GET ${VERSION_JSON} "name")
string(JSON PACKAGE_VERSION_MAJOR GET ${VERSION_JSON} "version_major")
string(JSON PACKAGE_VERSION_MINOR GET ${VERSION_JSON} "version_minor")
string(JSON PACKAGE_VERSION_PATCH GET ${VERSION_JSON} "version_patch")
string(JSON PACKAGE_VERSION_SUFFIX GET ${VERSION_JSON} "version_suffix")
string(JSON PACKAGE_BUILD_VERSION GET ${VERSION_JSON} "build_version")

project(${_PROJECT_NAME})

# project() initializes launcher settings from the toolchain and environment.
# A defined launcher (including an empty one) is an explicit choice.
if(FREECAD_USE_CCACHE)
    if(NOT DEFINED CMAKE_C_COMPILER_LAUNCHER OR NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
        find_program(CCACHE_PROGRAM ccache)
        if(CCACHE_PROGRAM)
            foreach(_lang C CXX)
                if(NOT DEFINED CMAKE_${_lang}_COMPILER_LAUNCHER)
                    set(CMAKE_${_lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
                    message(STATUS "Using ccache for ${_lang}: ${CCACHE_PROGRAM}")
                endif()
            endforeach()
        endif()
    endif()
endif()
string(TIMESTAMP PACKAGE_COPYRIGHT_YEAR "%Y")

set(PACKAGE_VERSION "${PACKAGE_VERSION_MAJOR}.${PACKAGE_VERSION_MINOR}.${PACKAGE_VERSION_PATCH}")
set(PACKAGE_STRING "${PROJECT_NAME} ${PACKAGE_VERSION}")

# include local modules
include(CheckCXXCompilerFlag)
include(AddFileDependencies)
include(cMake/FreeCadMacros.cmake)
# include helper functions/macros
add_subdirectory(cMake/FreeCAD_Helpers)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cMake")

CompilerChecksAndSetups()
ConfigureCMakeVariables()
InitializeFreeCADBuildOptions()
CheckInterModuleDependencies()
FreeCADLibpackChecks()
SetupDoxygen()
SetupJSON()
SetupKDTree()
SetupSanitizers()
SetupYamlCpp()
SetupZipIos()
SetupClipper2()

find_package(Qt6 REQUIRED COMPONENTS Core)
set(FREECAD_QT_MAJOR_VERSION 6)

find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(cpptrace QUIET)
if(NOT FREECAD_LIBPACK_USE OR FREECAD_LIBPACK_CHECKFILE_CLBUNDLER OR FREECAD_LIBPACK_CHECKFILE_VERSION)
    SetupPython()
    SetupPCL()
    SetupPybind11()
    SetupXercesC()
    find_package(ZLIB REQUIRED)
    find_package(PyCXX REQUIRED)
    SetupOpenCasCade()
    if(BUILD_GUI)
        # Do this before the check for SMESH because it depends on vtk
        # that may have its own OpenGL check but possibly fails and leaves
        # OPENGL_gl_LIBRARY empty that results into linker errors
        SetupOpenGL()
    endif(BUILD_GUI)
    SetupSalomeSMESH()
    if (BUILD_FEM_NETGEN)
        set(NETGEN_DEFINITIONS -DNO_PARALLEL_THREADS -DOCCGEOMETRY)
        if (NOT FREECAD_USE_EXTERNAL_SMESH)
            find_package(NETGEN REQUIRED)
        endif()
    endif()
    SetupSwig()
    SetupEigen()
    # This file is included directly due to some cMake macros that are defined within.
    # If these macro definitions are embedded within another macro, it causes problems.
    include(cMake/FreeCAD_Helpers/SetupQt.cmake)
    SetupFreetype()

    if(BUILD_GUI)
        SetupCoinPivy()
        SetupSpaceball()
        SetupShibokenAndPyside()
        SetupMatplotlib()
    endif(BUILD_GUI)

    # SetupCoinPivy can overwrite find_package(Boost) output so keep this after.
    SetupBoost()

    if(BUILD_BIM)
        SetupLark()
        find_python_runtime_dep(ifcopenshell ifcopenshell ifcopenshell_VERSION "IFC files support disabled.")
    endif()
    if(BUILD_ASSEMBLY)
        find_python_runtime_dep(av av av_VERSION "Assembly simulation video export will not work properly.")
        find_python_runtime_dep(pillow PIL pillow_VERSION "Assembly simulation video export will not work properly.")
    endif()

    # HDF5 might have been imported by any of the above dependencies (SMESH, VTK, ...)
    # and added _FORTIFY_SOURCE defines to the build. Clean them at the end of import chains
    # to make sure nothing will reinstate them.
    hdf5_clean_fortify_source()
endif()

SetLibraryVersions()
SetGlobalCompilerAndLinkerSettings()

add_subdirectory(src)

# The Linux CI embedded-loop job uses paths from this exact configured build.
# Failure to capture the binding leaves the explicit job unable to run; it
# does not make PySide a new configure-time requirement for other builds.
if(BUILD_GUI AND UNIX AND NOT APPLE AND FREECAD_QT_MAJOR_VERSION EQUAL 6
   AND TARGET FreeCADMainPy AND TARGET FreeCADGuiPy
   AND TARGET PySide6::pyside6 AND TARGET Shiboken6::libshiboken)
    set(_embedded_origins "${CMAKE_BINARY_DIR}/embedded-gui-origins.json")
    file(REMOVE "${_embedded_origins}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I
        "${CMAKE_SOURCE_DIR}/.github/scripts/embedded_gui_config.py" "${_embedded_origins}"
        RESULT_VARIABLE _embedded_capture_result)
    if(NOT _embedded_capture_result EQUAL 0)
        message(WARNING "Could not capture configured Python Qt binding origins")
    endif()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/embedded-gui-config-$<CONFIG>.txt" CONTENT
"${Python3_EXECUTABLE}
$<TARGET_FILE:FreeCADMainPy>
$<TARGET_FILE:FreeCADGuiPy>
${CMAKE_BINARY_DIR}/Ext
${_embedded_origins}
$<TARGET_FILE:Qt6::Core>
$<TARGET_FILE:Qt6::Gui>
$<TARGET_FILE:Qt6::Widgets>
$<TARGET_FILE:PySide6::pyside6>
$<TARGET_FILE:Shiboken6::libshiboken>
")
    # Single-config CI has CMAKE_BUILD_TYPE=Debug.
else()
    # A reused build may no longer support the handoff after reconfiguration.
    file(GLOB _obsolete_embedded_configs "${CMAKE_BINARY_DIR}/embedded-gui-config-*.txt")
    file(REMOVE ${_obsolete_embedded_configs} "${CMAKE_BINARY_DIR}/embedded-gui-origins.json")
endif()
add_subdirectory(data)

BuildAndInstallDesignerPlugin()

CreatePackagingTargets()

if(MSVC AND FREECAD_LIBPACK_USE AND LIBPACK_FOUND)
    CopyLibpackDirectories()
endif()

if (ENABLE_DEVELOPER_TESTS)
    include(CTest)
    enable_testing()
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()

PrintFinalReport()

message("\n=================================================\n"
        "Now run 'cmake --build ${CMAKE_BINARY_DIR}' to build ${PROJECT_NAME}\n"
        "=================================================\n")
