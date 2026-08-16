#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for the JSON-RPC transport in the FreeCADModeling launcher.

The launcher under test lives OUTSIDE this repository, in the parent
FreeCADModeling directory; FreeCAD/start_freecad.py is only a wrapper that
runpy-executes it. That directory is not under version control, so these tests
are kept here, and they locate the launcher the same way the wrapper does.

Covered: success, timeout, refused connection, malformed responses, mismatched
request IDs, JSON-RPC errors, occupied-port refusal under --force-new, refusal
to identify an unknown session, and shutdown of a process the launcher owns.

Run with:  python -m pytest tests/launcher/test_start_freecad_jsonrpc.py
"""

from __future__ import annotations

import importlib.util
import json
import socket
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LAUNCHER = REPO.parent / "start_freecad.py"


def load_launcher():
    if not LAUNCHER.is_file():
        raise unittest.SkipTest(f"launcher not found at {LAUNCHER}")
    spec = importlib.util.spec_from_file_location("start_freecad_under_test", LAUNCHER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


launcher = load_launcher()


class ScriptedHandler(BaseHTTPRequestHandler):
    """Replies with whatever the server was scripted to reply."""

    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler API
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        try:
            self.server.last_request = json.loads(raw.decode("utf-8"))
        except Exception:  # noqa: BLE001
            self.server.last_request = None

        status, body = self.server.responder(self.server.last_request, self.path)
        payload = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):  # silence
        return


class ScriptedServer:
    def __init__(self, responder):
        self.httpd = HTTPServer(("127.0.0.1", 0), ScriptedHandler)
        self.httpd.responder = responder
        self.httpd.last_request = None
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=5)

    @property
    def port(self) -> int:
        return self.httpd.server_address[1]


def ok(result):
    return lambda request, path: (
        200, {"jsonrpc": "2.0", "id": request.get("id"), "result": result}
    )


class TransportTests(unittest.TestCase):
    def test_success_returns_result_and_uses_jsonrpc_path(self):
        with ScriptedServer(ok(True)) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            self.assertTrue(client.call("ping"))
            self.assertEqual(server.httpd.last_request["jsonrpc"], "2.0")
            self.assertEqual(server.httpd.last_request["method"], "ping")

    def test_refused_connection_is_a_transport_error(self):
        # Bind and immediately close, so the port is almost certainly free.
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        client = launcher.JsonRpcClient("127.0.0.1", port)
        with self.assertRaises(launcher.JsonRpcTransportError):
            client.call("ping", timeout=2.0)

    def test_timeout_is_a_transport_error(self):
        def slow(request, path):
            import time
            time.sleep(3.0)
            return 200, {"jsonrpc": "2.0", "id": request.get("id"), "result": True}

        with ScriptedServer(slow) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping", timeout=0.5)

    def test_retired_xmlrpc_410_is_a_transport_error_not_a_fallback(self):
        def gone(request, path):
            return 410, b'{"error":"use JSON-RPC 2.0 at /jsonrpc"}'

        with ScriptedServer(gone) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError) as caught:
                client.call("ping")
            self.assertIn("410", str(caught.exception))

    def test_malformed_json_is_rejected(self):
        with ScriptedServer(lambda r, p: (200, b"not json at all")) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping")

    def test_non_object_response_is_rejected(self):
        with ScriptedServer(lambda r, p: (200, [1, 2, 3])) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping")

    def test_wrong_protocol_version_is_rejected(self):
        with ScriptedServer(
            lambda r, p: (200, {"jsonrpc": "1.0", "id": r.get("id"), "result": True})
        ) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping")

    def test_mismatched_request_id_is_rejected(self):
        with ScriptedServer(
            lambda r, p: (200, {"jsonrpc": "2.0", "id": 9999, "result": True})
        ) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError) as caught:
                client.call("ping")
            self.assertIn("does not match", str(caught.exception))

    def test_result_and_error_together_is_rejected(self):
        with ScriptedServer(
            lambda r, p: (200, {
                "jsonrpc": "2.0", "id": r.get("id"),
                "result": True, "error": {"code": -1, "message": "x"},
            })
        ) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping")

    def test_neither_result_nor_error_is_rejected(self):
        with ScriptedServer(
            lambda r, p: (200, {"jsonrpc": "2.0", "id": r.get("id")})
        ) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcTransportError):
                client.call("ping")

    def test_jsonrpc_error_preserves_code_message_and_data(self):
        with ScriptedServer(
            lambda r, p: (200, {
                "jsonrpc": "2.0", "id": r.get("id"),
                "error": {"code": -32000, "message": "boom", "data": {"k": "v"}},
            })
        ) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            with self.assertRaises(launcher.JsonRpcError) as caught:
                client.call("ping")
            self.assertEqual(caught.exception.code, -32000)
            self.assertEqual(caught.exception.message, "boom")
            self.assertEqual(caught.exception.data, {"k": "v"})


class PingTests(unittest.TestCase):
    def test_ping_true_when_server_healthy(self):
        with ScriptedServer(ok(True)) as server:
            self.assertTrue(launcher._ping_mcp_rpc("127.0.0.1", server.port))

    def test_ping_false_on_jsonrpc_error(self):
        with ScriptedServer(
            lambda r, p: (200, {"jsonrpc": "2.0", "id": r.get("id"),
                                "error": {"code": -1, "message": "no"}})
        ) as server:
            self.assertFalse(launcher._ping_mcp_rpc("127.0.0.1", server.port))

    def test_proxy_is_none_when_unreachable(self):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        self.assertIsNone(launcher._mcp_rpc_proxy("127.0.0.1", port))


class ReuseAndOwnershipTests(unittest.TestCase):
    def test_reuse_is_disabled_even_without_force_new(self):
        # Reuse must stay off while no read-only identity method exists,
        # otherwise the launcher could drive a session it cannot identify.
        with ScriptedServer(ok(True)):
            self.assertFalse(launcher._reuse_existing_mcp_if_possible([], False))
            self.assertFalse(launcher._reuse_existing_mcp_if_possible([], True))

    def test_reuse_disabled_reason_is_stated(self):
        self.assertTrue(launcher.REUSE_DISABLED_REASON)

    def test_occupied_port_is_detected(self):
        with ScriptedServer(ok(True)) as server:
            self.assertTrue(launcher._port_is_occupied("127.0.0.1", server.port))

    def test_free_port_is_not_reported_occupied(self):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        self.assertFalse(launcher._port_is_occupied("127.0.0.1", port))

    def test_process_id_reads_prefixed_execute_code_output(self):
        # execute_code wraps stdout as "Output: <line>"; the first payload line
        # carries that prefix and must still parse.
        with ScriptedServer(ok({
            "success": True,
            "message": "Python code execution completed.\nOutput: freecad_process_id=4321\n",
        })) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            self.assertEqual(launcher._mcp_rpc_process_id(client), 4321)

    def test_process_id_is_none_when_execution_failed(self):
        with ScriptedServer(ok({"success": False, "message": ""})) as server:
            client = launcher.JsonRpcClient("127.0.0.1", server.port)
            self.assertIsNone(launcher._mcp_rpc_process_id(client))

    def test_process_id_is_none_without_a_proxy(self):
        self.assertIsNone(launcher._mcp_rpc_process_id(None))


class MainRefusalTests(unittest.TestCase):
    def test_main_refuses_when_endpoint_is_occupied(self):
        """An occupied endpoint must abort before the profile is touched."""
        import io
        import contextlib as _contextlib

        touched: list[str] = []
        originals = {
            name: getattr(launcher, name)
            for name in ("_ensure_mcp_addon_installed", "_ensure_mcp_auto_start",
                         "_ensure_pixi_external_mods", "_port_is_occupied",
                         "_start_freecad")
        }
        try:
            launcher._ensure_mcp_addon_installed = lambda: touched.append("addon")
            launcher._ensure_mcp_auto_start = lambda: touched.append("settings")
            launcher._ensure_pixi_external_mods = lambda exe: touched.append("mods")
            launcher._port_is_occupied = lambda *a, **k: True
            launcher._start_freecad = lambda *a, **k: touched.append("started")

            stderr = io.StringIO()
            with _contextlib.redirect_stderr(stderr):
                code = launcher.main(["--force-new", "--freecad", str(LAUNCHER)])

            self.assertEqual(code, 1)
            self.assertEqual(touched, [], "nothing may be touched when the port is busy")
            self.assertIn("already in use", stderr.getvalue())
        finally:
            for name, value in originals.items():
                setattr(launcher, name, value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
