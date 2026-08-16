# SPDX-License-Identifier: LGPL-2.1-or-later

# Helper executable for the cross-process save-lock anchor gate.
#
# The lock that guards a save is half process-local mutex and half kernel
# byte-range lock. Only the kernel half arbitrates between processes, and POSIX
# record locks never conflict within one process, so the contract is invisible
# to any in-process test. save_lock_anchor_gate.py drives several of these
# helpers against one destination to measure it.
add_executable(save_lock_probe save_lock_probe.cpp)

target_link_libraries(save_lock_probe PRIVATE FreeCADApp)

if(WIN32)
    set_target_properties(save_lock_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
else()
    set_target_properties(save_lock_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
endif()
