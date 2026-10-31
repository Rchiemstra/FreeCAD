# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: 2026 Joao Matos
# SPDX-FileNotice: Part of the FreeCAD project.

SET(Test_SRCS
    __init__.py
    Init.py
    BaseTests.py
    FreeCADInitTests.py
    AutoSaverStress.py
    RunAutoSaverStress.py
    Document.py
    GuiDocument.py
    TestQuarterViewGui.py
    Metadata.py
    StringHasher.py
    Menu.py
    Recompute.py
    ParallelRecomputeBaseline.py
    GuiRecompute.py
    TestApp.py
    TestLinkArrayTransactions.py
    TestLinkSuppression.py
    TestIntPairList.py
    TestGui.py
    TestPySideUic.py
    UnicodeTests.py
    UnitTests.py
    Workbench.py
    unittestgui.py
    testmakeWireString.py
    TestCrashReporter.py
    TestPythonSyntax.py
    TestPerf.py
    TestTreeSelection.py
    TestGraphicsViewWrapping.py
    TestMDIView.py
    TestRubberbandSelection.py
    TestCoinNodeSnapshots.py
    TestView3DFramebufferCapture.py
    TestSelectionVisual.py
    TestCornerAxisCrossVisual.py
    TestViewProviderLink.py
)

SET(TestData_SRCS
    TestData/basic_metadata.xml
    TestData/bad_root_node.xml
    TestData/bad_xml.xml
    TestData/bad_version.xml
    TestData/content_items.xml
    TestData/DXFSample.dxf
)

SOURCE_GROUP("" FILES ${Test_SRCS} ${TestData_SRCS})

if(BUILD_GUI)
    add_subdirectory(Gui)
    list (APPEND Test_SRCS InitGui.py)
endif(BUILD_GUI)

set(Test_COPY_OUTPUTS)
foreach(test_src ${Test_SRCS})
    list(APPEND Test_COPY_OUTPUTS "${CMAKE_BINARY_DIR}/Mod/Test/${test_src}")
endforeach()
foreach(test_data_src ${TestData_SRCS})
    list(APPEND Test_COPY_OUTPUTS "${CMAKE_BINARY_DIR}/Mod/Test/${test_data_src}")
endforeach()

add_custom_target(TestSources DEPENDS ${Test_COPY_OUTPUTS})

if(BUILD_MATERIAL AND TARGET MaterialLib)
    add_dependencies(TestSources MaterialLib)
endif()

ADD_CUSTOM_TARGET(Test ALL
    DEPENDS TestSources
    SOURCES ${Test_SRCS} ${TestData_SRCS}
)

fc_copy_sources(Test "${CMAKE_BINARY_DIR}/Mod/Test" ${Test_SRCS} ${TestData_SRCS})

# Coin node snapshot visual test baselines (Python unittest `TestCoinNodeSnapshots`).
# For out-of-source builds, copy baselines into the build tree so the test can find them
# without requiring FC_VISUAL_BASELINE_DIR to be set.
set(_coin_node_snapshot_baseline_dir "${CMAKE_SOURCE_DIR}/tests/visual/baselines/coin-nodes")
if(EXISTS "${_coin_node_snapshot_baseline_dir}")
    file(
        GLOB
        TestCoinNodeSnapshotBaselines
        CONFIGURE_DEPENDS
        "${_coin_node_snapshot_baseline_dir}/*.png"
    )
    if(TestCoinNodeSnapshotBaselines)
        set(_coin_node_snapshot_baseline_outdir "${CMAKE_BINARY_DIR}/tests/visual/baselines/coin-nodes")
        set(_coin_node_snapshot_baseline_outputs)
        foreach(_baseline ${TestCoinNodeSnapshotBaselines})
            get_filename_component(_baseline_name "${_baseline}" NAME)
            set(_baseline_out "${_coin_node_snapshot_baseline_outdir}/${_baseline_name}")
            add_custom_command(
                OUTPUT "${_baseline_out}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_coin_node_snapshot_baseline_outdir}"
                COMMAND "${CMAKE_COMMAND}" -E copy "${_baseline}" "${_baseline_out}"
                DEPENDS "${_baseline}"
                COMMENT "Copying ${_baseline} to ${_baseline_out}"
            )
            list(APPEND _coin_node_snapshot_baseline_outputs "${_baseline_out}")
        endforeach()
        add_custom_target(TestCoinNodeSnapshotBaselines ALL DEPENDS ${_coin_node_snapshot_baseline_outputs})
        add_dependencies(Test TestCoinNodeSnapshotBaselines)
    endif()
