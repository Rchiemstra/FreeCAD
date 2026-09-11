#!/usr/bin/env python3
"""Cross-process proof that ``<document>.FCStd.FreeCAD-save.lock`` is a lock anchor.

The zero-byte file left beside every saved document has so far only been
*characterized* from source as a rendezvous name for a kernel byte-range lock
rather than a leaked artifact. This gate measures it.

``DocumentFileLock`` pairs a process-local ``std::timed_mutex`` with
``Base::FileLock``. Only the second half crosses process boundaries, and POSIX
record locks are per-process by definition, so nothing about the cross-process
contract is observable from a single process. This driver therefore runs real
child processes (``save_lock_probe``) and arbitrates them over an explicit
line protocol: every state transition is *acknowledged* before the next step
begins. No step is sequenced by sleeping.

Proved here:

* A holds the lock and B, in a different process, cannot take it;
* A and B never report simultaneous ownership;
* B acquires only after A has acknowledged its release;
* a third process C later coordinates through the same pathname;
* the pathname persists across every acquire/release cycle;
* a process that dies while holding the lock still releases it, so the
  surviving zero-byte name never implies stale ownership.

Usage: save_lock_anchor_gate.py <save_lock_probe> [work-directory]
"""

import os
import queue
import subprocess
import sys
import threading

# Only a deadlock guard. Every acquire uses a non-blocking timeout in the probe,
# so a healthy step answers immediately; this bound exists so a defect surfaces
# as a failure instead of a hung gate.
REPLY_TIMEOUT_SECONDS = 60

failures = []
checks = 0


def check(condition, description, detail=""):
    global checks
    checks += 1
    if condition:
        print("  ok   - %s" % description)
    else:
        print("  FAIL - %s %s" % (description, detail))
        failures.append(description)


