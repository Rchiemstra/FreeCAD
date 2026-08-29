# SPDX-License-Identifier: LGPL-2.1-or-later
"""Static architecture gate over tests/gui/part3/ (ADR §11.6)."""

from __future__ import annotations

import ast
import re
from pathlib import Path

import pytest

from tests.gui.part3.remote_agent_driver import (
    FORBIDDEN_REMOTE_AGENT_RPC_METHODS,
    REMOTE_AGENT_TYPED_RPC_ALLOWLIST,
)

pytestmark = pytest.mark.unit

PACKAGE_ROOT = Path(__file__).resolve().parent
REPO_ROOT = Path(__file__).resolve().parents[3]

FORBIDDEN_CALL_TOKENS = frozenset({"execute_code", "execute_code_async"})

FORBIDDEN_TRANSACTION_TOKENS = frozenset(
    {
        "openTransaction",
        "commitTransaction",
        "abortTransaction",
        "setTransactionMode",
    }
)

FORBIDDEN_HISTORY_IN_CODE_STRING = re.compile(
    r"(?:\.|\b)(?:newDocument|saveAs|saveWithOutcome|saveCopyWithOutcome|undo|redo|recompute)\s*\("
)

FORBIDDEN_PIVY_TOKENS = frozenset(
    {
        "pivy",
        "coin",
        "getCameraNode",
        "getSceneGraph",
    }
)

READINESS_SLEEP_ALLOWLIST = frozenset(
    {
        ("local_user_driver.py", "wait_for_endpoint"),
        ("conftest.py", "launch_freecad_gui_session"),
        ("conftest.py", "_wait_for_port_closed"),
        ("rpc_session_client.py", "authenticate_json_rpc"),
        ("local_driver/actions.py", "_wait_for_property_editor"),
    }
)

NEW_MODULES_MUST_NOT_SLEEP = frozenset(
    {
        "scenarios.py",
        "evidence.py",
        "remote_agent_driver.py",
        "stress_coordinator.py",
    }
)

POLICY_SOURCE_FILES = frozenset(
    {
        "test_part3_architecture.py",
        "remote_agent_driver.py",
    }
)

# Modules whose ``*.call(...)`` sites carry Part 3 typed RPC verbs. The stage
# path joined this set in P3-WP10: stress_coordinator.py now issues every
# acceptance-path RPC, so the ADR section 11.6 allowlist must reach it too
# (GRK-P3-081).
RPC_CALL_SITE_MODULES = frozenset(
    {
        "remote_agent_driver.py",
        "stress_coordinator.py",
    }
)

# Helpers that forward a verb to the typed client: name -> index of the
# positional argument that carries the RPC method name.
RPC_METHOD_FORWARDERS = {
    "_remote_action": 2,
    "_call_expecting_failure": 1,
}


def _python_sources() -> list[Path]:
    return sorted(
        path
        for path in PACKAGE_ROOT.rglob("*.py")
        if path.is_file() and "__pycache__" not in path.parts
    )


def _relative(path: Path) -> str:
    return path.relative_to(PACKAGE_ROOT).as_posix()


