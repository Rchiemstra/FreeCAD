#!/bin/sh
# Build a candidate archive without publishing. Serialize the final recheck,
# keyed publication and compatibility-alias repair across both workflows/local.
set -eu
which=${1:?usage: ci-images-build.sh <deps|mcp>}
case "$which" in deps|mcp) ;; *) echo "unknown image '$which'" >&2; exit 1 ;; esac
. ci/woodpecker/ci-images-common.sh
load_image_tags
case "$which" in deps) tag=$deps_tag ;; mcp) tag=$mcp_tag ;; esac
parent=
if [ "$which" = mcp ]; then
  parent=$(image_digest deps "$deps_tag") || { echo 'ERROR: build deps before MCP' >&2; exit 1; }
fi
existing=
if existing=$(image_digest "$which" "$tag"); then
  if [ "$which" = deps ] || [ "$(image_parent "$existing")" = "$parent" ]; then
    lock_publisher
    # Re-enter the common final check under the lock, even on no-op paths.
  else
    existing=
  fi
else
  rc=$?; [ "$rc" -eq 1 ] || exit "$rc"
fi
candidate=$image_state/$which.tar
if [ -z "$existing" ]; then
  workspace=${CI_WORKSPACE:-$(pwd)}
  case "$which" in
    deps) context="$workspace/package/ubuntu"; dockerfile="$context/Dockerfile" ;;
    mcp) context="$workspace/tools/mcp/freecad-mcp"; dockerfile="$context/Dockerfile.ci" ;;
  esac
  [ -f "$dockerfile" ] || { echo "missing Dockerfile: $dockerfile" >&2; exit 1; }
  if [ "${FREECAD_CI_BUILD_ENGINE:-kaniko}" = docker ]; then
    set -- --pull -f "$dockerfile" -t "freecad-ci-$which:$tag"
    if [ "$which" = mcp ]; then
      set -- "$@" --build-arg "FREECAD_CI_DEPS_IMAGE=$registry_host/freecad-ci-deps@$parent" \
        --label "org.freecad.ci.deps-digest=$parent"
    fi
    docker build "$@" "$context"
    docker save -o "$candidate" "freecad-ci-$which:$tag"
  else
    set -- --context="dir://$context" --dockerfile="$dockerfile" \
      --destination="$registry_host/freecad-ci-$which:$tag" --no-push --tar-path="$candidate" \
      --insecure --skip-tls-verify --insecure-pull --skip-tls-verify-pull --single-snapshot
    if [ "$which" = mcp ]; then
      set -- "$@" --build-arg="FREECAD_CI_DEPS_IMAGE=$registry_host/freecad-ci-deps@$parent" \
        --label="org.freecad.ci.deps-digest=$parent"
    fi
    "${KANIKO_EXECUTOR:-/kaniko/executor}" "$@"
  fi
  lock_publisher
fi
# No work may change the parent between the build and its publication.
if [ "$which" = mcp ] && [ "$(image_digest deps "$deps_tag")" != "$parent" ]; then
  echo 'ERROR: deps changed during MCP build; rerun the pipeline' >&2
  exit 1
fi
publish=0
old_digest=; old_parent=
if digest=$(image_digest "$which" "$tag"); then
  if [ "$which" = mcp ]; then
    old_parent=$(image_parent "$digest")
    if [ -z "$old_parent" ]; then
      echo 'ERROR: existing MCP key has no recorded parent; refusing to overwrite it' >&2
      exit 1
    fi
    if [ "$old_parent" != "$parent" ]; then
      old_digest=$digest
      publish=1
    fi
  fi
else
  rc=$?; [ "$rc" -eq 1 ] || exit "$rc"
  publish=1
fi
if [ "$publish" -eq 1 ]; then
  [ -f "$candidate" ] || { echo 'ERROR: image evicted during no-op; rerun pipeline' >&2; exit 1; }
  if [ -n "$old_digest" ]; then
    echo "replacing stale-parent MCP tag $tag: old MCP digest=$old_digest; old parent=$old_parent; new parent=$parent"
  fi
  "$crane_bin" push --insecure "$candidate" "$registry_host/freecad-ci-$which:$tag"
  digest=$(image_digest "$which" "$tag")
else
  echo "freecad-ci-$which:$tag already present or appeared during build ($digest); using it"
fi
repair_alias "$which" "$digest"
rm -f "$candidate"
