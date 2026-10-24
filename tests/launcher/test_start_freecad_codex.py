"""Codex launcher setup, ordering, and failure propagation."""

from __future__ import annotations

import contextlib
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "start_freecad_codex_under_test", REPO / "tools/launcher/start_freecad_impl.py"
)
launcher = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(launcher)


class CodexLauncherTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.repo = Path(self.stack.enter_context(tempfile.TemporaryDirectory(prefix="FreeCAD source ")))
        self.mcp = self.repo / "tools/mcp/freecad-mcp"
        self.python = self.mcp / ".venv" / (
            "Scripts/python.exe" if os.name == "nt" else "bin/python"
        )
        self.python.parent.mkdir(parents=True)
        self.python.touch()
        self.stack.enter_context(mock.patch.object(launcher, "_repo_root", return_value=self.repo))
        self.stack.enter_context(mock.patch.object(launcher, "_freecad_mcp_dir", return_value=self.mcp))
        self.stack.enter_context(mock.patch.object(launcher, "_default_freecad", return_value=sys.executable))
        self.stack.enter_context(mock.patch.object(launcher, "_wslg_display_env", return_value={}))
        self.which = self.stack.enter_context(
            mock.patch.object(launcher.shutil, "which", side_effect=lambda name: f"/tools/{name}")
        )
        self.run = self.stack.enter_context(
            mock.patch.object(launcher.subprocess, "run", return_value=SimpleNamespace(returncode=0))
        )
        self.stack.enter_context(mock.patch.dict(os.environ))
        os.environ.pop("FREECAD_MCP_PROFILE_DIR", None)

    def test_sync_launch_then_register_exact_authenticated_command(self):
        self.assertEqual(launcher.main(["--codex", "model with spaces.FCStd"]), 0)
        sync, setup, launch, register = self.run.call_args_list
        self.assertEqual(
            sync.args[0],
            ["/tools/uv", "sync", "--locked", "--inexact", "--directory", str(self.mcp)],
        )
        self.assertEqual(setup.args[0], [str(self.python), str(self.mcp / "scripts/setup_isolated_profile.py")])
        self.assertEqual(launch.args[0][-1], "model with spaces.FCStd")
        self.assertEqual(launch.args[0][0], str(self.python))
        self.assertEqual(
            launch.kwargs["env"]["FREECAD_MCP_ISOLATED_FREECAD"], str(Path(sys.executable).resolve())
        )
        self.assertEqual(
            register.args[0],
            [
                "/tools/codex", "mcp", "add", "freecad",
                "--env", "FREECAD_MCP_CLIENT=codex",
                "--", str(self.python), "-m", "freecad_mcp.server",
                "--instance-manifest", str(self.repo / ".freecad-mcp-isolated/instance-manifest.json"),
            ],
        )

    def test_registration_honors_profile_override(self):
        profile = self.repo / "custom profile"
        os.environ["FREECAD_MCP_PROFILE_DIR"] = str(profile)
        self.assertEqual(launcher.main(["--codex"]), 0)
        self.assertEqual(self.run.call_args.args[0][-1], str(profile / "instance-manifest.json"))

    def test_missing_codex_stops_before_sync_or_launch(self):
        self.which.side_effect = lambda name: None if name == "codex" else "/tools/uv"
        self.assertEqual(launcher.main(["--codex"]), 1)
        self.run.assert_not_called()

    def test_missing_freecad_stops_before_sync(self):
        with mock.patch.object(launcher, "_default_freecad", return_value=None):
            self.assertEqual(launcher.main(["--codex"]), 1)
        self.run.assert_not_called()

    def test_failures_stop_later_steps(self):
        for failed_step in range(4):
            with self.subTest(failed_step=failed_step):
                self.run.reset_mock()
                self.run.side_effect = [SimpleNamespace(returncode=0)] * failed_step + [
                    SimpleNamespace(returncode=7)
                ]
                self.assertEqual(launcher.main(["--codex"]), 7)
                self.assertEqual(self.run.call_count, failed_step + 1)

    def test_codex_rejects_legacy_lifecycle_flags_before_setup(self):
        for flags in (["--wait"], ["--force-new"], ["--no-wait-for-mcp"], ["--mcp-timeout", "1"]):
            with self.subTest(flags=flags):
                self.assertEqual(launcher.main(["--codex", *flags]), 2)
        self.run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