class _ArchitectureVisitor(ast.NodeVisitor):
    def __init__(self, rel: str) -> None:
        self.rel = rel
        self.function_stack: list[str] = []
        self.rpc_methods: set[str] = set()
        self.violations: dict[str, list[str]] = {
            "forbidden_calls": [],
            "transaction_literals": [],
            "history_code_strings": [],
            "pivy_tokens": [],
            "sync_sleeps": [],
            "remote_rpc_methods": [],
        }

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        self.function_stack.append(node.name)
        self.generic_visit(node)
        self.function_stack.pop()

    def visit_AsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self.function_stack.append(node.name)
        self.generic_visit(node)
        self.function_stack.pop()

    def visit_Call(self, node: ast.Call) -> None:
        name = self._call_name(node)
        if name in FORBIDDEN_CALL_TOKENS:
            self.violations["forbidden_calls"].append(
                f"{self.rel}:{node.lineno} calls {name}"
            )
        method_index = self._rpc_method_argument_index(name)
        if (
            self.rel in RPC_CALL_SITE_MODULES
            and method_index is not None
            and len(node.args) > method_index
            and isinstance(node.args[method_index], ast.Constant)
            and isinstance(node.args[method_index].value, str)
        ):
            method = node.args[method_index].value
            self.rpc_methods.add(method)
            if method in FORBIDDEN_REMOTE_AGENT_RPC_METHODS:
                self.violations["remote_rpc_methods"].append(
                    f"{self.rel}:{node.lineno} uses forbidden RPC {method!r}"
                )
            elif method not in REMOTE_AGENT_TYPED_RPC_ALLOWLIST:
                self.violations["remote_rpc_methods"].append(
                    f"{self.rel}:{node.lineno} uses unlisted RPC {method!r}"
                )
        if (
            isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "time"
            and node.func.attr == "sleep"
        ):
            func_name = self.function_stack[-1] if self.function_stack else "<module>"
            allow_key = (self.rel, func_name)
            if self.rel in NEW_MODULES_MUST_NOT_SLEEP:
                self.violations["sync_sleeps"].append(
                    f"{self.rel}:{node.lineno} time.sleep in {func_name}"
                )
            elif allow_key not in READINESS_SLEEP_ALLOWLIST:
                self.violations["sync_sleeps"].append(
                    f"{self.rel}:{node.lineno} time.sleep outside readiness allowlist "
                    f"({func_name})"
                )
        self.generic_visit(node)

    def visit_Constant(self, node: ast.Constant) -> None:
        if not isinstance(node.value, str):
            return
        if self.rel in POLICY_SOURCE_FILES:
            return
        literal = node.value
        for token in FORBIDDEN_TRANSACTION_TOKENS:
            if token in literal:
                self.violations["transaction_literals"].append(
                    f"{self.rel}:{node.lineno} literal contains {token}"
                )
        if FORBIDDEN_HISTORY_IN_CODE_STRING.search(literal):
            self.violations["history_code_strings"].append(
                f"{self.rel}:{node.lineno} history/lifecycle via code string"
            )
        for token in FORBIDDEN_PIVY_TOKENS:
            if token in literal:
                self.violations["pivy_tokens"].append(
                    f"{self.rel}:{node.lineno} literal contains {token}"
                )

    def visit_Name(self, node: ast.Name) -> None:
        if self.rel in POLICY_SOURCE_FILES:
            return
        if node.id in FORBIDDEN_PIVY_TOKENS:
            self.violations["pivy_tokens"].append(
                f"{self.rel}:{node.lineno} references {node.id}"
            )

    @staticmethod
    def _rpc_method_argument_index(name: str | None) -> int | None:
        """Index of the argument holding the RPC verb, or None for other calls."""

        if name == "call":
            return 0
        if name in RPC_METHOD_FORWARDERS:
            return RPC_METHOD_FORWARDERS[name]
        return None

    @staticmethod
    def _call_name(node: ast.Call) -> str | None:
        func = node.func
        if isinstance(func, ast.Name):
            return func.id
        if isinstance(func, ast.Attribute):
            return func.attr
        return None


def _scan_source(
    source: str,
    rel: str,
    *,
    filename: str | None = None,
) -> _ArchitectureVisitor:
    """Run the gate over one source text as if it were ``rel``."""

    tree = ast.parse(source, filename=filename or rel)
    visitor = _ArchitectureVisitor(rel)
    visitor.visit(tree)
    return visitor


def _scan_file(path: Path) -> dict[str, list[str]]:
    rel = _relative(path)
    source = path.read_text(encoding="utf-8")
    return _scan_source(source, rel, filename=str(path)).violations


def test_package_has_no_execute_code_or_async() -> None:
    violations: list[str] = []
    for path in _python_sources():
        violations.extend(_scan_file(path)["forbidden_calls"])
    assert violations == [], "\n".join(violations)


def test_package_has_no_transaction_literals() -> None:
    violations: list[str] = []
    for path in _python_sources():
        violations.extend(_scan_file(path)["transaction_literals"])
    assert violations == [], "\n".join(violations)


