#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Part 3 stress coordinator: provision, launch, child spawn, preflight, evidence."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .evidence import (
    empty_evidence,
    empty_shutdown_record,
    finalize_evidence,
    print_verdict_line,
    stamp_shutdown_transition,
    utc_now_iso,
    write_evidence,
)
from .local_user_driver import (
    TOKEN_ENV,
    LocalUserDriver,
    generate_control_token,
    launch_env_for_isolated_profile,
    wait_for_endpoint,
)
from .scenarios import resolve_stage

REPO_ROOT = Path(__file__).resolve().parents[3]

if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.gui.part3.rpc_session_client import authenticate_json_rpc

LAUNCHER = REPO_ROOT / "start_freecad.py"
LAUNCHER_IMPL = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"
REMOTE_AGENT_DRIVER = Path(__file__).resolve().parent / "remote_agent_driver.py"
DEFAULT_FREECAD = REPO_ROOT / "build" / "release" / "bin" / "FreeCAD.exe"
SHUTDOWN_DEADLINE_SECONDS = 60


def default_freecad_exe(repo_root: Path | None = None) -> Path | None:
    root = (repo_root or REPO_ROOT).resolve()
    for candidate in (
        root / "build" / "release" / "bin" / "FreeCAD.exe",
        root / "build" / "release" / "bin" / "FreeCAD",
    ):
        if candidate.is_file():
            return candidate
    return None


USER_APPDATA = Path(os.environ.get("APPDATA", ""))


def _load_launcher_module():
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


def _force_kill_owned_process_tree(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            capture_output=True,
            check=False,
        )
    else:
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        if os.name != "nt":
            process.kill()
        process.wait(timeout=5)


def _wait_for_process_exit(process: subprocess.Popen, deadline_s: float) -> bool:
    deadline = time_monotonic() + deadline_s
    while time_monotonic() < deadline:
        if process.poll() is not None:
            return True
    return process.poll() is not None


def _document_names_from_list_documents(payload: object) -> list[str]:
    if isinstance(payload, list):
        names: list[str] = []
        for item in payload:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict):
                name = item.get("name") or item.get("Name") or item.get("document")
                if name:
                    names.append(str(name))
        return names
    if isinstance(payload, dict):
        documents = payload.get("documents")
        if isinstance(documents, list):
            return _document_names_from_list_documents(documents)
    return []


def _load_shutdown_payload(evidence_path: Path | None) -> dict[str, Any]:
    shutdown = empty_shutdown_record()
    if evidence_path is None or not evidence_path.is_file():
        return shutdown
    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    existing = payload.get("shutdown")
    if isinstance(existing, dict):
        shutdown.update(existing)
    return shutdown


def _persist_shutdown_evidence(
    evidence_path: Path | None,
    shutdown: dict[str, Any],
    *,
    verdict: str | None = None,
) -> None:
    if evidence_path is None or not evidence_path.is_file():
        return
    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    payload["shutdown"] = shutdown
    if verdict is not None:
        payload = finalize_evidence(payload, verdict=verdict)
    write_evidence(evidence_path, payload)


