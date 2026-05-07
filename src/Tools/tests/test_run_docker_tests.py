# SPDX-License-Identifier: LGPL-2.1-or-later

"""Regression tests for repo-root :mod:`run_docker_tests` log filtering."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import run_docker_tests as rdt


class TestKeepLogLine(unittest.TestCase):
    def assert_keeps(self, line: str, msg: str | None = None) -> None:
        self.assertTrue(rdt.keep_log_line(line), msg or repr(line))

    def assert_drops(self, line: str, msg: str | None = None) -> None:
        self.assertFalse(rdt.keep_log_line(line), msg or repr(line))

    def test_phase_and_section_markers_kept(self) -> None:
        self.assert_keeps("========== Build release ==========\n")
        self.assert_keeps("\nRequired phase failed with exit code 2\n")
        self.assert_keeps("Test phase passed\n")
        self.assert_keeps("Test phase failed with exit code 1\n")
        self.assert_keeps("Install phase failed with exit code 3\n")
        self.assert_keeps("All Docker test phases passed\n")
        self.assert_keeps("One or more Docker test phases failed; first failure exit code: 5\n")

    def test_traceback_and_assertions_kept(self) -> None:
        self.assert_keeps("Traceback (most recent call last):\n")
        self.assert_keeps("AssertionError: expected x\n")
        self.assert_keeps("RuntimeError: boom\n")

    def test_ctest_failure_artifacts_kept(self) -> None:
        self.assert_keeps("The following tests FAILED:\n")
        self.assert_keeps("Errors while running CTest\n")
        self.assert_keeps(
            "  42/1619 Test   #42: SomeTest ......................................................***Failed 0.01 sec\n"
        )
        self.assert_keeps(
            "         12 - MyTest (Failed)\n",
        )
        self.assert_keeps("100% tests passed, 0 tests failed out of 1612\n")
        self.assert_keeps("Total Test time (real) = 122.84 sec\n")

    def test_ctest_noise_dropped(self) -> None:
        self.assert_drops("          Start    1: AsyncRecomputeTest.Foo\n")
        self.assert_drops(
            "   1/1619 Test    #1: AsyncRecomputeTest.Foo ..........................................   Passed    0.20 sec\n"
        )

    def test_ctest_skipped_disabled_dropped_timeout_kept(self) -> None:
        self.assert_drops(
            "  68/1619 Test   #68: BackupPolicyTest.Foo ...........................................***Skipped   0.09 sec\n"
        )
        self.assert_drops(
            "  84/1619 Test   #84: BackupPolicyTest.Bar .............................................***Not Run (Disabled)   0.00 sec\n"
        )
        self.assert_keeps(
            "   7/200 Test    #7: T ...............................................................***Timeout 60.00 sec\n"
        )

    def test_unittest_summary_kept_dots_dropped(self) -> None:
        self.assert_keeps("Ran 41 tests in 0.039s\n")
        self.assert_keeps("OK\n")
        self.assert_keeps("OK (skipped=13, expected failures=1)\n")
        self.assert_keeps("FAILED\n")
        self.assert_keeps("FAILED (failures=2)\n")
        self.assert_drops(".........................................\n")

    def test_system_exit_kept(self) -> None:
        self.assert_keeps("System exit\n")

    def test_freecad_cli_filtered_noise_dropped(self) -> None:
        self.assert_drops("\t\t\t\t(50 %)\n")
        self.assert_drops("(33 %)\n")
        self.assert_drops("Processing......\n")
        self.assert_drops("Processing.ok\n")
        self.assert_drops("ok\n")
        self.assert_keeps("OK\n")
        self.assert_drops("Importing project files......\n")
        self.assert_drops("Import Draft snapping. ...\n")
        self.assert_drops("Test Testing makeBuilding function\n")
        self.assert_drops(
            "Verify sketch-based wall uses AUTOJOIN when sketch joining is off. "
            "... Test Testing sketch-based wall with JOIN_SKETCHES=False, AUTOJOIN=True...\n"
        )
        self.assert_drops(
            "Tests that the Draft_Stretch tool correctly handles a rotated baseless wall. "
            "... Test Testing stretch on a rotated baseless wall...\n"
        )
        self.assert_drops(
            "<Sketch> SketchObject.cpp(426): Failed to make face for sketch: "
            "Part::FaceMaker: result shape is null.\n"
        )
        self.assert_drops("migration.INFO: Adding Units as 'Metric' for 5mm Endmill\n")
        self.assert_drops(
            "test_pow (TestSpreadsheet.SpreadsheetFunction.test_pow) ... "
            "<Spreadsheet> Cell.cpp(690): Unnamed#Spreadsheet.D21: bad.\n"
        )

    def test_unittest_header_with_trailing_junk_dropped(self) -> None:
        self.assert_drops(
            "test_foo (pkg.mod.Class.test_foo) ... extra noise on same line\n"
        )

    def test_unittest_verbose_ok_and_headers_dropped_failures_kept(self) -> None:
        self.assert_drops("test_import_draft (drafttests.test_import.DraftImport.test_import_draft)\n")
        self.assert_drops("Import the Draft module. ... ok\n")
        self.assert_drops("Some doc ... skipped\n")
        self.assert_keeps("Broken case ... FAIL\n")
        self.assert_keeps("Broken case ... ERROR\n")

    def test_freecad_splash_lines_dropped(self) -> None:
        self.assert_drops("FreeCAD 1.2.0, Libs: 1.2.0devR46728 (Git)\n")
        self.assert_drops("(C) 2001-2026 FreeCAD contributors\n")
        self.assert_drops(
            "FreeCAD is free and open-source software licensed under the terms of LGPL2+ license.\n"
        )

    def test_filter_log_trims_trailing_spaces(self) -> None:
        self.assertEqual(rdt.filter_log("========== x ==========   \n"), "========== x ==========\n")

    def test_build_progress_dropped(self) -> None:
        self.assert_drops("[ 12%] Building CXX object foo.o\n")
        self.assert_drops("[42/1203] Linking bar\n")

    def test_apt_dpkg_noise_dropped(self) -> None:
        self.assert_drops("(Reading database ... 25%\n")
        self.assert_drops("(Reading database ... 4381 files and directories currently installed.)\n")

    def test_pixi_task_echo_dropped(self) -> None:
        self.assert_drops("✨ Pixi task (build-release): cmake --build build/release\n")

    def test_decorative_equals_and_cmake_hint_dropped(self) -> None:
        self.assert_drops("   ==============\n")
        self.assert_drops("=================================================\n")
        self.assert_drops("Now run 'cmake --build /workspace/build/release' to build FreeCAD\n")

    def test_ctest_label_summary_dropped(self) -> None:
        self.assert_drops("Label Time Summary:\n")
        self.assert_drops("Qt    =   0.85 sec*proc (4 tests)\n")

    def test_occt_progress_messages_dropped(self) -> None:
        self.assert_drops("Preparing history information...\n")
        self.assert_drops("Post treatment of result shape...\n")
        self.assert_drops("Performing intersection of shapes...\n")
        self.assert_drops("Initialization of Intersection algorithm...\n")
        self.assert_drops("Building result of SECTION operation...\n")

    def test_triple_dot_progress_lines_dropped(self) -> None:
        self.assert_drops("-- Building splits of compounds...\n")
        self.assert_drops("-- Try importing femexamples.ccx_cantilever_ele_tetra4 ...\n")
        self.assert_drops("-- Test setback collapse fallback...\n")
        self.assert_keeps("ValueError: bad token ...\n")
        self.assert_keeps('  File "tests/test_x.py", line 12, in foo\n')

    def test_workspace_path_and_empty_bracket_drop_keeps_signals(self) -> None:
        self.assert_drops("/workspace/build/release/bin/FreeCADCmd\n")
        self.assert_drops("  4: Result '[]'\n")
        self.assert_drops("args=[]\n")
        self.assert_keeps('  File "/workspace/tests/test_x.py", line 3, in foo\n')
        self.assert_keeps("ValueError: bad value in /workspace/src/x.py\n")

    def test_cam_numeric_test_header_dropped(self) -> None:
        self.assert_drops(
            "test02 (CAMTests.TestPathSetupSheet.TestPathSetupSheet.test02)\n",
        )
        self.assert_drops(
            "test02 (CAMTests.TestPathSetupSheet.TestPathSetupSheet.test02) ... tail\n",
        )

    def test_gcode_motion_lines_dropped(self) -> None:
        self.assert_drops("G0 X6.500000 Y4.500000 Z0.000000\n")
        self.assert_drops("  G01 X-1.25 Y0 Z3\n")

    def test_added_group_dropped(self) -> None:
        self.assert_drops("Added Group something\n")

    def test_ellipsis_progress_dropped_signal_kept(self) -> None:
        self.assert_drops("Create a rectangle, and a circular array. .....\n")
        self.assert_drops("foo......bar\n")
        self.assert_keeps("Failed to parse config........ please fix\n")

    def test_freecad_gui_test_chatter_dropped(self) -> None:
        self.assert_drops("  Temporary document 'DraftCreation___test_arc'\n")
        self.assert_drops("  Test 'Draft Arc'\n")
        self.assert_drops("  Try importing 'Draft'\n")
        self.assert_drops("import TestFemImport \n")
        self.assert_drops("Recompute......\n")
        self.assert_drops("\t\t\t\t(100 %)\t\n")
        self.assert_drops("  Occasionally crashes\n")
        self.assert_drops("  radius=2\n")

    def test_compiler_errors_and_warnings_kept(self) -> None:
        self.assert_keeps("/src/file.cpp:10:5: error: unknown type name 'X'\n")
        self.assert_keeps("/src/file.cpp:3:1: fatal error: file not found\n")
        self.assert_keeps("/src/file.cpp:10:5: warning: implicit conversion loses precision\n")
        self.assert_keeps("In file included from /workspace/src/Mod/Foo.cpp:1:\n")
        self.assert_keeps("/workspace/src/Mod/Foo.h:60:10: warning: missing override [-W]\n")
        self.assert_drops("   60 |     void onSelectionChanged(const Gui::SelectionChanges& msg);\n")

    def test_filter_log_strips_cmake_warning_block(self) -> None:
        raw = (
            "✨ Pixi task (configure-release): cmake --preset foo\n"
            "CMake Warning (dev) at foo.cmake:45:\n"
            "  Policy CMP0144 is not set.\n"
            "\n"
            "=================================================\n"
            "========== Build release ==========\n"
        )
        out = rdt.filter_log(raw)
        self.assertNotIn("CMake Warning", out)
        self.assertNotIn("Policy CMP0144", out)
        self.assertIn("========== Build release ==========", out)

    def test_filter_log_skips_ctest_did_not_run_section(self) -> None:
        raw = (
            "100% tests passed, 0 tests failed out of 1612\n"
            "The following tests did not run:\n"
            "\t68 - BackupPolicyTest.Foo (Skipped)\n"
            "         78 - OtherTest.Timestamp (Skipped)\n"
            "\n"
            "Test phase passed\n"
        )
        out = rdt.filter_log(raw)
        self.assertNotIn("The following tests did not run", out)
        self.assertNotIn("BackupPolicyTest.Foo", out)
        self.assertNotIn("OtherTest.Timestamp", out)
        self.assertIn("Test phase passed", out)

    def test_cmake_found_spam_still_dropped(self) -> None:
        self.assert_drops("-- Found JPEG: /usr/lib/libjpeg.so\n")

    def test_filter_log_end_to_end_preserves_failures(self) -> None:
        raw = (
            "[ 10%] Building CXX object a.o\n"
            "          Start    1: T\n"
            "   1/2 Test    #1: T ...........................................................   Passed    0.01 sec\n"
            "   2/2 Test    #2: U ...........................................................***Failed 0.02 sec\n"
            "The following tests FAILED:\n"
            "         2 - U (Failed)\n"
            "50% tests passed, 1 tests failed out of 2\n"
        )
        out = rdt.filter_log(raw)
        self.assertNotIn("[ 10%]", out)
        self.assertNotIn("Start    1:", out)
        self.assertNotIn("Test    #1:", out)
        self.assertIn("***Failed", out)
        self.assertIn("The following tests FAILED:", out)
        self.assertIn("50% tests passed", out)

    def test_filter_log_cli_phase_keeps_summaries_only(self) -> None:
        raw = (
            "========== FreeCAD CLI tests on build dir ==========\n"
            "Processing......\n"
            "\t\t\t\t(50 %)\n"
            "test_x (mod.Class.test_x) ... tail\n"
            "ok\n"
            "migration.INFO: Adding Units\n"
            "Ran 2212 tests in 108.060s\n"
            "OK (skipped=13, expected failures=1)\n"
            "System exit\n"
            "Test phase passed\n"
        )
        out = rdt.filter_log(raw)
        self.assertIn("========== FreeCAD CLI tests on build dir ==========", out)
        self.assertIn("Ran 2212 tests", out)
        self.assertIn("OK (skipped=", out)
        self.assertIn("System exit", out)
        self.assertIn("Test phase passed", out)
        self.assertNotIn("Processing", out)
        self.assertNotIn("(50 %)", out)
        self.assertNotIn("migration.INFO", out)

    def test_filter_log_drops_verbose_unittest_noise(self) -> None:
        raw = (
            "========== FreeCAD CLI tests on build dir ==========\n"
            "test_bar (drafttests.test_import.DraftImport.test_bar)\n"
            "Do something. ... ok\n"
            "Ran 3 tests in 0.1s\n"
            "OK\n"
        )
        out = rdt.filter_log(raw)
        self.assertIn("========== FreeCAD CLI tests on build dir ==========", out)
        self.assertNotIn("test_bar (drafttests", out)
        self.assertNotIn("... ok", out)
        self.assertIn("Ran 3 tests", out)


    # ── New tests for improved filtering (noise categories found in real filtered logs) ─────

    def test_freecad_module_logger_dropped(self) -> None:
        """Module.LEVEL: messages are FreeCAD-style logger output — always noise."""
        self.assert_drops("SetupSheet.INFO: No support for Helix toolpath\n")
        self.assert_drops("Processor.WARNING: something unexpected happened\n")
        self.assert_drops("machine.WARNING: Invalid joint data for linear axis X\n")
        self.assert_drops("DrillCycleExpander.DEBUG: expanding drill cycle\n")
        # ERROR level must also be dropped (it's a logger format, not a Python error)
        self.assert_drops("TestPathLog.ERROR: some log message\n")
        self.assert_drops("Adaptive.INFO: convergence reached\n")

    def test_timestamp_freecad_tag_lines_dropped(self) -> None:
        """Lines with numeric timestamp prefix followed by <Tag> are internal FreeCAD traces."""
        self.assert_drops("2.7e-08 <App> Document.cpp(3475): pending remove object\n")
        self.assert_drops("58.5766 <App> Document.cpp(1234): saving...\n")
        self.assert_drops("0.001 <Base> Exception.cpp(42): something\n")

    def test_cam_property_and_startup_noise_dropped(self) -> None:
        """High-volume CAM migration / startup lines should be dropped."""
        self.assert_drops('New property added to "Job": [FeedRate]\n')
        self.assert_drops("Stock Material property is deprecated. Please use the stock object.\n")
        self.assert_drops("Startup!\n")
        self.assert_drops("All cleared.\n")

    def test_mbd_solver_output_dropped(self) -> None:
        self.assert_drops("MbD: Assembling system.\n")
        self.assert_drops("MbD: Convergence = 0\n")
        self.assert_drops("Time = 0\n")
        self.assert_drops("Time = 3.14\n")

    def test_fem_banner_and_signal_noise_dropped(self) -> None:
        """FEM decorative star banners are section headers, not errors."""
        self.assert_drops("******* run FEM TestFem tests *******\n")
        self.assert_drops("************************************************************\n")
        self.assert_drops("onChanged App::Document changed\n")

    def test_add_property_type_dropped(self) -> None:
        self.assert_drops("Add property type: App::PropertyBoolList\n")
        self.assert_drops("Add property type: Sketcher::PropertyConstraintList\n")

    def test_cam_tool_output_dropped(self) -> None:
        self.assert_drops("Tool Diameter: 5\n")
        self.assert_drops("Min step size: 100 um\n")
        self.assert_drops("** Processing region: 1\n")
        self.assert_drops("Spindle speed will be controlled using percentages\n")
        self.assert_drops("Spindle speed will be controlled using using RPM\n")
        self.assert_drops("Boundary check: job exceeds machine limit\n")
        self.assert_drops("Invalid substitution strings will be ignored in output path:\n")

    def test_cam_machine_compatibility_dropped(self) -> None:
        self.assert_drops("Machine Snapmaker2_A250 is not compatible with modkit X\n")
        self.assert_drops("Machine Foo has multiple compatible toolheads\n")
        self.assert_drops(" Choose from the following: ...\n")

    def test_cam_section_labels_dropped(self) -> None:
        self.assert_drops("=== Pentagram Test: G-code Drill Cycle ===\n")
        self.assert_drops("=== Complex Wire Test ===\n")
        self.assert_drops("long axis G-code:\n")
        self.assert_drops("Angled rectangle short axis G-code:\n")

    def test_testpathlog_lines_dropped(self) -> None:
        self.assert_drops("TestPathLog(42).testSomething(args)\n")
        self.assert_drops("TestPathLog.DEBUG: detailed trace line\n")

    def test_show_editor_and_here_dropped(self) -> None:
        self.assert_drops("Show editor = 0\n")
        self.assert_drops("Show editor = 1\n")
        self.assert_drops("here\n")

    def test_fem_verbose_output_dropped(self) -> None:
        self.assert_drops("doc objects count: 7\n")
        self.assert_drops("PropertyPostDataObject::SaveDocFile: writing\n")
        self.assert_drops("load old document objects\n")
        self.assert_drops("load master head document objects\n")
        self.assert_drops("AddGroupElements: adding group\n")
        self.assert_drops("Writing /tmp/FEM_unittests/TestFem/ccx_analysis.inp\n")
        self.assert_drops("Getting mesh data time: 0.01s\n")
        self.assert_drops("Writing time CalculiX input file: 0.02s\n")

    def test_elmer_z88_verbose_dropped(self) -> None:
        self.assert_drops("Saved unit schema: MKS Standard\n")
        self.assert_drops("Write elmer input files to: /tmp/FEM_unittests/\n")
        self.assert_drops("Write z88 input files to: /tmp/FEM_unittests/\n")
        self.assert_drops("The FreeCAD standard unit schema\n")
        self.assert_drops("The SI unit schema (MKS)\n")
        self.assert_drops("Test writing STARTINFO file\n")
        self.assert_drops("Test writing case file\n")
        self.assert_drops("Writing time input file: 0.01s\n")
        self.assert_drops("Reset unit schema back to standard\n")
        self.assert_drops("'Coordinate Scaling Revert: 1'\n")
        self.assert_drops("['z88i5.txt', 'z88i2.txt']\n")
        self.assert_drops("Comparing /tmp/FEM_unittests/result.z88\n")
        self.assert_drops("Filling splits of Gmsh mesh\n")
        self.assert_drops("ProcGmsh binary not found.\n")

    def test_techdraw_drawview_verbose_dropped(self) -> None:
        self.assert_drops("DrawViewDetail test: view created\n")
        self.assert_drops("testing DrawViewDetail\n")
        self.assert_drops("DrawViewDetail test finished\n")
        self.assert_drops("DVDTest.tearDown()\n")

    def test_toposhapelisttest_markers_dropped(self) -> None:
        self.assert_drops("TopoShapeListTest: setUp complete\n")
        self.assert_drops("running TopoShapeListTest\n")
        self.assert_drops("TopoShapeListTest finished\n")

    def test_verbose_test_description_prints_dropped(self) -> None:
        self.assert_drops("Test Arch Link addition (Immediate Lifecycle)\n")
        self.assert_drops("Test makeStructure auto-label Beam\n")
        self.assert_drops("Test placeAlongEdge base == p1\n")

    def test_attachment_offset_dropped(self) -> None:
        self.assert_drops("Converting attachment offset of Sketch001\n")

    def test_body_tip_empty_dropped(self) -> None:
        self.assert_drops("Body: Tip shape is empty\n")

    def test_short_quoted_string_dropped(self) -> None:
        self.assert_drops("'abcef'\n")
        self.assert_drops("'abc_ef'\n")

    def test_garbled_ok_short_words_dropped(self) -> None:
        """Single-letter + ok garbled variants must be caught."""
        self.assert_drops("Pok\n")
        self.assert_drops("Rok\n")
        self.assert_drops("Prok\n")
        self.assert_drops("Prook\n")

    def test_sketcher_solver_noise_dropped(self) -> None:
        self.assert_drops("Invalid solution from DogLeg solver.\n")
        self.assert_drops("Updating geometry: Error build geometry(0): Both points are equal\n")

    def test_techdraw_verbose_dropped(self) -> None:
        self.assert_drops("making a page\n")
        self.assert_drops("Page created\n")
        self.assert_drops("making a projection group\n")
        self.assert_drops("adding views\n")
        self.assert_drops("added Front\n")
        self.assert_drops("View: Front TechDraw::DrawViewPart/Front\n")
        self.assert_drops("adding balloon1\n")
        self.assert_drops("Item Label: test_label\n")
        self.assert_drops("recomputing document\n")

    def test_thread_test_verbose_dropped(self) -> None:
        self.assert_drops("Call from Python thread Thread-1\n")

    def test_dxf_import_info_dropped(self) -> None:
        self.assert_drops("DXF version: R12\n")
        self.assert_drops("File encoding: UTF-8\n")
        self.assert_drops("File units: mm\n")
        self.assert_drops("Import settings:\n")
        self.assert_drops("Entity counts:\n")
        self.assert_drops("Performance:\n")

    def test_progress_percentage_with_ok_dropped(self) -> None:
        """(N %) followed by ok or tab-ok should be dropped."""
        self.assert_drops("\t\t\t\t(18 %)\tok\n")
        self.assert_drops("(100 %)\tok\n")
        # garbled open-paren forms
        self.assert_drops("\t\t\t\t(ok\n")
        self.assert_drops("\t\t\t\t(\n")

    def test_unittest_header_with_exception_in_desc_dropped(self) -> None:
        """REGRESSION: test descriptions mentioning 'exception' must NOT be kept as errors."""
        self.assert_drops(
            "Verify Helix does not throw an exception. ... ok\n",
            "test description with 'exception' followed by ok should be dropped",
        )
        self.assert_drops(
            "testInvalidAttribute (CAMTests.TestSomething.testInvalidAttribute) "
            "... <Exception> No such attribute\n",
        )
        self.assert_drops(
            "testFoo (mod.Cls.testFoo) ... <Sketch> SketchObject.cpp(1): Failed to make face\n",
            "header line with FreeCAD tag after ... must be dropped even when 'Failed' appears",
        )

    def test_explicit_fail_error_suffixes_kept(self) -> None:
        """REGRESSION: lines ending in ' ... FAIL' or ' ... ERROR' must always be kept."""
        self.assert_keeps("testBroken (mod.Cls.testBroken) ... FAIL\n")
        self.assert_keeps("testBroken (mod.Cls.testBroken) ... ERROR\n")
        # Whitespace variants
        self.assert_keeps("some description ... FAIL  \n")
        self.assert_keeps("some description ... ERROR  \n")

    def test_filter_log_skips_bash_script_preamble(self) -> None:
        """Command: ... bash -lc '...' metadata block must be fully dropped in filtered output."""
        raw = (
            "Command: docker run --rm -v /host:/workspace myimage bash -lc '\n"
            "export PATH=/opt/bin:$PATH\n"
            "function run_phase() {\n"
            "  echo start\n"
            "}\n"
            "'\n"
            "========== Build release ==========\n"
        )
        out = rdt.filter_log(raw)
        self.assertNotIn("Command:", out)
        self.assertNotIn("export PATH", out)
        self.assertNotIn("function run_phase", out)
        self.assertNotIn("echo start", out)
        self.assertIn("========== Build release ==========", out)


if __name__ == "__main__":
    unittest.main()
