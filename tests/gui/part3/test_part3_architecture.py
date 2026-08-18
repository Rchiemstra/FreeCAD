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
        if (
            self.rel == "remote_agent_driver.py"
            and name == "call"
            and node.args
            and isinstance(node.args[0], ast.Constant)
            and isinstance(node.args[0].value, str)
        ):
            method = node.args[0].value
            if method not in REMOTE_AGENT_TYPED_RPC_ALLOWLIST:
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
    def _call_name(node: ast.Call) -> str | None:
        func = node.func
        if isinstance(func, ast.Name):
            return func.id
        if isinstance(func, ast.Attribute):
            return func.attr
        return None


def _scan_file(path: Path) -> dict[str, list[str]]:
    rel = _relative(path)
    source = path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(path))
    visitor = _ArchitectureVisitor(rel)
    visitor.visit(tree)
    return visitor.violations


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
