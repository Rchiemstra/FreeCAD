# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline session-renewal regression tests for the Part 3 JSON-RPC client."""

from __future__ import annotations

import json
import io
import os
import socket
import threading
import urllib.error
import urllib.request
import uuid
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest

from tests.gui.part3 import rpc_session_client as rpc

rpc._ensure_nested_path()

from addon.FreeCADMCP._shared.protocol.handshake_response import (  # noqa: E402
    sign_handshake_response,
)
from addon.FreeCADMCP._shared.protocol.constants import (  # noqa: E402
    DEFAULT_SESSION_TTL_SECONDS,
)
from addon.FreeCADMCP._shared.protocol.manifest import make_runtime_manifest  # noqa: E402
from addon.FreeCADMCP._shared.protocol.protocol_error import ProtocolError  # noqa: E402
from addon.FreeCADMCP._shared.protocol.session_manager import SessionManager  # noqa: E402


class FakeClock:
    def __init__(self) -> None:
        self._utc = datetime.now(UTC)
        self._monotonic = 1000.0

    def now(self) -> datetime:
        return self._utc

    def monotonic(self) -> float:
        return self._monotonic

    def advance(self, seconds: float) -> None:
        self._utc += timedelta(seconds=seconds)
        self._monotonic += seconds


class FakeJsonRpcError(Exception):
    def __init__(self, code, message, data=None) -> None:
        super().__init__(f"JSON-RPC error {code}: {message}")
        self.code = code
        self.message = message
        self.data = data


class FakeJsonRpcTransportError(Exception):
    pass


class FakeResponse:
    def __init__(self, body: object, *, raw: bytes | None = None) -> None:
        self._body = raw if raw is not None else json.dumps(body).encode("utf-8")
        self.status = 200

    def __enter__(self):
        return self

    def __exit__(self, *_args) -> None:
        return None

    def getcode(self) -> int:
        return self.status

    def read(self) -> bytes:
        return self._body


class FakeBaseClient:
    server: "OfflineServer"

    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.url = f"http://{host}:{port}/jsonrpc"
        self._next_id = 0

    def call(self, method: str, params=None, timeout: float = 30.0):
        del timeout
        if method == "get_instance_info":
            return self.server.instance_info()
        if method == "handshake_v2":
            request = params[0]
            self.server.handshake_requests.append(request)
            response = self.server.manager.perform_handshake(request)
            if self.server.handshake_mutator is not None:
                response = self.server.handshake_mutator(response)
            return response
        raise AssertionError(f"unexpected base-client call: {method}")


class OfflineServer:
    def __init__(self, clock: FakeClock, secret: bytes) -> None:
        self.clock = clock
        self.secret = secret
        self.manifest = make_runtime_manifest(
            profile_id="part3-profile",
            addon_runtime_id=str(uuid.uuid4()),
            freecad_pid=4242,
            freecad_process_started_at=clock.now().isoformat(),
            boot_id="part3-boot",
            rpc_host="127.0.0.1",
            rpc_port=9875,
            freecad_version="26.3.0",
            freecad_revision="48066",
            addon_version="1.2.3",
            addon_build_id="part3-addon-build",
            profile_path_fingerprint="part3-profile-fingerprint",
        )
        self.manager = SessionManager(
            manifest=self.manifest,
            secret=secret,
            monotonic=clock.monotonic,
            utcnow=clock.now,
        )
        self.handshake_requests: list[dict[str, object]] = []
        self.handshake_mutator = None

    def instance_info(self) -> dict[str, object]:
        manifest = self.manifest
        return {
            "profile_instance_id": manifest.profile_id,
            "pid": manifest.freecad_pid,
            "freecad_process_started_at": manifest.freecad_process_started_at,
            "addon_runtime_id": manifest.addon_runtime_id,
            "boot_id": manifest.boot_id,
            "actual_endpoint": {"host": manifest.rpc_host, "port": manifest.rpc_port},
            "protocol_version": manifest.protocol_version,
            "protocol_features": list(manifest.features),
            "addon_version": manifest.addon_version,
            "addon_build_id": manifest.addon_build_id,
            "freecad_version": [26, 3, 0, manifest.freecad_revision],
            "profile_path_fingerprint": manifest.profile_path_fingerprint,
        }


