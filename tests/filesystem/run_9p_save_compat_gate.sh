#!/usr/bin/env bash
# Explicit 9p save-compatibility gate.
#
# Ordinary App/GUI tests now run from a temporary CWD outside the 9p bind
# mount. That keeps unrelated tests honest, but it must never be mistaken for
# fixing or dropping 9p support, so this gate exercises a real Save As directly
# ON the 9p mount and must keep passing.
#
# Background: on 9p a rename performed while a descriptor is still held leaves
# the destination name unresolvable, and fstat on that retained descriptor also
# fails. DocumentFileWriter therefore releases the installed descriptor and
# re-resolves the destination through the pinned parent before it will report a
# replacement as verified.
#
# Usage: run_9p_save_compat_gate.sh <FreeCADCmd> <directory-on-9p>
set -uo pipefail

FREECADCMD="${1:?usage: run_9p_save_compat_gate.sh <FreeCADCmd> <directory-on-9p>}"
GATE_DIR="${2:?usage: run_9p_save_compat_gate.sh <FreeCADCmd> <directory-on-9p>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ ! -x "$FREECADCMD" ]; then
    echo "FAIL: FreeCADCmd not found or not executable: $FREECADCMD" >&2
    exit 2
fi

FSTYPE="$(findmnt -no FSTYPE --target "$(dirname "$GATE_DIR")" 2>/dev/null || echo unknown)"
echo "9p save-compatibility gate"
echo "  gate dir  : $GATE_DIR"
echo "  filesystem: $FSTYPE"
if [ "$FSTYPE" != "9p" ]; then
    echo "WARNING: gate directory is on '$FSTYPE', not 9p; this run proves nothing about 9p." >&2
fi

# Keep the process HOME/TMPDIR off the mount under test so only the save target
# exercises 9p. Dot-free, for the same reason as the ordinary lane.
RUN_DIR="$(mktemp -d /tmp/fc9pXXXXXX)"
export HOME="$RUN_DIR/home"
export FREECAD_USER_HOME="$HOME"
export TMPDIR="$RUN_DIR/tmp"
export TZ="${TZ:-UTC}"
export LANG="${LANG:-C.UTF-8}"
export LC_ALL="${LC_ALL:-C.UTF-8}"
mkdir -p "$HOME" "$TMPDIR" "$GATE_DIR"

export FC_9P_GATE_DIR="$GATE_DIR"

# The gate script must not call sys.exit(): under FreeCADCmd that discards all
# of its output. It prints a machine-readable verdict line instead.
OUTPUT="$(cd "$RUN_DIR" && "$FREECADCMD" -c "$SCRIPT_DIR/9p_save_compat_gate.py" 2>&1)"
echo "$OUTPUT" | grep -E '^  (ok   -|FAIL -)|^9P_GATE_RESULT' || true

echo ""
echo "gate directory contents:"
ls -1 "$GATE_DIR" 2>/dev/null | sed 's/^/  /' || true

if echo "$OUTPUT" | grep -q '^9P_GATE_RESULT: PASSED'; then
    exit 0
fi
echo "9p gate FAILED" >&2
exit 1
