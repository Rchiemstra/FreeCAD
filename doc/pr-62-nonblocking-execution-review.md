# PR #62 review: nonblocking document execution

- **PR:** [Rchiemstra/FreeCAD#62](https://github.com/Rchiemstra/FreeCAD/pull/62),
  `feature/nonblocking-document-execution` → `FreeCAD-start`
- **Reviewed head:** `4d8d579503` (2026-10-03), 171 commits, 313 files, +62,702 / −5,689
- **Reviewer:** Claude (Opus 5.5), 2026-10-03
- **Scope:** code review of the lane, ingress, GIL-gate and close/save paths; the four
  commits added after the PR's last documented review (`96aeb0d85a`, `6f7e67bd3b`,
  `e2d259d2c7`, `4d8d579503`); a Windows build; runtime tests in FreeCADCmd and the
  FreeCAD GUI; and live tests through the FreeCAD MCP server.

## Verdict

**Not ready to merge as-is.**

- The branch does **not build on Windows** with the repository's own pixi environment
  (Python 3.11): one compile error and two link errors. Linux CI can't catch any of them.
- **The pinned `freecad-mcp` addon cannot change a document in GUI FreeCAD.** Every
  mutating MCP tool is refused with `DocumentWouldBlock`, by a guard this PR adds. Verified
  live.
- Closing a never-saved document and choosing **Save** fails with "command failed" instead
  of opening Save As. Verified in the GUI.
- Headless `recomputeAsync()` reports a terminal `cancelled` state before the recompute
  has run. Verified.
- The newest large commit (`96aeb0d85a`) reverses the PR's central contract, "the GUI never
  waits for document execution". Neither the architecture document nor the PR description
  was updated.
- The architecture test tier is **red at HEAD**: 15 of 318 tests fail because the
  inventories are stale. No CI pipeline runs that tier.
- Several lifetime and threading issues in the close/save paths need fixing.

The architecture itself is sound, and the core promise holds in practice. During a 2 s
asynchronous recompute, GUI event-loop latency stayed at **p99 8.2 ms (max 11.2 ms)**. On
Windows:

- all 39 lane and presentation GUI gtests pass;
- all 61 lane-tier App gtests pass;
- the full App suite passes (1,091/1,091, 9 skipped);
- the `freecad-mcp` unit suite passes, apart from one timing flake.

The command types carry no pointers,
the handle state is held by `weak_ptr`, and the watchdog never terminates a thread. CI
(Woodpecker) is green on `4d8d579503`, but it covers neither Windows, the architecture
tier, nor GUI-hosted MCP.

## Findings summary

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| B1 | Blocking | Build (Windows) | `PyEval_TryAcquireLock` does not exist; `GuiPythonGate.cpp` fails to compile on Python < 3.12 |
| B2 | Blocking | Build (Windows) | Missing exports: `recommendedActionWhileLaneBusy` (`FreeCADGui.dll` fails to link) and `DocumentExecutionTelemetryTestAccess::reset` (`App_tests_run.exe` fails to link) |
| B3 | Blocking | Close/save UX | **Confirmed (R4).** "Save" on a never-saved document during close fails with "command failed" instead of opening Save As |
| B4 | Blocking | MCP integration | **Confirmed (R6).** The pinned addon calls the sync `commitCompatibilityMutation()` on the GUI thread, which this PR now refuses, so every MCP mutation fails |
| H1 | High | Contract | GUI-thread synchronous APIs now wait on the owner thread with no bound (measured 2.72 s GUI block, R3); docs and PR text still say the GUI never waits |
| H2 | High | Threading | The `Close` command runs `Application::closeDocument()` on the lane thread and mutates `DocMap` without a lock while the GUI iterates it |
| H3 | High | Data loss | "Exit the whole process" on a stalled lane skips save prompts and silently discards unsaved changes in **all** documents |
| H4 | High | Re-entrancy | Production close-save runs `QApplication::processEvents()` for up to 120 s while holding an `App::Document&` |
| H5 | High | API correctness | **Confirmed (R2).** Headless `recomputeAsync()` returns a handle that is immediately terminal `cancelled` ("recompute result is unavailable") while work is still pending |
| H6 | High | Tests/CI | **Confirmed (R1).** The architecture tier fails 15/318 at HEAD (stale inventories), and no CI pipeline runs it |
| M1 | Medium | Transactions | `4d8d579503` makes all transaction control a silent no-op during notification replay |
| M2 | Medium | Undo | **Confirmed (R5).** Property-editor edits apply through the lane but are not undoable; the lane `Edit` opens no transaction and is not atomic |
| M3 | Medium | Threading | `_active->recomputeId` is written without the mutex while other threads read it under the mutex |
| M4 | Medium | Resources | `_terminalSnapshots` grows without bound; `recomputeStatus()` scans it linearly on every poll |
| M5 | Medium | Lifetime | `dispatchToOwner()` after lane shutdown enqueues work that never runs |
| M6 | Medium | GIL | The non-blocking GIL probe spawns one thread per attempt and has a check-then-act gap that can still block the GUI |
| M7 | Medium | Tests | GUI test helpers for save/undo/redo/close never check that the operation completed |
| M8 | Medium | Process | A 140-file mega-commit landed after the PR's documented review and test evidence |
| M9 | Medium | API correctness | **Confirmed (R2).** After a successful close, `closeAsync()` reports `state: Failed`, `kind: Recompute`, "document execution lane is not active" |
| M10 | Medium | Diagnostics | **Confirmed (R2).** `saveAsync()` on an unnamed document fails with the generic "command failed" instead of saying there is no file name |
| L1–L8 | Low | Various | IFC partial migration, Mesh pick cost, CR CR LF headers, and other nits (see below) |
| X1 | Pre-existing (not this PR) | MCP on Windows | The `freecad-mcp` server deadlocks on its first tool call: `git` is spawned with inherited stdin (R6) |

