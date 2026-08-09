#!/bin/sh
set -e

cd tools/mcp/freecad-mcp
python -m pip install --upgrade pip
pip install -e ".[dev]" "mcp[cli]>=1.12.2,<2"
pytest -m unit -ra --tb=short --junitxml=results_unit.xml
