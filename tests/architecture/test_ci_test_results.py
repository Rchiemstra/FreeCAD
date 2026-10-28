# SPDX-License-Identifier: LGPL-2.1-or-later
"""Black-box regression tests for the shared Woodpecker result gate.

Run with python3 -m unittest discover -s tests/architecture -p test_ci_test_results.py.
These fixtures never launch FreeCAD.
"""

from pathlib import Path
import json
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER = ROOT / "ci/woodpecker/check-test-results.py"
INVENTORY = ROOT / "ci/woodpecker/debug-test-binaries.txt"


class ResultCheckerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def check(self, *args, success=False, message=None):
        result = subprocess.run(
            [sys.executable, str(CHECKER), *map(str, args)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        if message:
            self.assertIn(message, result.stdout + result.stderr)
        return result

    def xml(self, content):
        path = self.directory / "result.xml"
        path.write_text(content)
        return path

    def test_missing_binary(self):
        self.check("inventory", self.directory, message="App_tests_run")

    def test_nonexecutable_binary(self):
        names = [line for line in INVENTORY.read_text().splitlines()
                 if line and not line.startswith("#")]
        for name in names:
            path = self.directory / name
            path.write_text("#!/bin/sh\nexit 0\n")
            path.chmod(0o755)
        (self.directory / names[0]).chmod(0o644)
        self.check("inventory", self.directory, message="not executable")

    def test_green_inventory_and_cmake_source(self):
        names = [line for line in INVENTORY.read_text().splitlines()
                 if line and not line.startswith("#")]
        cmake = (ROOT / "tests/CMakeLists.txt").read_text()
        targets = set(re.findall(r"\b\w+_tests_run\b", cmake))
        self.assertEqual(set(names), targets)
        self.assertEqual(len(names), len(targets))
        for name in names:
            path = self.directory / name
            path.write_text("#!/bin/sh\nexit 0\n")
            path.chmod(0o755)
        result = self.check("inventory", self.directory, success=True)
        self.assertEqual(result.stdout.split(), names)

    def test_missing_xml(self):
        self.check("xml", self.directory / "missing.xml", message="missing.xml")

    def test_bad_xml(self):
        self.check("xml", self.xml("<testsuites"), message="invalid XML")

    def test_zero_tests(self):
        self.check("xml", self.xml('<testsuites tests="0"/>'), message="zero executed")

    def test_failing_xml(self):
        for kind in ("failure", "error"):
            with self.subTest(kind=kind):
                self.check("xml", self.xml(
                    f'<testsuites tests="1"><testsuite tests="1">'
                    f'<testcase name="bad"><{kind}/></testcase>'
                    '</testsuite></testsuites>'), message="failure/error")

    def test_failing_counters(self):
        for counter in ("failures", "errors"):
            with self.subTest(counter=counter):
                self.check("xml", self.xml(
                    f'<testsuite tests="1" {counter}="1"><testcase/></testsuite>'),
                    message="failure/error")

    def test_invalid_counts_and_shape(self):
        for content in ('<other tests="1"><testcase/></other>',
                        '<testsuite tests="2"><testcase/></testsuite>',
                        '<testsuite tests="bogus"><testcase/></testsuite>',
                        '<testsuite><testcase/></testsuite>'):
            with self.subTest(content=content):
                self.check("xml", self.xml(content), message="invalid")

    def test_only_skipped_or_disabled_is_not_execution(self):
        for case in ('<testcase><skipped/></testcase>',
                     '<testcase status="notrun"/>',
                     '<testcase status="run" result="skipped"/>'):
            with self.subTest(case=case):
                self.check("xml", self.xml(f'<testsuite tests="1">{case}</testsuite>'),
                           message="zero executed")

    def test_green_xml(self):
        self.check("xml", self.xml(
            '<testsuites tests="2" failures="0" errors="0"><testsuite tests="2">'
            '<testcase status="run" result="completed"/>'
            '<testcase status="notrun"/></testsuite></testsuites>'),
            success=True, message="executed=1")

    def test_each_xml_is_required(self):
        good = self.xml('<testsuite tests="1"><testcase/></testsuite>')
        self.check("xml", good, self.directory / "absent.xml", message="absent.xml")

    def process(self, binary="Gui_tests_run", rc=-11, entries=(),
                content='<testsuite tests="1"><testcase/></testsuite>',
                log="[  PASSED  ] 1 test.\nSegmentation fault\n", **kwargs):
        report = self.xml(content)
        output = self.directory / "process.log"
        output.write_text(log)
        quarantine = self.directory / "quarantine.json"
        quarantine.write_text(json.dumps(list(entries)))
        return self.check("gtest", "--binary", binary, "--exit-code", rc,
                          "--filter", "CollaborationResponsiveness*",
                          "--log", output, "--quarantine", quarantine, report, **kwargs)

    def quarantine_entry(self):
        return {"binary": "Gui_tests_run", "filter": "CollaborationResponsiveness*",
                "failure_mode": "signal:11-after-results", "reason": "fixture teardown bug",
                "tracking": "doc/fixture.md"}

    def test_passed_then_crash_fails(self):
        self.process(message="process exit -11")

    def test_quarantined_exact_case_passes(self):
        self.process(entries=[self.quarantine_entry()], success=True, message="QUARANTINE")

    def test_quarantined_binary_different_failure_fails(self):
        for rc in (1, -6, -9, 139):
            with self.subTest(rc=rc):
                self.process(rc=rc, entries=[self.quarantine_entry()], message=f"process exit {rc}")

    def test_unknown_binary_crash_fails(self):
        self.process(binary="Unknown_tests_run", entries=[self.quarantine_entry()],
                     message="process exit -11")

    def test_quarantine_requires_complete_green_results_and_summary(self):
        for content in ('<testsuite tests="1">', '<testsuite tests="0"/>',
                        '<testsuite tests="1"><testcase><failure/></testcase></testsuite>'):
            with self.subTest(content=content):
                self.process(entries=[self.quarantine_entry()], content=content)
        for log in ("", "[  PASSED  ] 2 tests.\n", "[  PASSED  ] 1 test.\n[  FAILED  ] late\n"):
            with self.subTest(log=log):
                self.process(entries=[self.quarantine_entry()], log=log)

    def test_successful_process_still_requires_results(self):
        self.process(rc=0, success=True)
        self.process(rc=0, content='<testsuite tests="0"/>')

    @unittest.skipUnless(sys.platform == "linux", "Linux signal status contract")
    def test_native_signal_cannot_be_impersonated_by_exit_code(self):
        binary = self.directory / "Gui_tests_run"
        quarantine = self.directory / "quarantine.json"
        quarantine.write_text(json.dumps([self.quarantine_entry()]))
        for ending, success in (("os.kill(os.getpid(), signal.SIGSEGV)", True),
                                ("sys.exit(139)", False)):
            with self.subTest(ending=ending):
                binary.write_text(
                    f"#!{sys.executable}\n"
                    "import os, pathlib, resource, signal, sys\n"
                    "resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n"
                    "report = next(a.split('xml:', 1)[1] for a in sys.argv if 'xml:' in a)\n"
                    "pathlib.Path(report).write_text('<testsuite tests=\"1\"><testcase/></testsuite>')\n"
                    "print('[  PASSED  ] 1 test.', flush=True)\n" + ending + "\n")
                binary.chmod(0o755)
                self.check("gtest", "--binary", binary.name, "--executable", binary,
                           "--filter", "CollaborationResponsiveness*",
                           "--log", self.directory / "native.log", "--quarantine", quarantine,
                           self.directory / "native.xml", success=success)

    def test_executing_process_cannot_reuse_stale_xml(self):
        report = self.xml('<testsuite tests="1"><testcase/></testsuite>')
        binary = self.directory / "App_tests_run"
        binary.write_text(f"#!{sys.executable}\nprint('[  PASSED  ] 1 test.')\n")
        binary.chmod(0o755)
        self.check("gtest", "--binary", binary.name, "--executable", binary,
                   "--log", self.directory / "stale.log", report, message="result.xml")

    @unittest.skipUnless(sys.platform == "linux", "Linux signal status contract")
    def test_crash_before_xml_preserves_status_and_last_test(self):
        binary = self.directory / "Gui_tests_run"
        binary.write_text(
            f"#!{sys.executable}\n"
            "import os, resource, signal\n"
            "resource.setrlimit(resource.RLIMIT_CORE, (0, 0))\n"
            "print('[ RUN      ] DockWidgetTest.example', flush=True)\n"
            "os.kill(os.getpid(), signal.SIGSEGV)\n")
        binary.chmod(0o755)
        result = self.check(
            "gtest", "--binary", binary.name, "--executable", binary,
            "--log", self.directory / "crash.log", self.directory / "missing.xml",
            message="process exit -11")
        self.assertIn("DockWidgetTest.example", result.stderr)
        self.assertIn("missing.xml", result.stderr)

    def summary(self, content, rc=0, success=False, message=None):
        log = self.directory / "integration.log"
        log.write_text(content)
        return self.check("integration", "--exit-code", rc, log,
                          success=success, message=message)

    def test_integration_missing_zero_failed_and_truncated_summary(self):
        for content in ("", "Ran 0 tests in 0.0s\n\nOK\n",
                        "Ran 2 tests in 0.1s\n\nFAILED (failures=1)\n",
                        "Ran 2 tests in 0.1s\n",
                        "Ran 2 tests in 0.1s\n\nOK (skipped=2)\n"):
            with self.subTest(content=content):
                self.summary(content, message="integration")

    def test_integration_exit_status_required(self):
        self.summary("Ran 2 tests in 0.1s\n\nOK\n", rc=139, message="exit 139")

    def test_integration_uses_final_summary(self):
        self.summary("Ran 2 tests in 0.1s\n\nOK\nRan 0 tests in 0.0s\n\nOK\n")

    def test_green_integration(self):
        self.summary("test_x ... ok\nRan 3 tests in 0.125s\n\n"
                     "OK (skipped=1)\nFreeCAD shutdown\n", success=True,
                     message="executed=2 skipped=1 duration=0.125s")


if __name__ == "__main__":
    unittest.main()
