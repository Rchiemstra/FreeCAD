# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(App)

if(BUILD_GUI)
    add_subdirectory(Gui)
endif()

if (NOT FREECAD_USE_EXTERNAL_ONDSELSOLVER)
	target_include_directories(Assembly_tests_run PUBLIC
		${CMAKE_SOURCE_DIR}/src/3rdParty/OndselSolver
	)
endif ()

target_link_libraries(Assembly_tests_run
    GTest::gtest_main
	${Python3_LIBRARIES}
    Assembly
    PartDesign
)