def test_package_has_no_history_via_code_strings() -> None:
    violations: list[str] = []
    for path in _python_sources():
        violations.extend(_scan_file(path)["history_code_strings"])
    assert violations == [], "\n".join(violations)


def test_package_has_no_pivy_or_coin_camera_nodes() -> None:
    violations: list[str] = []
    for path in _python_sources():
        violations.extend(_scan_file(path)["pivy_tokens"])
    assert violations == [], "\n".join(violations)


def test_package_has_no_actor_sync_sleeps() -> None:
    violations: list[str] = []
    for path in _python_sources():
        violations.extend(_scan_file(path)["sync_sleeps"])
    assert violations == [], "\n".join(violations)


def test_remote_agent_driver_methods_are_allowlisted() -> None:
    result = _scan_file(PACKAGE_ROOT / "remote_agent_driver.py")
    assert result["remote_rpc_methods"] == [], "\n".join(result["remote_rpc_methods"])
    assert REMOTE_AGENT_TYPED_RPC_ALLOWLIST.isdisjoint(FORBIDDEN_REMOTE_AGENT_RPC_METHODS)


def test_stage_path_rpc_methods_are_allowlisted() -> None:
    """ADR section 11.6 must reach the module that issues the stage's RPC.

    stress_coordinator.py now carries every acceptance-path call, directly and
    through ``_remote_action`` / ``_call_expecting_failure``. The allowlist is
    not widened for it: the verbs it already uses are checked against the same
    allowlist and the same forbidden set as remote_agent_driver.py.
    """

    path = PACKAGE_ROOT / "stress_coordinator.py"
    visitor = _scan_source(
        path.read_text(encoding="utf-8"),
        "stress_coordinator.py",
        filename=str(path),
    )
    assert visitor.rpc_methods, "no RPC method literal was extracted from the stage path"
    unlisted = sorted(visitor.rpc_methods - REMOTE_AGENT_TYPED_RPC_ALLOWLIST)
    assert unlisted == [], unlisted
    assert visitor.rpc_methods.isdisjoint(FORBIDDEN_REMOTE_AGENT_RPC_METHODS)
    assert visitor.violations["remote_rpc_methods"] == []

    # The gate must bite on the stage path, at a direct call site and at both
    # forwarders, exactly as it does for remote_agent_driver.py.
    fixture = (
        'rpc.call("execute_code", {})\n'
        '_remote_action(context, cycle, "get_gui_state", {})\n'
        '_call_expecting_failure(context, "totally_unknown_method", {})\n'
    )
    bitten = _scan_source(fixture, "stress_coordinator.py")
    reported = bitten.violations["remote_rpc_methods"]
    assert len(reported) == 3, reported
    assert "forbidden RPC 'execute_code'" in reported[0], reported
    assert "forbidden RPC 'get_gui_state'" in reported[1], reported
    assert "unlisted RPC 'totally_unknown_method'" in reported[2], reported


def test_retired_harness_is_absent() -> None:
    retired = REPO_ROOT / "tests" / "gui" / "collaboration_gui_stress.py"
    assert not retired.is_file()


FORCE_KILL_HELPER_NAMES = frozenset(
    {
        "_force_kill_owned_process_tree",
        "_terminate_owned_process_tree",
    }
)


def _taskkill_force_call_sites(path: Path) -> list[tuple[str, int]]:
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    sites: list[tuple[str, int]] = []

    class _Visitor(ast.NodeVisitor):
        def __init__(self) -> None:
            self.function_stack: list[str] = []

        def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
            self.function_stack.append(node.name)
            self.generic_visit(node)
            self.function_stack.pop()

        def visit_Call(self, node: ast.Call) -> None:
            args: list[str] = []
            for arg in node.args:
                if isinstance(arg, ast.Constant) and isinstance(arg.value, str):
                    args.append(arg.value)
            if any("taskkill" in value for value in args) and any(
                "/F" in value or value == "/F" for value in args
            ):
                func_name = self.function_stack[-1] if self.function_stack else "<module>"
                sites.append((func_name, node.lineno))
            self.generic_visit(node)

    _Visitor().visit(tree)
    return sites


