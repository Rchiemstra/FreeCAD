#!/usr/bin/env bash
# Build and publish the FreeCAD Woodpecker CI images to the local registry
# (default registry:5000 -- the same registry the Woodpecker agent pulls from).
#
#   freecad-ci-deps:24.04   -- Ubuntu 24.04 + package/ubuntu/install-apt-packages.sh
#                              + ccache/ninja/xvfb/clang-format/python3-pip/git.
#   freecad-ci-mcp:24.04    -- FROM freecad-ci-deps:24.04 + pip install -e ".[dev]"
#                              (the freecad-mcp package + pytest/hatchling/mcp[cli]).
#
# Dockerfiles live in their build-context dirs (so the context stays tiny, no
# .dockerignore needed, and kaniko can build them with the same context):
#   package/ubuntu/Dockerfile               (context: package/ubuntu/)
#   tools/mcp/freecad-mcp/Dockerfile.ci     (context: tools/mcp/freecad-mcp/)
#
# Each image is tagged twice:
#   :24.04              -- rolling; what .woodpecker/ci.yml references (always latest).
#   :24.04-<hash>       -- immutable; keyed on the file that defines the image
#                          content (install-apt-packages.sh for deps, pyproject.toml
#                          for mcp). Use for traceability / rollback.
#
# Rebuild whenever:
#   * package/ubuntu/install-apt-packages.sh changes  -> rebuild deps (and mcp).
#   * tools/mcp/freecad-mcp/pyproject.toml changes    -> rebuild mcp.
#   * a weekly security refresh of the apt base       -> rebuild deps (and mcp).
#
# NOTE: this script is for LOCAL/optional builds. The PRIMARY build path is the
# .woodpecker/build-images.yml kaniko workflow, which builds on the Woodpecker
# agent (where registry:5000 resolves). Use this script only if you want to build
# from your own machine -- which requires your Docker daemon to reach registry:5000
# AND to list it under insecure-registries (it is an HTTP registry):
#   Settings -> Docker Engine -> {"insecure-registries": ["registry:5000"]}
#
# Usage (from the repo root):
#   ci/build-images.sh            # build + push deps, then mcp
#   ci/build-images.sh deps       # only deps
#   ci/build-images.sh mcp        # only mcp (deps must already exist)
#   ci/build-images.sh --no-push  # build locally, don't push
#
# Overrides (env):
#   REGISTRY=registry:5000   # registry host:port
#   NS=                      # optional namespace segment (default none -> registry/<repo>:tag)
set -euo pipefail

REGISTRY="${REGISTRY:-registry:5000}"
NS="${NS:-}"            # default: no namespace (image = registry/<repo>:tag)
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

# image_name <repo> -> REGISTRY/[NS/]repo
image_name() {
  if [ -n "${NS}" ]; then echo "${REGISTRY}/${NS}/$1"; else echo "${REGISTRY}/$1"; fi
}

short_hash() {  # short_hash <file>
  sha256sum "$1" | awk '{print substr($1,1,12)}'
}

build_deps() {
  local script="package/ubuntu/install-apt-packages.sh"
  local h; h="$(short_hash "$script")"
  local rolling; rolling="$(image_name freecad-ci-deps):24.04"
  local pinned;  pinned="$(image_name freecad-ci-deps):24.04-${h}"
  echo "==> freecad-ci-deps  (key=${h}  script=${script})"
  docker build -f package/ubuntu/Dockerfile \
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
  local rolling; rolling="$(image_name freecad-ci-mcp):24.04"
  local pinned;  pinned="$(image_name freecad-ci-mcp):24.04-${h}"
  echo "==> freecad-ci-mcp  (key=${h}  pyproject=${pf})"
  # FROM the (just-built/pushed) deps rolling tag, so ABI tracks the same base.
  docker build -f tools/mcp/freecad-mcp/Dockerfile.ci \
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
