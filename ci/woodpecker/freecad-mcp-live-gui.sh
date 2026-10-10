#!/bin/sh
set -e

# Live GUI stress scenarios (tools/mcp/freecad-mcp/tests/live_gui): launch THIS
# branch's build/debug FreeCAD GUI under Xvfb with the MCP addon and drive it
# through the real MCP server. A scenario fails on any MCP error and on any new
# warning or error line FreeCAD prints (Report view, Coin, Qt). The runner
# treats a skipped or empty run as a failure.
export FREECAD_MCP_ISOLATED_FREECAD="$CI_WORKSPACE/build/debug/bin/FreeCAD"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"

cd tools/mcp/freecad-mcp
pip install --no-build-isolation --no-deps -e .
sh scripts/run_live_gui_tests.sh -rfE --tb=short
