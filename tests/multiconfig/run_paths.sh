#!/bin/sh
# Run through the local compiler-slot wrapper where one is required.
set -eu
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_root=${1:?supply a fresh build directory}
for config in Debug Release RelWithDebInfo MinSizeRel; do
    cmake -S "$source_dir/tests/multiconfig/fixture" -B "$build_root/single-$config" -G Ninja -DFREECAD_SOURCE_DIR="$source_dir" -DCMAKE_BUILD_TYPE="$config"
done
cmake -S "$source_dir/tests/multiconfig/fixture" -B "$build_root/multi" -G 'Ninja Multi-Config' -DFREECAD_SOURCE_DIR="$source_dir" '-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo;MinSizeRel'
python3 "$source_dir/tests/multiconfig/check_paths.py" "$build_root"
