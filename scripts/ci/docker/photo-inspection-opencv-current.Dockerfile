# SPDX-License-Identifier: LGPL-2.1-or-later

ARG BASE_IMAGE=127.0.0.1:5001/freecad-ci-deps:24.04
FROM ${BASE_IMAGE}

USER root

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libopencv-contrib-dev \
        libopencv-dev \
    && rm -rf /var/lib/apt/lists/*

RUN pkg-config --modversion opencv4 \
    && dpkg-query -W \
        libopencv-calib3d-dev \
        libopencv-contrib-dev \
        libopencv-core-dev \
        libopencv-imgcodecs-dev \
        libopencv-imgproc-dev \
        libopencv-objdetect-dev
