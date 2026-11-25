#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the embedded test with the interpreter selected by this build."""
from pathlib import Path
import os
import signal
import subprocess
import sys
import time

# Bounds imports, the event loop, and interpreter shutdown as well as the test.
DEADLINE_SECONDS = 120


def run(command, timeout=DEADLINE_SECONDS):
    child = subprocess.Popen(command, start_new_session=True)
    timed_out = False
    try:
        deadline = time.monotonic() + timeout
        while True:
            # WNOWAIT keeps the leader (and therefore its PID/PGID) reserved
            # until the initial group has been signaled.
            if os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT):
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                print(f"Embedded GUI test exceeded {timeout} seconds", file=sys.stderr, flush=True)
                break
            time.sleep(min(remaining, 0.05))
    finally:
        try:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        finally:
            try:
                status = child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                print("Embedded GUI test leader could not be reaped", file=sys.stderr, flush=True)
                # An unexpected inability to reap is a failure, never success.
                status = 124
    return 124 if timed_out else status



def main():
    config = Path(sys.argv[1]).resolve(strict=True)
    python = config.read_text(encoding="utf-8").splitlines()[0]
    script = Path(__file__).with_name("test_embedded_gui_loop.py")
    return run([python, "-I", str(script), str(config)])


if __name__ == "__main__":
    raise SystemExit(main())
