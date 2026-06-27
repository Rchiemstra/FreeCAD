#!/usr/bin/env bash
# Recreate tools/mcp/gazebo-mcp WSL venv (fixes stale shebang / missing packages).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PKG="$ROOT/tools/mcp/gazebo-mcp"

cd "$PKG"
rm -rf .venv
python3 -m venv --without-pip .venv 2>/dev/null || python3 -m venv .venv
if [[ ! -x .venv/bin/pip ]]; then
  curl -sS https://bootstrap.pypa.io/get-pip.py | .venv/bin/python3
fi
.venv/bin/pip install -q -e .
.venv/bin/python3 -c "import gazebo_mcp"

# geometry_msgs stub for import without full ROS on WSL
site="$(.venv/bin/python3 -c 'import sys; print(next(p for p in sys.path if "site-packages" in p and ".venv" in p))')"
mkdir -p "$site/geometry_msgs/msg"
: > "$site/geometry_msgs/__init__.py"
cat > "$site/geometry_msgs/msg/__init__.py" <<'PY'
class _M:
    def __init__(self, **k):
        for a, b in k.items(): setattr(self, a, b)
class Pose(_M): pass
class Twist(_M): pass
class Vector3(_M): pass
class Quaternion(_M): pass
class Point(_M): pass
class Wrench(_M): pass
class Transform(_M): pass
class PoseStamped(_M): pass
class TwistStamped(_M): pass
PY

echo "gazebo-mcp venv ready at $PKG/.venv"
