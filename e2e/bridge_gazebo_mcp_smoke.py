#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
E2E smoke: ``bridge.gazebo_bridge`` APIs used by the SimWorkbench Gazebo Status panel.

Requires:
  - Sourced ROS 2 (e.g. /opt/ros/jazzy/setup.bash)
  - Running ``gz sim`` with worlds/e2e_world.sdf (camera + sensors plugin)
  - ``pip install -e tools/mcp/gazebo-mcp`` into MCP_VENV (see e2e/run_e2e.sh)

Artifacts (default under ``$E2E_RUN_DIR/bridge_gazebo_mcp_smoke/``):
  - summary.json   — ok flags + paths + short messages
  - status.json    — raw get_simulation_status payload
  - sensors.json   — raw list_gazebo_sensors payload
  - screenshots/   — PNG from capture_camera_snapshot()
"""
from __future__ import annotations

import json
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))


def _write_json(path: Path, obj: object) -> None:
    path.write_text(json.dumps(obj, indent=2, default=str), encoding="utf-8")


def _inner_payload(d: object) -> dict:
    if isinstance(d, dict):
        inner = d.get("data")
        if isinstance(inner, dict):
            return inner
        return d
    return {}


def _png_ihdr(path: Path) -> tuple[int | None, int | None]:
    try:
        b = path.read_bytes()
        if len(b) < 24 or b[:8] != b"\x89PNG\r\n\x1a\n":
            return None, None
        if b[12:16] != b"IHDR":
            return None, None
        w, h = struct.unpack(">II", b[16:24])
        return int(w), int(h)
    except OSError:
        return None, None


def main() -> int:
    run_root = os.environ.get("E2E_RUN_DIR", "").strip()
    if not run_root:
        run_root = str(ROOT / "sim_runs" / "e2e_bridge_smoke_manual")
    out = Path(run_root) / "bridge_gazebo_mcp_smoke"
    out.mkdir(parents=True, exist_ok=True)
    shots = out / "screenshots"
    shots.mkdir(parents=True, exist_ok=True)

    # Ensure Docker E2E picks shared venv (lazy per-session in gazebo_bridge)
    os.environ.setdefault("MCP_VENV", "/opt/mcp-venv")

    from bridge import gazebo_bridge

    print("[bridge_gazebo_mcp_smoke] get_simulation_status …")
    st = gazebo_bridge.get_simulation_status(timeout=45.0)
    _write_json(out / "status.json", {"ok": st.ok, "data": st.data, "messages": st.messages})

    print("[bridge_gazebo_mcp_smoke] list_gazebo_sensors …")
    ls = gazebo_bridge.list_gazebo_sensors(timeout=45.0)
    _write_json(out / "sensors.json", {"ok": ls.ok, "data": ls.data, "messages": ls.messages})

    cam = gazebo_bridge.pick_camera_sensor_from_mcp_list(ls.data)
    if not cam:
        cam = os.environ.get("SIMWORKBENCH_GAZEBO_CAMERA_SENSOR", "").strip() or None

    print(f"[bridge_gazebo_mcp_smoke] capture_camera_snapshot (sensor={cam!r}) …")
    cap = gazebo_bridge.capture_camera_snapshot(
        sensor_name=cam,
        output_dir=shots,
        timeout=60.0,
    )

    ls_inner = _inner_payload(ls.data)
    cap_data = cap.data or {}

    png_w, png_h = (None, None)
    cap_path = cap_data.get("path")
    if cap_path:
        png_w, png_h = _png_ihdr(Path(cap_path))

    summary = {
        "artifact_dir": str(out),
        "screenshot_dir": str(shots),
        "simulation_status_ok": st.ok,
        "list_sensors_ok": ls.ok,
        "sensor_catalog": ls_inner.get("sensor_catalog"),
        "camera_sensor_resolved": cam,
        "capture_ok": cap.ok,
        "capture_path": cap_data.get("path"),
        "camera_source_mode": cap_data.get("camera_source_mode"),
        "image_width_reported": cap_data.get("image_width"),
        "image_height_reported": cap_data.get("image_height"),
        "gz_image_topic": cap_data.get("gz_image_topic"),
        "screenshot_png_width": png_w,
        "screenshot_png_height": png_h,
        "screenshot_bytes": Path(cap_path).stat().st_size if cap_path and Path(cap_path).is_file() else None,
        "messages": {
            "status": st.messages,
            "sensors": ls.messages,
            "capture": cap.messages,
        },
    }
    _write_json(out / "summary.json", summary)

    live_ok = cap_data.get("camera_source_mode") == "live"
    # Real frames are multi-hundred-pixel; PNG can still be <2 KB for flat scenes.
    big_enough = (
        png_w is not None
        and png_h is not None
        and png_w >= 64
        and png_h >= 64
        and (summary["screenshot_bytes"] or 0) > 400
    )

    ok = st.ok and ls.ok and cap.ok and summary["capture_path"] and live_ok and big_enough
    if not cam:
        print("[bridge_gazebo_mcp_smoke] WARN: no camera sensor name resolved from list", file=sys.stderr)
    if not ok:
        print("[bridge_gazebo_mcp_smoke] FAIL:", json.dumps(summary, indent=2, default=str), file=sys.stderr)
        if not live_ok:
            print(
                "[bridge_gazebo_mcp_smoke] Expected camera_source_mode=live from gz.msgs.Image; "
                "check gz sim -r (running) and GAZEBO_MCP_SENSOR_MODE.",
                file=sys.stderr,
            )
        if not big_enough:
            print(
                "[bridge_gazebo_mcp_smoke] Screenshot PNG missing IHDR or unrealistically small — "
                "expected a gz camera frame (>=64×64, PNG >400 bytes).",
                file=sys.stderr,
            )
        return 1

    print("[bridge_gazebo_mcp_smoke] OK →", out)
    print("  screenshot:", summary["capture_path"])
    from bridge.structured_log import resolve_structured_log_path

    slog = resolve_structured_log_path()
    if slog and slog.is_file():
        print("  structured JSONL:", slog)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