@pytest.fixture
def client_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    clock = FakeClock()
    secret = b"s" * 32
    secret_path = tmp_path / "FreeCAD" / "freecad_mcp_auth.secret"
    secret_path.parent.mkdir()
    secret_path.write_bytes(secret)
    secret_path.chmod(0o600)
    server = OfflineServer(clock, secret)
    FakeBaseClient.server = server
    base = FakeBaseClient("127.0.0.1", 9875)
    requests: list[urllib.request.Request] = []
    outcomes: list[object] = []

    def urlopen(request, timeout=30.0):
        del timeout
        requests.append(request)
        outcome = outcomes.pop(0) if outcomes else {
            "jsonrpc": "2.0",
            "id": json.loads(request.data)["id"],
            "result": True,
        }
        if isinstance(outcome, BaseException):
            raise outcome
        if isinstance(outcome, bytes):
            return FakeResponse({}, raw=outcome)
        if callable(outcome):
            outcome = outcome(request)
        return FakeResponse(outcome)

    monkeypatch.setattr(urllib.request, "urlopen", urlopen)
    client = rpc.authenticate_json_rpc(
        base,
        tmp_path,
        json_rpc_error=FakeJsonRpcError,
        json_rpc_transport_error=FakeJsonRpcTransportError,
        utcnow=clock.now,
    )
    return client, clock, server, requests, outcomes


def _structured_expired(request: urllib.request.Request) -> dict[str, object]:
    return {
        "jsonrpc": "2.0",
        "id": json.loads(request.data)["id"],
        "error": {
            "code": -32000,
            "message": "RPC session has expired",
            "data": {"error_code": "SESSION_EXPIRED"},
        },
    }


def _success(request: urllib.request.Request) -> dict[str, object]:
    return {"jsonrpc": "2.0", "id": json.loads(request.data)["id"], "result": "ok"}


def _headers(request: urllib.request.Request) -> dict[str, str]:
    return {key.lower(): value for key, value in request.header_items()}


@pytest.mark.parametrize("method", ["get_document_info", "edit_object"])
@pytest.mark.parametrize(
    ("case", "error", "expected_error"),
    [
        (
            "missing-code",
            {
                "message": "RPC session has expired",
                "data": {"error_code": "SESSION_EXPIRED"},
            },
            FakeJsonRpcTransportError,
        ),
        (
            "boolean-code",
            {
                "code": True,
                "message": "RPC session has expired",
                "data": {"error_code": "SESSION_EXPIRED"},
            },
            FakeJsonRpcTransportError,
        ),
        (
            "string-code",
            {
                "code": "-32000",
                "message": "RPC session has expired",
                "data": {"error_code": "SESSION_EXPIRED"},
            },
            FakeJsonRpcTransportError,
        ),
        (
            "missing-message",
            {"code": -32000, "data": {"error_code": "SESSION_EXPIRED"}},
            FakeJsonRpcTransportError,
        ),
        (
            "non-string-message",
            {
                "code": -32000,
                "message": ["RPC session has expired"],
                "data": {"error_code": "SESSION_EXPIRED"},
            },
            FakeJsonRpcTransportError,
        ),
        (
            "other-integer-code",
            {
                "code": -32001,
                "message": "RPC session has expired",
                "data": {"error_code": "SESSION_EXPIRED"},
            },
            FakeJsonRpcError,
        ),
    ],
)
def test_semantic_expired_requires_exact_error_envelope_without_retry(
    client_env, method, case, error, expected_error
):
    del case
    client, _clock, server, requests, outcomes = client_env
    outcomes.append(
        lambda request: {
            "jsonrpc": "2.0",
            "id": json.loads(request.data)["id"],
            "error": error,
        }
    )
    params = {"operation_id": "op-safe"} if method == "edit_object" else {}

    with pytest.raises(expected_error):
        client.call(method, params)

    assert len(requests) == 1
    assert len(server.handshake_requests) == 1


