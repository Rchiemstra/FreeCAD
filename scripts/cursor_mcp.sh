#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${1:-}"
if [[ -z "$SERVER" ]]; then
    echo "Usage: cursor_mcp.sh <freecad|ros2|gz-sim>" >&2
    exit 2
fi
shift || true

ensure_venv() {
    local package_dir="$1"
    local entrypoint="$2"

    cd "$package_dir"
    if [[ -d ".venv" ]]; then
        for generated in ".venv/bin/pip" ".venv/bin/$entrypoint"; do
            if [[ -x "$generated" ]] && ! head -1 "$generated" | grep -q "$package_dir/.venv/bin/python"; then
                rm -rf ".venv"
                break
            fi
        done
    fi

    if [[ ! -x ".venv/bin/$entrypoint" ]]; then
        if [[ ! -x ".venv/bin/python3" ]]; then
            python3 -m venv .venv 2>/dev/null || python3 -m venv --without-pip .venv
        fi
        if [[ ! -x ".venv/bin/pip" ]]; then
            curl -sS https://bootstrap.pypa.io/get-pip.py | .venv/bin/python3
        fi
        .venv/bin/pip install -e . -q
    fi
    test -x ".venv/bin/$entrypoint"
}

install_geometry_msgs_stub() {
    .venv/bin/python3 - <<'PY'
import os
import sys

for path in sys.path:
    if "site-packages" in path and ".venv" in path:
        gm = os.path.join(path, "geometry_msgs")
        gm_msg = os.path.join(gm, "msg")
        os.makedirs(gm_msg, exist_ok=True)
        open(os.path.join(gm, "__init__.py"), "a").close()
        with open(os.path.join(gm_msg, "__init__.py"), "w", encoding="utf-8") as fh:
            fh.write(
                "class _M:\n"
                "    def __init__(self, **k):\n"
                "        for a, b in k.items(): setattr(self, a, b)\n"
                "class Pose(_M): pass\n"
                "class Twist(_M): pass\n"
                "class Vector3(_M): pass\n"
                "class Quaternion(_M): pass\n"
                "class Point(_M): pass\n"
                "class Wrench(_M): pass\n"
                "class Transform(_M): pass\n"
                "class PoseStamped(_M): pass\n"
                "class TwistStamped(_M): pass\n"
            )
        raise SystemExit(0)
raise SystemExit(1)
PY
}

case "$SERVER" in
    freecad)
        ensure_venv "$ROOT/tools/mcp/freecad-mcp" "freecad-mcp"
        freecad_host="${MCP_FREECAD_HOST:-$(ip route show default | awk '{print $3; exit}')}"
        exec "$ROOT/tools/mcp/freecad-mcp/.venv/bin/freecad-mcp" --host "$freecad_host" "$@"
        ;;
    ros2)
        ensure_venv "$ROOT/tools/mcp/ros-mcp-server" "ros-mcp"
        exec "$ROOT/tools/mcp/ros-mcp-server/.venv/bin/ros-mcp" "$@"
        ;;
    gz-sim)
        ensure_venv "$ROOT/tools/mcp/gazebo-mcp" "gazebo-mcp-server"
        cd "$ROOT/tools/mcp/gazebo-mcp"
        install_geometry_msgs_stub
        exec "$ROOT/tools/mcp/gazebo-mcp/.venv/bin/gazebo-mcp-server" "$@"
        ;;
    *)
        echo "Unknown MCP server: $SERVER" >&2
        exit 2
        ;;
esac
