#!/bin/sh
set -e

# Real-CLI check for the MCP Git sidecar adapter (addon/FreeCADMCP/git_sidecar.py).
# freecad-mcp-unit-tests.sh only ever exercises this adapter against a mocked
# subprocess.run, so a broken/uninstalled freecad-git CLI can't fail that job --
# export_sidecar_after_save() would return {"ok": False, "error": ...} to a
# post-save observer whose return value nobody reads, and the save just looks
# fine. This step installs the real freecad-git package (a sibling tool in this
# monorepo, not a freecad-mcp dependency) and runs the un-mocked integration
# test that shells out to it for real.
python -m pip install --upgrade pip
pip install -e tools/freecad_git

cd tools/mcp/freecad-mcp
pip install -e ".[dev]" "mcp[cli]>=1.12.2,<2"
pytest -m integration -ra --tb=short --junitxml=results_integration.xml
