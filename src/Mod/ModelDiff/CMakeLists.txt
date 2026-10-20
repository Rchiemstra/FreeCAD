# SPDX-License-Identifier: LGPL-2.1-or-later
set(ModelDiff_SOURCES Init.py modeldiff.py modeldiff_worker.py TestModelDiff.py README.md)
if(BUILD_GUI)
    list(APPEND ModelDiff_SOURCES InitGui.py TestModelDiffGui.py)
endif()
add_custom_target(ModelDiffScripts ALL SOURCES ${ModelDiff_SOURCES})
fc_copy_sources(ModelDiffScripts "${CMAKE_BINARY_DIR}/Mod/ModelDiff" ${ModelDiff_SOURCES})
install(FILES ${ModelDiff_SOURCES} DESTINATION Mod/ModelDiff)
