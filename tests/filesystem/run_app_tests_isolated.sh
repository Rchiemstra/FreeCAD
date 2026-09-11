#!/usr/bin/env bash
# Run ordinary App/GUI tests from a fresh per-run temporary working directory.
#
# Why: several tests save through *relative* paths, so they write into the
# process CWD. Running them from the repository root put those saves on the 9p
# bind mount and also littered the working tree. Neither is acceptable for an
# ordinary test lane.
#
# This is an environment correction only. It does NOT fix or excuse 9p support:
# 9p is covered explicitly by run_9p_save_compat_gate.sh, which must keep
# running and must keep passing.
#
# The per-run directory is deliberately dot-free. Base::FileInfo::extension()
# reports everything after the last dot in the whole path, so a dotted TMPDIR
# (as `mktemp -d` produces) makes BackupPolicy write timestamp backups outside
# the document directory. That defect is tracked separately; this lane must not
# be contaminated by it.
#
# Usage: run_app_tests_isolated.sh <test-binary> [gtest args...]
set -uo pipefail

BINARY="${1:?usage: run_app_tests_isolated.sh <test-binary> [gtest args...]}"
shift

if [ ! -x "$BINARY" ]; then
    echo "FAIL: test binary not found or not executable: $BINARY" >&2
    exit 2
fi

export TZ="${TZ:-UTC}"
export LANG="${LANG:-C.UTF-8}"
export LC_ALL="${LC_ALL:-C.UTF-8}"

RUN_DIR="$(mktemp -d /tmp/fcrunXXXXXX)"   # dot-free by construction
export HOME="$RUN_DIR/home"
export FREECAD_USER_HOME="$HOME"
export TMPDIR="$RUN_DIR/tmp"
CWD="$RUN_DIR/cwd"
mkdir -p "$HOME" "$TMPDIR" "$CWD"

case "$RUN_DIR" in
    *.*) echo "FAIL: run directory contains a dot: $RUN_DIR" >&2; exit 2 ;;
esac

# Since Python 3.8 a Windows extension module resolves its dependencies through
# the directories registered with os.add_dll_directory(), not through PATH.
# FreeCADInit registers FREECAD_LIBPACK_BIN for exactly that. Without it Part
# and Sketcher fail to import, every fixture that adds a Sketcher object throws
# in SetUp, and roughly a dozen tests fail for a reason that has nothing to do
# with the code under test. Say so up front rather than letting the run be read
# as a source regression.
#
# A warning is not enough: an unset value costs roughly a dozen failures that
# look like source regressions. Derive it from the build that produced the
# binary, verify it really holds the OpenCASCADE DLLs, and refuse to start
# otherwise, so this can never again be misread as a FreeCAD defect.
libpack_has_occ() {
    [ -d "$1" ] || return 1
    # A directory without TK*.dll cannot satisfy Part/Sketcher, whatever it is.
    ls "$1"/TK*.dll >/dev/null 2>&1
}

derive_libpack_bin() {
    # Walk up from the binary to the CMake build directory that produced it and
    # take the prefix that build was actually configured against.
    dir="$(cd "$(dirname "$BINARY")" && pwd)"
    while [ -n "$dir" ] && [ "$dir" != "/" ]; do
        if [ -f "$dir/CMakeCache.txt" ]; then
            prefix="$(sed -n 's/^CMAKE_PREFIX_PATH:[^=]*=//p' "$dir/CMakeCache.txt" | head -1)"
            [ -n "$prefix" ] && echo "$prefix/bin"
            return
        fi
        dir="$(dirname "$dir")"
    done
}

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        if [ -z "${FREECAD_LIBPACK_BIN:-}" ]; then
            derived="$(derive_libpack_bin)"
            if libpack_has_occ "$derived"; then
                export FREECAD_LIBPACK_BIN="$derived"
                echo "derived FREECAD_LIBPACK_BIN from the build: $derived"
            else
                echo "FAIL: FREECAD_LIBPACK_BIN is not set and could not be derived." >&2
                echo "      Tried: ${derived:-<no CMakeCache.txt above the binary>}" >&2
                echo "      Since Python 3.8 a Windows extension module resolves its" >&2
                echo "      dependencies through os.add_dll_directory(), not PATH, so" >&2
                echo "      Part and Sketcher would fail to import and roughly a dozen" >&2
                echo "      tests would fail in SetUp for reasons unrelated to the code" >&2
                echo "      under test. Refusing to run a suite whose result could not" >&2
                echo "      be read honestly." >&2
                exit 2
            fi
        elif ! libpack_has_occ "$FREECAD_LIBPACK_BIN"; then
            echo "FAIL: FREECAD_LIBPACK_BIN does not contain the OpenCASCADE DLLs." >&2
            echo "      Value: $FREECAD_LIBPACK_BIN" >&2
            exit 2
        fi

        # The executable also needs its own core DLLs and the libpack on PATH.
        # Without them Windows fails the image load with exit 127 and no output
        # whatsoever -- an empty run that is even easier to misread as a passing
        # or hanging suite than the missing-libpack case above.
        to_posix() {
            if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; else echo "$1"; fi
        }
        PATH="$(cd "$(dirname "$BINARY")" && pwd):$(to_posix "$FREECAD_LIBPACK_BIN"):$PATH"
        export PATH
        ;;
esac

echo "isolated test run"
echo "  binary : $BINARY"
echo "  run dir: $RUN_DIR"
echo "  cwd    : $CWD"
echo "  libpack: ${FREECAD_LIBPACK_BIN:-<unset>}"

( cd "$CWD" && "$BINARY" "$@" )
STATUS=$?

echo ""
echo "artifacts left in the test CWD (should never appear in the repository):"
ls -1 "$CWD" 2>/dev/null | sed 's/^/  /' || true

exit "$STATUS"
