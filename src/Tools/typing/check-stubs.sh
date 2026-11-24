#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "${script_dir}/../../.." && pwd)"

cd "${repo_root}"

# Keep the existing check running even if the unit suite fails, so both
# failures are visible in one invocation.
unit_status=0
python3 -m unittest discover -s src/Tools/typing/stubgen || unit_status=$?

check_status=0
python3 src/Tools/typing/generate_stubs.py check \
    --root . \
    --out-dir src/Tools/typing/generated || check_status=$?

if (( check_status != 0 )); then
    exit "$check_status"
fi
exit "$unit_status"
