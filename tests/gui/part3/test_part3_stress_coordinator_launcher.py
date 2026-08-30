# SPDX-License-Identifier: LGPL-2.1-or-later
"""Launcher and isolation gates for StressCoordinator.

Provisioning, isolation and typed-verb policy only. The Stage A/B execute path
and its evidence live in ``test_part3_stage_acceptance.py``; Stage C belongs to
P3-WP11 and is not executable from here.
"""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

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
from tests.gui.part3.scenarios import (
    EXECUTABLE_STAGES,
    STAGE_A,
    STAGE_B,
    STAGE_C,
    resolve_executable_stage,
)
from tests.gui.part3.stress_coordinator import CoordinatorHandoff, StressCoordinator

pytestmark = pytest.mark.unit

REPO_ROOT = Path(__file__).resolve().parents[3]
REMOTE_AGENT_DRIVER = Path(__file__).resolve().parent / "remote_agent_driver.py"


def test_stage_definitions_match_adr_section_13() -> None:
    assert STAGE_A.view_mutation_cycles == 10 and STAGE_A.save_cycles == 5
    assert STAGE_B.view_mutation_cycles == 50 and STAGE_B.save_cycles == 20
    assert STAGE_C.view_mutation_cycles == 500 and STAGE_C.save_cycles == 100


def test_only_stage_a_and_b_are_executable_here() -> None:
    assert EXECUTABLE_STAGES == frozenset({STAGE_A.stage, STAGE_B.stage})
    assert resolve_executable_stage("a") is STAGE_A
    assert resolve_executable_stage("b") is STAGE_B
    with pytest.raises(ValueError, match="not executable in P3-WP10"):
        resolve_executable_stage(STAGE_C.stage)


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
    config_root = coordinator.profile_root / "config"
    cache_root = coordinator.profile_root / "cache"
    assert env["HOME"] == str(coordinator.profile_root)
    assert env["APPDATA"] == str(coordinator.profile_root)
    assert env["XDG_CONFIG_HOME"] == str(config_root)
    assert env["XDG_CACHE_HOME"] == str(cache_root)
    assert env["XDG_DATA_HOME"] == str(coordinator.profile_root)
    assert config_root.is_dir()
    assert cache_root.is_dir()
    for key in ("HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"):
        assert Path(env[key]).resolve().is_relative_to(coordinator.profile_root.resolve())
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


