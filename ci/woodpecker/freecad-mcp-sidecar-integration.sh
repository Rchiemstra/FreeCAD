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
fixture=tools/freecad_git/tests/fixtures/basic.FCStd
if [ ! -f "$fixture" ]; then
    echo "missing required freecad-git sidecar fixture: $fixture" >&2
    exit 1
fi

python -m pip install --upgrade pip
pip install -e tools/freecad_git

cd tools/mcp/freecad-mcp
pip install -e ".[dev]" "mcp[cli]>=1.12.2,<2"
pytest -m integration -ra --tb=short --junitxml=results_integration.xml

# Pytest exits successfully when a selected test is skipped.  This lane is
# specifically the real-CLI proof, so do not report green unless every
# selected integration test actually ran.
python - <<'PY'
from pathlib import Path
from xml.etree import ElementTree

report = Path("results_integration.xml")
try:
    root = ElementTree.parse(report).getroot()
except (OSError, ElementTree.ParseError) as exc:
    raise SystemExit(f"could not read pytest JUnit report {report}: {exc}") from exc

cases = root.findall(".//testcase")
skipped = [case for case in cases if case.find("skipped") is not None]
if not cases:
    raise SystemExit("sidecar integration lane selected no tests")
if skipped:
    raise SystemExit(
        "sidecar integration lane skipped selected tests: "
        + ", ".join(
            f"{case.get('classname', '<unknown>')}.{case.get('name', '<unknown>')}"
            for case in skipped
        )
    )
PY
