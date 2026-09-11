# SPDX-License-Identifier: LGPL-2.1-or-later
"""Authenticated JSON-RPC client wrapper for Part 3 GUI acceptance tests."""

from __future__ import annotations

import importlib.util
import sys
import threading
import time
import uuid
from collections.abc import Callable, Mapping
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
LAUNCHER_IMPL = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"
NESTED_ROOT = REPO_ROOT / "tools" / "mcp" / "freecad-mcp"
_SESSION_REFRESH_SKEW = timedelta(seconds=60)


def _load_launcher_module():
    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER_IMPL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER_IMPL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _ensure_nested_path() -> None:
    nested = str(NESTED_ROOT)
    if nested not in sys.path:
        sys.path.insert(0, nested)


class SessionedJsonRpcClient:
    """JsonRpcClient with MCP v2 session headers for authenticated methods."""

    def __init__(
        self,
        *,
        host: str,
        port: int,
        session_token: str,
        session_id: str,
        session_expires_at: str,
        mcp_instance_id: str,
        session_refresher: Callable[[], object],
        client_factory=None,
        json_rpc_error=None,
        json_rpc_transport_error=None,
        utcnow: Callable[[], datetime] | None = None,
    ) -> None:
        launcher = _load_launcher_module()
        factory = client_factory or launcher.JsonRpcClient
        self._client = factory(host=host, port=port)
        self.host = host
        self.port = port
        self.session_token = session_token
        self.session_id = session_id
        self.session_expires_at = session_expires_at
        self.mcp_instance_id = mcp_instance_id
        self._session_refresher = session_refresher
        self._utcnow = utcnow or (lambda: datetime.now(UTC))
        self._session_lock = threading.RLock()
        self._request_id_lock = threading.Lock()
        self.JsonRpcError = json_rpc_error or launcher.JsonRpcError
        self.JsonRpcTransportError = (
            json_rpc_transport_error or launcher.JsonRpcTransportError
        )

    @staticmethod
    def _parse_expiry(value: str) -> datetime:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        if parsed.tzinfo is None:
            parsed = parsed.replace(tzinfo=UTC)
        return parsed.astimezone(UTC)

    def _install_verified_session(self, verified: object) -> None:
        self.session_token = str(getattr(verified, "session_token"))
        self.session_id = str(getattr(verified, "session_id"))
        self.session_expires_at = str(getattr(verified, "session_expires_at"))

    def _renew_locked(self) -> None:
        verified = self._session_refresher()
        self._install_verified_session(verified)

    def _ensure_session_fresh(self) -> None:
        with self._session_lock:
            expires = self._parse_expiry(self.session_expires_at)
            if expires > self._utcnow().astimezone(UTC) + _SESSION_REFRESH_SKEW:
                return
            self._renew_locked()

    def _renew_if_current(self, observed_session_id: str) -> None:
        with self._session_lock:
            if self.session_id == observed_session_id:
                self._renew_locked()

    @staticmethod
    def _is_recoverable_auth_refusal(error: object) -> bool:
        if not isinstance(error, Mapping):
            return False
        data = error.get("data")
        return (
            type(error.get("code")) is int
            and error.get("code") == -32000
            and isinstance(error.get("message"), str)
            and isinstance(data, Mapping)
            and data.get("error_code") == "SESSION_EXPIRED"
        )

    def _dispatch(
        self,
        *,
        payload: bytes,
        headers: dict[str, str],
        request_id: int,
        timeout: float,
    ) -> dict[str, object]:
        import json
        import urllib.error
        import urllib.request

        request = urllib.request.Request(
            self._client.url,
            data=payload,
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                status = getattr(response, "status", response.getcode())
                raw_body = response.read()
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            raise self.JsonRpcTransportError(
                f"HTTP {exc.code} from {self._client.url}: {detail}"
            ) from exc
        except urllib.error.URLError as exc:
            raise self.JsonRpcTransportError(
                f"cannot reach {self._client.url}: {exc.reason}"
            ) from exc
        except OSError as exc:
            raise self.JsonRpcTransportError(
                f"cannot reach {self._client.url}: {exc}"
            ) from exc
        if status != 200:
            raise self.JsonRpcTransportError(
                f"HTTP {status} from {self._client.url}"
            )
        try:
            body = json.loads(raw_body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise self.JsonRpcTransportError(
                f"malformed JSON from {self._client.url}: {exc}"
            ) from exc
        if not isinstance(body, dict):
            raise self.JsonRpcTransportError("response is not a JSON object")
        if body.get("jsonrpc") != "2.0":
            raise self.JsonRpcTransportError("unexpected jsonrpc version")
        if body.get("id") != request_id:
            raise self.JsonRpcTransportError("response id mismatch")
        has_result = "result" in body
        has_error = "error" in body
        if has_result == has_error:
            raise self.JsonRpcTransportError(
                "response must carry exactly one of result or error"
            )
        if has_error:
            error = body["error"]
            if not isinstance(error, Mapping):
                raise self.JsonRpcTransportError("error member is not an object")
            if type(error.get("code")) is not int:
                raise self.JsonRpcTransportError("error code is not an integer")
            if not isinstance(error.get("message"), str):
                raise self.JsonRpcTransportError("error message is not a string")
        return body

    def call(self, method: str, params: object = None, timeout: float = 30.0) -> object:
        import json

        self._ensure_session_fresh()
        with self._request_id_lock:
            self._client._next_id += 1
            request_id = self._client._next_id
        payload = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": request_id,
                "method": method,
                "params": {} if params is None else params,
            }
        ).encode("utf-8")
        logical_headers = {
            "Content-Type": "application/json",
            "X-MCP-Request-Id": str(uuid.uuid4()),
            "X-MCP-Instance-Id": self.mcp_instance_id,
            "X-MCP-Lease-Credentials": "[]",
            "X-MCP-Rpc-Port": str(self.port),
        }

        for attempt in range(2):
            with self._session_lock:
                token = self.session_token
                observed_session_id = self.session_id
            headers = dict(logical_headers)
            headers["X-MCP-Session-Token"] = token
            body = self._dispatch(
                payload=payload,
                headers=headers,
                request_id=request_id,
                timeout=timeout,
            )
            if "error" not in body:
                return body["result"]
            error = body["error"]
            if attempt == 0 and self._is_recoverable_auth_refusal(error):
                self._renew_if_current(observed_session_id)
                continue
            raise self.JsonRpcError(
                error.get("code"), error.get("message"), error.get("data")
            )
        raise AssertionError("bounded retry loop exhausted")