## Blocking

### B1: `PyEval_TryAcquireLock` is not a CPython API

[GuiPythonGate.cpp:137-149](../src/Gui/GuiPythonGate.cpp) (at `4d8d579503`) calls
`PyEval_TryAcquireLock()` when `PY_VERSION_HEX < 0x030c0000`. No CPython version provides
that function, and the repository does not define it. The pixi environment is Python 3.11.14,
so the build fails:

```
GuiPythonGate.cpp(138): error C3861: 'PyEval_TryAcquireLock': identifier not found
```

Linux CI uses Python 3.12 and compiles the other branch, so CI stays green.

**Fix:** use `tryAcquireGilWithoutBlocking()` (it only needs `PyGILState_Ensure/Release`) on
every Python version, and delete the `_usedDeprecatedGilLock` path. That is the local patch
used for this review.

### B2: missing `AppExport` on `recommendedActionWhileLaneBusy`

[DocumentExecutionLane.h:77](../src/App/DocumentExecutionLane.h) declares a free function
without `AppExport`. It is defined in `FreeCADApp.dll` and called from
`Gui/DocumentExecutionIngress.cpp`:

```
DocumentExecutionIngress.cpp.obj : error LNK2019: unresolved external symbol
  App::DocumentExecutionClosePolicy::recommendedActionWhileLaneBusy(bool)
bin\FreeCADGui.dll : fatal error LNK1120: 1 unresolved externals
```

The test binary fails the same way, because `Internal::DocumentExecutionTelemetryTestAccess`
([DocumentExecutionTelemetry.h:25](../src/App/DocumentExecutionTelemetry.h)) has no export
macro:

```
DocumentExecutionTelemetry.cpp.obj : error LNK2019: unresolved external symbol
  App::Internal::DocumentExecutionTelemetryTestAccess::reset(App::DocumentExecutionTelemetry &)
bin\App_tests_run.exe : fatal error LNK1120: 1 unresolved externals
```

GCC exports every symbol by default, so Linux hides this class of error.

**Fix:** add `AppExport` to both. Also consider a Windows build lane, or
`-fvisibility=hidden` on the Linux lane, so that missing exports fail in CI.

### B3: closing a never-saved document and choosing "Save"

Every document gets a lane unconditionally ([Application.cpp:564](../src/App/Application.cpp)).
In `Gui::Document::canClose()` ([Gui/Document.cpp:4168-4176](../src/Gui/Document.cpp)) and
`MainWindow::closeAllDocuments` ([MainWindow.cpp:1188-1191](../src/Gui/MainWindow.cpp)), a
document with a lane now saves through `submitDocumentSaveAwaitingCompletion()`. That submits
a lane `Save`, which calls `App::Document::save()`, which returns `false` when `FileName` is
empty ([App/Document.cpp:5185](../src/App/Document.cpp)). The legacy `Gui::Document::save()`
path, which opens the Save As dialog for unnamed documents, is never reached.

**Confirmed in the GUI (R4).** Choosing *Save Changes* produced an error dialog: "There was
a problem saving the file … Error details: 'command failed'. Do you want to save the file
under another name?" The headless `saveAsync()` on an unnamed document likewise ends
`Failed` / "command failed".