def test_taskkill_force_confined_to_last_resort_helper() -> None:
    violations: list[str] = []
    for rel in ("stress_coordinator.py", "conftest.py"):
        path = PACKAGE_ROOT / rel
        for func_name, line in _taskkill_force_call_sites(path):
            if func_name not in FORCE_KILL_HELPER_NAMES:
                violations.append(
                    f"{rel}:{line} taskkill /F outside last-resort helper ({func_name})"
                )
    assert violations == [], "\n".join(violations)


def test_shutdown_success_path_uses_graceful_sequence() -> None:
    path = PACKAGE_ROOT / "stress_coordinator.py"
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))

    graceful_fn: ast.FunctionDef | None = None
    shutdown_launcher_fn: ast.FunctionDef | None = None
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == "graceful_shutdown_owned_session":
            graceful_fn = node
        if isinstance(node, ast.ClassDef) and node.name == "StressCoordinator":
            for item in node.body:
                if isinstance(item, ast.FunctionDef) and item.name == "shutdown_launcher":
                    shutdown_launcher_fn = item

    assert graceful_fn is not None, "graceful_shutdown_owned_session missing"
    graceful_body = ast.get_source_segment(source, graceful_fn) or ""
    assert "shutdown_rpc_server" in graceful_body
    assert "close_main_window" in graceful_body
    assert "_force_kill_owned_process_tree" in graceful_body

    assert shutdown_launcher_fn is not None, "StressCoordinator.shutdown_launcher missing"
    launcher_body = ast.get_source_segment(source, shutdown_launcher_fn) or ""
    assert "graceful_shutdown_owned_session" in launcher_body
    assert "_force_kill_owned_process_tree" not in launcher_body


STAGE_C_EXECUTION_TOKENS = frozenset({"resolve_stage"})

PAUSE_FALLBACK_TOKENS = frozenset(
    {
        "automation_pause",
        "admit_remote_write",
        "request_local_pause_after_current",
        "resume_local_agent_writes",
    }
)


def _module_function_source(path: Path, name: str) -> str:
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return ast.get_source_segment(source, node) or ""
    raise AssertionError(f"{path.name} does not define {name}")


def test_stage_execute_path_resolves_only_executable_stages() -> None:
    """Stage C stays defined for P3-WP11 but is unreachable from the execute path."""

    path = PACKAGE_ROOT / "stress_coordinator.py"
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))

    called: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            name = _ArchitectureVisitor._call_name(node)
            if name in STAGE_C_EXECUTION_TOKENS:
                called.append(f"stress_coordinator.py:{node.lineno} calls {name}")
    assert called == [], "; ".join(called)

    run_stage_body = _module_function_source(path, "run_stage")
    assert "resolve_executable_stage" in run_stage_body
    main_body = _module_function_source(path, "main")
    assert "resolve_executable_stage" in main_body


def test_stage_result_is_bound_to_graceful_shutdown() -> None:
    """A forced kill can never be reported as a green stage."""

    path = PACKAGE_ROOT / "stress_coordinator.py"
    verdict_body = _module_function_source(path, "stage_verdict")
    assert "if not stage_ok or not shutdown_ok:" in verdict_body
    assert 'return "FAILED"' in verdict_body

    run_stage_body = _module_function_source(path, "run_stage")
    assert "stage_verdict(" in run_stage_body
    assert "shutdown_launcher(" in run_stage_body
    assert "_force_kill_owned_process_tree" not in run_stage_body


def test_stage_path_has_no_pause_checkbox_fallback() -> None:
    """ADR section 7: acceptance pause/resume is the real checkbox or nothing."""

    path = PACKAGE_ROOT / "stress_coordinator.py"
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))
    violations: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            for token in PAUSE_FALLBACK_TOKENS:
                if token in node.value:
                    violations.append(
                        f"stress_coordinator.py:{node.lineno} pause fallback {token}"
                    )
    assert violations == [], "; ".join(violations)
    assert '"pause_writes"' in source
    assert '"resume_writes"' in source
