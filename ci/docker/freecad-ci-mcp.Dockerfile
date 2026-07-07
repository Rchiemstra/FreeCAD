# FreeCAD MCP CI image.
#
# FROM ghcr.io/rchiemstra/freecad-ci-deps:24.04 -- same apt Qt6/OCCT/Coin/Python3.12
# as the FreeCAD build/test image, so the build/debug FreeCAD tree (built against
# those exact libs) loads correctly in this image: ABI-compatible, and
# FreeCADCmd's embedded python3.12 is the SAME interpreter whose site-packages
# receive the editable install below (the interpreter-identity assumption the
# freecad-mcp-load-preflight step verifies).
#
# Bakes the freecad-mcp package + [dev] extras (pytest, pytest-asyncio,
# pytest-xdist, pytest-cov, mcp[cli], validators, hatchling) so the runtime
# `pip install --no-build-isolation --no-deps -e .` is network-restricted-safe:
# the build backend (hatchling) and all test deps are already present, and the
# deps track pyproject.toml rather than a frozen list. The baked /tmp/mcp copy is
# dead weight at runtime (it only carries deps); the runtime editable install
# repoints the finder at the workspace source.
#
# Rebuild when tools/mcp/freecad-mcp/pyproject.toml changes. ci/docker/build-images.sh
# tags this image twice: a rolling :24.04 and an immutable :24.04-<hash-of-pyproject>.
# Published as a PUBLIC image to ghcr.io/rchiemstra/freecad-ci-mcp.
#
# Build context must be the MCP package dir (see ci/docker/build-images.sh) so
# the COPY paths are relative to it and the context stays tiny:
#   docker build -t ghcr.io/rchiemstra/freecad-ci-mcp:24.04 \
#       -f ci/docker/freecad-ci-mcp.Dockerfile tools/mcp/freecad-mcp/

FROM ghcr.io/rchiemstra/freecad-ci-deps:24.04

# Bake the MCP package + [dev] deps. COPY only what the install needs (mirrors
# the package's own Dockerfile COPY set: pyproject, README, src, addon, tests).
WORKDIR /tmp/mcp
COPY pyproject.toml README.md ./
COPY src ./src
COPY addon ./addon
COPY tests ./tests

# PIP_BREAK_SYSTEM_PACKAGES=1 is inherited from the base image.
RUN pip install --no-cache-dir -e ".[dev]"

# Drop the build-only source copy; runtime uses the workspace source via the
# editable finder repointed by the in-step `pip install --no-build-isolation
# --no-deps -e .`. Keep the installed deps + hatchling.
RUN rm -rf /tmp/mcp

WORKDIR /woodpecker
