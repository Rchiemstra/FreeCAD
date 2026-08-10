#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

from __future__ import annotations

import json
import pathlib
import sys


EXPECTED_TOOLS = {
    "photo_inspection_capabilities",
    "photo_inspection_validate_profile",
    "photo_inspection_calibrate_camera",
    "photo_inspection_calibrate_printer",
    "photo_inspection_create_sheet",
    "photo_inspection_analyze",
    "photo_inspection_get_status",
    "photo_inspection_cancel",
    "photo_inspection_get_result",
    "photo_inspection_save_result",
    "photo_inspection_export",
}
MUTATIONS_REQUIRING_LEASE = {
    "photo_inspection_create_sheet",
    "photo_inspection_save_result",
}


def fail(message: str) -> None:
    raise ValueError(message)


def validate(path: pathlib.Path) -> None:
    raw = path.read_bytes()
    if len(raw) > 64 * 1024:
        fail("contract exceeds 64 KiB")
    contract = json.loads(raw)
    if contract.get("schema") != "freecad.photo_inspection.mcp_contract":
        fail("wrong contract schema")
    if contract.get("version") != "1.0":
        fail("wrong contract version")
    tools = contract.get("tools")
    if not isinstance(tools, list):
        fail("tools must be a list")
    by_name = {}
    for tool in tools:
        if not isinstance(tool, dict) or set(tool) - {
            "name",
            "mutation",
            "lease_required",
            "asynchronous",
            "gui_required",
            "gui_required_for_formats",
            "headless_formats",
        }:
            fail("tool has malformed or unknown fields")
        name = tool.get("name")
        if name in by_name:
            fail(f"duplicate tool: {name}")
        by_name[name] = tool
        if not isinstance(tool.get("mutation"), bool):
            fail(f"{name}: mutation must be boolean")
        if not isinstance(tool.get("lease_required"), bool):
            fail(f"{name}: lease_required must be boolean")
    if set(by_name) != EXPECTED_TOOLS:
        fail("typed tool set is incomplete or contains extras")
    for name in MUTATIONS_REQUIRING_LEASE:
        if not by_name[name]["mutation"] or not by_name[name]["lease_required"]:
            fail(f"{name}: document mutation must require a lease")
    for name, tool in by_name.items():
        if name not in MUTATIONS_REQUIRING_LEASE and tool["lease_required"]:
            fail(f"{name}: observation/export authority cannot claim a document lease")

    export = by_name["photo_inspection_export"]
    if export.get("headless_formats") != ["json", "csv"]:
        fail("headless export formats must be exactly JSON and CSV")
    if export.get("gui_required_for_formats") != ["svg", "pdf"]:
        fail("GUI-only export formats must be exactly SVG and PDF")

    limits = contract.get("limits")
    if not isinstance(limits, dict) or any(
        not isinstance(value, int) or value <= 0 for value in limits.values()
    ):
        fail("all resource limits must be positive integers")
    if limits["maximum_profile_bytes"] > limits["maximum_request_bytes"]:
        fail("profile limit exceeds request limit")
    privacy = contract.get("privacy")
    if (
        not isinstance(privacy, dict)
        or privacy.get("image_bytes_in_response") is not False
        or privacy.get("absolute_paths_in_response") is not False
        or privacy.get("raw_qr_payload_in_log") is not False
    ):
        fail("privacy defaults are not fail-closed")
    errors = contract.get("errors")
    if not isinstance(errors, list) or len(errors) != len(set(errors)):
        fail("error codes must be a unique list")
    for required in (
        "CapabilityUnavailable",
        "LeaseRequired",
        "IdentityMismatch",
        "Cancelled",
        "ResourceLimit",
    ):
        if required not in errors:
            fail(f"missing required error code: {required}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: ValidateMcpContract.py CONTRACT", file=sys.stderr)
        return 2
    try:
        validate(pathlib.Path(sys.argv[1]))
    except (OSError, ValueError, json.JSONDecodeError) as exception:
        print(f"PHOTO_INSPECTION_MCP_CONTRACT_FAILED: {exception}", file=sys.stderr)
        return 1
    print("PHOTO_INSPECTION_MCP_CONTRACT_OK tools=11")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
