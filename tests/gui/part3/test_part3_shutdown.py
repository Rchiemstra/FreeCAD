# SPDX-License-Identifier: LGPL-2.1-or-later
"""Repeated native shutdown gate for Part 3 WP09 (ADR §8/§9)."""

from __future__ import annotations

import contextlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

from tests.gui.part3.evidence import SHUTDOWN_TIMESTAMP_KEYS, empty_shutdown_record
from tests.gui.part3.local_user_driver import (
    LocalUserDriver,
    generate_control_token,
    launch_env_for_isolated_profile,
    wait_for_endpoint,
)
from tests.gui.part3.rpc_session_client import authenticate_json_rpc
from tests.gui.part3.stress_coordinator import (
    default_freecad_exe,
    graceful_shutdown_owned_session,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = REPO_ROOT / "start_freecad.py"
LAUNCHER_IMPL = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"
RESULTS_ROOT = (
    REPO_ROOT
    / "results"
    / "part3-orchestration"
    / "2026-08-18"
    / "P3-WP09"
    / "iteration-001"
)

REPEATED_RUNS = 2


def _load_launcher_module():
    import importlib.util

    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER_IMPL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER_IMPL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _port_is_open(host: str, port: int) -> bool:
    import contextlib
    import socket

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


def _wait_for_mcp_ready(launcher_module, process: subprocess.Popen, timeout_s: float) -> None:
    import time

    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("launcher exited before MCP became ready")
        if _port_is_open("127.0.0.1", launcher_module.MCP_RPC_PORT):
            try:
                if launcher_module.JsonRpcClient().call("ping", timeout=2.0):
                    return
            except Exception:
                pass
    raise TimeoutError("MCP RPC did not become ready")


def _launch_owned_session() -> dict[str, object]:
    freecad_exe = default_freecad_exe(REPO_ROOT)
    if freecad_exe is None:
        pytest.skip("FreeCAD GUI binary not found under build/release/bin")
    launcher_module = _load_launcher_module()
    if _mcp_endpoint_busy(launcher_module):
        pytest.skip(
            f"MCP port {launcher_module.MCP_RPC_PORT} is already occupied; "
            "refusing to attach to an unknown session"
        )

    profile_root = Path(tempfile.mkdtemp(prefix="part3-shutdown-"))
    run_root = profile_root / "run"
    endpoint_dir = run_root / "control"
    evidence_dir = run_root / "evidence"
    evidence_dir.mkdir(parents=True, exist_ok=True)
    evidence_path = evidence_dir / "evidence.json"
    evidence_path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "shutdown": empty_shutdown_record(),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
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

    _wait_for_mcp_ready(launcher_module, process, 120.0)
    endpoint = wait_for_endpoint(endpoint_dir, timeout_s=30.0)
    local_driver = LocalUserDriver(token, endpoint)
    local_driver.preflight()
    rpc = authenticate_json_rpc(
        launcher_module.JsonRpcClient(),
        profile_root,
        json_rpc_error=launcher_module.JsonRpcError,
        json_rpc_transport_error=launcher_module.JsonRpcTransportError,
    )
    with contextlib.suppress(Exception):
        rpc.call("create_document", "ShutdownDisposable", timeout=30.0)

    return {
        "process": process,
        "profile_root": profile_root,
        "run_root": run_root,
        "endpoint_dir": endpoint_dir,
        "evidence_path": evidence_path,
        "local_driver": local_driver,
        "launcher_module": launcher_module,
        "launcher_log": launcher_log,
    }


def _assert_shutdown_evidence(shutdown: dict[str, object], *, forced: bool) -> None:
    for key in SHUTDOWN_TIMESTAMP_KEYS:
        assert key in shutdown
    assert shutdown.get("forced") is forced
    assert shutdown.get("deadline_seconds") == 60
    if not forced:
        assert shutdown.get("requested_utc")
        assert shutdown.get("documents_closed_utc")
        assert shutdown.get("rpc_admission_closed_utc")
        assert shutdown.get("window_closed_utc")
        assert shutdown.get("process_exit_utc")
        assert shutdown.get("stalled_stage") is None


@pytest.mark.parametrize("run_idx", range(REPEATED_RUNS))
def test_graceful_shutdown_forced_false_native(run_idx: int) -> None:
    if sys.platform == "win32":
        platform_label = "windows"
    elif sys.platform.startswith("linux"):
        platform_label = "linux"
    else:
        pytest.skip(f"unsupported platform for WP09 shutdown gate: {sys.platform}")

    session = _launch_owned_session()
    process = session["process"]
    try:
        result = graceful_shutdown_owned_session(
            process=process,
            profile_root=session["profile_root"],
            launcher_module=session["launcher_module"],
            repo_root=REPO_ROOT,
            mcp_port=session["launcher_module"].MCP_RPC_PORT,
            local_driver=session["local_driver"],
            evidence_path=session["evidence_path"],
        )
        shutdown = result["shutdown"]
        _assert_shutdown_evidence(shutdown, forced=False)
        assert result["success"] is True
        assert result["forced"] is False
        assert process.poll() is not None
        evidence = json.loads(session["evidence_path"].read_text(encoding="utf-8"))
        _assert_shutdown_evidence(evidence["shutdown"], forced=False)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)

    RESULTS_ROOT.mkdir(parents=True, exist_ok=True)
    marker = RESULTS_ROOT / f"shutdown-{platform_label}-run-{run_idx}.json"
    marker.write_text(
        json.dumps(
            {
                "platform": platform_label,
                "run_idx": run_idx,
                "forced": False,
                "PART3_RESULT": "PASSED",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )


def test_stall_without_window_close_fails_with_forced_true() -> None:
    freecad_exe = default_freecad_exe(REPO_ROOT)
    if freecad_exe is None:
        pytest.skip("FreeCAD GUI binary not found under build/release/bin")
    launcher_module = _load_launcher_module()
    if _mcp_endpoint_busy(launcher_module):
        pytest.skip(
            f"MCP port {launcher_module.MCP_RPC_PORT} is already occupied"
        )

    profile_root = Path(tempfile.mkdtemp(prefix="part3-shutdown-stall-"))
    run_root = profile_root / "run"
    run_root.mkdir(parents=True, exist_ok=True)
    endpoint_dir = run_root / "control"
    evidence_path = run_root / "evidence.json"
    evidence_path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "shutdown": empty_shutdown_record(),
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    token = generate_control_token()
    env = launch_env_for_isolated_profile(
        profile_root,
        control_token=token,
        endpoint_dir=endpoint_dir,
        repo_root=REPO_ROOT,
    )
    with (run_root / "launcher.log").open("wb") as log_handle:
        process = subprocess.Popen(
            [
                sys.executable,
                str(LAUNCHER),
                "--force-new",
                "--freecad",
                str(freecad_exe),
                "--mcp-timeout",
                "120",
                "--wait",
            ],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
        )
    try:
        _wait_for_mcp_ready(launcher_module, process, 120.0)
        endpoint = wait_for_endpoint(endpoint_dir, timeout_s=30.0)
        local_driver = LocalUserDriver(token, endpoint)
        local_driver.preflight()
        result = graceful_shutdown_owned_session(
            process=process,
            profile_root=profile_root,
            launcher_module=launcher_module,
            repo_root=REPO_ROOT,
            mcp_port=launcher_module.MCP_RPC_PORT,
            local_driver=local_driver,
            evidence_path=evidence_path,
            skip_window_close=True,
            skip_rpc_shutdown=True,
            skip_document_close=True,
        )
        assert result["success"] is False
        assert result["forced"] is True
        assert result["stalled_stage"] in {"window_close", "rpc_shutdown"}
        shutdown = result["shutdown"]
        assert shutdown.get("forced") is True
        assert shutdown.get("stalled_stage") in {"window_close", "rpc_shutdown"}
        assert shutdown.get("process_exit_utc")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
