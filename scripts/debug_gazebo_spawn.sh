#!/usr/bin/env bash
# Debug spawn: unpause + minimal box (gz service in sim container; ros2 optional).
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env
WORLD="$GZ_SIM_WORLD_NAME"
GZ="${GZ_SIM_CONTAINER_NAME:-gz-sim-sever}"

echo "--- unpause (gz service) ---"
docker exec "$GZ" gz service -s "/world/${WORLD}/control" \
  --reqtype gz.msgs.WorldControl --reptype gz.msgs.Boolean \
  --timeout 8000 --req "pause: false"

echo "--- spawn box (gz service) ---"
docker exec "$GZ" bash -lc 'cat > /tmp/debug_box.sdf <<EOF
<?xml version="1.0"?>
<sdf version="1.9">
  <model name="debug_box">
    <link name="link">
      <inertial><mass>1</mass></inertial>
      <collision name="c"><geometry><box><size>0.1 0.1 0.1</size></box></geometry></collision>
    </link>
  </model>
</sdf>
EOF'
docker exec "$GZ" gz service -s "/world/${WORLD}/create" \
  --reqtype gz.msgs.EntityFactory --reptype gz.msgs.Boolean \
  --timeout 10000 \
  --req 'sdf_filename: "/tmp/debug_box.sdf", name: "debug_box", allow_renaming: false'

echo "debug_box spawn requested (world=$WORLD)"
