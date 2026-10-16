#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for Pixi bootstrapping in the local FreeCAD build wrapper."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = REPO / "build_freecad.py"


def load_build_script():
    spec = importlib.util.spec_from_file_location("build_freecad_under_test", BUILD_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


builder = load_build_script()


class PixiBootstrapTests(unittest.TestCase):
    def test_reads_required_pixi_version_from_manifest(self):
        self.assertEqual(builder.required_pixi_version(REPO), "0.81.0")

    def test_outdated_pixi_is_updated_to_required_version(self):
        calls = []
        originals = (
            builder.required_pixi_version,
            builder.installed_pixi_version,
            builder.run,
        )
        try:
            builder.required_pixi_version = lambda repo: "0.81.0"
            builder.installed_pixi_version = lambda pixi: "0.79.0"
            builder.run = lambda command, **kwargs: calls.append((command, kwargs))

            builder.ensure_compatible_pixi("/usr/bin/pixi", REPO, False)
        finally:
            (
                builder.required_pixi_version,
                builder.installed_pixi_version,
                builder.run,
            ) = originals

        self.assertEqual(
            calls[0][0],
            ["/usr/bin/pixi", "self-update", "--version", "0.81.0"],
        )

    def test_compatible_pixi_is_not_updated(self):
        calls = []
        originals = (
            builder.required_pixi_version,
            builder.installed_pixi_version,
            builder.run,
        )
        try:
            builder.required_pixi_version = lambda repo: "0.81.0"
            builder.installed_pixi_version = lambda pixi: "0.82.1"
            builder.run = lambda *args, **kwargs: calls.append(args)

            builder.ensure_compatible_pixi("/usr/bin/pixi", REPO, False)
        finally:
            (
                builder.required_pixi_version,
                builder.installed_pixi_version,
                builder.run,
            ) = originals

        self.assertEqual(calls, [])

    def test_command_can_retry_once_after_a_transient_failure(self):
        return_codes = iter((1, 0))
        calls = []
        original_run = builder.subprocess.run
        try:
            builder.subprocess.run = lambda command, **kwargs: calls.append(command) or type(
                "Result", (), {"returncode": next(return_codes)}
            )()
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(
                io.StringIO()
            ):
                builder.run(
                    ["pixi", "install"],
                    cwd=REPO,
                    env=None,
                    dry_run=False,
                    retries=1,
                )
        finally:
            builder.subprocess.run = original_run

        self.assertEqual(calls, [["pixi", "install"], ["pixi", "install"]])


class ConfigureTests(unittest.TestCase):
    def test_pixi_build_uses_platform_specific_conda_preset(self):
        expected = f"conda-{builder.host_platform_name()}-release"
        self.assertEqual(builder.configure_preset("release", True), expected)

    def test_system_build_uses_plain_preset_like_ci(self):
        self.assertEqual(builder.configure_preset("debug", False), "debug")

    def test_changed_pixi_lock_invalidates_configure_fingerprint(self):
        import tempfile

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "pixi.toml").write_text("[workspace]\n", encoding="utf-8")
            lock = root / "pixi.lock"
            lock.write_text("old\n", encoding="utf-8")
            before = builder.configure_fingerprint(
                root, preset="conda-linux-release", cmake_args=[], use_pixi=True
            )

            lock.write_text("new\n", encoding="utf-8")
            after = builder.configure_fingerprint(
                root, preset="conda-linux-release", cmake_args=[], use_pixi=True
            )

        self.assertNotEqual(before, after)


if __name__ == "__main__":
    unittest.main(verbosity=2)
