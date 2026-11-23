#!/usr/bin/env python3
"""Exercise the compile-database adapter and its fail-closed CLI without FreeCAD."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("check-clang-production.py")


class ProductionClangCheck(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="clang check ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "src space" / "DocumentPyImp.cpp"
        self.source.parent.mkdir()
        self.source.write_text("// stand-in source for command routing only\n")
        self.log = self.root / "invocation.json"
        self.compiler = self.root / "fake clang++"
        self.compiler.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, sys\n"
            "with open(os.environ['FAKE_LOG'], 'w') as f: json.dump([os.getcwd(), sys.argv[1:], sys.argv[0]], f)\n"
            "sys.exit(int(os.environ.get('FAKE_STATUS', '0')))\n"
        )
        self.compiler.chmod(0o755)
        self.db = self.root / "compile_commands.json"
        self.args = ["/usr/bin/g++", "-DSPACE=value with spaces", "-I", "include dir", "-std=c++23", "-o", "out file.o", "-c", str(self.source)]
        self.entry = {"directory": str(self.root), "file": str(self.source), "command": shlex.join(self.args)}

    def run_check(self, entries, *, status="0", compiler=None, cwd=None):
        self.db.write_text(json.dumps(entries))
        env = dict(os.environ, FAKE_LOG=str(self.log), FAKE_STATUS=status)
        return subprocess.run([sys.executable, str(SCRIPT), "--database", str(self.db),
                               "--source", str(self.source), "--compiler", str(compiler or self.compiler)],
                              env=env, cwd=cwd, text=True, capture_output=True, check=False)

    def test_real_entry_keeps_argument_boundaries_and_directory(self):
        result = self.run_check([self.entry])
        self.assertEqual(result.returncode, 0, result.stderr)
        cwd, args, _invoked = json.loads(self.log.read_text())
        self.assertEqual(cwd, str(self.root))
        self.assertEqual(args, ["-DSPACE=value with spaces", "-I", "include dir", "-std=c++23", str(self.source), "-fsyntax-only"])
        self.assertFalse((self.root / "out file.o").exists())

    def test_argument_array_and_relative_source_are_supported(self):
        entry = {"directory": str(self.root), "file": "src space/DocumentPyImp.cpp", "arguments": ["ccache", *self.args]}
        result = self.run_check([entry])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(self.source), json.loads(self.log.read_text())[1])

    def test_compiler_failure_is_propagated(self):
        result = self.run_check([self.entry], status="42")
        self.assertEqual(result.returncode, 42)
        self.assertTrue(self.log.exists())

    def test_missing_compiler_fails_without_running(self):
        result = self.run_check([self.entry], compiler=self.root / "not-installed")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required Clang compiler not found", result.stderr)
        self.assertFalse(self.log.exists())

    def test_missing_or_duplicate_source_fails_closed(self):
        for entries in ([], [self.entry, self.entry]):
            with self.subTest(entries=len(entries)):
                result = self.run_check(entries)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("expected one production compile command", result.stderr)
                self.assertFalse(self.log.exists())

    def test_malformed_compile_command_fails_closed(self):
        for args in (self.args[:-1], ["/usr/bin/g++", "-c", str(self.source)], ["/usr/bin/g++", "-o"], ["/usr/bin/false", "-o", "x", "-c", str(self.source)]):
            with self.subTest(args=args):
                entry = dict(self.entry, arguments=args)
                entry.pop("command")
                result = self.run_check([entry])
                # A missing source argument cannot be allowed even if the DB's file field matches.
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.log.exists())

    def test_relative_compiler_path_is_resolved_before_changing_directory(self):
        caller = self.root / "calling directory"
        caller.mkdir()
        (caller / "clang++").symlink_to(self.compiler)
        result = self.run_check([self.entry], compiler="./clang++", cwd=caller)
        self.assertEqual(result.returncode, 0, result.stderr)
        cwd, _, invoked = json.loads(self.log.read_text())
        self.assertEqual(cwd, str(self.root))
        self.assertEqual(invoked, str(caller / "clang++"))

    def test_source_as_only_output_or_include_is_not_the_compile_input(self):
        clang = shutil.which(os.environ.get("CLANG_TEST", "clang++"))
        self.assertIsNotNone(clang, "Clang is required for these production-check tests")
        self.source.write_text("this is invalid C++\n")
        other = self.root / "other.cpp"
        other.write_text("int valid() { return 0; }\n")
        for prefix, output in (([], str(self.source)), (["-I", str(self.source)], "other.o")):
            with self.subTest(prefix=prefix):
                args = ["/usr/bin/g++", *prefix, "-o", output, "-c", str(other)]
                entry = dict(self.entry, arguments=args)
                entry.pop("command")
                result = self.run_check([entry], compiler=clang)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_clang_action_and_side_effect_flags_are_rejected(self):
        clang = shutil.which(os.environ.get("CLANG_TEST", "clang++"))
        self.assertIsNotNone(clang, "Clang is required for these production-check tests")
        self.source.write_text("this is invalid C++\n")
        cases = (["-E"], ["-M"], ["-MM"], ["-MD"], ["-MMD"],
                 ["-MF", "dependencies.d"], ["-MT", "target"], ["-MQ", "target"],
                 ["-MJ", "compilation.json"], ["-S"], ["--analyze"],
                 ["-save-temps"], ["-Xclang", "-E"], ["-Wp,-M"],
                 ["-oother.o"], ["@flags.rsp"], ["-fdriver-only"],
                 ["-xassembler"], ["-dumpversion"], ["--help-hidden"],
                 ["-dependency-file", "dependencies.d"])
        for action in cases:
            with self.subTest(action=action):
                args = [self.args[0], *action, *self.args[1:]]
                entry = dict(self.entry, arguments=args)
                entry.pop("command")
                result = self.run_check([entry], compiler=clang)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("unsupported", result.stderr)
                self.assertFalse((self.root / "dependencies.d").exists())
                self.assertFalse((self.root / "compilation.json").exists())

    def test_response_file_in_option_operand_is_rejected(self):
        clang = shutil.which(os.environ.get("CLANG_TEST", "clang++"))
        self.assertIsNotNone(clang, "Clang is required for these production-check tests")
        self.source.write_text("this is invalid C++\n")
        (self.root / "flags.rsp").write_text(". -E\n")
        entry = dict(self.entry, arguments=[self.args[0], "-I", "@flags.rsp", *self.args[1:]])
        entry.pop("command")
        result = self.run_check([entry], compiler=clang)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("unsupported", result.stderr)

    def test_joined_at_sign_is_a_literal_option_value(self):
        args = [self.args[0], "-I@literal", "-DVALUE=@literal", *self.args[1:]]
        entry = dict(self.entry, arguments=args)
        entry.pop("command")
        result = self.run_check([entry])
        self.assertEqual(result.returncode, 0, result.stderr)
        forwarded = json.loads(self.log.read_text())[1]
        self.assertIn("-I@literal", forwarded)
        self.assertIn("-DVALUE=@literal", forwarded)


if __name__ == "__main__":
    unittest.main()