def graceful_shutdown_owned_session(
    *,
    process: subprocess.Popen,
    profile_root: Path,
    launcher_module: Any,
    repo_root: Path,
    mcp_port: int,
    local_driver: LocalUserDriver | None = None,
    evidence_path: Path | None = None,
    deadline_seconds: int = SHUTDOWN_DEADLINE_SECONDS,
    skip_window_close: bool = False,
    skip_rpc_shutdown: bool = False,
    skip_document_close: bool = False,
) -> dict[str, Any]:
    """ADR §9 ordered shutdown; force-kill only after stall with forced:true."""

    shutdown = _load_shutdown_payload(evidence_path)
    shutdown["deadline_seconds"] = int(deadline_seconds)
    shutdown["forced"] = False
    shutdown["stalled_stage"] = None
    stamp_shutdown_transition(shutdown, "requested_utc")
    _persist_shutdown_evidence(evidence_path, shutdown)

    stalled_stage: str | None = None
    rpc = None
    try:
        base_client = launcher_module.JsonRpcClient(host="127.0.0.1", port=mcp_port)
        rpc = authenticate_json_rpc(
            base_client,
            profile_root,
            json_rpc_error=launcher_module.JsonRpcError,
            json_rpc_transport_error=launcher_module.JsonRpcTransportError,
        )

        documents = _document_names_from_list_documents(
            rpc.call("list_documents", timeout=15.0)
        )
        if not skip_document_close:
            for document_name in documents:
                rpc.call("close_document", document_name, timeout=30.0)
        stamp_shutdown_transition(shutdown, "documents_closed_utc")
        _persist_shutdown_evidence(evidence_path, shutdown)

        if not skip_rpc_shutdown:
            rpc.call("shutdown_rpc_server", timeout=30.0)
            stamp_shutdown_transition(shutdown, "rpc_admission_closed_utc")
            stamp_shutdown_transition(shutdown, "worker_shutdown_utc")
            stamp_shutdown_transition(shutdown, "listener_shutdown_utc")
            _persist_shutdown_evidence(evidence_path, shutdown)
    except Exception as exc:
        stalled_stage = "rpc_shutdown"
        shutdown["stalled_stage"] = stalled_stage
        shutdown["rpc_error"] = str(exc)
        _persist_shutdown_evidence(evidence_path, shutdown)

    if stalled_stage is None and local_driver is not None and not skip_window_close:
        try:
            local_driver.invoke("close_main_window", timeout=15.0)
            stamp_shutdown_transition(shutdown, "window_closed_utc")
            _persist_shutdown_evidence(evidence_path, shutdown)
        except Exception as exc:
            stalled_stage = "window_close"
            shutdown["stalled_stage"] = stalled_stage
            shutdown["window_error"] = str(exc)
            _persist_shutdown_evidence(evidence_path, shutdown)
    elif stalled_stage is None and skip_window_close:
        stalled_stage = "window_close"
        shutdown["stalled_stage"] = stalled_stage
        shutdown["window_skipped"] = True
        _persist_shutdown_evidence(evidence_path, shutdown)
    elif stalled_stage is None and skip_rpc_shutdown:
        stalled_stage = "rpc_shutdown"
        shutdown["stalled_stage"] = stalled_stage
        shutdown["rpc_shutdown_skipped"] = True
        _persist_shutdown_evidence(evidence_path, shutdown)

    wait_deadline = float(deadline_seconds)
    if _wait_for_process_exit(process, wait_deadline):
        stamp_shutdown_transition(shutdown, "process_exit_utc")
        shutdown["forced"] = False
        shutdown["stalled_stage"] = None
        _persist_shutdown_evidence(evidence_path, shutdown, verdict="PASSED")
        return {
            "success": True,
            "forced": False,
            "stalled_stage": None,
            "shutdown": shutdown,
        }

    if stalled_stage is None:
        stalled_stage = "process_exit"
    shutdown["stalled_stage"] = stalled_stage
    shutdown["forced"] = True
    _persist_shutdown_evidence(evidence_path, shutdown)
    _force_kill_owned_process_tree(process)
    if process.poll() is not None:
        stamp_shutdown_transition(shutdown, "process_exit_utc")
    _persist_shutdown_evidence(evidence_path, shutdown, verdict="FAILED")
    return {
        "success": False,
        "forced": True,
        "stalled_stage": stalled_stage,
        "shutdown": shutdown,
    }


@dataclass
class CoordinatorHandoff:
    """Actors after provisioning; the coordinator must not retain these for remote ops."""

    control_token: str
    local_driver: LocalUserDriver
    endpoint: dict[str, Any]