**Fix:** if `!getDocument()->isSaved()`, keep using `Gui::Document::saveAs()`, which already
routes through the lane for the actual write.

### B4: the pinned MCP addon cannot change a document in GUI FreeCAD

`59669da72a` (in this PR) adds
`DocumentWouldBlock::throwIfGuiThread("Document.commitCompatibilityMutation()", …)` to
[DocumentPyImp.cpp:1828-1830](../src/App/DocumentPyImp.cpp). The `freecad-mcp` pin carried
by this PR (`16086577`) runs its mutations through
`collaboration_api.py:180 → document.commitCompatibilityMutation(...)`, dispatched onto the
GUI thread. In GUI FreeCAD every MCP mutation is therefore refused:

```
FreeCAD RPC error -32000: Document.commitCompatibilityMutation() would block the GUI thread;
use Document.commitCompatibilityMutationAsync() instead
```

The failures are not limited to one tool. Verified live (R6) for `create_object`,
`edit_object`, `recompute_document`, `body_create`, `sketch_create`, `sketch_add_rectangle`,
`pad_feature` and mutating `execute_code`. `undo`/`redo` fail with "The authenticated RPC
request could not be processed". Reads, `save_document_as` and `close_document` still work.

CI stays green because its MCP lanes run headless, where there is no GUI thread and the
guard never fires.

**Fix:** move the addon to `commitCompatibilityMutationAsync()` and await completion off the
GUI thread, or exempt this path, before merging. Add one GUI-hosted MCP smoke test to CI.

## High

### H1: GUI-thread synchronous APIs now wait, and the docs still say they don't

`96aeb0d85a` changes `DocumentExecutionLane::dispatchToOwner()`
([DocumentExecutionLane.h:163-247](../src/App/DocumentExecutionLane.h)). On the GUI thread it
now **waits** for running model work and then for the functor. While waiting it services only
marshalled functors, never the Qt event loop. `Document.recompute()`, `undo/redo`,
`save/saveAs`, `touch()` and `FreeCAD.closeDocument()` all take this path.

That may be the right compatibility trade-off for macros and add-ons, but:

- [nonblocking_document_execution_and_gui_presentation_plan.md](nonblocking_document_execution_and_gui_presentation_plan.md)
  still says *"Synchronous document APIs fail fast with WouldBlock when called on the GUI
  thread"* and *"No GUI interaction may synchronously wait for document execution."*
- The PR description still says "GUI never waits (DocumentWouldBlock)".
- The wait has **no bound**. A stalled or very long recompute freezes painting for the whole
  wait, which is the exact failure the plan exists to prevent. **Measured (R3):** with a 3 s
  async recompute in flight, a GUI-thread `doc.recompute()` blocked the GUI thread for
  **2.72 s** without raising.

**Ask:** record the decision in the plan document, and either bound the wait (time out to
`DocumentWouldBlock`) or limit it to code paths that are known to be short.

### H2: `Close` mutates `DocMap` off the GUI thread

A lane `Close` command calls `GetApplication().closeDocument()` **on the lane thread**
([DocumentExecutionLane.cpp:575-598](../src/App/DocumentExecutionLane.cpp)). That function
runs `DocMap.erase(pos)` and may reset `_pActiveDoc`
([Application.cpp:785](../src/App/Application.cpp)). `DocMap` is a plain `std::map` with no
mutex, and `getDocuments()` ([Application.cpp:954](../src/App/Application.cpp)) copies it
without locking.

GUI-thread code iterates it concurrently. For example,
`GuiPythonGate::anyDocumentExecutionLaneBusy()`
([GuiPythonGate.cpp:36-48](../src/Gui/GuiPythonGate.cpp)) runs on every gated Python admission
and dereferences each returned `Document*` (`->executionLane()`). The concurrent erase is a
data race, and the dereference can become a use-after-free if the lane deletes the document
in between.

`scheduleRecoverySnapshotWrite()` also calls `getDocument(name)` from a detached worker thread
([DocumentExecutionIngress.cpp:464-492](../src/Gui/DocumentExecutionIngress.cpp)).

**Fix:** run the registry mutation (`DocMap`, `DocFileMap`, `_pActiveDoc`) on the GUI thread
through a blocking marshal, or protect the registry with a mutex and take a
`shared_ptr`-style pin for callers that dereference documents.

### H3: "exit the whole process" discards every document's unsaved work

