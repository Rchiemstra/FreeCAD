#!/usr/bin/env bash
# Optional local Docker builder; shares Woodpecker's keys, parent provenance,
# publication lock and no-op alias repair. Requires Docker and crane on PATH.
# Registry defaults to the LAN endpoint; Docker needs it in insecure-registries.
# Run on the agent host to share /tmp/freecad-ci-publish with both workflows.
# See ci/woodpecker/ci-images.md for remote-builder and migration limits.
# Usage: ci/build-images.sh [deps|mcp] [--no-push]
set -euo pipefail
REGISTRY=${REGISTRY:-192.168.2.23:5001}
NS=${NS:-}
PUSH=1
TARGETS=()
for arg in "$@"; do
  case "$arg" in
    --no-push) PUSH=0 ;;
    deps|mcp) TARGETS+=("$arg") ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done
if [ "${#TARGETS[@]}" -eq 0 ]; then TARGETS=(deps mcp); fi
export FREECAD_CI_REGISTRY="http://${REGISTRY}${NS:+/$NS}"
export FREECAD_CI_REGISTRY_HOST="${REGISTRY}${NS:+/$NS}"
export CRANE_BIN=${CRANE_BIN:-$(command -v crane)}
export FREECAD_CI_BUILD_ENGINE=docker
export FREECAD_CI_IMAGE_STATE
FREECAD_CI_IMAGE_STATE=$(mktemp -d /tmp/freecad-ci-local.XXXXXX)
trap 'rm -rf "$FREECAD_CI_IMAGE_STATE"' EXIT
sh ci/woodpecker/ci-images-key.sh check
sh ci/woodpecker/ci-images-key.sh tags > "$FREECAD_CI_IMAGE_STATE/tags"
sed -n '1p' "$FREECAD_CI_IMAGE_STATE/tags" > "$FREECAD_CI_IMAGE_STATE/deps-tag"
sed -n '2p' "$FREECAD_CI_IMAGE_STATE/tags" > "$FREECAD_CI_IMAGE_STATE/mcp-tag"
for which in "${TARGETS[@]}"; do
  if [ "$PUSH" -eq 0 ]; then
    deps_tag=$(cat "$FREECAD_CI_IMAGE_STATE/deps-tag")
    tag=$(cat "$FREECAD_CI_IMAGE_STATE/$which-tag")
    case "$which" in
      deps) docker build --pull -f package/ubuntu/Dockerfile -t "freecad-ci-deps:$tag" package/ubuntu ;;
      mcp) docker build -f tools/mcp/freecad-mcp/Dockerfile.ci \
        --build-arg "FREECAD_CI_DEPS_IMAGE=freecad-ci-deps:$deps_tag" \
        -t "freecad-ci-mcp:$tag" tools/mcp/freecad-mcp ;;
    esac
  else
    sh ci/woodpecker/ci-images-build.sh "$which"
  fi
done
