#!/bin/sh
# Decide whether the CI images already exist in the local registry.
#
# Writes one flag file per image into .ci-images/ for the conditional build
# steps that follow. This step never fails: a missing image is a normal state
# that the build steps repair, not an error.
#
# Why this exists: the registry has a short retention policy, so
# freecad-ci-deps/freecad-ci-mcp can be evicted between runs. build-images.yml
# does not run on pull_request, so nothing would republish them and ci.yml used
# to wait 60 minutes for tags that were never coming.
set -e

registry="$FREECAD_CI_REGISTRY"
[ -n "$registry" ] || { echo "FREECAD_CI_REGISTRY is required"; exit 1; }

expected_tag="$CI_COMMIT_SHA"
if [ "$CI_PIPELINE_EVENT" = "pull_request" ]; then
  expected_tag="24.04"
fi

# ci.yml always pulls :24.04, so that tag must exist no matter what the
# freshness tag is. Check both and rebuild if either is absent.
required_tags="24.04"
if [ "$expected_tag" != "24.04" ]; then
  required_tags="24.04 $expected_tag"
fi

rm -rf .ci-images
mkdir -p .ci-images
echo "$expected_tag" > .ci-images/expected-tag

# On push/tag/manual, build-images.yml starts alongside this pipeline and may
# already be building. Give it a bounded grace period before we duplicate a
# ~15 minute apt build. On pull_request it never runs, so do not wait at all.
if [ "$CI_PIPELINE_EVENT" = "pull_request" ]; then
  attempts=1
else
  attempts=90   # 90 x 10s = 15 minutes
fi

check_repo() {
  tags="$(wget -qO- "$registry/v2/$1/tags/list" 2>/dev/null || true)"
  for t in $required_tags; do
    echo "$tags" | grep -q "\"$t\"" || return 1
  done
  return 0
}

attempt=1
while [ "$attempt" -le "$attempts" ]; do
  deps_ok=0; mcp_ok=0
  check_repo freecad-ci-deps && deps_ok=1
  check_repo freecad-ci-mcp && mcp_ok=1
  if [ "$deps_ok" = 1 ] && [ "$mcp_ok" = 1 ]; then
    echo "CI images already present in $registry (tags: $required_tags)"
    exit 0
  fi
  if [ "$attempt" -eq "$attempts" ]; then
    break
  fi
  if [ "$attempt" -eq 1 ] || [ $((attempt % 30)) -eq 0 ]; then
    echo "waiting for a concurrent build-images run ($attempt/$attempts)"
  fi
  sleep 10
  attempt=$((attempt + 1))
done

[ "$deps_ok" = 1 ] || touch .ci-images/deps-missing
[ "$mcp_ok" = 1 ] || touch .ci-images/mcp-missing

# The mcp image FROMs the deps image, so a deps rebuild invalidates it.
if [ -f .ci-images/deps-missing ] && [ ! -f .ci-images/mcp-missing ]; then
  echo "deps is being rebuilt; rebuilding mcp on top of it as well"
  touch .ci-images/mcp-missing
fi

echo "missing images will be built in this pipeline:"
[ -f .ci-images/deps-missing ] && echo "  freecad-ci-deps"
[ -f .ci-images/mcp-missing ] && echo "  freecad-ci-mcp"
exit 0
