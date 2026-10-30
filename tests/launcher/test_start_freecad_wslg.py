#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for the WSLg display settings the launcher passes to FreeCAD.

Run with:  python -m pytest tests/launcher/test_start_freecad_wslg.py
"""

from __future__ import annotations

import importlib.util
import socket
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
LAUNCHER = REPO / "tools" / "launcher" / "start_freecad_impl.py"


def load_launcher():
    if not LAUNCHER.is_file():
        raise unittest.SkipTest(f"launcher not found at {LAUNCHER}")
    spec = importlib.util.spec_from_file_location("start_freecad_wslg_under_test", LAUNCHER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


launcher = load_launcher()


@unittest.skipUnless(hasattr(socket, "AF_UNIX"), "needs Unix domain sockets")
class WslgDisplayEnvTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.wslg = root / "wslg-runtime"
        self.runtime = root / "xdg-runtime"
        self.wslg.mkdir()
        self.runtime.mkdir()
        self.wslg_socket = self.wslg / "wayland-0"
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(sock.close)
        sock.bind(str(self.wslg_socket))

        patcher = mock.patch.object(launcher.sys, "platform", "linux")
        patcher.start()
        self.addCleanup(patcher.stop)

    def display_env(self, env):
        return launcher._wslg_display_env(env, wslg_runtime_dir=self.wslg)

    def test_defaults_to_xcb_with_standard_cursor(self):
        updates = self.display_env({"DISPLAY": ":0"})
        self.assertEqual(updates["QT_QPA_PLATFORM"], "xcb")
        self.assertEqual(updates["XCURSOR_SIZE"], launcher.DEFAULT_CURSOR_SIZE)

    def test_no_x_display_does_not_force_xcb(self):
        updates = self.display_env({"WAYLAND_DISPLAY": "wayland-0"})
        self.assertNotIn("QT_QPA_PLATFORM", updates)

    def test_hidden_socket_is_redirected_to_wslg(self):
        updates = self.display_env(
            {"WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": str(self.runtime)}
        )
        self.assertEqual(updates["WAYLAND_DISPLAY"], str(self.wslg_socket))

    def test_visible_socket_is_left_alone(self):
        (self.runtime / "wayland-0").touch()
        updates = self.display_env(
            {"WAYLAND_DISPLAY": "wayland-0", "XDG_RUNTIME_DIR": str(self.runtime)}
        )
        self.assertNotIn("WAYLAND_DISPLAY", updates)

    def test_user_settings_are_respected(self):
        updates = self.display_env(
            {
                "DISPLAY": ":0",
                "WAYLAND_DISPLAY": "wayland-0",
                "XDG_RUNTIME_DIR": str(self.runtime),
                "QT_QPA_PLATFORM": "wayland",
                "XCURSOR_SIZE": "32",
            }
        )
        self.assertEqual(updates, {"WAYLAND_DISPLAY": str(self.wslg_socket)})

    def test_absolute_wayland_display_is_left_alone(self):
        updates = self.display_env({"WAYLAND_DISPLAY": "/elsewhere/wayland-1"})
        self.assertNotIn("WAYLAND_DISPLAY", updates)

    def test_missing_wslg_socket_keeps_wayland_display(self):
        updates = self.display_env(
            {"WAYLAND_DISPLAY": "wayland-9", "XDG_RUNTIME_DIR": str(self.runtime)}
        )
        self.assertNotIn("WAYLAND_DISPLAY", updates)

    def test_no_changes_outside_wslg(self):
        env = {
            "DISPLAY": ":0",
            "WAYLAND_DISPLAY": "wayland-0",
            "XDG_RUNTIME_DIR": str(self.runtime),
        }
        self.assertEqual(
            launcher._wslg_display_env(env, wslg_runtime_dir=self.wslg / "absent"), {}
        )
        with mock.patch.object(launcher.sys, "platform", "win32"):
            self.assertEqual(self.display_env(env), {})


if __name__ == "__main__":
    sys.exit(unittest.main())
