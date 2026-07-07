#!/usr/bin/env bash
# Build and publish the FreeCAD Woodpecker CI images as PUBLIC images on ghcr.io.
#
#   freecad-ci-deps:24.04   -- Ubuntu 24.04 + package/ubuntu/install-apt-packages.sh
#                              + ccache/ninja/xvfb/clang-format/python3-pip/git.
#   freecad-ci-mcp:24.04    -- FROM freecad-ci-deps:24.04 + pip install -e ".[dev]"
#                              (the freecad-mcp package + pytest/hatchling/mcp[cli]).
#
# Each image is tagged twice:
#   :24.04              -- rolling; what .woodpecker.yml references (always latest).
#   :24.04-<hash>       -- immutable; keyed on the file that defines the image
#                          content (install-apt-packages.sh for deps, pyproject.toml
#                          for mcp). Use for traceability / rollback.
#
# Rebuild whenever:
#   * package/ubuntu/install-apt-packages.sh changes  -> rebuild deps (and mcp).
#   * tools/mcp/freecad-mcp/pyproject.toml changes    -> rebuild mcp.
#   * a weekly security refresh of the apt base       -> rebuild deps (and mcp).
# (A scheduled job can call this script on a timer; until then run it by hand.)
#
# PREREQUISITE: authenticate to ghcr.io once per shell (needs a GitHub PAT with
# `write:packages`):
#   echo "$GHCR_TOKEN" | docker login ghcr.io -u rchiemstra --password-stdin
#
# Usage:
#   ci/docker/build-images.sh            # build + push deps, then mcp
#   ci/docker/build-images.sh deps       # only deps
#   ci/docker/build-images.sh mcp        # only mcp (deps must already exist/pushed)
#   ci/docker/build-images.sh --no-push  # build locally, don't push
#
# Overrides (env):
#   REGISTRY=ghcr.io  NS=rchiemstra   # image namespace
set -euo pipefail

REGISTRY="${REGISTRY:-ghcr.io}"
NS="${NS:-rchiemstra}"
PUSH=1
TARGETS=()

for arg in "$@"; do
  case "$arg" in
    --no-push) PUSH=0 ;;
    deps|mcp)  TARGETS+=("$arg") ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done
if [ "${#TARGETS[@]}" -eq 0 ]; then TARGETS=(deps mcp); fi

short_hash() {  # short_hash <file>
  sha256sum "$1" | awk '{print substr($1,1,12)}'
}

build_deps() {
  local script="package/ubuntu/install-apt-packages.sh"
  local h; h="$(short_hash "$script")"
  local rolling="${REGISTRY}/${NS}/freecad-ci-deps:24.04"
  local pinned="${REGISTRY}/${NS}/freecad-ci-deps:24.04-${h}"
  echo "==> freecad-ci-deps  (key=${h}  script=${script})"
  docker build -f ci/docker/freecad-ci-deps.Dockerfile \
    -t "$rolling" -t "$pinned" \
    package/ubuntu/
  if [ "$PUSH" -eq 1 ]; then
    docker push "$rolling"
    docker push "$pinned"
  fi
  echo "    deps image: ${rolling}  (pinned: ${pinned})"
}

build_mcp() {
  local pf="tools/mcp/freecad-mcp/pyproject.toml"
  local h; h="$(short_hash "$pf")"
  local rolling="${REGISTRY}/${NS}/freecad-ci-mcp:24.04"
  local pinned="${REGISTRY}/${NS}/freecad-ci-mcp:24.04-${h}"
  echo "==> freecad-ci-mcp  (key=${h}  pyproject=${pf})"
  # FROM the (just-built/pushed) deps rolling tag, so ABI tracks the same base.
  docker build -f ci/docker/freecad-ci-mcp.Dockerfile \
    -t "$rolling" -t "$pinned" \
    tools/mcp/freecad-mcp/
  if [ "$PUSH" -eq 1 ]; then
    docker push "$rolling"
    docker push "$pinned"
  fi
  echo "    mcp image:  ${rolling}  (pinned: ${pinned})"
}

for t in "${TARGETS[@]}"; do
  case "$t" in
    deps) build_deps ;;
    mcp)  build_mcp  ;;
  esac
done
echo "==> done."
