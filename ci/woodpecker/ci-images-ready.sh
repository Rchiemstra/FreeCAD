#!/bin/sh
set -e

registry="$FREECAD_CI_REGISTRY"
[ -n "$registry" ] || { echo "FREECAD_CI_REGISTRY is required"; exit 1; }

expected_tag="$CI_COMMIT_SHA"
if [ "$CI_PIPELINE_EVENT" = "pull_request" ]; then
  expected_tag="24.04"
fi

for attempt in $(seq 1 360); do
  deps_tags="$(wget -qO- "$registry/v2/freecad-ci-deps/tags/list" 2>/dev/null || true)"
  mcp_tags="$(wget -qO- "$registry/v2/freecad-ci-mcp/tags/list" 2>/dev/null || true)"
  if echo "$deps_tags" | grep -q "\"$expected_tag\"" && echo "$mcp_tags" | grep -q "\"$expected_tag\""; then
    echo "CI images ready in local registry"
    echo "expected tag: $expected_tag"
    echo "deps: $deps_tags"
    echo "mcp:  $mcp_tags"
    exit 0
  fi
  if [ "$attempt" -eq 1 ] || [ $((attempt % 30)) -eq 0 ]; then
    echo "waiting for freecad-ci-deps:$expected_tag and freecad-ci-mcp:$expected_tag in $registry ($attempt/360)"
  fi
  sleep 10
done

echo "CI images were not published within 60 minutes"
if [ -z "$deps_tags" ]; then
  deps_tags="<none>"
fi
if [ -z "$mcp_tags" ]; then
  mcp_tags="<none>"
fi
echo "deps tags: $deps_tags"
echo "mcp tags:  $mcp_tags"
exit 1
