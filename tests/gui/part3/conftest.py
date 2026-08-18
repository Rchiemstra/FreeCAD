# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared fixtures for Part 3 LocalUserDriver GUI acceptance tests."""

from __future__ import annotations

import contextlib
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import uuid
from collections.abc import Iterator
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = REPO_ROOT / "start_freecad.py"
LAUNCHER_IMPL = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"

if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.gui.part3.local_user_driver import (
    LocalUserDriver,
    generate_control_token,
    launch_env_for_isolated_profile,
    wait_for_endpoint,
)
from tests.gui.part3.stress_coordinator import (
    _force_kill_owned_process_tree,
    default_freecad_exe,
    graceful_shutdown_owned_session,
)
from tests.gui.part3.rpc_session_client import authenticate_json_rpc


def _load_json_rpc_client():
    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER_IMPL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER_IMPL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _port_is_open(host: str, port: int) -> bool:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(0.75)
        return sock.connect_ex((host, port)) == 0


def _mcp_endpoint_busy(launcher_module, host: str = "127.0.0.1", port: int | None = None) -> bool:
    rpc_port = port if port is not None else launcher_module.MCP_RPC_PORT
    try:
        return bool(
            launcher_module.JsonRpcClient(host=host, port=rpc_port).call("ping", timeout=2.0)
        )
    except Exception:
        return False


def _default_freecad_exe() -> Path | None:
    return default_freecad_exe(REPO_ROOT)


def _terminate_owned_process_tree(process: subprocess.Popen) -> None:
    """Last-resort kill when ADR §9 graceful shutdown did not finish."""

    _force_kill_owned_process_tree(process)


def _wait_for_port_closed(host: str, port: int, timeout_s: float = 45.0) -> bool:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if not _port_is_open(host, port):
            return True
        time.sleep(0.25)
    return False


@contextlib.contextmanager
def launch_freecad_gui_session() -> Iterator[dict[str, object]]:
    freecad_exe = _default_freecad_exe()
    if freecad_exe is None:
        pytest.skip("FreeCAD GUI binary not found under build/release/bin")
    launcher_module = _load_json_rpc_client()
    if _mcp_endpoint_busy(launcher_module):
        pytest.skip(
            f"MCP port {launcher_module.MCP_RPC_PORT} is already occupied; "
            "refusing to attach to an unknown session"
        )

    profile_root = Path(tempfile.mkdtemp(prefix="part3-local-driver-"))
    run_root = profile_root / "run"
    endpoint_dir = run_root / "control"
    token = generate_control_token()
    env = launch_env_for_isolated_profile(
        profile_root,
        control_token=token,
        endpoint_dir=endpoint_dir,
        repo_root=REPO_ROOT,
    )
    launcher_log = run_root / "launcher.log"
    command = [
        sys.executable,
        str(LAUNCHER),
        "--force-new",
        "--freecad",
        str(freecad_exe),
        "--mcp-timeout",
        "120",
        "--wait",
    ]
    with launcher_log.open("wb") as log_handle:
        process = subprocess.Popen(
            command,
            cwd=str(REPO_ROOT),
            env=env,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
        )

    try:
        deadline = time.monotonic() + 120.0
        ready = False
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            if _port_is_open("127.0.0.1", launcher_module.MCP_RPC_PORT):
                try:
                    if launcher_module.JsonRpcClient().call("ping", timeout=2.0):
                        ready = True
                        break
                except Exception:
                    pass
            time.sleep(0.25)

        if not ready:
            pytest.skip(
                "FreeCAD GUI with MCP did not become ready within 120s; "
                f"see {launcher_log}"
            )

        endpoint = wait_for_endpoint(endpoint_dir, timeout_s=30.0)
        local_driver = LocalUserDriver(token, endpoint)
        local_driver.preflight()
        rpc = authenticate_json_rpc(
            launcher_module.JsonRpcClient(),
            profile_root,
            json_rpc_error=launcher_module.JsonRpcError,
            json_rpc_transport_error=launcher_module.JsonRpcTransportError,
        )

        session = {
            "process": process,
            "profile_root": profile_root,
            "run_root": run_root,
            "endpoint_dir": endpoint_dir,
            "token": token,
            "local_driver": local_driver,
            "rpc": rpc,
            "launcher_module": launcher_module,
            "freecad_exe": freecad_exe,
        }
        try:
            yield session
        finally:
            shutdown = graceful_shutdown_owned_session(
                process=process,
                profile_root=profile_root,
                launcher_module=launcher_module,
                repo_root=REPO_ROOT,
                mcp_port=launcher_module.MCP_RPC_PORT,
                local_driver=local_driver,
            )
            if not shutdown.get("success"):
                _terminate_owned_process_tree(process)
    finally:
        if process.poll() is None:
            _terminate_owned_process_tree(process)
        if not _wait_for_port_closed("127.0.0.1", launcher_module.MCP_RPC_PORT):
            pytest.skip(
                f"MCP port {launcher_module.MCP_RPC_PORT} remained occupied after teardown"
            )


@pytest.fixture(scope="module")
def freecad_gui_session() -> Iterator[dict[str, object]]:
    with launch_freecad_gui_session() as session:
        yield session


@pytest.fixture(autouse=True)
def _reset_pause_gate_between_tests(request) -> None:
    module_name = request.node.path.name
    if module_name in {
        "test_part3_shutdown.py",
        "test_part3_architecture.py",
        "test_part3_stress_coordinator_launcher.py",
        "test_part3_control_channel.py",
    }:
        yield
        return
    session = request.getfixturevalue("freecad_gui_session")
    local = session["local_driver"]
    with contextlib.suppress(Exception):
        local.invoke("resume_writes")
    yield
    with contextlib.suppress(Exception):
        local.invoke("resume_writes")
    with contextlib.suppress(Exception):
        local.invoke("clear_selection")
