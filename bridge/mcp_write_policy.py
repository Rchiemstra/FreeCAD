# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Shared MCP / filesystem write policy for ``bridge/``.

Loads ``config/mcp_permissions.yaml`` and provides:

- Absolute-path checks for bridge-local writes (``sim_runs/``, ``generated/``).
- Optional denial of mutating *gazebo-mcp* stdio tool calls (``BRIDGE_MCP_DENY_MUTATING=1``).
- Path-argument validation for MCP tools that accept filesystem paths.

Upstream MCP servers (gazebo-mcp, freecad-mcp, ros-mcp) are unchanged; enforcement
happens at the SimWorkbench / handoff bridge boundary.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, FrozenSet, Mapping, Optional, Sequence, Tuple

import yaml

_REPO_ROOT = Path(__file__).resolve().parents[1]
_CONFIG = _REPO_ROOT / "config" / "mcp_permissions.yaml"


class MCPWritePolicyError(RuntimeError):
    """Base class for policy violations."""


class MCPFilesystemWriteDenied(MCPWritePolicyError):
    """Local or MCP-requested write outside approved directories."""


class MCPMutatingToolDenied(MCPWritePolicyError):
    """Mutating gazebo-mcp tool blocked by deny policy."""


class MCPRepoReadDenied(MCPWritePolicyError):
    """Model/world file read outside repository root."""


@dataclass(frozen=True)
class MCPWritePolicy:
    repo_root: Path
    deny_mutating_mcp: bool
    read_only_tools: FrozenSet[str]
    allowed_write_roots: Tuple[Path, ...]
    extra_write_roots: Tuple[Path, ...]
    path_args_write: Mapping[str, Sequence[str]]
    path_args_read_repo: Mapping[str, Sequence[str]]
    enforce_repo_model_reads: bool


def _parse_bool(val: str) -> bool:
    return val.strip().lower() in ("1", "true", "yes", "on")


def _extra_write_roots_from_env() -> Tuple[Path, ...]:
    raw = os.environ.get("BRIDGE_MCP_EXTRA_WRITE_ROOTS", "").strip()
    if not raw:
        return ()
    roots: list[Path] = []
    for part in raw.replace("|", ";").split(";"):
        p = part.strip()
        if not p:
            continue
        roots.append(Path(p).expanduser().resolve(strict=False))
    return tuple(roots)


def _resolve_under_repo(repo_root: Path, relative_name: str) -> Path:
    return (repo_root / relative_name).resolve(strict=False)


def _is_under(parent: Path, child: Path) -> bool:
    try:
        child.relative_to(parent)
        return True
    except ValueError:
        return False


