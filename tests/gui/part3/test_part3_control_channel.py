# SPDX-License-Identifier: LGPL-2.1-or-later
"""Unit coverage for the Part 3 local control channel."""

from __future__ import annotations

import json
import threading
import uuid
from pathlib import Path
from urllib import error as urllib_error
from urllib import request as urllib_request

import pytest

from tests.gui.part3.local_driver.control_channel import (
    ControlChannel,
    generate_control_token,
    write_endpoint_atomically,
)


def test_generate_control_token_is_32_bytes_hex() -> None:
    token = generate_control_token()
    assert len(bytes.fromhex(token)) == 32


def test_bad_token_rejected(tmp_path: Path) -> None:
    token = b"a" * 32
    endpoint = tmp_path / "endpoint.json"

    def handler(action: str, params: dict, operation_id: str) -> dict:
        return {
            "operation_id": operation_id,
            "success": True,
            "result": {"action": action, "params": params},
        }

    channel = ControlChannel(token, endpoint, handler)
    endpoint_info = channel.start()
    payload = {
        "token": "00" * 32,
        "operation_id": "op-bad-token",
        "action": "noop",
        "params": {},
    }
    request = urllib_request.Request(
        f"http://127.0.0.1:{endpoint_info['port']}/action",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib_request.urlopen(request, timeout=5.0) as response:
            body = json.loads(response.read().decode("utf-8"))
    except urllib_error.HTTPError as exc:
        body = json.loads(exc.read().decode("utf-8"))
    assert body["success"] is False
    assert body["operation_id"] == "op-bad-token"
    channel.stop()


def test_operation_id_is_echoed(tmp_path: Path) -> None:
    token = bytes.fromhex(generate_control_token())
    endpoint = tmp_path / "endpoint.json"
    seen: list[str] = []

    def handler(action: str, params: dict, operation_id: str) -> dict:
        seen.append(operation_id)
        return {
            "operation_id": operation_id,
            "success": True,
            "result": {"action": action},
        }

    channel = ControlChannel(token, endpoint, handler)
    endpoint_info = channel.start()
    operation_id = str(uuid.uuid4())
    payload = {
        "token": token.hex(),
        "operation_id": operation_id,
        "action": "noop",
        "params": {"x": 1},
    }
    request = urllib_request.Request(
        f"http://127.0.0.1:{endpoint_info['port']}/action",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib_request.urlopen(request, timeout=5.0) as response:
        body = json.loads(response.read().decode("utf-8"))
    assert body["operation_id"] == operation_id
    assert body["success"] is True
    assert seen == [operation_id]
    channel.stop()


def test_endpoint_json_is_atomic_and_usable(tmp_path: Path) -> None:
    endpoint = tmp_path / "control" / "endpoint.json"
    write_endpoint_atomically(
        endpoint,
        {"host": "127.0.0.1", "port": 54321, "ready": True, "path": "/action"},
    )
    payload = json.loads(endpoint.read_text(encoding="utf-8"))
    assert payload["ready"] is True
    assert payload["port"] == 54321


def test_handler_blocks_on_event_without_sleep(tmp_path: Path) -> None:
    token = bytes.fromhex(generate_control_token())
    endpoint = tmp_path / "endpoint.json"
    gate = threading.Event()

    def handler(action: str, params: dict, operation_id: str) -> dict:
        gate.wait(timeout=2.0)
        return {
            "operation_id": operation_id,
            "success": True,
            "result": {"action": action, "params": params},
        }

    channel = ControlChannel(token, endpoint, handler)
    endpoint_info = channel.start()
    operation_id = "sync-by-ack"
    payload = {
        "token": token.hex(),
        "operation_id": operation_id,
        "action": "wait",
        "params": {},
    }

    def caller() -> dict:
        request = urllib_request.Request(
            f"http://127.0.0.1:{endpoint_info['port']}/action",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib_request.urlopen(request, timeout=5.0) as response:
            return json.loads(response.read().decode("utf-8"))

    result_holder: dict[str, dict] = {}

    def run_caller() -> None:
        result_holder["body"] = caller()

    thread = threading.Thread(target=run_caller)
    thread.start()
    gate.set()
    thread.join(timeout=5.0)
    body = result_holder["body"]
    assert body["operation_id"] == operation_id
    assert body["success"] is True
    channel.stop()
