# SPDX-License-Identifier: LGPL-2.1-or-later

ARG BASE_IMAGE=127.0.0.1:5001/freecad-ci-deps:24.04
FROM ${BASE_IMAGE}

USER root

# This is an isolated compatibility lane, not a product submodule or bundled
# dependency. The exact upstream commit is pinned and built only into this
# disposable CI image.
ARG OPENCV_COMMIT=2e1f8da65e4f9fa1a98423e6ac223187438a4db8

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        git \
        libjpeg-dev \
        libpng-dev \
        libtiff-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git init /tmp/opencv \
    && git -C /tmp/opencv remote add origin https://github.com/opencv/opencv.git \
    && git -C /tmp/opencv fetch --depth 1 origin "${OPENCV_COMMIT}" \
    && test "$(git -C /tmp/opencv rev-parse FETCH_HEAD)" = "${OPENCV_COMMIT}" \
    && git -C /tmp/opencv checkout --detach FETCH_HEAD \
    && cmake -S /tmp/opencv -B /tmp/opencv-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/opencv-4.13 \
        -DBUILD_LIST=core,imgproc,imgcodecs,calib3d,objdetect \
        -DBUILD_SHARED_LIBS=ON \
        -DBUILD_TESTS=OFF \
        -DBUILD_PERF_TESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_opencv_apps=OFF \
        -DBUILD_opencv_python2=OFF \
        -DBUILD_opencv_python3=OFF \
        -DBUILD_JAVA=OFF \
        -DWITH_FFMPEG=OFF \
        -DWITH_GSTREAMER=OFF \
        -DWITH_OPENCL=OFF \
        -DWITH_QT=OFF \
    && cmake --build /tmp/opencv-build --parallel 8 \
    && cmake --install /tmp/opencv-build \
    && echo /opt/opencv-4.13/lib > /etc/ld.so.conf.d/opencv-4.13.conf \
    && ldconfig \
    && rm -rf /tmp/opencv /tmp/opencv-build

ENV OpenCV_DIR=/opt/opencv-4.13/lib/cmake/opencv4

RUN test -f "${OpenCV_DIR}/OpenCVConfig.cmake" \
    && grep -Fq "4.13.0" "${OpenCV_DIR}/OpenCVConfig-version.cmake"
