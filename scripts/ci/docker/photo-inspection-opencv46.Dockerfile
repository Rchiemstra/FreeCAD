# SPDX-License-Identifier: LGPL-2.1-or-later

ARG BASE_IMAGE=127.0.0.1:5001/freecad-ci-deps:24.04
FROM ${BASE_IMAGE}

USER root

# The FreeCAD CI base also carries KDE Neon's repository, which currently
# overrides Ubuntu Noble's OpenCV floor. Disable only that source in this
# derived image so the lane genuinely tests Ubuntu's supported 4.6 packages.
RUN rm -f /etc/apt/sources.list.d/neon-qt.list \
    && apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libopencv-contrib-dev \
        libopencv-dev \
    && rm -rf /var/lib/apt/lists/*

RUN test "$(pkg-config --modversion opencv4 | cut -d. -f1,2)" = "4.6" \
    && dpkg-query -W \
        libopencv-calib3d-dev \
        libopencv-contrib-dev \
        libopencv-core-dev \
        libopencv-imgcodecs-dev \
        libopencv-imgproc-dev \
        libopencv-objdetect-dev