endif()

# Vendored fonts used by visual snapshot tests.
# For out-of-source builds, copy them into the build tree so the test can find them
# without relying on host-installed fonts.
set(_coin_node_snapshot_font_dir "${CMAKE_SOURCE_DIR}/tests/visual/fonts")
if(EXISTS "${_coin_node_snapshot_font_dir}")
    file(
        GLOB
        TestCoinNodeSnapshotFonts
        CONFIGURE_DEPENDS
        "${_coin_node_snapshot_font_dir}/*"
    )
    if(TestCoinNodeSnapshotFonts)
        set(_coin_node_snapshot_font_outdir "${CMAKE_BINARY_DIR}/tests/visual/fonts")
        set(_coin_node_snapshot_font_outputs)
        foreach(_font_file ${TestCoinNodeSnapshotFonts})
            get_filename_component(_font_name "${_font_file}" NAME)
            set(_font_out "${_coin_node_snapshot_font_outdir}/${_font_name}")
            add_custom_command(
                OUTPUT "${_font_out}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_coin_node_snapshot_font_outdir}"
                COMMAND "${CMAKE_COMMAND}" -E copy "${_font_file}" "${_font_out}"
                DEPENDS "${_font_file}"
                COMMENT "Copying ${_font_file} to ${_font_out}"
            )
            list(APPEND _coin_node_snapshot_font_outputs "${_font_out}")
        endforeach()
        add_custom_target(TestCoinNodeSnapshotFonts ALL DEPENDS ${_coin_node_snapshot_font_outputs})
        add_dependencies(Test TestCoinNodeSnapshotFonts)
    endif()
endif()

INSTALL(
    FILES
        ${Test_SRCS}
    DESTINATION
        Mod/Test
)

INSTALL(
    FILES
        ${TestData_SRCS}
    DESTINATION
        Mod/Test/TestData
)

# Serial recompute baselines live under data/tests and are copied next to the
# Test module, the same way TestData is copied for in-tree and installed runs.
set(ParallelRecomputeFixture_SRCS
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/featuretest-abz-clean.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/featuretest-abz-dirty.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/part-siblings.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/part-chain.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/part-abz.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/part-invalid-box.FCStd
    ${CMAKE_SOURCE_DIR}/data/tests/parallel-recompute/part-dirty-box.FCStd
)
set(_parallel_recompute_fixture_outdir
    "${CMAKE_BINARY_DIR}/Mod/Test/TestData/parallel-recompute")
set(_parallel_recompute_fixture_outputs)
foreach(_fixture ${ParallelRecomputeFixture_SRCS})
    get_filename_component(_fixture_name "${_fixture}" NAME)
    set(_fixture_out "${_parallel_recompute_fixture_outdir}/${_fixture_name}")
    add_custom_command(
        OUTPUT "${_fixture_out}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_parallel_recompute_fixture_outdir}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${_fixture}" "${_fixture_out}"
        DEPENDS "${_fixture}"
        COMMENT "Copying ${_fixture_name} to TestData/parallel-recompute"
    )
    list(APPEND _parallel_recompute_fixture_outputs "${_fixture_out}")
endforeach()
add_custom_target(ParallelRecomputeFixtures ALL DEPENDS ${_parallel_recompute_fixture_outputs})
add_dependencies(Test ParallelRecomputeFixtures)

INSTALL(
    FILES
        ${ParallelRecomputeFixture_SRCS}
    DESTINATION
        Mod/Test/TestData/parallel-recompute
)