When the watchdog has marked a lane *Stalled* (5 s without progress, so any single feature
that takes longer than 5 s qualifies), `submitDocumentClose()` offers to exit and calls
`QCoreApplication::exit(1)`
([DocumentExecutionIngress.cpp:650-692](../src/Gui/DocumentExecutionIngress.cpp)). After the
event loop returns, `Gui::Application::shutdown()` calls
`App::GetApplication().closeAllDocuments()` ([Gui/Application.cpp:3274](../src/Gui/Application.cpp)),
which closes documents **without save prompts**. Unsaved changes in every *other* document are
silently lost. The dialog says only "exit the whole FreeCAD process".

**Fix:** go through `MainWindow::close()` and the normal save prompts (skipping only the
stalled document), and state the data-loss scope in the dialog. Also consider a longer
watchdog threshold, or one based on per-feature progress, before offering a process exit.

### H4: production close-save pumps the event loop while holding a document reference

The PR description says the pump helpers that remain are test-only. However,
`submitDocumentSaveAwaitingCompletion()`
([DocumentExecutionIngress.cpp:539-587](../src/Gui/DocumentExecutionIngress.cpp)) is called
from production close paths (`Gui::Document::canClose`, `MainWindow::closeAllDocuments`). It
loops `QApplication::processEvents()` for up to 120 s while holding an `App::Document&`, and
calls `document.getName()` after each pump.

Anything that runs during the pump can close that document: an MCP RPC call dispatched
through the event loop, a second close request, a Python timer. The next `getName()` is then
a use-after-free.

**Fix:** finish close-after-save asynchronously (save completion → re-issue close), or at
least hold the document by `DocumentHandle` and re-resolve it after each pump.

### H5: `recomputeAsync()` reports a terminal `cancelled` state before the work runs

Headless (FreeCADCmd), after any property edit, `doc.recomputeAsync()` returns a handle whose
**first** `status()` is already terminal:

```json
{"state": "cancelled", "diagnostic": "recompute result is unavailable",
 "total": 0, "terminal": true}
```

The recompute has not actually finished. `App.closeDocument()` issued right after
`handle.done()` returns `True` fails ("Closing the document … failed") and succeeds about
10 ms later. This reproduced in 5 of 5 attempts (R2). A second `recomputeAsync()` completes
normally (1 feature committed).

**Root cause (traced):**

1. With a lane, `DocumentPy::recomputeAsync()` → `submitRecomputeCommand()` →
   `DocumentHandle::trySubmit()`. Admission reserves a coordinator id and stores it in
   `_active->snapshot.recompute->id`, with the diagnostic "admitted; awaiting owner-thread
   coordinator submit". Python immediately receives `RecomputeHandle(admissionId)`.
2. `RecomputeHandle::status()` ([RecomputeHandle.cpp:101-123](../src/App/RecomputeHandle.cpp))
   asks `lane->recomputeStatus(id)`. That function matches only `_active->recomputeId`
   ([DocumentExecutionLane.cpp:443](../src/App/DocumentExecutionLane.cpp)), which the owner
   thread sets only once it has actually submitted (lines 643/737). So the pending command
   does not match.
3. The handle falls through to `recomputeCoordinator().status(id)`, which does not know the
   reserved id yet. Lines 114-120 then synthesize a **terminal** `Cancelled` / "recompute
   result is unavailable".

Any caller that trusts `done()` then races the lane. In GUI mode the R3 test polled later and
saw `completed`, so the race is easiest to hit headless or in scripts that poll immediately.

**Fix:** in `recomputeStatus()`, also match `_active->snapshot.recompute->id` and report the
pending state (`Running`). Never synthesize a terminal state for an id the lane has admitted.
This also overlaps with M3, since `recomputeId` is written without the lock.

### H6: the architecture tier is red at HEAD, and no CI pipeline runs it

Run against a clean export of `4d8d579503` (so the local build patches cannot interfere):
**303 passed, 15 failed** out of 318. Three further failures were artifacts of
`src/Tools/.gitattributes: embedded export-ignore`; they pass once the files are restored.

The 15 failures are stale inventories, not Windows noise. For example,
`gui_blocking_live_model/inventory.json` places `getActiveDocument()` at
`src/Gui/Application.cpp:835`, but at HEAD that line is `"SoQtOffscreenRenderer");` and the
call is at line 918. Failing areas:

- updateData provider inventory anchors;
- GUI blocking/live-model inventory evidence (236 lines drifted);
- uninventoried `MainWindow.cpp:2140 waitForFinished()`;
- live-App dereferences in `Gui/Application.cpp`, `Gui/Document.cpp` and `MainWindow.cpp`;
- CC-WP05 BIM anchors;
- the `_document.recompute(` count in `DocumentCommitCoordinator.cpp`.

