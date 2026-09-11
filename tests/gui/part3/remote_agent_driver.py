#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""RemoteAgentDriver child process: typed JSON-RPC verbs only; no control token."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
LAUNCHER = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"
TOKEN_ENV = "PART3_LOCAL_CONTROL_TOKEN"

FORBIDDEN_REMOTE_AGENT_RPC_METHODS = frozenset(
    {
        "execute_code",
        "execute_code_async",
        "request_local_pause_after_current",
        "resume_local_agent_writes",
        "refresh_view",
        "get_view",
        "save_view_sequence",
        "encode_view_video",
        "animate_placement",
        "set_section_view",
        "select_subshapes",
        "get_selection",
        "get_gui_state",
        "repair_view_placements",
        "getCameraNode",
        "getSceneGraph",
    }
)

REMOTE_AGENT_TYPED_RPC_ALLOWLIST = frozenset(
    {
        "ping",
        "get_instance_info",
        "handshake_v2",
        "shutdown_rpc_server",
        "create_document",
        "close_document",
        "open_document",
        "reload_document",
        "list_documents",
        "create_object",
        "edit_object",
        "delete_object",
        "body_create",
        "body_set_tip",
        "sketch_create",
        "sketch_attach",
        "sketch_add_geometry",
        "sketch_add_constraint",
        "sketch_delete_geometry",
        "sketch_delete_constraint",
        "pad_feature",
        "pocket_feature",
        "revolve_feature",
        "loft_feature",
        "sweep_feature",
        "linear_pattern_feature",
        "polar_pattern_feature",
        "mirror_feature",
        "fillet_feature",
        "chamfer_feature",
        "recompute_document",
        "recompute_and_wait",
        "save_document",
        "save_document_as",
        "save_document_copy",
        "finalize_document_edit",
        "undo",
        "redo",
        "get_semantic_revisions",
        "begin_checked_edit",
        "commit_checked_property",
        "cancel_checked_edit",
        "get_mutation_readiness",
        "get_objects",
        "get_object",
        "inspect_references",
        "capture_state",
        "get_document_tree",
        "compare_documents",
        "geometric_diff",
        "validate_geometry",
        "get_recompute_log",
    }
)


def _load_json_rpc_client():
    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.JsonRpcClient, module.JsonRpcError, module.JsonRpcTransportError


def inspect_token_absence(
    token_env: str,
    token_value: str | None,
) -> dict[str, object]:
    leaks: list[str] = []
    observed = os.environ.get(token_env, "")
    if observed:
        leaks.append(f"env:{token_env}")
    if token_value:
        for key, value in os.environ.items():
            if token_value in value:
                leaks.append(f"env:{key}")
        for argument in sys.argv:
            if token_value in argument:
                leaks.append("argv")
    return {
        "token_env": token_env,
        "token_present_in_env": bool(observed),
        "token_present_in_argv": bool(
            token_value and any(token_value in argument for argument in sys.argv)
        ),
        "leaks": leaks,
        "absent": not leaks,
    }


def validate_remote_method(method: str) -> None:
    if method in FORBIDDEN_REMOTE_AGENT_RPC_METHODS:
        raise ValueError(f"forbidden remote RPC method: {method}")
    if method not in REMOTE_AGENT_TYPED_RPC_ALLOWLIST:
        raise ValueError(f"unlisted remote RPC method: {method}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspect-token-absence", action="store_true")
    parser.add_argument("--token-env", default=TOKEN_ENV)
    parser.add_argument("--rpc-host", default="127.0.0.1")
    parser.add_argument("--rpc-port", type=int, default=9875)
    parser.add_argument("--method", default="")
    parser.add_argument("--params", default="{}")
    args = parser.parse_args(argv)

    inspect_payload: dict[str, object] | None = None
    if args.inspect_token_absence and not sys.stdin.isatty():
        try:
            loaded = json.loads(sys.stdin.read() or "{}")
            if isinstance(loaded, dict):
                inspect_payload = loaded
        except json.JSONDecodeError:
            inspect_payload = None

    token_value = None
    if isinstance(inspect_payload, dict):
        raw = inspect_payload.get("token")
        if isinstance(raw, str) and raw.strip():
            token_value = raw.strip()
    if args.inspect_token_absence:
        report = inspect_token_absence(args.token_env, token_value)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["absent"] else 2

    if not args.method:
        print("missing --method for typed RPC call", file=sys.stderr)
        return 2
    try:
        validate_remote_method(args.method)
    except ValueError as exc:
        print(json.dumps({"success": False, "error": str(exc)}))
        return 2

    JsonRpcClient, JsonRpcError, JsonRpcTransportError = _load_json_rpc_client()
    client = JsonRpcClient(host=args.rpc_host, port=args.rpc_port)
    try:
        params = json.loads(args.params)
    except json.JSONDecodeError as exc:
        print(f"invalid --params JSON: {exc}", file=sys.stderr)
        return 2
    try:
        result = client.call(args.method, params)
    except (JsonRpcError, JsonRpcTransportError) as exc:
        print(json.dumps({"success": False, "error": str(exc)}))
        return 1
    print(json.dumps({"success": True, "result": result}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
