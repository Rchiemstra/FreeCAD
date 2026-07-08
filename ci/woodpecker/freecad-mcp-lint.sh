#!/bin/sh
set -e

cd tools/mcp/freecad-mcp
python -m compileall -q src tests