def _freecad_build_identity(value: object) -> tuple[str, str]:
    """Match isolated-launcher / addon handshake version rendering."""

    parts = list(value) if isinstance(value, (list, tuple)) else [value]
    rendered = [str(part) for part in parts]
    version = ".".join(rendered[:3])
    revision = rendered[3] if len(rendered) > 3 and rendered[3] else "unknown"
    return version, revision


def _rpc_endpoint_from_instance_info(info: dict[str, Any], base_client) -> tuple[str, int]:
    endpoint = info.get("actual_endpoint")
    if not isinstance(endpoint, dict):
        endpoint = {}
    host = str(endpoint.get("host") or info.get("host") or base_client.host)
    port = int(endpoint.get("port") or info.get("port") or base_client.port)
    return host, port


def authenticate_json_rpc(
    base_client,
    profile_root: Path,
    *,
    json_rpc_error=None,
    json_rpc_transport_error=None,
    utcnow: Callable[[], datetime] | None = None,
) -> SessionedJsonRpcClient:
    """Perform handshake_v2 using the isolated profile secret."""

    _ensure_nested_path()
    from addon.FreeCADMCP._shared.protocol.handshake_request import (
        build_handshake_request,
    )
    from addon.FreeCADMCP._shared.protocol.handshake_response import (
        verify_handshake_response,
    )
    from addon.FreeCADMCP._shared.protocol.manifest import make_mcp_runtime_identity
    from addon.FreeCADMCP._shared.protocol.profile_secret import load_profile_secret

    info = base_client.call("get_instance_info", timeout=30.0)
    if not isinstance(info, dict):
        raise RuntimeError("get_instance_info returned unexpected payload")
    secret_path = profile_root / "FreeCAD" / "freecad_mcp_auth.secret"
    deadline = time.monotonic() + 30.0
    while not secret_path.is_file() and time.monotonic() < deadline:
        time.sleep(0.1)
    if not secret_path.is_file():
        raise FileNotFoundError(f"profile auth secret missing: {secret_path}")
    secret = load_profile_secret(secret_path)

    freecad_version, freecad_revision = _freecad_build_identity(
        info.get("freecad_version")
    )
    rpc_host, rpc_port = _rpc_endpoint_from_instance_info(info, base_client)

    mcp = make_mcp_runtime_identity(client_build_id="part3-wp04")
    def perform_handshake():
        request = build_handshake_request(
            secret=secret,
            mcp=mcp,
            expected_profile_id=str(info["profile_instance_id"]),
            expected_freecad_pid=int(info["pid"]),
            expected_freecad_process_started_at=str(info["freecad_process_started_at"]),
            expected_addon_runtime_id=str(info["addon_runtime_id"]),
            expected_boot_id=str(info["boot_id"]),
            expected_rpc_host=rpc_host,
            expected_rpc_port=rpc_port,
            expected_protocol_version=int(info["protocol_version"]),
            expected_protocol_features=list(info["protocol_features"]),
            expected_addon_version=str(info["addon_version"]),
            expected_addon_build_id=str(info["addon_build_id"]),
            expected_freecad_version=freecad_version,
            expected_freecad_revision=freecad_revision,
            expected_profile_path_fingerprint=str(info["profile_path_fingerprint"]),
        )
        response = base_client.call("handshake_v2", [request], timeout=30.0)
        return verify_handshake_response(
            response,
            secret=secret,
            expected_client_nonce=request["client_nonce"],
            expected_profile_id=str(info["profile_instance_id"]),
            expected_freecad_pid=int(info["pid"]),
            expected_addon_runtime_id=str(info["addon_runtime_id"]),
            expected_freecad_process_started_at=str(info["freecad_process_started_at"]),
            expected_rpc_host=rpc_host,
            expected_rpc_port=rpc_port,
            expected_protocol_version=int(info["protocol_version"]),
            expected_protocol_features=list(info["protocol_features"]),
            expected_addon_version=str(info["addon_version"]),
            expected_addon_build_id=str(info["addon_build_id"]),
            expected_freecad_version=freecad_version,
            expected_freecad_revision=freecad_revision,
            expected_boot_id=str(info["boot_id"]),
            expected_profile_path_fingerprint=str(info["profile_path_fingerprint"]),
        )

    verified = perform_handshake()
    return SessionedJsonRpcClient(
        host=base_client.host,
        port=base_client.port,
        session_token=verified.session_token,
        session_id=verified.session_id,
        session_expires_at=verified.session_expires_at,
        mcp_instance_id=mcp.runtime_id,
        session_refresher=perform_handshake,
        client_factory=type(base_client),
        json_rpc_error=json_rpc_error,
        json_rpc_transport_error=json_rpc_transport_error,
        utcnow=utcnow,
    )
