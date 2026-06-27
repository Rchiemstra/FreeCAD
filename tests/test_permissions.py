"""Tests for bridge.permissions write policy."""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from bridge.permissions import (
    PermissionDenied,
    WriteOperation,
    assert_write_allowed,
    effective_write_policy,
    list_write_capabilities,
)


@pytest.fixture(autouse=True)
def _clear_policy_env(monkeypatch):
    monkeypatch.delenv("BRIDGE_WRITE_POLICY", raising=False)
    monkeypatch.delenv("CI", raising=False)


def test_registry_lists_all_operations():
    caps = list_write_capabilities()
    ops = {c["operation"] for c in caps}
    assert ops == {o.value for o in WriteOperation}


def test_allow_policy_permits_writes():
    os.environ["BRIDGE_WRITE_POLICY"] = "allow"
    assert effective_write_policy() == "allow"
    assert_write_allowed(WriteOperation.GAZEBO_SPAWN)


def test_deny_policy_blocks_writes():
    os.environ["BRIDGE_WRITE_POLICY"] = "deny"
    with pytest.raises(PermissionDenied):
        assert_write_allowed(WriteOperation.CAD_EXPORT_URDF)


def test_ci_defaults_to_generated_only(monkeypatch):
    monkeypatch.setenv("CI", "true")
    assert effective_write_policy() == "generated_only"
    assert_write_allowed(
        WriteOperation.CAD_EXPORT_URDF,
        target=Path("generated/arm_2dof"),
    )


def test_generated_only_blocks_robots_path():
    os.environ["BRIDGE_WRITE_POLICY"] = "generated_only"
    with pytest.raises(PermissionDenied):
        assert_write_allowed(
            WriteOperation.CAD_EXPORT_URDF,
            target=Path("robots/arm_2dof.urdf"),
        )


def test_export_urdf_respects_deny(tmp_path, monkeypatch):
    os.environ["BRIDGE_WRITE_POLICY"] = "deny"
    from bridge.freecad_bridge import export_urdf

    out = tmp_path / "generated" / "arm_2dof"
    out.mkdir(parents=True)
    result = export_urdf("arm_2dof", out)
    assert not result.ok
    assert "Write blocked" in result.messages[0]
