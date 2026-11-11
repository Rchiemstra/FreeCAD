#!/bin/sh
# Best-effort gdb for the GUI test steps. The deps image has no gdb; the GUI
# runners attach to a hung or crashed FreeCAD when it is there. gdb only adds
# diagnostics, so an apt outage (a third-party list without a Release file, a
# mirror timeout) must not fail the step: warn and go on without it.
# Woodpecker runs these images as root (no USER in the image or the step).
command -v gdb >/dev/null 2>&1 && exit 0
if [ "$(id -u)" != 0 ]; then
  echo "ensure-gdb: not root, gdb not installed; hang and crash backtraces will be missing" >&2
  exit 0
fi
# apt-get update exits non-zero when one list fails but still refreshes the
# others, so try the install either way.
apt-get update -qq || echo "ensure-gdb: apt-get update reported errors; trying the install anyway" >&2
apt-get install -y -qq --no-install-recommends gdb >/dev/null ||
  echo "ensure-gdb: gdb could not be installed; hang and crash backtraces will be missing" >&2
exit 0
