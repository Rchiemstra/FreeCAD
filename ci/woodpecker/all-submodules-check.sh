#!/bin/sh
set -e

git config --global --add safe.directory '*'

echo "== git submodule status =="
git submodule status > /tmp/sub_status.txt
cat /tmp/sub_status.txt

# '-' not initialized, '+' wrong commit, 'U' merge conflict
if grep -E '^-|^\+|^U' /tmp/sub_status.txt; then
  echo "ERROR: a submodule is not initialized / wrong commit / in conflict"
  exit 1
fi

required="src/3rdParty/OndselSolver src/3rdParty/GSL src/Mod/AddonManager tools/mcp/freecad-mcp"
for r in $required; do
  if [ ! -d "$r" ] || [ -z "$(ls -A "$r" 2>/dev/null)" ]; then
    echo "ERROR: required submodule $r missing or empty"
    exit 1
  fi
  if [ ! -f "$r/.git" ] && [ ! -d "$r/.git" ]; then
    echo "ERROR: required submodule $r not initialized (no .git)"
    exit 1
  fi
done

echo "all-submodules-check: OK (every submodule initialized at expected commit)"
