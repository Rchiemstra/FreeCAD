#!/usr/bin/env python3
"""Exercise the real bundled E57 targets and FreeCAD's launcher setup with Ninja.

Requires CMake, Ninja, C/C++ compilers and Xerces-C. The optional E57 test's
missing GoogleTest submodule is replaced with a stub target; no code is built.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class E57Launchers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="freecad e57 launchers ")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.src = self.dir / "source"
        third = self.src / "src/3rdParty"
        third.mkdir(parents=True)
        shutil.copy(ROOT / "src/3rdParty/CMakeLists.txt", third)
        shutil.copytree(ROOT / "src/3rdParty/libE57Format", third / "libE57Format")
        for name in ("FastSignals", "lazy_loader"):
            (third / name).mkdir()
            (third / name / "CMakeLists.txt").write_text("# unrelated target stub\n")
        googletest = third / "libE57Format/test/extern/googletest"
        googletest.mkdir(parents=True, exist_ok=True)
        (googletest / "CMakeLists.txt").write_text(
            "project(gtest LANGUAGES CXX)\nadd_library(gtest_main STATIC stub.cpp)\n")
        (googletest / "stub.cpp").write_text("int gtest_stub() { return 0; }\n")
        (googletest / "include").mkdir(exist_ok=True)
        prefix, marker, _ = (ROOT / "CMakeLists.txt").read_text().partition("# include local modules")
        self.assertTrue(marker, "root setup boundary changed")
        (self.src / "CMakeLists.txt").write_text(
            prefix + '\nadd_library(probe OBJECT probe.c probe.cpp)\nadd_subdirectory(src/3rdParty)\n')
        (self.src / "version.json").write_text(json.dumps({
            "name": "LauncherProbe", "version_major": 1, "version_minor": 0,
            "version_patch": 0, "version_suffix": "", "build_version": 0}))
        (self.src / "probe.c").write_text("int c_probe(void) { return 1; }\n")
        (self.src / "probe.cpp").write_text("int cxx_probe() { return 2; }\n")
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        self.bin = bin_dir
        self.ccache = bin_dir / "ccache"
        self.ccache.write_text('#!/bin/sh\nexec "$@"\n')
        self.ccache.chmod(0o755)
        self.custom = self.dir / "custom launcher"
        self.custom.write_text('#!/bin/sh\nshift\nexec "$@"\n')
        self.custom.chmod(0o755)
        self.env = os.environ.copy()
        for lang in ("C", "CXX"):
            self.env.pop(f"CMAKE_{lang}_COMPILER_LAUNCHER", None)
        self.env["PATH"] = str(bin_dir) + os.pathsep + self.env["PATH"]
        self.ninja = shutil.which("ninja", path=self.env["PATH"])
        self.assertIsNotNone(self.ninja, "ninja is required")

    def configure(self, name, args=(), standalone=False):
        build = self.dir / name
        source = self.src / "src/3rdParty/libE57Format" if standalone else self.src
        cmd = ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
               "-DE57_BUILD_TEST=ON", "-DE57_RELEASE_LTO=OFF",
               "-DCMAKE_PROGRAM_PATH=" + str(self.bin), *args]
        result = subprocess.run(cmd, env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = subprocess.run([self.ninja, "-C", str(build), "-t", "commands"],
                                text=True, capture_output=True, check=True).stdout
        commands = {}
        for line in output.splitlines():
            tokens = shlex.split(line)
            for label, target in (("library", "E57Format.dir/"), ("test", "testE57.dir/"),
                                  ("c", "probe.dir/probe.c.o"), ("cxx", "probe.dir/probe.cpp.o")):
                if label not in commands and any(target in token for token in tokens) and "-c" in tokens:
                    commands[label] = tokens
                    print(f"{name} {label}: {line}", flush=True)
        self.assertEqual(set(commands), {"library", "test"} if standalone else
                         {"library", "test", "c", "cxx"}, output)
        return commands

    def check_prefix(self, commands, expected):
        for name, prefix in expected.items():
            with self.subTest(target=name):
                tokens = commands[name]
                self.assertEqual(tokens[:len(prefix)], prefix, tokens)
                self.assertIn(Path(tokens[len(prefix)]).name,
                              ("cc", "gcc", "clang", "c++", "g++", "clang++"), tokens)

    def test_bundled(self):
        auto = [str(self.ccache)]
        custom = [str(self.custom), "--launcher-argument"]
        value = ";".join(custom)
        cases = (
            ("default", [], auto, auto),
            ("c_only", [f"-DCMAKE_C_COMPILER_LAUNCHER={value}"], custom, auto),
            ("cxx_only", [f"-DCMAKE_CXX_COMPILER_LAUNCHER={value}"], auto, custom),
            ("empty_cxx", ["-DCMAKE_CXX_COMPILER_LAUNCHER="], auto, []),
            ("empty_c", ["-DCMAKE_C_COMPILER_LAUNCHER="], [], auto),
            ("off", ["-DFREECAD_USE_CCACHE=OFF"], [], []),
            ("off_explicit", ["-DFREECAD_USE_CCACHE=OFF",
                              f"-DCMAKE_CXX_COMPILER_LAUNCHER={value}"], [], custom),
        )
        for name, args, c, cxx in cases:
            with self.subTest(case=name):
                self.check_prefix(self.configure(name, args),
                                  {"c": c, "cxx": cxx, "library": cxx, "test": cxx})

    def test_standalone_defaults(self):
        commands = self.configure("standalone", standalone=True)
        self.check_prefix(commands, {"library": [str(self.ccache)], "test": [str(self.ccache)]})


if __name__ == "__main__":
    unittest.main()
