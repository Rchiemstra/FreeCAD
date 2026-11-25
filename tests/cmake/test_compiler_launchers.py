#!/usr/bin/env python3
"""Check FreeCAD's compiler-launcher setup with real CMake/Ninja generation.

Run directly with Python; cmake, ninja, and C/C++ compilers must be on PATH.
The small project uses the actual root setup through project() and the launcher
block, but does not require FreeCAD's third-party dependencies.
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


class CompilerLaunchers(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="freecad launchers ")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / "source"
        self.src.mkdir()
        # Retain the root's order, including project() and its toolchain and
        # environment initialization, without configuring the rest of FreeCAD.
        root_cmake = (ROOT / "CMakeLists.txt").read_text()
        prefix, marker, _ = root_cmake.partition("# include local modules")
        self.assertTrue(marker, "root setup boundary changed")
        (self.src / "CMakeLists.txt").write_text(
            prefix + '\nadd_library(probe OBJECT probe.c probe.cpp)\n'
        )
        (self.src / "version.json").write_text(json.dumps({
            "name": "LauncherProbe", "version_major": 1, "version_minor": 0,
            "version_patch": 0, "version_suffix": "", "build_version": 0,
        }))
        (self.src / "probe.c").write_text("int c_probe(void) { return 1; }\n")
        (self.src / "probe.cpp").write_text("int cxx_probe() { return 2; }\n")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.ccache = self.bin / "ccache"
        self.ccache.write_text("#!/bin/sh\nexec \"$@\"\n")
        self.ccache.chmod(0o755)
        self.custom = self.root / "custom launcher"
        self.custom.write_text("#!/bin/sh\nshift\nexec \"$@\"\n")
        self.custom.chmod(0o755)
        self.env = os.environ.copy()
        for lang in ("C", "CXX"):
            self.env.pop(f"CMAKE_{lang}_COMPILER_LAUNCHER", None)
        self.env["PATH"] = str(self.bin) + os.pathsep + self.env["PATH"]
        self.ninja = shutil.which("ninja", path=self.env["PATH"])
        self.assertIsNotNone(self.ninja, "ninja is required")

    def configure(self, name, args=(), env=None, preset=False):
        build = self.root / name
        if preset:
            command = ["cmake", "--preset", name, "-DCMAKE_PROGRAM_PATH=" + str(self.bin)]
            cwd = self.src
        else:
            command = ["cmake", "-S", str(self.src), "-B", str(build),
                       "-G", "Ninja", "-DCMAKE_PROGRAM_PATH=" + str(self.bin), *args]
            cwd = None
        result = subprocess.run(command, env=env or self.env, cwd=cwd,
                                text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([self.ninja, "-C", str(build), "-t", "commands"],
                                text=True, capture_output=True, check=True)
        commands = {}
        print(f"\n{name} generated Ninja commands:", flush=True)
        for line in result.stdout.splitlines():
            tokens = shlex.split(line)
            for lang, source in (("C", "probe.c"), ("CXX", "probe.cpp")):
                if any(Path(token).name == source for token in tokens):
                    self.assertNotIn(lang, commands)
                    commands[lang] = tokens
                    print(f"  {lang}: {line}", flush=True)
        self.assertEqual(set(commands), {"C", "CXX"}, result.stdout)
        return commands

    def assert_launchers(self, commands, c, cxx):
        for lang, expected in (("C", c), ("CXX", cxx)):
            with self.subTest(lang=lang):
                tokens = commands[lang]
                self.assertEqual(tokens[:len(expected)], expected, tokens)
                compiler = Path(tokens[len(expected)]).name
                self.assertIn(compiler, ("cc", "gcc", "clang") if lang == "C"
                              else ("c++", "g++", "clang++"), tokens)

    def test_command_line_defaults_and_disabled(self):
        auto = [str(self.ccache)]
        custom = [str(self.custom), "--launcher-argument"]
        value = ";".join(custom)
        cases = (
            ("default", [], auto, auto),
            ("c_only", [f"-DCMAKE_C_COMPILER_LAUNCHER={value}"], custom, auto),
            ("cxx_only", [f"-DCMAKE_CXX_COMPILER_LAUNCHER={value}"], auto, custom),
            ("both", [f"-DCMAKE_C_COMPILER_LAUNCHER={value}",
                      "-DCMAKE_CXX_COMPILER_LAUNCHER="], custom, []),
            ("empty_c", ["-DCMAKE_C_COMPILER_LAUNCHER="], [], auto),
            ("off", ["-DFREECAD_USE_CCACHE=OFF"], [], []),
            ("off_explicit", ["-DFREECAD_USE_CCACHE=OFF",
                              f"-DCMAKE_CXX_COMPILER_LAUNCHER={value}"], [], custom),
        )
        for name, args, c, cxx in cases:
            with self.subTest(case=name):
                self.assert_launchers(self.configure(name, args), c, cxx)

    def test_environment_and_cache_precedence(self):
        custom = [str(self.custom), "--env-arg"]
        env = dict(self.env, CMAKE_C_COMPILER_LAUNCHER=";".join(custom),
                   CMAKE_CXX_COMPILER_LAUNCHER=";".join(custom))
        self.assert_launchers(self.configure("environment", env=env), custom, custom)
        # Explicit cache variables override environment initialization, even empty.
        commands = self.configure("environment_cache", ["-DCMAKE_C_COMPILER_LAUNCHER="], env)
        self.assert_launchers(commands, [], custom)

    def test_toolchain_and_preset(self):
        custom = [str(self.custom), "--toolchain-arg"]
        toolchain = self.root / "toolchain.cmake"
        toolchain.write_text(
            f'set(CMAKE_C_COMPILER_LAUNCHER "{";".join(custom)}")\n'
            'set(CMAKE_CXX_COMPILER_LAUNCHER "")\n'
        )
        self.assert_launchers(self.configure("toolchain", [f"-DCMAKE_TOOLCHAIN_FILE={toolchain}"]),
                              custom, [])
        preset_custom = [str(self.custom), "--preset-arg"]
        (self.src / "CMakePresets.json").write_text(json.dumps({
            "version": 3, "configurePresets": [{"name": "preset", "generator": "Ninja",
                "binaryDir": str(self.root / "preset"), "cacheVariables": {
                    "CMAKE_C_COMPILER_LAUNCHER": "",
                    "CMAKE_CXX_COMPILER_LAUNCHER": ";".join(preset_custom),
                }}],
        }))
        self.assert_launchers(self.configure("preset", preset=True), [], preset_custom)

    def test_absent_ccache_and_reconfigure(self):
        # Restrict find_program search without assuming where a host ccache lives.
        # Supply build tools explicitly because these switches affect every lookup.
        no_cache = self.configure("not_found", [
            "-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=FALSE",
            "-DCMAKE_FIND_USE_CMAKE_SYSTEM_PATH=FALSE",
            "-DCMAKE_FIND_USE_CMAKE_PATH=FALSE",
            "-DCMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH=FALSE",
            "-DCMAKE_MAKE_PROGRAM=" + self.ninja,
            "-DCMAKE_C_COMPILER=" + shutil.which("cc", path=self.env["PATH"]),
            "-DCMAKE_CXX_COMPILER=" + shutil.which("c++", path=self.env["PATH"]),
        ])
        self.assert_launchers(no_cache, [], [])
        self.assert_launchers(self.configure("again"), [str(self.ccache)], [str(self.ccache)])
        custom = [str(self.custom), "--reconfigured"]
        self.assert_launchers(self.configure("again", [f'-DCMAKE_C_COMPILER_LAUNCHER={";".join(custom)}',
                                                       "-DFREECAD_USE_CCACHE=OFF"]), custom, [])


if __name__ == "__main__":
    unittest.main()
