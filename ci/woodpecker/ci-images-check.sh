#!/bin/sh
# Missing images are recoverable; stale keys and registry errors fail closed.
# The consumer gives a concurrently scheduled build-images workflow a bounded
# grace period. The producer never waits for itself; PRs have no companion run.
set -eu
sh ci/woodpecker/ci-images-key.sh check
image_state=${FREECAD_CI_IMAGE_STATE:-.ci-images}
rm -rf "$image_state"
mkdir -p "$image_state"
if [ -z "${CRANE_BIN:-}" ]; then
  cp /ko-app/crane "$image_state/crane"
fi
sh ci/woodpecker/ci-images-key.sh tags > "$image_state/tags"
sed -n '1p' "$image_state/tags" > "$image_state/deps-tag"
sed -n '2p' "$image_state/tags" > "$image_state/mcp-tag"
. ci/woodpecker/ci-images-common.sh
load_image_tags
attempts=1
if [ "${CI_IMAGES_WORKFLOW:-producer}" = consumer ]; then
  case "${CI_PIPELINE_EVENT:-}" in
    push|tag|manual) attempts=${FREECAD_CI_CHECK_ATTEMPTS:-90} ;;
  esac
fi
attempt=1
while :; do
  deps_digest=; mcp_digest=; parent=
  if deps_digest=$(image_digest deps "$deps_tag"); then :; else
    rc=$?; [ "$rc" -eq 1 ] || exit "$rc"
  fi
  if mcp_digest=$(image_digest mcp "$mcp_tag"); then
    parent=$(image_parent "$mcp_digest")
  else
    rc=$?; [ "$rc" -eq 1 ] || exit "$rc"
  fi
  if [ -n "$deps_digest" ] && [ -n "$mcp_digest" ] && [ "$parent" = "$deps_digest" ]; then
    echo 'CI keyed images present; MCP parent matches deps'
    exit 0
  fi
  [ "$attempt" -lt "$attempts" ] || break
  echo "waiting for a concurrent build-images run ($attempt/$attempts)"
  sleep "${FREECAD_CI_CHECK_INTERVAL:-10}"
  attempt=$((attempt + 1))
done
[ -n "$deps_digest" ] || touch "$image_state/deps-missing"
if [ -z "$deps_digest" ] || [ -z "$mcp_digest" ] || [ "$parent" != "$deps_digest" ]; then
  touch "$image_state/mcp-missing"
fi
echo 'Missing images or MCP parent mismatch; conditional builders will repair them'
