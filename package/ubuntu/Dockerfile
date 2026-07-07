# FreeCAD CI dependencies image (Ubuntu 24.04).
#
# Mirrors what FreeCAD's own GitHub Actions does on an ubuntu-24.04 runner: it
# runs install-apt-packages.sh (sits next to this Dockerfile), which adds the
# KDE Neon Qt6/PySide6 apt repo and installs the full FreeCAD build dependency
# set (Qt6, OCCT, Coin, Python3.12 dev, Boost, VTK, PCL, Eigen, Xerces, etc.)
# plus build-essential, ccache, ninja, cmake and xvfb. On top of that it adds
# clang-format, python3-pip and git which the CI steps need but the apt script
# does not pull in.
#
# This is the image every FreeCAD-track build/test step runs in, AND the base for
# freecad-ci-mcp:24.04 (so the MCP live-test image is ABI-compatible with the
# build/debug FreeCAD tree -- same apt Qt6/OCCT/Coin/libpython3.12).
#
# Build context is THIS directory (package/ubuntu/), so the COPY path below is
# relative to it and the context stays tiny (just install-apt-packages.sh). Built
# and pushed to the local registry by .woodpecker/build-images.yml (kaniko):
#   image: 127.0.0.1:8081/freecad-ci-deps:24.04
# Rebuild when install-apt-packages.sh changes (trigger the build-images workflow
# manually) or on a weekly cron for an apt security refresh.
# For a local build: docker build -t 192.168.2.23:8081/freecad-ci-deps:24.04 package/ubuntu/

FROM ubuntu:24.04

# Avoid apt tzdata/region prompts during build.
ENV DEBIAN_FRONTEND=noninteractive

# install-apt-packages.sh uses sudo, wget and gpg -- install those first so the
# script can run unattended as root.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
         sudo wget ca-certificates gnupg \
    && rm -rf /var/lib/apt/lists/*

# Run the upstream apt-deps script (build context is this dir, so the COPY path
# is relative to package/ubuntu/).
COPY install-apt-packages.sh /tmp/install-apt-packages.sh
RUN /tmp/install-apt-packages.sh \
    && rm -f /tmp/install-apt-packages.sh \
    && rm -rf /var/lib/apt/lists/*

# Extra tools the CI steps need that the apt script does not pull in:
#   clang-format - C++ formatting check
#   python3-pip  - MCP editable install + pytest in the MCP child image
#   git          - changed-files detection in lint/build steps
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
         clang-format \
         python3-pip \
         git \
    && rm -rf /var/lib/apt/lists/*

# PEP 668: Ubuntu 24.04's pip is externally-managed; the MCP child image (and any
# in-step pip) needs --break-system-packages. Make that the default so runtime
# `pip install --no-build-isolation --no-deps -e .` works without extra flags.
ENV PIP_BREAK_SYSTEM_PACKAGES=1

WORKDIR /woodpecker
