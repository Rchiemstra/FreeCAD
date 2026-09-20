#!/bin/sh
# GATE for pull requests into FreeCAD-start: the freecad-mcp submodule pin must
# already be on freecad-mcp main. MCP work is merged to MCP main by its own PR
# first; only then may FreeCAD-start point at it.
# Other PR targets (fix/*, chore/*, integrate/*), tags and manual runs skip.
set -eu

git config --global --add safe.directory '*'

target="${CI_COMMIT_TARGET_BRANCH:-}"
if [ "$target" != "FreeCAD-start" ]; then
  echo "mcp-pin-on-main: target is '${target:-<none>}', not FreeCAD-start; skipping"
  exit 0
fi

sub=tools/mcp/freecad-mcp
pin=$(git rev-parse "HEAD:$sub")
url=$(git config -f .gitmodules --get "submodule.$sub.url")

cd "$sub"
# The clone may be shallow; ancestry needs the full history of MCP main.
unshallow=""
if [ -f "$(git rev-parse --git-dir)/shallow" ]; then
  unshallow="--unshallow"
fi
git fetch --quiet --no-tags $unshallow "$url" "+refs/heads/main:refs/remotes/mcp-gate/main"
main=$(git rev-parse refs/remotes/mcp-gate/main)

if git merge-base --is-ancestor "$pin" "$main"; then
  echo "mcp-pin-on-main: OK - pin $pin is on freecad-mcp main ($main)"
  exit 0
fi

echo "ERROR: freecad-mcp pin $pin is not on freecad-mcp main ($main)."
echo "Merge the MCP change into freecad-mcp main with its own PR first, then"
echo "point $sub at a commit on main (e.g. main's head) in this PR."
exit 1