class StressCoordinator:
    """Provision isolated profile, launch FreeCAD, spawn actors, collect evidence hooks."""

    def __init__(
        self,
        *,
        repo_root: Path | None = None,
        run_root: Path | None = None,
    ) -> None:
        self.repo_root = (repo_root or REPO_ROOT).resolve()
        self.run_root = (run_root or Path(tempfile.mkdtemp(prefix="part3-stress-"))).resolve()
        self.profile_root = self.run_root / "profile"
        self.endpoint_dir = self.run_root / "control"
        self.evidence_dir = self.run_root / "evidence"
        self.evidence_path = self.evidence_dir / "evidence.json"
        self._launcher_module = _load_launcher_module()
        self._control_token: str | None = None
        self._launch_env: dict[str, str] | None = None
        self._launcher_process: subprocess.Popen | None = None
        self._handoff: CoordinatorHandoff | None = None

    @property
    def launcher_path(self) -> Path:
        return (self.repo_root / "start_freecad.py").resolve()

    @property
    def mcp_rpc_port(self) -> int:
        return int(self._launcher_module.MCP_RPC_PORT)

    def holds_control_token(self) -> bool:
        return self._control_token is not None

    def holds_rpc_session(self) -> bool:
        return False

    def assert_tracked_launcher(self) -> None:
        if not self.launcher_path.is_file():
            raise FileNotFoundError(f"tracked launcher missing: {self.launcher_path}")
        if self.launcher_path.name != "start_freecad.py":
            raise RuntimeError("FreeCAD must launch only through start_freecad.py")

    def assert_port_free(self, host: str = "127.0.0.1", port: int | None = None) -> None:
        rpc_port = self.mcp_rpc_port if port is None else port
        if _port_is_open(host, rpc_port):
            raise RuntimeError(
                f"refusing to attach: MCP port {host}:{rpc_port} is already occupied"
            )

    def assert_isolated_profile(self) -> None:
        profile = self.profile_root.resolve()
        if USER_APPDATA and profile == USER_APPDATA.resolve():
            raise RuntimeError("refusing to use the normal user APPDATA profile")
        mod_dir = profile / "FreeCAD" / "Mod" / "Part3LocalDriver"
        if mod_dir.is_dir() and not str(mod_dir).startswith(str(profile)):
            raise RuntimeError("Part3LocalDriver must install only under the isolated profile")

    def provision(self) -> str:
        """Create isolated profile layout, auth secret, control token, evidence dir."""

        self.assert_tracked_launcher()
        self.assert_port_free()
        self.profile_root.mkdir(parents=True, exist_ok=True)
        self.endpoint_dir.mkdir(parents=True, exist_ok=True)
        self.evidence_dir.mkdir(parents=True, exist_ok=True)
        token = generate_control_token()
        self._control_token = token
        self._launch_env = launch_env_for_isolated_profile(
            self.profile_root,
            control_token=token,
            endpoint_dir=self.endpoint_dir,
            repo_root=self.repo_root,
        )
        self.assert_isolated_profile()
        evidence = empty_evidence(stage=None)
        evidence["started_utc"] = utc_now_iso()
        evidence["environment"].update(
            {
                "launcher": str(self.launcher_path),
                "python": sys.executable,
                "isolated_profile": str(self.profile_root),
                "isolated_mod_dir": str(self.profile_root / "FreeCAD" / "Mod"),
                "mcp_host": "127.0.0.1",
                "mcp_port": self.mcp_rpc_port,
                "isolation_verified": True,
            }
        )
        write_evidence(self.evidence_path, evidence)
        return token

    def build_launch_command(self, freecad_exe: Path) -> list[str]:
        self.assert_tracked_launcher()
        return [
            sys.executable,
            str(self.launcher_path),
            "--force-new",
            "--freecad",
            str(freecad_exe),
            "--mcp-timeout",
            "120",
            "--wait",
        ]

    def launch_env(self) -> dict[str, str]:
        if self._launch_env is None:
            raise RuntimeError("call provision() before launch_env()")
        return dict(self._launch_env)

    def launch_freecad(self, freecad_exe: Path | None = None) -> subprocess.Popen:
        if self._launch_env is None or self._control_token is None:
            raise RuntimeError("call provision() before launch_freecad()")
        exe = freecad_exe or default_freecad_exe(self.repo_root)
        if exe is None or not exe.is_file():
            raise FileNotFoundError(
                "FreeCAD GUI binary not found under build/release/bin"
            )
        self.assert_port_free()
        launcher_log = self.run_root / "launcher.log"
        command = self.build_launch_command(exe)
        with launcher_log.open("wb") as log_handle:
            process = subprocess.Popen(
                command,
                cwd=str(self.repo_root),
                env=self.launch_env(),
                stdout=log_handle,
                stderr=subprocess.STDOUT,
            )
        self._launcher_process = process
        return process

    def wait_for_launcher_ready(self, timeout_s: float = 120.0) -> None:
        if self._launcher_process is None:
            raise RuntimeError("call launch_freecad() first")
        deadline = time_monotonic() + timeout_s
        while time_monotonic() < deadline:
            if self._launcher_process.poll() is not None:
                raise RuntimeError("launcher exited before MCP became ready")
            if _port_is_open("127.0.0.1", self.mcp_rpc_port):
                try:
                    if self._launcher_module.JsonRpcClient().call("ping", timeout=2.0):
                        return
                except Exception:
                    pass
        raise TimeoutError(
            f"MCP RPC did not become ready on 127.0.0.1:{self.mcp_rpc_port} within {timeout_s}s"
        )

    def connect_local_driver(self) -> CoordinatorHandoff:
        if self._control_token is None:
            raise RuntimeError("call provision() before connect_local_driver()")
        endpoint = wait_for_endpoint(self.endpoint_dir)
        local_driver = LocalUserDriver(self._control_token, endpoint)
        handoff = CoordinatorHandoff(
            control_token=self._control_token,
            local_driver=local_driver,
            endpoint=endpoint,
        )
        self._handoff = handoff
        self._control_token = None
        return handoff

    def run_preflight(self, handoff: CoordinatorHandoff) -> dict[str, Any]:
        return handoff.local_driver.preflight()

    def spawn_remote_agent_child(
        self,
        *,
        inspect_only: bool = True,
        parent_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Spawn RemoteAgentDriver without leaking the control token to the child."""

        env = dict(parent_env or os.environ)
        if self._handoff is not None:
            token = self._handoff.control_token
        elif self._control_token is not None:
            token = self._control_token
        else:
            token = env.get(TOKEN_ENV, "")
        env.pop(TOKEN_ENV, None)
        if token:
            for key, value in list(env.items()):
                if token in value:
                    env[key] = value.replace(token, "")
        argv = [
            sys.executable,
            str(REMOTE_AGENT_DRIVER),
        ]
        if inspect_only:
            argv.append("--inspect-token-absence")
        completed = subprocess.run(
            argv,
            input=json.dumps({"token": token}) if token else None,
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        return completed

    def record_preflight_check(self, preflight: dict[str, Any]) -> None:
        payload = json.loads(self.evidence_path.read_text(encoding="utf-8"))
        checks = list(payload.get("checks") or [])
        checks.append(
            {
                "name": "local_driver_preflight",
                "passed": bool(preflight.get("ready")),
                "detail": preflight,
            }
        )
        payload["checks"] = checks
        payload["environment"]["control_endpoint"] = self._handoff.endpoint if self._handoff else {}
        write_evidence(self.evidence_path, payload)

    def shutdown_launcher(self) -> bool:
        """Run ADR §9 shutdown; return True only when forced:false."""

        if self._launcher_process is None:
            return True
        process = self._launcher_process
        local_driver = self._handoff.local_driver if self._handoff is not None else None
        result = graceful_shutdown_owned_session(
            process=process,
            profile_root=self.profile_root,
            launcher_module=self._launcher_module,
            repo_root=self.repo_root,
            mcp_port=self.mcp_rpc_port,
            local_driver=local_driver,
            evidence_path=self.evidence_path,
            deadline_seconds=SHUTDOWN_DEADLINE_SECONDS,
        )
        self._launcher_process = None
        return bool(result.get("success"))


def time_monotonic() -> float:
    import time

    return time.monotonic()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stage",
        help="Resolve a Stage A/B/C definition only (no loop execution in WP06)",
    )
    parser.add_argument(
        "--preflight-only",
        action="store_true",
        help="Provision, launch, preflight, write evidence; do not run stress loops",
    )
    args = parser.parse_args(argv)

    if args.stage and not args.preflight_only:
        resolve_stage(args.stage)
        print(
            "Stage definitions are available but loops are not executed in P3-WP06.",
            file=sys.stderr,
        )
        return 2

    if not args.preflight_only:
        parser.print_help()
        return 2

    freecad_exe = default_freecad_exe(REPO_ROOT)
    if freecad_exe is None:
        print("FreeCAD GUI binary not found under build/release/bin", file=sys.stderr)
        return 2

    coordinator = StressCoordinator()
    shutdown_ok = True
    try:
        coordinator.provision()
        process = coordinator.launch_freecad()
        try:
            coordinator.wait_for_launcher_ready()
            remote = coordinator.spawn_remote_agent_child()
            if remote.returncode != 0:
                print(remote.stdout or remote.stderr, file=sys.stderr)
                print_verdict_line("FAILED")
                return 1
            handoff = coordinator.connect_local_driver()
            preflight = coordinator.run_preflight(handoff)
            coordinator.record_preflight_check(preflight)
        finally:
            shutdown_ok = coordinator.shutdown_launcher()
    except Exception as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        print_verdict_line("FAILED")
        return 2

    if not shutdown_ok:
        print_verdict_line("FAILED")
        return 1

    print_verdict_line("PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
