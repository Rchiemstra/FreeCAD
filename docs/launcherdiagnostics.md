# Launcher GUI diagnostics

Run `python3 start_freecad.py --codex --diagnostics` to record GUI responsiveness
in the isolated profile. Add file paths after the launcher options as usual.
`--gui-diagnostics` is an alias, and legacy launches support the same option.
Without this option, launcher behavior is unchanged. Diagnostics never reads,
exports, saves, closes, restarts, or terminates a document or process.

The launcher prints a separate run directory under
`<isolated-profile>/diagnostics/gui-*/`. It honors `FREECAD_MCP_PROFILE_DIR` and
the isolated helper's `--profile-name` selection. Legacy launches use the
FreeCAD user data directory instead. On POSIX, each run directory is private to
the launching user. A copied runtime and startup macro make each launch
independent of later edits to the repository. The macro runs before the supplied
document paths, including paths containing spaces.

A Qt timer updates `heartbeat.json` every two seconds. Immediately on
installation, and after each successful timer callback, a CPython faulthandler
watchdog is armed for 30 seconds. A blocked GUI produces one Python traceback in
`python-timeout-current.txt`; the watchdog neither repeats nor exits FreeCAD.
After the GUI recovers, the callback retains that dump as
`python-timeout-last.txt`, clears the current file, increments the recovery
counter, and arms a fresh timeout. A slow operation can also exceed this
threshold: a timeout establishes a missed GUI heartbeat, without identifying the
cause or proving a deadlock.

The status files contain counters, timings, and process identity. The monitor
does not inspect documents, local variables, authentication tokens, or RPC
payloads. Python tracebacks contain function names, source filenames, and line
numbers. If no fatal-signal handler is already enabled, diagnostics also enables
faulthandler to write `python-faults.txt`; an existing handler and its destination
are preserved. Normal Qt shutdown stops the monitor, so shutdown stalls after
that point are outside its scope.

The monitor supports CPython 3.10–3.13. These versions bound Python tracebacks to
100 threads, 100 frames per thread, and 500 characters per filename/function
string. Allowing for Unicode escapes, one Python dump fits within 128 MiB. There
is one current and one retained timeout file; status files are capped at 16 KiB.
Recovery copies at most 128 MiB and disables the monitor if a foreign writer
exceeds that bound. This is a bound on the qualified traceback writer and fixed
retention, rather than an operating-system disk quota. Separate launches retain
separate directories; remove old run directories when their evidence is no
longer needed. This opt-in mode owns faulthandler's process-wide timeout timer;
other code should not replace that timer while it is active.

FreeCAD's native `--log-file` is directed to `FreeCAD.log` in the run directory
unless a native log option was supplied already. Native logging is separate
from the bounded monitor files and has no new rotation or size limit. Standard
output and error remain inherited; capture them explicitly when needed:

```sh
python3 start_freecad.py --codex --diagnostics > /tmp/freecad-launch.log 2>&1
```

Native and console logs can contain filenames and messages from workbenches;
review them before sharing. A heartbeat timeout with a live process indicates a
stall. For authenticated isolated launches, diagnostics also uses the existing
helper's `--exit-status-file` mode. The helper stays alive as the exact child's
exit-code owner, while Codex registration proceeds after the helper publishes
authenticated readiness. `exit-status.json` initially contains the child PID
and `returncode: null`; after exit it contains the raw return code. On POSIX, a
negative code identifies the terminating signal (for example, `-11` for
SIGSEGV); `0` records a normal exit. A missing, stale, or still-pending receipt
alone does not prove that a process is currently alive: check its process
identity as well. If the helper itself is terminated, the final receipt can be
missing. Diagnostics prints the monitor PID and receipt path. An explicit
`--exit-status-file` is honored. `--supervise` retains its existing lifecycle
and gets no automatic exit-status option; combining it with an explicit status
option remains invalid. Legacy launches retain their existing exit handling.

A fatal traceback can help diagnose a crash, but sudden termination,
out-of-memory kills, and faults outside the Python handler may produce no dump.
Python tracebacks can show the Python call that entered Coin/Qt/native code;
they do not replace a debugger's native thread backtraces and do not establish
which native function blocked. The watchdog can run while a C extension holds
the Python GIL, using faulthandler's native watchdog thread.

The implementation follows the documented nonfatal timeout behavior and Python
traceback limits in the [Python 3.13 faulthandler documentation](https://docs.python.org/3.13/library/faulthandler.html)
and [CPython 3.13 traceback implementation](https://github.com/python/cpython/blob/v3.13.0/Python/traceback.c).
It deliberately excludes newer Python native C-stack dumping from its bound.
