#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Minimal RemoteAgentDriver child process for Part 3 token isolation proofs."""

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


def _load_json_rpc_client():
    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.JsonRpcClient, module.JsonRpcError, module.JsonRpcTransportError


def inspect_token_absence(token_env: str, token_value: str | None) -> dict[str, object]:
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inspect-token-absence", action="store_true")
    parser.add_argument("--token-env", default=TOKEN_ENV)
    parser.add_argument("--rpc-host", default="127.0.0.1")
    parser.add_argument("--rpc-port", type=int, default=9875)
    parser.add_argument("--method", default="ping")
    parser.add_argument("--params", default="{}")
    args = parser.parse_args(argv)

    token_value = os.environ.get(args.token_env, "").strip() or None
    if args.inspect_token_absence:
        report = inspect_token_absence(args.token_env, token_value)
        print(json.dumps(report, sort_keys=True))
        return 0 if report["absent"] else 2

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
