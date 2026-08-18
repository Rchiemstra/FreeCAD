# SPDX-License-Identifier: LGPL-2.1-or-later
"""Launcher and isolation gates for StressCoordinator (WP06; not Stage A/B/C)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from tests.gui.part3.local_user_driver import (
    MCP_SECRET_FILENAME,
    TOKEN_ENV,
    LocalUserDriver,
    ensure_disposable_profile_auth_secret,
    generate_control_token,
)
from tests.gui.part3.remote_agent_driver import (
    FORBIDDEN_REMOTE_AGENT_RPC_METHODS,
    REMOTE_AGENT_TYPED_RPC_ALLOWLIST,
    validate_remote_method,
)
from tests.gui.part3.scenarios import STAGE_A, STAGE_B, STAGE_C
from tests.gui.part3.stress_coordinator import CoordinatorHandoff, StressCoordinator

pytestmark = pytest.mark.unit

REPO_ROOT = Path(__file__).resolve().parents[3]
REMOTE_AGENT_DRIVER = Path(__file__).resolve().parent / "remote_agent_driver.py"


def test_stage_definitions_match_adr_section_13() -> None:
    assert STAGE_A.view_mutation_cycles == 10 and STAGE_A.save_cycles == 5
    assert STAGE_B.view_mutation_cycles == 50 and STAGE_B.save_cycles == 20
    assert STAGE_C.view_mutation_cycles == 500 and STAGE_C.save_cycles == 100


def test_coordinator_uses_tracked_start_freecad_launcher(tmp_path: Path) -> None:
    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    coordinator.provision()
    command = coordinator.build_launch_command(
        REPO_ROOT / "build" / "release" / "bin" / "FreeCAD.exe"
    )
    assert command[1].endswith("start_freecad.py")
    assert Path(command[1]).resolve() == coordinator.launcher_path
    assert "--wait" in command


def test_coordinator_launch_env_uses_isolated_profile(tmp_path: Path) -> None:
    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    coordinator.provision()
    env = coordinator.launch_env()
    assert env["APPDATA"] == str(coordinator.profile_root)
    assert env[TOKEN_ENV]
    assert (coordinator.profile_root / "FreeCAD" / "Mod" / "Part3LocalDriver").exists()


def test_disposable_profile_auth_secret_is_owner_only(tmp_path: Path) -> None:
    profile_root = tmp_path / "appdata"
    secret_path = ensure_disposable_profile_auth_secret(profile_root)
    assert secret_path == profile_root / "FreeCAD" / MCP_SECRET_FILENAME
    assert secret_path.stat().st_size == 32
    if os.name == "nt":
        return

    assert (secret_path.stat().st_mode & 0o777) == 0o600
    secret_path.chmod(0o644)
    ensure_disposable_profile_auth_secret(profile_root)
    assert (secret_path.stat().st_mode & 0o777) == 0o600


def test_coordinator_refuses_occupied_mcp_port(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)

    def always_open(_host: str, _port: int) -> bool:
        return True

    monkeypatch.setattr(
        "tests.gui.part3.stress_coordinator._port_is_open",
        always_open,
    )
    with pytest.raises(RuntimeError, match="already occupied"):
        coordinator.assert_port_free()


def test_coordinator_spawn_strips_control_token_from_remote_child(tmp_path: Path) -> None:
    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    token = coordinator.provision()
    parent_env = coordinator.launch_env()
    assert parent_env[TOKEN_ENV] == token
    completed = coordinator.spawn_remote_agent_child(parent_env=parent_env)
    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert report["absent"] is True


def test_remote_child_inspect_with_known_token_value() -> None:
    token = generate_control_token()
    env = dict(os.environ)
    env.pop(TOKEN_ENV, None)
    completed = subprocess.run(
        [
            sys.executable,
            str(REMOTE_AGENT_DRIVER),
            "--inspect-token-absence",
        ],
        input=json.dumps({"token": token}),
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert report["absent"] is True


def test_remote_agent_refuses_execute_code() -> None:
    with pytest.raises(ValueError, match="execute_code"):
        validate_remote_method("execute_code")


def test_remote_agent_refuses_remote_pause_resume_methods() -> None:
    for method in (
        "request_local_pause_after_current",
        "resume_local_agent_writes",
    ):
        with pytest.raises(ValueError, match=method):
            validate_remote_method(method)


def test_remote_agent_refuses_unlisted_method() -> None:
    with pytest.raises(ValueError, match="unlisted"):
        validate_remote_method("totally_unknown_method")


def test_remote_agent_cli_refuses_execute_code() -> None:
    completed = subprocess.run(
        [
            sys.executable,
            str(REMOTE_AGENT_DRIVER),
            "--method",
            "execute_code",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 2
    payload = json.loads(completed.stdout)
    assert payload["success"] is False
    assert "execute_code" in payload["error"]


def test_coordinator_holds_neither_token_nor_rpc_after_handoff(tmp_path: Path) -> None:
    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    coordinator.provision()
    assert coordinator.holds_control_token()
    assert coordinator.holds_rpc_session() is False
    token = coordinator._control_token or ""
    endpoint = {
        "host": "127.0.0.1",
        "port": 54321,
        "path": "/action",
        "ready": True,
    }
    coordinator._handoff = CoordinatorHandoff(
        control_token=token,
        local_driver=LocalUserDriver(token, endpoint),
        endpoint=endpoint,
    )
    coordinator._control_token = None
    assert coordinator.holds_control_token() is False
    assert coordinator.holds_rpc_session() is False


def test_forbidden_methods_disjoint_from_allowlist() -> None:
    assert FORBIDDEN_REMOTE_AGENT_RPC_METHODS.isdisjoint(REMOTE_AGENT_TYPED_RPC_ALLOWLIST)