def test_coordinator_child_absence_proof_never_sends_bearer_on_stdin(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    coordinator = StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    token = coordinator.provision()
    captured: dict[str, object] = {}
    def fake_run(*_args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        captured.update(kwargs)
        return subprocess.CompletedProcess([], 0, '{"absent": true}', "")
    monkeypatch.setattr(module.subprocess, "run", fake_run)
    completed = coordinator.spawn_remote_agent_child(parent_env=coordinator.launch_env())
    assert completed.returncode == 0
    stdin_payload = json.loads(str(captured["input"]))
    assert stdin_payload["token"] == module.TOKEN_ABSENCE_SENTINEL
    assert token not in str(captured["input"])


@pytest.mark.parametrize(
    "result",
    [
        subprocess.CompletedProcess(["powershell"], 1, "", "access denied"),
        subprocess.CompletedProcess(["powershell"], 0, "not-json", ""),
    ],
)
def test_windows_owned_inventory_is_fail_closed_on_ambiguous_output(
    monkeypatch: pytest.MonkeyPatch, result: subprocess.CompletedProcess[str]
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    monkeypatch.setattr(module.subprocess, "run", lambda *_args, **_kwargs: result)
    inventory = module._windows_owned_pid_inventory(1234)
    assert inventory["complete"] is False
    assert inventory["existing"] == []


def test_windows_owned_inventory_is_fail_closed_on_timeout(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    def timeout(*_args: object, **_kwargs: object) -> subprocess.CompletedProcess[str]:
        raise subprocess.TimeoutExpired("powershell", 10)

    monkeypatch.setattr(module.subprocess, "run", timeout)
    inventory = module._windows_owned_pid_inventory(1234)
    assert inventory["complete"] is False
    assert inventory["timed_out"] is True


def test_windows_forced_cleanup_queries_each_initial_descendant_after_taskkill(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    class Process:
        pid = 1234
        exited = False

        def poll(self):
            return 0 if self.exited else None

    process = Process()
    calls: list[list[int] | None] = []

    def inventory(_root: int, *, exact_pids: list[int] | None = None):
        calls.append(exact_pids)
        if exact_pids is None:
            return {"complete": True, "existing": [1234, 5678], "diagnostics": ""}
        return {"complete": True, "existing": [5678], "queried_pids": exact_pids, "diagnostics": ""}

    def taskkill(*_args: object, **_kwargs: object):
        process.exited = True
        return subprocess.CompletedProcess(["taskkill"], 0, "", "")

    monkeypatch.setattr(module.os, "name", "nt")
    monkeypatch.setattr(module, "_windows_owned_pid_inventory", inventory)
    monkeypatch.setattr(module.subprocess, "run", taskkill)
    result = module._force_kill_owned_process_tree(process)
    assert calls == [None, [1234, 5678]]
    assert result["passed"] is False
    assert result["aftermath"]["existing"] == [5678]


def test_posix_owned_group_inventory_filters_exact_process_group(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    completed = subprocess.CompletedProcess(
        ["ps"], 0, " 100 100\n 101 100\n 200 200\n", ""
    )
    monkeypatch.setattr(module.subprocess, "run", lambda *_args, **_kwargs: completed)
    inventory = module._posix_owned_process_group_inventory(100)
    assert inventory["complete"] is True
    assert inventory["existing"] == [100, 101]
    assert inventory["process_group_id"] == 100


def test_posix_forced_cleanup_signals_and_verifies_whole_owned_group(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    class Process:
        pid = 1234
        exited = False

        def poll(self):
            return 0 if self.exited else None

    process = Process()
    inventories = iter(
        [
            {
                "complete": True,
                "existing": [1234, 5678],
                "process_group_id": 1234,
                "diagnostics": "",
            },
            {
                "complete": True,
                "existing": [],
                "process_group_id": 1234,
                "diagnostics": "",
            },
        ]
    )
    signals: list[tuple[int, int]] = []

    def killpg(process_group_id: int, sent_signal: int) -> None:
        signals.append((process_group_id, sent_signal))
        process.exited = True

    monkeypatch.setattr(
        module,
        "os",
        SimpleNamespace(name="posix", killpg=killpg),
    )
    monkeypatch.setattr(
        module, "_posix_owned_process_group_inventory", lambda _pgid: next(inventories)
    )
    result = module._force_kill_owned_process_tree(process)
    assert signals == [(1234, signal.SIGTERM)]
    assert result["passed"] is True
    assert result["initial"]["existing"] == [1234, 5678]
    assert result["aftermath"]["existing"] == []


def test_close_main_window_acknowledges_before_scheduled_close(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3.local_driver import actions

    callbacks: list[object] = []

    class MainWindow:
        close_calls = 0

        def close(self) -> None:
            self.close_calls += 1

    main_window = MainWindow()

    class QTimer:
        @staticmethod
        def singleShot(delay_ms: int, callback: object) -> None:
            callbacks.append((delay_ms, callback))

    monkeypatch.setitem(
        sys.modules,
        "FreeCADGui",
        SimpleNamespace(getMainWindow=lambda: main_window),
    )
    monkeypatch.setitem(
        sys.modules,
        "PySide",
        SimpleNamespace(QtCore=SimpleNamespace(QTimer=QTimer)),
    )
    result = actions._close_main_window({})
    assert result == {"close_scheduled": True, "delay_ms": 250}
    assert main_window.close_calls == 0
    delay_ms, callback = callbacks.pop()
    assert delay_ms == 250
    callback()
    assert main_window.close_calls == 1


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


def test_coordinator_reports_its_real_token_and_session_state(tmp_path: Path) -> None:
    """The boundary predicates must report state, never a constant (GRK-P3-076).

    ADR section 1 wants the authenticated typed session in the RemoteAgentDriver
    child. The P3-WP10 Stage A/B path keeps it in the coordinator process, an
    accepted deviation recorded in ADR section 1.4, so ``holds_rpc_session`` has
    to answer truthfully: False before the coordinator authenticates, True while
    it holds a session, False again once it releases it at shutdown. A predicate
    stubbed to a constant - True or False - fails this test.
    """

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

    # Handing the coordinator a session must flip the predicate. The injected
    # authenticator only replaces the network handshake; the retention and the
    # predicate under test are the real ones.
    sentinel = object()
    session = coordinator.authenticate_typed_session(
        authenticator=lambda *_args, **_kwargs: sentinel
    )
    assert session is sentinel
    assert coordinator.holds_rpc_session() is True
    assert coordinator.holds_control_token() is False

    coordinator.release_typed_session()
    assert coordinator.holds_rpc_session() is False


def test_forbidden_methods_disjoint_from_allowlist() -> None:
    assert FORBIDDEN_REMOTE_AGENT_RPC_METHODS.isdisjoint(REMOTE_AGENT_TYPED_RPC_ALLOWLIST)
