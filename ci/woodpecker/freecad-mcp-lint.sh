#!/bin/sh
set -e

cd tools/mcp/freecad-mcp

# Syntax-compile every Python dir in the package, not just src/tests: ci/ holds
# the CI helper scripts (preflight_imports.py, run_freecad_tests.py) that run
# under FreeCADCmd, and addon/ ships to users -- a syntax error in either would
# otherwise sail through this gate. Drop compileall's -q so the step prints what
# it checked: a passing lint was previously a completely blank log with no proof
# it ran or covered anything.
echo "freecad-mcp lint: byte-compiling src tests ci addon"
python -m compileall src tests ci addon
echo "freecad-mcp lint: installing pinned typed contract checkers"
# Exercise the minimum supported SDK as well as the pinned type checker.
python -m pip install . "mypy==2.3.1" "mcp==1.26.0"
python ci/run_contract_checks.py
echo "freecad-mcp lint: OK"