def test_proactive_refresh_reuses_runtime_identity_with_fresh_nonce(client_env):
    client, clock, server, _requests, _outcomes = client_env
    initial_session = client.session_id
    initial_token = client.session_token
    clock.advance(DEFAULT_SESSION_TTL_SECONDS - 61)

    assert client.call("get_document_info", {"document": "A"}) is True
    assert len(server.handshake_requests) == 1

    clock.advance(1)

    assert client.call("get_document_info", {"document": "A"}) is True

    assert client.session_id != initial_session
    assert client.session_token != initial_token
    assert len(server.handshake_requests) == 2
    first, second = server.handshake_requests
    assert first["client_nonce"] != second["client_nonce"]
    assert first["mcp"]["runtime_id"] == second["mcp"]["runtime_id"]
    assert client.mcp_instance_id == second["mcp"]["runtime_id"]


@pytest.mark.parametrize(
    "drift",
    [
        ("manifest", "profile_id", "other-profile"),
        ("manifest", "freecad_pid", 5252),
        ("manifest", "freecad_process_started_at", "2026-01-01T00:00:00Z"),
        ("manifest", "addon_runtime_id", str(uuid.uuid4())),
        ("manifest", "rpc_host", "localhost"),
        ("manifest", "rpc_port", 9876),
        ("manifest", "boot_id", "other-boot"),
        ("manifest", "protocol_version", 1),
        ("manifest", "features", []),
        ("manifest", "addon_version", "9.9.9"),
        ("manifest", "addon_build_id", "other-build"),
        ("manifest", "freecad_version", "99.0.0"),
        ("manifest", "freecad_revision", "other-revision"),
        ("manifest", "profile_path_fingerprint", "other-fingerprint"),
        ("response", "client_nonce", "A" * 32),
        ("response", "proof", "hmac-sha256:" + "0" * 64),
    ],
)
def test_refresh_fails_closed_on_identity_nonce_or_hmac_drift(client_env, drift):
    client, clock, server, _requests, _outcomes = client_env
    old_session = client.session_id
    old_token = client.session_token

    def mutate(response):
        location, key, value = drift
        changed = dict(response)
        if location == "manifest":
            manifest = dict(changed["manifest"])
            manifest[key] = value
            if key in {"rpc_host", "rpc_port"}:
                host = manifest["rpc_host"]
                manifest["endpoint"] = f"{host}:{manifest['rpc_port']}"
            changed["manifest"] = manifest
        else:
            changed[key] = value
        if key != "proof":
            changed = sign_handshake_response(changed, server.secret)
        return changed

    server.handshake_mutator = mutate
    clock.advance(241)
    with pytest.raises(ProtocolError):
        client.call("get_document_info", {})
    assert client.session_id == old_session
    assert client.session_token == old_token


def test_structured_expiry_retries_once_with_identical_logical_envelope(client_env):
    client, _clock, server, requests, outcomes = client_env
    outcomes.extend([_structured_expired, _success])
    params = {
        "document": "A",
        "operation_id": "operation-123",
        "value": {"x": 1},
    }

    assert client.call("edit_object", params) == "ok"
    assert len(requests) == 2
    assert len(server.handshake_requests) == 2
    assert requests[0].data == requests[1].data
    first_payload = json.loads(requests[0].data)
    second_payload = json.loads(requests[1].data)
    assert first_payload == second_payload
    assert first_payload["jsonrpc"] == "2.0"
    assert first_payload["id"] == second_payload["id"]
    assert first_payload["method"] == "edit_object"
    assert first_payload["params"] == params
    assert first_payload["params"]["operation_id"] == "operation-123"
    first_headers = _headers(requests[0])
    second_headers = _headers(requests[1])
    assert first_headers["x-mcp-request-id"] == second_headers["x-mcp-request-id"]
    assert first_headers["x-mcp-lease-credentials"] == "[]"
    assert first_headers["x-mcp-lease-credentials"] == second_headers["x-mcp-lease-credentials"]
    assert first_headers["x-mcp-session-token"] != second_headers["x-mcp-session-token"]
    first_without_token = dict(first_headers)
    second_without_token = dict(second_headers)
    first_without_token.pop("x-mcp-session-token")
    second_without_token.pop("x-mcp-session-token")
    assert first_without_token == second_without_token
    assert json.loads(requests[0].data)["params"] == params


