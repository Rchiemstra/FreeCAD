#!/bin/sh
# Final readiness gate. The identity record is a snapshot, not a retention lease.
set -eu
. ci/woodpecker/ci-images-common.sh
load_image_tags
rm -f "$image_state/identity.txt"
attempt=1
while [ "$attempt" -le "${FREECAD_CI_VERIFY_ATTEMPTS:-30}" ]; do
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
    pull_host=${FREECAD_CI_PULL_HOST:-127.0.0.1:5001}
    printf '%s\n' \
      "$pull_host/freecad-ci-deps@$deps_digest (tag $deps_tag)" \
      "$pull_host/freecad-ci-mcp@$mcp_digest (tag $mcp_tag; deps parent $parent)" > "$image_state/identity.txt"
    cat "$image_state/identity.txt"
    exit 0
  fi
  [ "$attempt" -eq "${FREECAD_CI_VERIFY_ATTEMPTS:-30}" ] || sleep 2
  attempt=$((attempt + 1))
done
echo "ERROR: CI images missing or MCP parent mismatch: deps:$deps_tag mcp:$mcp_tag in $registry" >&2
echo 'Re-run the pipeline to repair missing images; for stale MCP parents see ci-images.md.' >&2
exit 1
