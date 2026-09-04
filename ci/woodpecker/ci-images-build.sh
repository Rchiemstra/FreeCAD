#!/bin/sh
# Build and push ONE CI image, but only if ci-images-check.sh flagged it missing.
#
# Usage: ci-images-build.sh <deps|mcp>
#
# Runs inside the kaniko executor image. kaniko does not support building more
# than one image per container, so deps and mcp get their own pipeline steps and
# this script is invoked once per image.
set -e

which="$1"
[ -n "$which" ] || { echo "usage: ci-images-build.sh <deps|mcp>"; exit 1; }

if [ ! -f ".ci-images/$which-missing" ]; then
  echo "freecad-ci-$which is already in the registry; nothing to build"
  exit 0
fi

registry="${FREECAD_CI_REGISTRY_HOST:-registry-freecad:5000}"
workspace="${CI_WORKSPACE:-$(pwd)}"
sha="$CI_COMMIT_SHA"

case "$which" in
  deps)
    context="$workspace/package/ubuntu"
    dockerfile="$context/Dockerfile"
    ;;
  mcp)
    context="$workspace/tools/mcp/freecad-mcp"
    dockerfile="$context/Dockerfile.ci"
    ;;
  *)
    echo "unknown image '$which'"; exit 1 ;;
esac

[ -f "$dockerfile" ] || { echo "missing Dockerfile: $dockerfile"; exit 1; }

echo "building freecad-ci-$which from $dockerfile"

# --insecure/--skip-tls-verify: the local registry speaks plain HTTP.
# The mcp image FROMs freecad-ci-deps out of that same registry, so it also
# needs the pull-side flags.
set -- \
  --context="dir://$context" \
  --dockerfile="$dockerfile" \
  --destination="$registry/freecad-ci-$which:24.04" \
  --insecure \
  --skip-tls-verify \
  --insecure-pull \
  --skip-tls-verify-pull \
  --single-snapshot

if [ -n "$sha" ]; then
  set -- "$@" --destination="$registry/freecad-ci-$which:$sha"
fi

/kaniko/executor "$@"
echo "pushed freecad-ci-$which"