The inventories were last resynced in `3e9d057b5c`, before the `FreeCAD-start` merge and
`96aeb0d85a`. Only `ci/woodpecker/freecad-nonblocking-tests.sh` (manual) runs
`tests/architecture`; `.woodpecker/ci.yml` does not. A couple of the ordering and
byte-stability checks may also be sensitive to Windows' case-insensitive path sorting.

**Fix:** resync the inventories, and add the arch tier (about 15 minutes here, faster on
Linux) to the PR pipeline.

## Medium

### M1: transaction control silently no-ops during replay (`4d8d579503`)

`4d8d579503` adds `if (collaborationNotificationsReplaying()) return …;` to about 16
transaction functions, including the raw `_openTransaction`, `_commitTransaction`,
`_abortTransaction` and `_clearRedos`. The check runs **before**
`ensureCollaborationTransactionControlAllowed()`, so that function's own replay check
([App/Document.cpp:1550](../src/App/Document.cpp)) becomes dead code for these callers.

The crash it fixes was real: an exception crossing a Qt slot. The cost is that any slot which
opens a command during replay now edits the model outside any transaction. The change is not
undoable and nothing is logged. The flag (`DocumentP::collaborationReplayingNotifications`) is
also a plain `bool` that the GUI reads while the lane writes it.

**Fix:** catch and log at the Qt-slot or `Gui::Command` boundary, keep the App core
fail-loud, and make the flag atomic. At minimum, emit `FC_WARN` when a call is suppressed.

### M2: property-editor edits and undo

`PropertyItem::setPropertyValue` now serializes the value and submits an async lane `Edit`.
`executeInstantCommand(Edit)` ([DocumentExecutionLane.cpp:865-905](../src/App/DocumentExecutionLane.cpp))
then:

- opens **no transaction**, so whether the change is undoable depends on which global
  transaction happens to be booked when the lane runs. The GUI-side auto-transaction may
  already be closed by then;
- applies values one at a time with **no rollback**: if the second property fails to
  resolve, the first one stays changed while the command reports failure;
- scans every object per property to find its stable identity, swallowing exceptions
  (O(objects × properties)).

**Confirmed in the GUI (R5).** The test drove the real `PropertyModel`, setting
`Part::RegularPolygon.Polygon` from 6 to 8 inside
`App.setActiveTransaction()`/`closeActiveTransaction()`, which approximates the property
editor's own transaction. The value applied, but `doc.UndoNames` stayed `[]` and
`doc.undo()` did not restore 6.

### M3: data race on `ActiveCommand`

The owner thread writes `_active->recomputeId`
([DocumentExecutionLane.cpp:643, 737](../src/App/DocumentExecutionLane.cpp)),
`lastProgressEpochMilliseconds` and `crossDocumentReservations` without `_mutex`.
`recomputeStatus()` reads `_active->recomputeId` under `_mutex` (line 443) from the GUI
thread, through `RecomputeHandle::status()`. A lock taken on only one side does not
synchronize anything; ThreadSanitizer will report it. Write these fields under the lock, or
make `recomputeId` atomic.

### M4: `_terminalSnapshots` is never pruned

Each command adds a snapshot, including per-feature diagnostics, at line 986, and nothing
ever erases one. `recomputeStatus()` scans the whole map linearly on every poll (line 446).
For long MCP or agent sessions with thousands of recomputes, memory grows and polling slows
down. Bound it with an LRU or a "drop once observed terminal" policy.

### M5: `dispatchToOwner()` after shutdown

`threadMain()` exits once `_shutdownRequested` is set and no command is active. A task pushed
after the final `drainDispatchQueue()` is never executed. Off the GUI thread, `future.get()`
blocks until the lane is destroyed (`broken_promise`). On the GUI thread the wait loop spins
for as long as anything keeps the lane alive. Reject in `dispatchToOwner()` when
`_shutdownRequested` is set, and fail any queued promises when the thread exits.

### M6: non-blocking GIL probe

On Python ≥ 3.12, every GUI attempt to acquire the GIL starts a new `std::thread`
([GuiPythonGate.cpp:74-101](../src/Gui/GuiPythonGate.cpp)). Probes that time out are kept as
"orphans" until they get the GIL. While a non-lane Python thread holds the GIL, repeated
attempts pile up blocked threads.

After a probe succeeds, the GUI thread calls `PyGILState_Ensure()`. Another thread can take
the GIL in between, and then the GUI blocks, contradicting "native GUI never waits for the
GIL". Consider a single long-lived probe thread, or a GIL-release handshake owned by the lane.

