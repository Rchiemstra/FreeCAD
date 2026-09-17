#!/bin/sh
# Final gate before the heavy image pulls: the images must now exist, whether
# they were already there or this pipeline rebuilt them.
set -e

registry="$FREECAD_CI_REGISTRY"
[ -n "$registry" ] || { echo "FREECAD_CI_REGISTRY is required"; exit 1; }

expected_tag="24.04"
if [ -f .ci-images/expected-tag ]; then
  expected_tag="$(cat .ci-images/expected-tag)"
fi

required_tags="24.04"
if [ "$expected_tag" != "24.04" ]; then
  required_tags="24.04 $expected_tag"
fi

# A push to the registry can take a moment to become listable.
for attempt in $(seq 1 30); do
  deps_tags="$(wget -qO- "$registry/v2/freecad-ci-deps/tags/list" 2>/dev/null || true)"
  mcp_tags="$(wget -qO- "$registry/v2/freecad-ci-mcp/tags/list" 2>/dev/null || true)"
  ok=1
  for t in $required_tags; do
    echo "$deps_tags" | grep -q "\"$t\"" || ok=0
    echo "$mcp_tags"  | grep -q "\"$t\"" || ok=0
  done
  if [ "$ok" = 1 ]; then
    echo "CI images ready in $registry"
    echo "required tags: $required_tags"
    echo "deps: $deps_tags"
    echo "mcp:  $mcp_tags"
    exit 0
  fi
  sleep 2
done

echo "CI images are still not available after the rebuild attempt"
echo "required tags: $required_tags"
echo "deps tags: ${deps_tags:-<none>}"
echo "mcp tags:  ${mcp_tags:-<none>}"
echo
echo "tags:null means the registry still lists the repo names but every tag was"
echo "deleted (short retention / GC). ci-images-check can see :24.04 at the start"
echo "of the pipeline, the kaniko steps then no-op, and this gate runs later."
echo
echo "Repair (pick one):"
echo "  1. Re-run this PR pipeline. ci-images-check will flag the missing tags"
echo "     and ci-images-build-deps / ci-images-build-mcp will kaniko-push :24.04."
echo "  2. Manually run the build-images workflow (.woodpecker/build-images.yml)"
echo "     on FreeCAD-start, then re-run the PR pipeline."
exit 1
