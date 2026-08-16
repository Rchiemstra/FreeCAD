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
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*)
        if [ -z "${FREECAD_LIBPACK_BIN:-}" ]; then
            echo "WARNING: FREECAD_LIBPACK_BIN is not set." >&2
            echo "         Windows extension modules ignore PATH, so Part/Sketcher" >&2
            echo "         will fail to import and their tests will fail in SetUp." >&2
        fi
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