### M7: test helpers hide failed saves, undos and closes

In [GuiRecompute.py:266-310](../src/Mod/Test/GuiRecompute.py), `save_document`,
`save_document_as`, `undo_document` and `redo_document` wait for `handle.done()` and return.
A `Failed` or `Cancelled` terminal state passes silently, unlike the headless branch, which
raises. More than 60 GUI test files were moved onto these helpers in `96aeb0d85a`. Assert
`status()["state"] == "Completed"`.

### M8: review and evidence trail

The PR body's adversarial review and its Docker-tier evidence are for `edd188b269`. Since
then the branch gained a merge from `FreeCAD-start`, `96aeb0d85a` (140 files, +3,987/−1,087,
described as "also contains the in-progress nonblocking execution work that was uncommitted
in the working tree") and three more fixes. CI is green on the head, but the large commit
mixes a contract change, TechDraw, Sketcher, BIM, Draft, NaviCube, CI image changes and test
migrations. That makes it hard to review and to bisect. Consider splitting it, or at least
updating the PR description to describe the head as it is now.

Also: `writeRecoverySnapshotAwaitingOwnerThread()` (exported, test-only) has a 120 s
deadline followed by an unbounded `worker.join()`, so its timeout branch can never run.

### M9: `closeAsync()` reports failure after a successful close

Once the document is gone, the lane is unregistered, and `DocumentCommandHandle::status()`
falls back to a synthetic snapshot: `{"kind": "Recompute", "state": "Failed",
"diagnostic": "document execution lane is not active"}`. The document *was* closed. Callers
cannot tell success from failure, and the reported `kind` is wrong. Keep terminal snapshots
reachable after the lane is gone, for example in a process-level map keyed by instance id.

### M10: unhelpful diagnostic for an unnamed save

`saveAsync()` on a document without `FileName` ends `Failed` with "command failed". That is
the same text the B3 dialog shows to users. Report "document has no file name; use Save As".

### X1 (pre-existing, not introduced by this PR): MCP first call deadlocks on Windows

This is not a regression from this PR; it was already present at the old pin (`f7c93135`,
2026-09-17). It still blocks MCP use on Windows today.

`freecad_mcp/server_ops/runtime_identity.py::read_checkout_git()` runs
`git rev-parse` and `git status` through `subprocess.run(..., capture_output=True)`, with
**no `stdin=` and no `timeout`**. Under an MCP stdio server on Windows, a thread is blocked
reading stdin. The `git` child inherits that pipe, and its start-up blocks until the parent's
pending read completes, i.e. until the *next* MCP message arrives.

Effect: the first tool call (`get_runtime_info`) hangs. In this session the Claude Code MCP
client gave up after its 1,800 s limit. `git rev-parse` processes from 12:38 were still
blocked an hour later.

Minimal repro (`sleep 40 | python repro.py`): `stdin=DEVNULL: 0.03s`, `stdin=inherited: HUNG`.
Feeding MCP pings unblocks it, after 5 pings / 4 s.

**Fix:** pass `stdin=subprocess.DEVNULL` and a `timeout` at this call site and at the 9 other
`subprocess` calls in `src/freecad_mcp` that do not set `stdin=`.

## Low / nits

- **L1 (IFC):** `6f7e67bd3b` migrates three call sites off `wrapped_data`, but 9 uses remain
  in `nativeifc/` and `importers/`. One commit earlier, `96aeb0d85a` pinned the CI image to
  `ifcopenshell<0.9` with a build-time check that `wrapped_data` exists. These two approaches
  contradict each other; pick one. `getattr(ifcentity, "declaration", None)` also relies on
  entity attribute-lookup semantics that differ between ifcopenshell versions.
- **L2 (Mesh pick):** `SoFCMeshObjectShape::rayPick` now calls `generatePrimitives` a second
  time on every miss inside the bounding box. Preselection runs on every mouse move, so that
  is double the cost on large meshes. Invalidating the stale bbox cache would fix the root
  cause.
- **L3 (Arch section cache):** the geometry token uses `shape.hashCode()`, which is based on
  addresses and can repeat after a free and reallocation. This is rare but produces a stale
  SVG.
- **L4:** `src/Gui/PresentationDelta.h` and `src/App/DocumentCommandHandle.h` are committed
  with `\r\r\n` line endings. MSVC warns C4335 ("Mac file format") and git classifies both
  files as `-text`.
- **L5:** a forced recompute is detected with `coalescingKey.find("force;")`. Make it a real
  field.
- **L6:** `cancelCommand()` ignores its `reason` argument.
- **L7:** `draftfunctions/svg.py` now duplicates the angular-dimension geometry from the
  Draft view provider, so the two can diverge.
- **L8:** `scheduleRecoverySnapshotWrite()` clears the recovery flag on whichever lane has
  the same document *name* when it finishes. A reused name clears the wrong lane. The worker
  thread is detached and never joined at shutdown, which the PR already notes.

## Runtime testing

### Environment

- Windows 11, MSVC 19.44, pixi env (Python 3.11.14, Qt 6.8.3), `build/release` (Ninja,
  Release, `ENABLE_DEVELOPER_TESTS=ON`).
- The build needed the three local patches listed below. With them, every target built,
  including all gtest binaries.
- Headless and GUI scripted tests used a throwaway `FREECAD_USER_HOME`.
- MCP tests used `python start_freecad.py --authenticated-isolated --freecad
  build/release/bin/FreeCAD.exe`, which runs the isolated profile on 127.0.0.1:9876 with the
  addon junctioned to the pinned submodule. The production MCP instance was not touched.

### R1: architecture tier (`tests/architecture`, clean export of HEAD)

| Subset | Result |
|--------|--------|
| All except `RepositoryInventoryTests` | 250 passed, 8 failed (plus 3 export artifacts that pass once fixed) |
| `RepositoryInventoryTests` (repository scanner, about 18 min on Windows) | 53 passed, 7 failed |
| **Total** | **303 / 318 passed, 15 failed** (see H6) |

### R2: headless (`FreeCADCmd`)

| Test | Result |
|------|--------|
| `Part::Box` + sync `recompute()` | PASS (volume 1000) |
| Edit in transaction, `undo()`, `redo()` | PASS |
| Save As → close → reopen | PASS (volume preserved) |
| `saveAsync()` on a named document | PASS (`Completed`) |
| PartDesign Body → Sketch → Pad | PASS (volume 200, valid, Tip = Pad) |
| Sync recompute → `closeDocument()` × 5 | PASS |
| `recomputeAsync()` after an edit | **FAIL**: immediately terminal `cancelled` / "recompute result is unavailable" (H5) |
| `closeDocument()` right after `recomputeAsync()` reports done | **FAIL** on the first try (4 of 5); succeeds about 10 ms later (H5) |
| `closeAsync()` status after a successful close | **FAIL**: `Failed`, `kind: Recompute` (M9) |
| `saveAsync()` on an unnamed document | `Failed` / "command failed" (B3, M10) |

### R3: GUI (`FreeCAD.exe`, scripted)

| Test | Result |
|------|--------|
| GUI-thread sync `recompute()` on a Box | PASS |
| Event-loop latency during a 2 s async recompute (5 ms timer, 398 samples) | **PASS: p99 8.2 ms, max 11.2 ms, median 5.0 ms**; recompute `completed` |
| GUI-thread sync `recompute()` while the lane is busy (3 s feature) | GUI thread **blocked 2.72 s**, no exception (H1) |

### R4: close → Save on a never-saved document (GUI)

The dialogs were driven automatically:

1. "Save all changes to document 'NeverSaved' before closing?" → **Save Changes**
2. Error: "… Error details: **'command failed'** … save the file under another name?" →
   **Yes**
3. About 228 s passed before the next Qt dialog. This is consistent with a native Save As
   dialog, which the harness cannot see and which was dismissed without saving.
4. "Failed to save document 'NeverSaved'. Would you like to cancel the closure?" → Cancel

The document stayed open with no `FileName`. B3 is confirmed.

### R5: property-editor edit and undo (GUI)

`Polygon` was set from 6 to 8 through the real `PropertyModel`. The value applied, but
`UndoNames` was `[]` before and after, and `undo()` did not restore 6. M2 is confirmed.

### R6: live MCP (`freecad-mcp` at the pinned `16086577`, started with `uv run` as `start_freecad.py` prints)

| Tool | Result |
|------|--------|
| First call (`get_runtime_info`) | Hangs (X1). Completes once stdin is fed with MCP pings (5 pings, 4 s) |
| `check_rpc_sync`, `list_documents`, `create_document`, `get_mutation_readiness`, `get_document_tree`, `get_report_view`, `close_document` | PASS |
| `execute_code` (read-only, worker) | PASS |
| `save_document_as` (empty document) | PASS (verified write; thumbnail present) |
| `create_object`, `edit_object`, `recompute_document`, `body_create`, `sketch_create`, `sketch_add_rectangle`, `pad_feature`, mutating `execute_code` | **FAIL**: `commitCompatibilityMutation() would block the GUI thread` (B4) |
| `undo`, `redo` | **FAIL**: "authenticated RPC request could not be processed" |
| `acquire_document_lock`, `release_document_lock` | `LEGACY_LEASE_AUTHORITY_REMOVED` (by design) |

Notes on the MCP environment:

- The Claude Code session's own `freecad` MCP server hit X1 and never answered (1,800 s
  client timeout).
