#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for the WSLg display and graphics settings passed to FreeCAD.

Run with:  python -m pytest tests/launcher/test_start_freecad_wslg.py
"""

from __future__ import annotations

import importlib.util
import json
import os
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
    spec = importlib.util.spec_from_file_location(
        "start_freecad_wslg_under_test", LAUNCHER
    )
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


class WslgGraphicsEnvTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = Path(tmp.name)
        self.wslg = self.repo / "wslg-runtime"
        self.wslg.mkdir()
        self.settings = self.repo / launcher.LOCAL_LAUNCHER_SETTINGS
        self.adapter = "NVIDIA GeForce RTX 3070"
        patcher = mock.patch.object(launcher.sys, "platform", "linux")
        patcher.start()
        self.addCleanup(patcher.stop)

    def configure(self, renderer="d3d12", **extra):
        self.settings.write_text(
            json.dumps({"wslg": {"renderer": renderer, **extra}}), encoding="utf-8"
        )

    def graphics_env(self, env=None):
        return launcher._wslg_graphics_env(
            {} if env is None else env,
            settings_path=self.settings,
            wslg_runtime_dir=self.wslg,
        )

    def test_missing_settings_and_auto_leave_driver_selection_alone(self):
        self.assertEqual(self.graphics_env(), {})
        self.configure("auto", adapter=self.adapter)
        self.assertEqual(self.graphics_env(), {})

    def test_d3d12_uses_the_exact_locally_configured_adapter(self):
        self.configure(adapter=self.adapter)
        self.assertEqual(self.graphics_env(), {
            "GALLIUM_DRIVER": "d3d12",
            "MESA_D3D12_DEFAULT_ADAPTER_NAME": self.adapter,
        })

    def test_no_adapter_is_selected_without_a_local_name(self):
        self.configure()
        self.assertEqual(self.graphics_env(), {"GALLIUM_DRIVER": "d3d12"})

    def test_persistent_software_mode_selects_llvmpipe(self):
        self.configure("software", adapter=self.adapter)
        self.assertEqual(self.graphics_env(), {"GALLIUM_DRIVER": "llvmpipe"})

    def test_caller_driver_selection_wins_including_software_and_empty(self):
        for mode in ("d3d12", "software"):
            self.configure(mode, adapter=self.adapter)
            for name, value in (
                ("GALLIUM_DRIVER", "llvmpipe"),
                ("GALLIUM_DRIVER", "d3d12"),
                ("GALLIUM_DRIVER", ""),
                ("MESA_LOADER_DRIVER_OVERRIDE", "zink"),
                ("MESA_LOADER_DRIVER_OVERRIDE", ""),
            ):
                with self.subTest(mode=mode, name=name, value=value):
                    env = {name: value}
                    self.assertEqual(self.graphics_env(env), {})
                    self.assertEqual(env, {name: value})

    def test_caller_adapter_wins(self):
        self.configure(adapter=self.adapter)
        self.assertEqual(
            self.graphics_env({"MESA_D3D12_DEFAULT_ADAPTER_NAME": "another GPU"}),
            {"GALLIUM_DRIVER": "d3d12"},
        )

    def test_libgl_software_optout_selects_llvmpipe_instead_of_forcing_d3d12(self):
        self.configure(adapter=self.adapter)
        for value in ("1", "true", "TRUE", "yes", "on"):
            with self.subTest(value=value):
                self.assertEqual(
                    self.graphics_env({"LIBGL_ALWAYS_SOFTWARE": value}),
                    {"GALLIUM_DRIVER": "llvmpipe"},
                )
        for value in ("0", "false", "no", "off", ""):
            with self.subTest(value=value):
                updates = self.graphics_env({"LIBGL_ALWAYS_SOFTWARE": value})
                self.assertEqual(
                    updates["GALLIUM_DRIVER"], "d3d12",
                )

    def test_explicit_caller_driver_wins_over_conflicting_libgl_flag(self):
        self.configure(adapter=self.adapter)
        self.assertEqual(self.graphics_env({
            "GALLIUM_DRIVER": "d3d12", "LIBGL_ALWAYS_SOFTWARE": "1",
        }), {})

    def test_no_settings_are_read_outside_wslg(self):
        self.settings.write_text("invalid JSON", encoding="utf-8")
        self.assertEqual(launcher._wslg_graphics_env(
            {}, settings_path=self.settings, wslg_runtime_dir=self.wslg / "absent",
        ), {})
        with mock.patch.object(launcher.sys, "platform", "win32"):
            self.assertEqual(self.graphics_env(), {})

    def test_invalid_settings_report_the_file_and_problem(self):
        cases = (
            ("invalid JSON", "invalid WSLg graphics settings"),
            ("[]", "top level"),
            ('{"wslg": []}', "wslg must be"),
            ('{"wslg": {"renderer": "unknown"}}', "wslg.renderer"),
            ('{"wslg": {"adaptor": "GPU"}}', "unknown wslg setting"),
            ('{"wslg": {"adapter": "  "}}', "wslg.adapter"),
            ('{"wslg": {"adapter": null}}', "wslg.adapter"),
            ('{"wslg": {"adapter": 42}}', "wslg.adapter"),
        )
        for content, reason in cases:
            with self.subTest(content=content):
                self.settings.write_text(content, encoding="utf-8")
                with self.assertRaises(SystemExit) as caught:
                    self.graphics_env()
                self.assertIn(str(self.settings), str(caught.exception))
                self.assertIn(reason, str(caught.exception))

    def assert_shared_launch_environment(self, helper, *, normal_launch=False):
        graphics_env = helper._wslg_graphics_env
        with (
            mock.patch.object(helper, "_repo_root", return_value=self.repo),
            mock.patch.object(helper, "_runtime_path_entries", return_value=[]),
            mock.patch.object(helper, "_pixi_env_dir", return_value=None),
            mock.patch.object(helper, "_pixi_for_freecad", return_value=None),
            mock.patch.object(helper, "_wslg_display_env", return_value={}),
            mock.patch.object(
                helper, "_wslg_graphics_env", side_effect=lambda env:
                graphics_env(env, wslg_runtime_dir=self.wslg),
            ),
            mock.patch.dict(
                os.environ, {"UNCHANGED_PARENT_VALUE": "kept"}, clear=True,
            ),
        ):
            before = dict(os.environ)
            executable = self.repo / "build/release/bin/FreeCAD"
            if normal_launch:
                argv, _, env = helper._launch_details(executable, ["model.FCStd"])
                self.assertEqual(argv, [str(executable), "model.FCStd"])
            else:
                env = helper._launch_env(executable)
            self.assertEqual(env["GALLIUM_DRIVER"], "d3d12")
            self.assertEqual(env["MESA_D3D12_DEFAULT_ADAPTER_NAME"], self.adapter)
            self.assertEqual(env["UNCHANGED_PARENT_VALUE"], "kept")
            self.assertEqual(dict(os.environ), before)

    def test_normal_launch_uses_the_shared_environment_without_changing_parent(self):
        self.configure(adapter=self.adapter)
        self.assert_shared_launch_environment(launcher, normal_launch=True)

    def test_authenticated_isolated_launch_loads_the_same_shared_helper(self):
        self.configure(adapter=self.adapter)
        path = REPO / "tools/mcp/freecad-mcp/scripts/start_freecad_isolated.py"
        spec = importlib.util.spec_from_file_location("isolated_wslg_under_test", path)
        isolated = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        with mock.patch.object(sys, "path", list(sys.path)):
            spec.loader.exec_module(isolated)
            with mock.patch.object(isolated, "_repo_root", return_value=REPO):
                helper = isolated._load_parent_start_freecad()
        self.assertEqual(Path(helper.__file__).resolve(), LAUNCHER.resolve())
        self.assert_shared_launch_environment(helper)


if __name__ == "__main__":
    sys.exit(unittest.main())