def _load_yaml() -> dict:
    if not _CONFIG.is_file():
        raise MCPWritePolicyError(f"Missing policy file: {_CONFIG}")
    data = yaml.safe_load(_CONFIG.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise MCPWritePolicyError(f"Invalid YAML in {_CONFIG}")
    return data


@dataclass
class _PolicyCache:
    repo_root: Path
    deny_env: str
    extra_roots_env: str
    policy: MCPWritePolicy


_policy_cache: Optional[_PolicyCache] = None


def _build_policy(repo_root: Path) -> MCPWritePolicy:
    data = _load_yaml()

    deny = bool(data.get("deny_mutating_mcp_tools", False))
    env = os.environ.get("BRIDGE_MCP_DENY_MUTATING", "").strip()
    if env:
        deny = _parse_bool(env)

    rel_names = data.get("allowed_write_roots") or ["sim_runs", "generated"]
    if not isinstance(rel_names, list):
        raise MCPWritePolicyError("allowed_write_roots must be a list")
    allowed = tuple(_resolve_under_repo(repo_root, str(x)) for x in rel_names)

    ro = data.get("gazebo_mcp_read_only_tools") or []
    if not isinstance(ro, list):
        raise MCPWritePolicyError("gazebo_mcp_read_only_tools must be a list")
    read_only = frozenset(str(x) for x in ro)

    pa = data.get("gazebo_mcp_path_args") or {}
    write_map: Dict[str, Tuple[str, ...]] = {}
    read_map: Dict[str, Tuple[str, ...]] = {}
    if isinstance(pa, dict):
        w = pa.get("write_under_allowed_roots") or {}
        if isinstance(w, dict):
            for tool, keys in w.items():
                if isinstance(keys, list):
                    write_map[str(tool)] = tuple(str(k) for k in keys)
        r = pa.get("read_under_repo_only") or {}
        if isinstance(r, dict):
            for tool, keys in r.items():
                if isinstance(keys, list):
                    read_map[str(tool)] = tuple(str(k) for k in keys)

    enforce_reads = bool(data.get("enforce_repo_bounded_model_reads", True))

    return MCPWritePolicy(
        repo_root=repo_root,
        deny_mutating_mcp=deny,
        read_only_tools=read_only,
        allowed_write_roots=allowed,
        extra_write_roots=_extra_write_roots_from_env(),
        path_args_write=write_map,
        path_args_read_repo=read_map,
        enforce_repo_model_reads=enforce_reads,
    )


def load_mcp_write_policy(repo_root: Optional[Path] = None) -> MCPWritePolicy:
    """Load the policy (cached until repo root or relevant env vars change)."""
    global _policy_cache
    root = (repo_root or _REPO_ROOT).resolve(strict=False)
    deny_env = os.environ.get("BRIDGE_MCP_DENY_MUTATING", "")
    extra_roots_env = os.environ.get("BRIDGE_MCP_EXTRA_WRITE_ROOTS", "")
    if (
        _policy_cache is None
        or _policy_cache.repo_root != root
        or _policy_cache.deny_env != deny_env
        or _policy_cache.extra_roots_env != extra_roots_env
    ):
        _policy_cache = _PolicyCache(
            repo_root=root,
            deny_env=deny_env,
            extra_roots_env=extra_roots_env,
            policy=_build_policy(root),
        )
    return _policy_cache.policy


def reload_mcp_write_policy_for_tests() -> None:
    """Clear the policy cache (tests only)."""
    global _policy_cache
    _policy_cache = None


def is_gazebo_mcp_read_only(tool_name: str, policy: Optional[MCPWritePolicy] = None) -> bool:
    pol = policy or load_mcp_write_policy()
    return tool_name in pol.read_only_tools


def ensure_allowed_write_path(
    path: Path | str,
    policy: Optional[MCPWritePolicy] = None,
) -> Path:
    """
    Resolve *path* and ensure it lies under an allowed write root (or extra env roots).

    Used for bridge-local writes (screenshots, staged exports).
    """
    pol = policy or load_mcp_write_policy()
    target = Path(path).expanduser()
    try:
        resolved = target.resolve()
    except OSError:
        resolved = target.resolve(strict=False)

    candidates = pol.allowed_write_roots + pol.extra_write_roots
    for root in candidates:
        if _is_under(root, resolved) or resolved == root:
            return resolved

    roots = ", ".join(str(r) for r in candidates)
    try:
        from bridge.structured_log import append_event

        append_event(
            {
                "event": "permission_check",
                "decision": "denied_write_path",
                "component": "bridge.mcp_write_policy",
                "path": str(resolved),
            }
        )
    except Exception:
        pass
    raise MCPFilesystemWriteDenied(
        f"Write path outside approved roots ({roots}): {resolved}"
    )


def ensure_repo_read_path(path: Path | str, policy: Optional[MCPWritePolicy] = None) -> Path:
    """Resolve *path* and ensure it is inside ``policy.repo_root`` (model / world inputs)."""
    pol = policy or load_mcp_write_policy()
    p = Path(path).expanduser()
    try:
        resolved = p.resolve()
    except OSError:
        resolved = p.resolve(strict=False)
    if not _is_under(pol.repo_root, resolved):
        try:
            from bridge.structured_log import append_event

            append_event(
                {
                    "event": "permission_check",
                    "decision": "denied_repo_read_path",
                    "component": "bridge.mcp_write_policy",
                    "path": str(resolved),
                }
            )
        except Exception:
            pass
        raise MCPRepoReadDenied(f"Path outside repository root: {resolved}")
    return resolved


def _validate_path_arg_writes(tool: str, arguments: Dict[str, Any], policy: MCPWritePolicy) -> None:
    keys = policy.path_args_write.get(tool, ())
    for key in keys:
        val = arguments.get(key)
        if not val or not isinstance(val, str):
            continue
        ensure_allowed_write_path(val.strip(), policy)


def _validate_path_arg_reads(tool: str, arguments: Dict[str, Any], policy: MCPWritePolicy) -> None:
    keys = policy.path_args_read_repo.get(tool, ())
    for key in keys:
        val = arguments.get(key)
        if not val or not isinstance(val, str):
            continue
        ensure_repo_read_path(val.strip(), policy)


def enforce_gazebo_mcp_call(tool_name: str, arguments: Dict[str, Any]) -> None:
    """
    Run before every ``GazeboSession`` tools/call.

    - Validates path arguments for known tools.
    - Optionally denies mutating tools (``deny_mutating_mcp_tools`` / ``BRIDGE_MCP_DENY_MUTATING``).
    """
    pol = load_mcp_write_policy()
    _validate_path_arg_writes(tool_name, arguments, pol)
    _validate_path_arg_reads(tool_name, arguments, pol)

    if pol.deny_mutating_mcp and not is_gazebo_mcp_read_only(tool_name, pol):
        try:
            from bridge.structured_log import append_event

            append_event(
                {
                    "event": "permission_check",
                    "decision": "denied_mutating_mcp_tool",
                    "component": "bridge.mcp_write_policy",
                    "tool": tool_name,
                }
            )
        except Exception:
            pass
        raise MCPMutatingToolDenied(
            f"Mutating gazebo-mcp tool '{tool_name}' blocked by policy "
            f"(set deny_mutating_mcp_tools: false or unset BRIDGE_MCP_DENY_MUTATING)."
        )


def enforce_spawn_model_urdf_read(urdf_path: Path | str) -> Path:
    """Validate URDF path for :func:`bridge.gazebo_bridge.spawn_model`."""
    pol = load_mcp_write_policy()
    if not pol.enforce_repo_model_reads:
        return Path(urdf_path).expanduser().resolve(strict=False)
    return ensure_repo_read_path(urdf_path, pol)