- The submodule's `.venv-windows` is stale: `mcp` 1.13.0, while `pyproject.toml` requires
  ≥ 1.26. Run from that venv, every tool fails with `ctx Field required`. That is a local
  environment issue; `uv run` syncs the locked `mcp` 1.26.0.

### R7: Windows gtests

| Binary / filter | Result |
|-----------------|--------|
| `Gui_tests_run.exe`: CollaborationResponsiveness, SharedPresentationRevisionIndex, SharedPresentationCoordinator, DocumentPresentationCache, PresentationApplyScheduler, GuiPythonGate | **39 / 39 passed** |
| `App_tests_run.exe`: DocumentExecutionLane, DocumentHandleContract, DocumentCommandContract, DocumentCommandHandleContract, DocumentExecutionTelemetry(+Contract), DocumentExecutionStall, DocumentCrossDocumentSnapshot, RecomputeHandle | **61 / 61 passed** |
| `App_tests_run.exe`, full suite (run from a scratch working directory, as CI does) | **1,091 passed, 0 failed, 9 skipped** (symlink tests that are skipped on Windows) |

`GuiPythonGateTest` necessarily exercised the local B1 patch. `App_tests_run.exe` needed the
third local export patch (B2).

These unit and contract suites pass. The defects found at runtime (H5, M9, B3, B4) sit in
paths these suites do not exercise: Python handle polling, closing through a handle, unnamed
saves, and the GUI-hosted MCP addon.

