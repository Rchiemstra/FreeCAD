#!/bin/sh
set -e

git config --global --add safe.directory '*' 2>/dev/null || true
apt-get update -qq && apt-get install -y -qq --no-install-recommends git >/dev/null
mkdir -p /tmp/logs /tmp/report

# 1) version consistency (stdlib, whole-repo, no file list needed)
python3 src/Tools/sync_version.py --check

# 2) codespell on changed files only
base=""
if [ -n "$CI_COMMIT_TARGET_BRANCH" ]; then
  git fetch origin "$CI_COMMIT_TARGET_BRANCH" --depth=50 >/dev/null 2>&1 || true
  if git rev-parse --verify "origin/$CI_COMMIT_TARGET_BRANCH" >/dev/null 2>&1; then
    base="origin/$CI_COMMIT_TARGET_BRANCH"
  fi
fi

if [ -z "$base" ] && git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
  base="HEAD~1"
fi

if [ -z "$base" ]; then
  echo "no diff base available (shallow clone / tag); codespell skipped"
  exit 0
fi

changed=$(git diff --name-only --diff-filter=d "$base"...HEAD 2>/dev/null || true)
if [ -z "$changed" ]; then
  echo "no changed files to codespell"
  exit 0
fi

echo "== codespell on $(echo "$changed" | wc -l) changed file(s) =="
# shellcheck disable=SC2086  # intentional word-splitting into --files
python3 tools/lint/codespell.py \
  --files $changed \
  --ignore-words .github/codespellignore \
  --skip "./.git*,*.po,*.ts,*.svg,./src/3rdParty,./src/Base/swig*,./src/Mod/Robot/App/kdl_cp,./src/Mod/Import/App/SCL*,./src/Doc/FreeCAD.uml,./build/" \
  --log-dir /tmp/logs \
  --report-file /tmp/report/freecad-lint-report.md

echo "freecad-lint: OK (sync_version + codespell)"