def test_second_structured_expiry_is_surfaced(client_env):
    client, _clock, server, requests, outcomes = client_env
    outcomes.extend([_structured_expired, _structured_expired])
    with pytest.raises(FakeJsonRpcError) as caught:
        client.call("get_document_info", {})
    assert caught.value.data == {"error_code": "SESSION_EXPIRED"}
    assert len(requests) == 2
    assert len(server.handshake_requests) == 2


@pytest.mark.parametrize("method", ["get_document_info", "edit_object"])
@pytest.mark.parametrize(
    "outcome",
    [
        lambda request: {
            "jsonrpc": "2.0",
            "id": json.loads(request.data)["id"],
            "error": {"code": -32000, "message": "generic", "data": {}},
        },
        lambda request: {
            "jsonrpc": "2.0",
            "id": json.loads(request.data)["id"],
            "error": {"code": -32000, "message": "SESSION_EXPIRED"},
        },
        lambda request: {
            "jsonrpc": "2.0",
            "id": json.loads(request.data)["id"],
            "error": {
                "code": -32001,
                "message": "document conflict",
                "data": {"error_code": "DOCUMENT_CONFLICT"},
            },
        },
        urllib.error.HTTPError(
            "http://127.0.0.1:9875/jsonrpc",
            503,
            "unavailable",
            {},
            io.BytesIO(b"unavailable"),
        ),
        urllib.error.URLError("offline"),
        socket.timeout("timeout"),
        b"not-json",
        lambda request: {"jsonrpc": "2.0", "id": json.loads(request.data)["id"] + 1, "result": True},
    ],
)
def test_ambiguous_or_unstructured_failures_are_never_retried(
    client_env, method, outcome
):
    client, _clock, server, requests, outcomes = client_env
    outcomes.append(outcome)
    params = {"operation_id": "op-safe"} if method == "edit_object" else {}
    with pytest.raises(Exception):
        client.call(method, params)
    assert len(requests) == 1
    assert len(server.handshake_requests) == 1


def test_lost_mutation_response_is_never_retried_across_sessions(client_env):
    client, _clock, server, requests, outcomes = client_env
    outcomes.append(socket.timeout("response lost after dispatch"))
    with pytest.raises(Exception):
        client.call("edit_object", {"operation_id": "exactly-once-op", "value": 7})
    assert len(requests) == 1
    assert len(server.handshake_requests) == 1


def test_multiple_default_ttl_renewals_without_sleep_or_override(
    client_env, monkeypatch: pytest.MonkeyPatch
):
    client, clock, server, requests, _outcomes = client_env
    monkeypatch.delenv("FREECAD_MCP_SESSION_TTL_SECONDS", raising=False)
    for cycle in range(4):
        clock.advance(DEFAULT_SESSION_TTL_SECONDS - 59)
        assert client.call(
            "edit_object", {"operation_id": f"cycle-{cycle}", "value": cycle}
        ) is True
    assert "FREECAD_MCP_SESSION_TTL_SECONDS" not in os.environ
    assert len(server.handshake_requests) == 5
    assert len(requests) == 4
    assert len({request["client_nonce"] for request in server.handshake_requests}) == 5
    assert clock.monotonic() - 1000.0 > 2 * DEFAULT_SESSION_TTL_SECONDS


def test_concurrent_proactive_refresh_is_serialized(client_env):
    client, clock, server, requests, _outcomes = client_env
    clock.advance(DEFAULT_SESSION_TTL_SECONDS - 59)
    barrier = threading.Barrier(8)
    results: list[object] = []
    errors: list[BaseException] = []

    def invoke() -> None:
        try:
            barrier.wait()
            results.append(client.call("get_document_info", {}))
        except BaseException as exc:  # pragma: no cover - assertion reports contents
            errors.append(exc)

    threads = [threading.Thread(target=invoke) for _ in range(8)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    assert errors == []
    assert results == [True] * 8
    assert len(requests) == 8
    assert len(server.handshake_requests) == 2
    assert len({request["client_nonce"] for request in server.handshake_requests}) == 2