### R8: `freecad-mcp` unit suite (pinned `16086577`, Windows)

The run mirrors CI (`pytest -m unit -ra --tb=short`) in the uv-synced `dev` environment
(`mcp` 1.26.0), parallelized with `-n 8`.

**12,072 passed, 1 failed, 3 skipped, 1 xfailed** in 13 min.

The single failure, `test_running_timeout_quarantines_until_late_completion`, missed a 1 s
`started.wait(1.0)` deadline under parallel load. It passes 5/5 when run serially, so it is a
timing flake and not a regression. An earlier attempt from the stale `.venv-windows`
(`mcp` 1.13) showed many failures; those were environment-caused and are discarded.

The unit suite stubs FreeCAD, so it cannot catch B4 or X1. Both only appear against a live,
GUI-hosted FreeCAD started on Windows.

### R9: shutdown

The isolated FreeCAD started by `start_freecad.py` (no documents open) exited within 8 s of
a normal close request, with no hang in lane shutdown.

## Local patches used for testing (not committed)

Three minimal edits were needed to build on Windows. They are left **uncommitted** in the
working tree:

1. `src/Gui/GuiPythonGate.cpp`: use the probe path on every Python version (B1).
2. `src/App/DocumentExecutionLane.h`: add `AppExport` to `recommendedActionWhileLaneBusy` (B2).
3. `src/App/DocumentExecutionTelemetry.h`: add `AppExport` to
   `Internal::DocumentExecutionTelemetryTestAccess` (B2, test binary only).

Test scripts and logs are in the session scratchpad, not in the repository.

## Recommended next steps

1. Fix B1/B2 and add a Windows build, or a Linux build with hidden visibility, to CI.
2. Make the pinned `freecad-mcp` addon use `commitCompatibilityMutationAsync()` (B4), and
   add a GUI-hosted MCP smoke test to CI.
3. Fix the never-saved close→Save path (B3) and the close-save pump (H4).
4. Fix `RecomputeHandle` so an admitted-but-unsubmitted recompute is never terminal (H5),
   and keep `closeAsync()` results observable after close (M9).
5. Resync the architecture inventories and run `tests/architecture` in the PR pipeline (H6).
6. Decide and document the GUI-thread synchronous-wait contract (H1), including a bound.
7. Move the `DocMap` mutation for lane-thread close onto the GUI thread (H2).
8. Rework the stalled-lane exit so it never bypasses save prompts (H3).
9. Address M1–M10, and refresh the PR description and evidence for the current head.
10. Separately, in `freecad-mcp`: pass `stdin=subprocess.DEVNULL` and a timeout to every
    `subprocess` call (X1).