class Probe:
    """One helper process, driven by acknowledged commands."""

    def __init__(self, name, executable, destination):
        self.name = name
        self._process = subprocess.Popen(
            [executable, destination],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._lines = queue.Queue()
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def _pump(self):
        for line in self._process.stdout:
            self._lines.put(line.rstrip("\r\n"))
        self._lines.put(None)

    def readline(self):
        try:
            line = self._lines.get(timeout=REPLY_TIMEOUT_SECONDS)
        except queue.Empty:
            raise AssertionError("%s: no reply within %ds" % (self.name, REPLY_TIMEOUT_SECONDS))
        if line is None:
            raise AssertionError("%s: process closed its output unexpectedly" % self.name)
        return line

    def command(self, text):
        self._process.stdin.write(text + "\n")
        self._process.stdin.flush()
        reply = self.readline()
        print("    %s: %s -> %s" % (self.name, text, reply))
        return reply

    def wait_ready(self):
        line = self.readline()
        if line != "READY":
            raise AssertionError("%s: expected READY, got %r" % (self.name, line))
        print("    %s: READY" % self.name)

    def wait_exit(self, expected=None):
        code = self._process.wait(timeout=REPLY_TIMEOUT_SECONDS)
        if expected is not None and code != expected:
            raise AssertionError("%s: exit %r, expected %r" % (self.name, code, expected))
        return code

    def terminate(self):
        if self._process.poll() is None:
            self._process.kill()
            self._process.wait(timeout=REPLY_TIMEOUT_SECONDS)


def owners(*probes):
    """Ask every live probe whether it owns the lock, right now."""
    return [probe.name for probe in probes if probe.command("HELD") == "HELD yes"]


def anchor_state(lock_path):
    if not os.path.exists(lock_path):
        return "absent"
    return "present size=%d" % os.path.getsize(lock_path)


def run(executable, workdir):
    # --- 1. destination and the common lock pathname -------------------------
    os.makedirs(workdir, exist_ok=True)
    destination = os.path.join(workdir, "anchor.FCStd")
    with open(destination, "wb") as handle:
        handle.write(b"canonical document bytes")

    # lockPathFor() appends the suffix to the destination's filename in the
    # destination's own directory, so this is exactly the production pathname.
    lock_path = destination + ".FreeCAD-save.lock"

    print("cross-process save-lock anchor gate")
    print("  probe      : %s" % executable)
    print("  destination: %s" % destination)
    print("  lock path  : %s" % lock_path)
    print("  anchor     : %s (before any lock)" % anchor_state(lock_path))
    print("")

    probes = []
    try:
        # --- 2. A acquires and signals LOCKED ------------------------------
        a = Probe("A", executable, destination)
        probes.append(a)
        a.wait_ready()
        check(a.command("LOCK") == "LOCKED", "A acquires the lock")
        check(os.path.exists(lock_path), "the anchor pathname exists once A holds the lock")

        # --- 3/4. B starts and is refused while A holds --------------------
        b = Probe("B", executable, destination)
        probes.append(b)
        b.wait_ready()
        check(b.command("LOCK") == "BLOCKED", "B cannot acquire while A holds the lock")

        # Exclusivity is sampled at every ownership transition rather than
        # asserted once, so a defect that lets both sides in is caught wherever
        # it appears rather than only in the one window we guessed.
        held_by = owners(a, b)
        check(held_by == ["A"], "with A holding, exactly A owns the lock", "owners=%r" % held_by)

        # --- 5/6. A releases and acknowledges ------------------------------
        check(a.command("RELEASE") == "RELEASED", "A releases the lock")
        held_by = owners(a, b)
        check(held_by == [], "after A released, nobody owns the lock", "owners=%r" % held_by)

        # --- 7. B retries and now acquires ---------------------------------
        check(b.command("LOCK") == "LOCKED", "B acquires the lock after A released it")
        held_by = owners(a, b)
        check(held_by == ["B"], "with B holding, exactly B owns the lock", "owners=%r" % held_by)

        check(a.command("LOCK") == "BLOCKED", "A is now refused while B holds the lock")
        held_by = owners(a, b)
        check(held_by == ["B"], "A's refused retry did not create a second owner",
              "owners=%r" % held_by)

        # --- 8. B releases and exits, then C starts ------------------------
        check(b.command("RELEASE") == "RELEASED", "B releases the lock")
        check(b.command("EXIT") == "BYE", "B exits cleanly")
        b.wait_exit(expected=0)
        check(a.command("EXIT") == "BYE", "A exits cleanly")
        a.wait_exit(expected=0)

        check(os.path.exists(lock_path), "the anchor pathname survives both processes exiting")

        # --- 9. C coordinates through the same persistent pathname ---------
        c = Probe("C", executable, destination)
        probes.append(c)
        c.wait_ready()
        check(c.command("LOCK") == "LOCKED",
              "C acquires through the same persistent lock pathname")

        # --- crash release --------------------------------------------------
        # C dies while still holding the lock. Nothing unwinds, so only the
        # kernel can release it.
        check(c.command("ABANDON") == "ABANDONING", "C abandons the lock without unwinding")
        check(c.wait_exit() == 9, "C died while still holding the lock")

        check(os.path.exists(lock_path), "the anchor pathname survives an abandoned lock")

        d = Probe("D", executable, destination)
        probes.append(d)
        d.wait_ready()
        check(d.command("LOCK") == "LOCKED",
              "D acquires after C died holding it: process death releases the kernel lock")
        check(d.command("EXIT") == "BYE", "D exits cleanly")
        d.wait_exit(expected=0)

        # --- the anchor is not ownership -----------------------------------
        check(os.path.exists(lock_path), "the anchor pathname is still present at the end")
        check(os.path.getsize(lock_path) == 0, "the anchor is a zero-byte rendezvous name",
              "size=%d" % os.path.getsize(lock_path))

        e = Probe("E", executable, destination)
        probes.append(e)
        e.wait_ready()
        check(e.command("LOCK") == "LOCKED",
              "a surviving anchor pathname implies no stale ownership")
        check(e.command("EXIT") == "BYE", "E exits cleanly")
        e.wait_exit(expected=0)

    finally:
        for probe in probes:
            probe.terminate()

    print("")
    print("  final anchor: %s" % anchor_state(lock_path))
    print("  directory   : %s" % sorted(os.listdir(workdir)))


def main():
    if len(sys.argv) < 2:
        print("usage: save_lock_anchor_gate.py <save_lock_probe> [work-directory]")
        return 2

    executable = os.path.abspath(sys.argv[1])
    if not os.path.isfile(executable):
        print("FAIL - probe executable not found: %s" % executable)
        return 2

    workdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.getcwd(), "lock-anchor-gate")

    try:
        run(executable, workdir)
    except AssertionError as error:
        print("  FAIL - %s" % error)
        failures.append(str(error))
    except Exception as error:  # noqa: BLE001 - the verdict line must always print
        print("  FAIL - unexpected driver failure: %r" % (error,))
        failures.append(repr(error))

    print("")
    if failures:
        print("LOCK_ANCHOR_GATE_RESULT: FAILED (%d of %d) %s"
              % (len(failures), checks, "; ".join(failures)))
        return 1
    print("LOCK_ANCHOR_GATE_RESULT: PASSED (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
