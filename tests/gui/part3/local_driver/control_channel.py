# SPDX-License-Identifier: LGPL-2.1-or-later
"""Authenticated loopback control channel for Part 3 LocalUserDriver."""

from __future__ import annotations

import json
import os
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

TOKEN_ENV = "PART3_LOCAL_CONTROL_TOKEN"
ENDPOINT_DIR_ENV = "PART3_CONTROL_ENDPOINT_DIR"
ENDPOINT_FILENAME = "endpoint.json"

ActionHandler = Callable[[str, dict[str, Any], str], dict[str, Any]]


def generate_control_token() -> str:
    """Return a 32-byte per-run token encoded as hex for the environment."""

    return secrets.token_hex(32)


def _constant_time_token_ok(expected: bytes, provided: str) -> bool:
    try:
        supplied = bytes.fromhex(provided.strip())
    except ValueError:
        return False
    return len(supplied) == len(expected) and secrets.compare_digest(supplied, expected)


def write_endpoint_atomically(endpoint_path: Path, payload: dict[str, Any]) -> None:
    endpoint_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = endpoint_path.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, endpoint_path)


class ControlChannel:
    """ThreadingHTTPServer on 127.0.0.1:0 with token-gated POST /action."""

    def __init__(
        self,
        token: bytes,
        endpoint_path: Path,
        handler: ActionHandler,
    ) -> None:
        if len(token) != 32:
            raise ValueError("control token must be exactly 32 bytes")
        self._token = token
        self._endpoint_path = endpoint_path
        self._handler = handler
        self._server: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None

    @property
    def endpoint_path(self) -> Path:
        return self._endpoint_path

    def start(self) -> dict[str, Any]:
        channel = self

        class _Handler(BaseHTTPRequestHandler):
            def log_message(self, format: str, *args: Any) -> None:  # noqa: A003
                del format, args

            def do_POST(self) -> None:  # noqa: N802
                if self.path not in ("/", "/action"):
                    self.send_error(404)
                    return
                length = int(self.headers.get("Content-Length", "0") or "0")
                raw = self.rfile.read(length)
                try:
                    payload = json.loads(raw.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError):
                    self._write_json(400, {"error": "invalid JSON"})
                    return
                if not isinstance(payload, dict):
                    self._write_json(400, {"error": "payload must be an object"})
                    return
                operation_id = str(payload.get("operation_id") or "")
                action = str(payload.get("action") or "")
                params = payload.get("params")
                token_value = str(payload.get("token") or "")
                if not operation_id or not action:
                    self._write_json(
                        400,
                        {
                            "operation_id": operation_id,
                            "success": False,
                            "error": "operation_id and action are required",
                        },
                    )
                    return
                if not _constant_time_token_ok(channel._token, token_value):
                    self._write_json(
                        403,
                        {
                            "operation_id": operation_id,
                            "success": False,
                            "error": "invalid token",
                        },
                    )
                    return
                if not isinstance(params, dict):
                    params = {}
                try:
                    result = channel._handler(action, params, operation_id)
                except Exception as exc:  # noqa: BLE001 - surface to coordinator
                    self._write_json(
                        500,
                        {
                            "operation_id": operation_id,
                            "success": False,
                            "error": str(exc),
                        },
                    )
                    return
                if result.get("operation_id") != operation_id:
                    self._write_json(
                        500,
                        {
                            "operation_id": operation_id,
                            "success": False,
                            "error": "handler did not echo operation_id",
                        },
                    )
                    return
                self._write_json(200, result)

            def _write_json(self, status: int, payload: dict[str, Any]) -> None:
                body = json.dumps(payload).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self._server = ThreadingHTTPServer(("127.0.0.1", 0), _Handler)
        host, port = self._server.server_address
        endpoint = {
            "host": host,
            "port": int(port),
            "ready": True,
            "path": "/action",
        }
        write_endpoint_atomically(self._endpoint_path, endpoint)
        self._thread = threading.Thread(
            target=self._server.serve_forever,
            name="Part3LocalControlChannel",
            daemon=True,
        )
        self._thread.start()
        return endpoint

    def stop(self) -> None:
        if self._server is not None:
            self._server.shutdown()
            self._server.server_close()
            self._server = None
        if self._thread is not None:
            self._thread.join(timeout=5.0)
            self._thread = None


def start_control_channel(
    token: bytes,
    endpoint_dir: Path,
    handler: ActionHandler,
) -> ControlChannel:
    endpoint_path = endpoint_dir / ENDPOINT_FILENAME
    channel = ControlChannel(token, endpoint_path, handler)
    channel.start()
    return channel
