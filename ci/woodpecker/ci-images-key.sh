#!/bin/sh
# Run after MCP checkout. After input changes OR upstream merges, run:
#   sh ci/woodpecker/ci-images-key.sh update
# Commit BOTH YAML files with the inputs. Refresh external packages by changing
# ci/images-refresh.txt. See ci/woodpecker/ci-images.md.
set -eu

deps_hash=$(sha256sum package/ubuntu/Dockerfile \
  package/ubuntu/install-apt-packages.sh package/ubuntu/neon-archive-keyring.asc \
  ci/images-refresh.txt)
deps_key=$(printf 'deps-v2\n%s\n' "$deps_hash" | sha256sum | cut -c1-12)
mcp_hash=$(sha256sum tools/mcp/freecad-mcp/Dockerfile.ci \
  tools/mcp/freecad-mcp/pyproject.toml)
mcp_key=$(printf 'mcp-v2\n%s\n%s\n' "$deps_key" "$mcp_hash" | sha256sum | cut -c1-12)

case "${1:-check}" in
  tags) printf '24.04-%s\n24.04-%s\n' "$deps_key" "$mcp_key" ;;
  update|check)
    for workflow in .woodpecker/ci.yml .woodpecker/build-images.yml; do
      for which in deps mcp; do
        case "$which" in deps) key=$deps_key ;; mcp) key=$mcp_key ;; esac
        if [ "${1:-check}" = update ]; then
          sed -i -E "s#(freecad-ci-$which:)24\.04(-[a-f0-9]+)?#\124.04-$key#g" "$workflow"
        fi
        refs=$(sed '/^[[:space:]]*#/d' "$workflow" | grep -oE "freecad-ci-$which:[a-zA-Z0-9.-]+" || true)
        if [ -z "$refs" ] || [ "$(printf '%s\n' "$refs" | sort -u)" != "freecad-ci-$which:24.04-$key" ]; then
          echo "ERROR: $workflow has a stale $which image key (expected 24.04-$key)." >&2
          echo 'Run: sh ci/woodpecker/ci-images-key.sh update; commit both YAML files with the input changes.' >&2
          exit 1
        fi
      done
    done
    echo "CI image keys OK: deps=24.04-$deps_key mcp=24.04-$mcp_key"
    ;;
  *) echo 'usage: ci-images-key.sh [check|update|tags]' >&2; exit 2 ;;
esac
