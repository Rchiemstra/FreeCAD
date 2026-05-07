#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# ***************************************************************************
# Minimal substitute for Debian's xvfb-run(1). Prefer the host Xvfb from the
# distro xvfb package (/usr/bin/Xvfb). Conda cos7/sysroot Xvfb is skipped: it
# is built for OpenSSL 1.0 (libcrypto.so.10) and fails on modern images.
#
# Usage:
#   xvfb_run.sh [-a] [-s "Xvfb server args"] -- command [args...]
#
#   -a   accepted for compatibility with xvfb-run (display is always auto-picked)
#   -s   extra arguments for Xvfb after the display (default: -screen 0 1024x768x24)
# ***************************************************************************

set -euo pipefail

usage() {
    echo "Usage: $0 [-a] [-s \"server args\"] -- command [args...]" >&2
    exit 2
}

server_words=(-screen 0 1024x768x24)

while [ "$#" -gt 0 ]; do
    case "$1" in
        --)
            shift
            break
            ;;
        -a)
            shift
            ;;
        -s)
            shift
            if [ "$#" -lt 1 ]; then
                echo "$0: -s requires an argument" >&2
                usage
            fi
            read -r -a server_words <<<"$1"
            shift
            ;;
        *)
            echo "$0: unexpected argument: $1" >&2
            usage
            ;;
    esac
done

[ "$#" -ge 1 ] || usage

find_xvfb() {
    local c
    # Host / distro (e.g. apt install xvfb in Docker, GitHub ubuntu-* runners).
    for c in /usr/bin/Xvfb /usr/local/bin/Xvfb; do
        if [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    done
    if [ -n "${CONDA_PREFIX:-}" ]; then
        c="${CONDA_PREFIX}/bin/Xvfb"
        if [ -x "$c" ]; then
            echo "$c"
            return 0
        fi
    fi
    if command -v Xvfb >/dev/null 2>&1; then
        c=$(command -v Xvfb)
        case "$c" in
            */sysroot/usr/bin/Xvfb)
                ;;
            *)
                echo "$c"
                return 0
                ;;
        esac
    fi
    return 1
}

xvfb_bin="$(find_xvfb)" || {
    echo "$0: Xvfb not found. Install the distro xvfb package (e.g. apt install xvfb)." >&2
    exit 127
}

pick_display() {
    local n=99
    while [ -e "/tmp/.X11-unix/X$n" ] || [ -e "/tmp/.X$n-lock" ]; do
        n=$((n + 1))
        if [ "$n" -gt 250 ]; then
            echo "$0: no free display in range 99-250" >&2
            return 1
        fi
    done
    echo "$n"
}

# Always pick a free display so parallel runs do not collide on :99.
d="$(pick_display)"
export DISPLAY=:"$d"

logfile="$(mktemp "${TMPDIR:-/tmp}/xvfb_run.XXXXXX")"
"$xvfb_bin" "$DISPLAY" "${server_words[@]}" -nolisten tcp >"$logfile" 2>&1 &
xvfb_pid=$!

cleanup() {
    kill "$xvfb_pid" 2>/dev/null || true
    rm -f "$logfile"
}
trap cleanup EXIT

ready=0
for _ in $(seq 1 200); do
    if [ -S "/tmp/.X11-unix/X$d" ]; then
        ready=1
        break
    fi
    sleep 0.05
done

if [ "$ready" -ne 1 ]; then
    echo "$0: Xvfb did not create /tmp/.X11-unix/X$d; log:" >&2
    cat "$logfile" >&2 || true
    exit 1
fi

set +e
"$@"
rc=$?
set -e
exit "$rc"
