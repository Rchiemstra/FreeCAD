#!/bin/sh
set -e

. "$(dirname "$0")/ensure-coin3d.sh"

timeout 1200 build/debug/bin/FreeCADCmd -t 0
