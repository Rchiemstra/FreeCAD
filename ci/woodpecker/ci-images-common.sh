#!/bin/sh
# Shared POSIX/BusyBox helpers. See ci-images.md for the publication protocol.
# Variables loaded here are consumed by the sourcing gate/build scripts.
# shellcheck disable=SC2034
load_image_tags() {
  image_state=${FREECAD_CI_IMAGE_STATE:-.ci-images}
  deps_tag=$(cat "$image_state/deps-tag")
  mcp_tag=$(cat "$image_state/mcp-tag")
  registry=${FREECAD_CI_REGISTRY:?FREECAD_CI_REGISTRY is required}
  registry=${registry%/}
  registry_host=${FREECAD_CI_REGISTRY_HOST:-${registry#*://}}
  crane_bin=${CRANE_BIN:-$image_state/crane}
  [ -x "$crane_bin" ] || { echo 'ERROR: crane is required (run ci-images-check first)' >&2; exit 1; }
}

# Only an explicit registry NOT_FOUND means absent. Network/auth/server failures
# must fail closed, rather than treating an inaccessible existing key as absent.
image_digest() {
  if digest=$("$crane_bin" digest --insecure "$registry_host/freecad-ci-$1:$2" 2> "$image_state/registry-error"); then
    printf '%s\n' "$digest"
  elif grep -qE 'MANIFEST_UNKNOWN|NAME_UNKNOWN' "$image_state/registry-error"; then
    return 1
  else
    cat "$image_state/registry-error" >&2
    return 2
  fi
}

image_parent() {
  "$crane_bin" config --insecure "$registry_host/freecad-ci-mcp@$1" > "$image_state/mcp-config" || return 2
  tr '\n' ' ' < "$image_state/mcp-config" | sed -n 's/.*"org.freecad.ci.deps-digest"[[:space:]]*:[[:space:]]*"\(sha256:[a-f0-9]*\)".*/\1/p'
}

# All repo-owned publishers on the agent host use this same bind-mounted path,
# including the local Docker builder. Build outside the lock; recheck and publish
# inside it. A killed owner leaves a lock: time out, never guess it is safe.
lock_publisher() {
  lock_root=${FREECAD_CI_PUBLISH_LOCK_ROOT:-/tmp/freecad-ci-publish}
  mkdir -p "$lock_root"
  # CI runs as root, while the host's optional builder can run as its owner.
  # Sticky shared parent permits both to create a lock without stealing one.
  chmod 1777 "$lock_root" 2>/dev/null || [ -w "$lock_root" ]
  publish_lock=$lock_root/publisher
  lock_attempt=1
  until mkdir "$publish_lock" 2>/dev/null; do
    if [ "$lock_attempt" -ge "${FREECAD_CI_LOCK_ATTEMPTS:-120}" ]; then
      echo "ERROR: publisher lock timeout: $publish_lock (see ci-images.md)" >&2
      return 1
    fi
    sleep "${FREECAD_CI_LOCK_INTERVAL:-1}"
    lock_attempt=$((lock_attempt + 1))
  done
  trap 'rmdir "$publish_lock"' EXIT
  trap 'exit 1' HUP INT TERM
}

repair_alias() {
  "$crane_bin" copy --insecure "$registry_host/freecad-ci-$1@$2" \
    "$registry_host/freecad-ci-$1:24.04"
  echo "freecad-ci-$1:24.04 -> $2"
}
