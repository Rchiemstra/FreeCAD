#!/bin/sh
set -e

git config --global --add safe.directory '*' 2>/dev/null || true

cd tools/mcp/freecad-mcp
python -m pip install --break-system-packages -e ".[dev]" "mcp[cli]>=1.12.2,<2"
pytest -m unit -ra --tb=short --junitxml=results_unit.xml
