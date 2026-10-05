# PR #62 re-review: nonblocking document execution

- **PR:** [Rchiemstra/FreeCAD#62](https://github.com/Rchiemstra/FreeCAD/pull/62),
  `feature/nonblocking-document-execution` → `FreeCAD-start`
- **Reviewed state:** the **uncommitted working tree** on top of `4d8d579503` (224 modified
  files and one new, untracked test), plus the `freecad-mcp` submodule moved from `16086577`
  to `452b75c2` (itself dirty).
- **Previous review:** [pr-62-nonblocking-execution-review.md](pr-62-nonblocking-execution-review.md)
  (2026-10-03, at `4d8d579503`). Finding IDs below refer to that document.
- **Reviewer:** Claude (Opus 5.5), 2026-10-04
- **Scope:** every original finding, rechecked in code and, where possible, at runtime; the
  new code the fix pass added; a Windows build; headless, GUI, gtest, BIM and architecture
  runs.

## Verdict

**Not ready to merge. The fix pass resolves several findings but introduces four new
blocking regressions.**

- **Closing documents is broken in GUI FreeCAD (N1).** The H2 fix runs `closeDocument()` on
  the GUI thread, where `permitsApplicationClose()` refuses it. `App.closeDocument()`,
  `closeAsync()`, the tree's *Close document* and closing a tab all fail with "close failed".
  Every failed attempt also shuts the document's lane down, leaving a document that can no
  longer be recomputed, saved or undone. Verified.
- **Closing one document and choosing *Save* quits FreeCAD (N2).** The H4 replacement calls
  `MainWindow::close()` when the save completes. Verified: with a second document open, the
  user is next asked to save *that* document "before closing".
- **`commitCompatibilityMutationAsync()` aborts the process (N3).** Every call ends in
  `Fatal Python error: PyThreadState_Get` on Windows: a `Py::Dict` passes through
  `std::promise` without the GIL. Verified headless (3/3) and in the GUI. B4 is therefore not
  fixed. In addition, the addon's tool handlers never await the new waiter, so `create_object`
  reports "uncertain" while the mutation still runs (N6).
- **The tree does not build (N4).** Four files call a `getDocument()` that does not exist.
  `MeshGui` and `FemGui` fail to compile.
- **The mass `recompute()` → `recomputeAsync()` rewrite breaks callers (N5).** It touches 364
  Python call sites plus C++ modules. BIM tests now fail or hang (TestArchWall 6 of 17,
  TestArchRoof 1 of 7, TestArchComponent 18 of 18, TestArchSpace 3 of 5; the last two then
  never exit). Sync calls that follow an async recompute hit "lane busy".

Fixed and verified at runtime: **B1, B2, H1, H5, H6, M9, M10**. Also fixed: **M7, L1, L5, L6,
L8** and the main **X1** call site. The lane and presentation gtests pass (App 70/70, GUI
39/39), and so does the architecture tier (329/329). None of these suites exercise the paths
above.

## Status of the original findings

| ID | Status | Evidence |
|----|--------|----------|
| B1 `PyEval_TryAcquireLock` | **Fixed** | Probe used on every Python version; `FreeCADGui.dll` builds on Windows |
| B2 missing exports | **Fixed** | `AppExport` on both symbols; Gui and `App_tests_run.exe` link |
| B3 close → Save on an unnamed document | **Partly fixed** | Save As now opens and writes the file, but the document is **not closed** (R4) |
| B4 MCP mutations in GUI FreeCAD | **Not fixed; worse** | Async path aborts the process (N3); handlers mis-handle the waiter (N6); pin not pushed (N8) |
| H1 unbounded GUI-thread waits | **Fixed** | Admission bounded (default 500 ms); `doc.recompute()` while busy raised after **0.503 s** (was a 2.72 s block). Plan updated; one contradicting sentence remains (see below) |
| H2 `Close` mutates `DocMap` off the GUI thread | **Fixed in the GUI, but breaks close (N1)**; still on the lane thread headless | R4, R2 |
| H3 stalled-lane exit discards all work | **Changed, not as described** | No more `exit(1)`, but "skip this document" is not implemented (see below) |
| H4 close-save pumps the event loop | **Pump removed; the replacement quits the app (N2)** | R4 |
| H5 `recomputeAsync()` terminal before work | **Fixed** | First status `running` 5/5; close right after `done()` 5/5 |
| H6 arch tier red, not in CI | **Fixed** | 329/329 locally (R6); `freecad-arch-tests` step in `.woodpecker/ci.yml`; one test file untracked (N8) |
| M1 silent transaction no-ops during replay | **Partly fixed** | No-ops reverted, flag atomic; catch only in `Command::_invoke` (see below) |
| M2 property-editor edits not undoable | **Not fixed** | `UndoNames` stays `[]`; baseline edit is undoable (R4) |
| M3 data race on `ActiveCommand` | **Not fixed** | Owner-thread writes still unlocked |
| M4 `_terminalSnapshots` unbounded | **Not fixed** | Per-lane map still grows; new archive also never drops closed documents |
| M5 `dispatchToOwner()` after shutdown | **Partly fixed** | Rejects after shutdown; the new final drain can run tasks after the document is gone (see N3) |
| M6 GIL probe | **Partly fixed** | One long-lived probe thread; the check-then-act gap remains (acknowledged in the code) |
| M7 test helpers ignore failure | **Fixed** | `_check_completed()` asserts `Completed` |
| M8 review/evidence trail | **Still applies** | The fix pass is a 224-file uncommitted change; see N8 |
| M9 `closeAsync()` reports failure | **Fixed** | Headless status `Completed` / `Close` / "close completed" |
| M10 unnamed save diagnostic | **Fixed** | `ValueError: document has no file name; use Save As` |
| L1 IFC `wrapped_data` | **Addressed** | Version-tolerant helpers with explicit fallbacks |
| L2 Mesh pick cost | **Reworked** | No second `generatePrimitives`; the in-place-edit case has no test |
| L3 section cache token | **Changed** | New token can collide (see below) |
| L4 `\r\r\n` headers | **Half fixed** | `PresentationDelta.h` correct; `DocumentCommandHandle.h` now double-spaced (184 → 368 lines) |
| L5 force via coalescing key | **Fixed** | `force` field |
| L6 `cancelCommand()` ignores reason | **Fixed** | Reason stored and forwarded; see N7 for the locking it now does |
| L7 duplicated Draft SVG geometry | **Not addressed** | |
| L8 recovery write clears the wrong lane | **Fixed** | Worker captures the lane `shared_ptr` |
| X1 MCP first-call deadlock | **Fixed in the submodule working tree only** | `stdin=DEVNULL` + timeout at the hanging site; 5 calls still lack `stdin=` |

### Notes on partial fixes

- **H1:** the plan now records the bounded-admission decision. Its opening principle,
  [plan line 7](nonblocking_document_execution_and_gui_presentation_plan.md), still says "No
  GUI interaction may synchronously wait for document execution". Once admitted, a sync call
  still waits for completion with no bound, as the plan now states.
- **H3:** the dialog now promises "Skip this document and close everything else normally",
  and the code comment says "Mark this document skipped". Nothing marks it.
  `MainWindow::close()` prompts for the stalled document like any other, and its save is
  rejected by the stalled lane. This dialog is reachable only from the automatic orphan-document
  cleanup ([Application.cpp:1318](../src/Gui/Application.cpp)), so a background action can
  still pop a modal and start an application close.
- **M1:** `4d8d579503` was fixing "GUI slots that open or commit a transaction while
  collaboration notifications replay". The new catch is only in
  [Command.cpp:547-565](../src/Gui/Command.cpp) and matches on message text. A slot that calls
  `Gui::Command::openCommand()` directly, outside `_invoke`, throws through Qt again. I could
  not check this at runtime: the Sketcher GUI suite did not finish within 15 minutes (see R5).
  Replay notification failures were also downgraded from `FC_ERR` to `FC_WARN`.
- **M2:** the lane `Edit` now opens and commits a transaction
  ([DocumentExecutionLane.cpp:1024-1067](../src/App/DocumentExecutionLane.cpp)), but no undo
  entry appears (R4). It also names the transaction after `operationId`, which the Undo menu
  would show. The new test `EditCommandRollsBackOnUnknownObject` fails before any property
  changes, so it does not test rollback.
- **M3:** `recomputeId` is now also set at admission under the lock, but the owner thread still
  writes `recomputeId`, `recomputeSubmitted`, `crossDocumentReservations` and
  `lastProgressEpochMilliseconds` without `_mutex`
  ([lines 765, 768, 841, 859, 860, 1183](../src/App/DocumentExecutionLane.cpp)).
  `recomputeStatus()` and `cancelCommand()` read them under it.
- **M4:** `_terminalSnapshots.emplace` (line 1156) is never pruned, and `recomputeStatus()`
  still scans it linearly (line 541). The new process-level archive caps each document at 64
  entries but never erases the bucket of a closed document.
- **H5 test:** `AdmittedRecomputeStatusIsNotTerminalOnFirstCheck` checks
  `DocumentCommandHandle::status()`, which was never wrong. The bug was in
  `RecomputeHandle::status()` → `lane->recomputeStatus()`. Similarly,
  `AdmissionRejectedImmediatelyWhenStalled` and `AdmissionSucceedsBeforeTimeout` never run
  the GUI-thread admission path they are named after.
- **L2:** the shape now calls `touch()` when the `MeshObject*` changes. An edit that keeps the
  same pointer may leave the stale bounding-box cache in place. This needs the pick-after-edit
  GUI test that `e2d259d2c7` lacked.
- **L3:** the token (face/edge/vertex counts, volume, bounding box) is identical when, for
  example, a window opening slides along a wall, so that section SVG goes stale. It also computes
  `Volume` for every object on every check.

## New findings

| ID | Severity | Area | Finding |
|----|----------|------|---------|
| N1 | Blocking | Close | GUI close always fails; a failed close leaves a document with a dead lane |
| N2 | Blocking | Close/save UX | Closing one modified document with *Save* quits FreeCAD |
| N3 | Blocking | B4 / Python | `commitCompatibilityMutationAsync()` aborts the process; latent use-after-free when posted during `Close` |
| N4 | Blocking | Build | `MeshGui` and `FemGui` fail to compile (`getDocument` not found) |
| N5 | High | Compatibility | Mass `recompute()` → `recomputeAsync()` rewrite breaks BIM tests and headless callers |
| N6 | High | MCP addon | Handlers never await the async waiter: "uncertain" result while the mutation still applies |
| N7 | Medium | Cancel | `RecomputeHandle.cancel()` deadlocks against a running Python feature (verified); `DocumentCommandHandle::cancel()` now takes the same lock |
| N8 | Medium | Process | EOL churn in 25 files, a double-spaced header, an untracked test, an unpushed submodule pin |
| N9 | Low | Close | Never-saved close: `MainWindow` reads `FileName` while the lane is writing it |

### N1: closing a document fails in the GUI

The H2 fix marshals the `Close` command's `Application::closeDocument()` to the GUI thread
([DocumentExecutionLane.cpp:691](../src/App/DocumentExecutionLane.cpp)). `closeDocument()`
asks `permitsApplicationClose()`
([lines 343-357](../src/App/DocumentExecutionLane.cpp)), which returns `false` whenever a
command is active and the caller is not the owner thread. The active command is the `Close`
itself, so the answer is always no.

Then [line 719](../src/App/DocumentExecutionLane.cpp) calls `requestShutdown("document
closed")` unconditionally. The document stays in `DocMap` with a dead lane, and every later
lane operation fails with "document closed". The unconditional shutdown predates this pass;
before it, `closeDocument()` ran on the owner thread, where the check passed.

**Verified in the GUI (R4):**

- `App.closeDocument()` failed in 4 ms with "Closing the document … failed: close failed".
  The document stayed listed, and `recomputeAsync()` then raised "document closed".
- `doc.closeAsync()` ended `Failed` / "close failed". The tree's *Close document* uses this
  path.
- Closing the tab and choosing *Close Without Saving* left the document listed.

Headless close still works, because with no main-thread hooks `invoke()` runs inline on the
lane thread. That is also why the new gtest `CloseHandleStaysCompletedAfterLaneGone` passes,
and why H2's race remains in headless runs.

**Fix:** let `permitsApplicationClose()` admit a close that the active `Close` command
marshalled, for example with a token or a "close in progress" state set by the lane. Only call
`requestShutdown()` after a successful close.

### N2: closing one document with "Save" quits FreeCAD

`Gui::Document::canClose()` now submits an async lane `Save`, sets `_pendingLaneClose`, and
returns `false`
([Gui/Document.cpp:4182-4196](../src/Gui/Document.cpp)). When the save completes,
`scheduleSaveCommandCompletion()` calls `getMainWindow()->close()`
([DocumentExecutionIngress.cpp:455-457](../src/Gui/DocumentExecutionIngress.cpp)). That is
the application-close path.

`canClose()` is the single-document path, reached from `MDIView::canClose()` (closing a tab or
*Std_CloseActiveWindow*) and `TreeWidget::onCloseDoc()`.

**Verified (R4):**

- With one modified, named document: *Save Changes* → the file was written → `aboutToQuit`
  fired 270 ms later.
- With a second document open: *Save Changes* → 330 ms later, "Save all changes to document
  'Other' before closing?" with *Apply to all*.

The failure branch has the same problem. "Failed to save … Close without saving?" →
*Close Without Saving* also calls `MainWindow::close()`
([line 445](../src/Gui/DocumentExecutionIngress.cpp)), which then prompts again for the same
document.

The pending flag is also never cleared if the save never reaches a terminal state, for example
on a stalled lane. `closeAllDocuments()` then returns `false` on every attempt
([MainWindow.cpp:1253-1259](../src/Gui/MainWindow.cpp)), so FreeCAD cannot be closed.

**Fix:** record *which* close is pending (document close or application close), and on save
completion re-issue that close: the document close for `canClose()`, the window close only
for `closeAllDocuments()`. Clear the flag on every terminal state.

### N3: `commitCompatibilityMutationAsync()` aborts the process

The B4 fix posts the mutation with `postToOwner()`, typed `std::future<Py::Dict>`
([DocumentPyImp.cpp:2102-2124](../src/App/DocumentPyImp.cpp),
[DocumentExecutionLane.h:162](../src/App/DocumentExecutionLane.h)). The sequence is:

1. MSVC's `std::promise<T>` default-constructs its stored value. `_Associated_state`
   initializes `_Result()` (`<future>` line 207, MSVC 14.44).
2. `Py::Dict()` calls `set(PyDict_New(), true)`
   ([PyCXX Objects.hxx:3192-3194](../src/3rdParty/PyCXX/CXX/Python3/Objects.hxx)).
   So creating the promise allocates a placeholder dict.
3. On the lane thread, after `Base::PyGILStateLocker` has gone out of scope, `set_value()`
   *assigns* `_Result = value` (`<future>` line 314).
4. PyCXX releases the placeholder, its refcount reaches zero, and `dict_dealloc` →
   `Py_TRASHCAN_BEGIN` → `PyThreadState_Get()` aborts.

PyCXX `Py::Object` has no move constructor, so even on libstdc++ the hand-off changes
refcounts without the GIL. Linux CI will therefore see at most a refcount race, not this
crash.

**Verified:**

- Headless `wait()`: 3 of 3 runs abort with "Fatal Python error: PyThreadState_Get: the
  function must be called with the GIL held".
- Dropping the handle without `wait()` aborts the same way.
- In GUI FreeCAD, the addon's `commit_native_mutation()` path aborts seconds after the call.

Two further problems are latent behind the crash:

- **Use-after-free after `Close`.** The capture comment says "the owner thread holds a lane
  self-pin that keeps the Document alive". The pin keeps the *lane* alive, not the `Document`.
  `postToOwner()` does not check for an active `Close`. A task posted after `closeAsync()` is
  therefore run by the drain after `closeDocument()` deleted the document (top of the loop,
  and the new final drain at [line 653](../src/App/DocumentExecutionLane.cpp)), and it
  dereferences `docPtr`.
- **`wait()` on the GUI thread** blocks without servicing marshalled tasks. Any blocking
  signal marshal from the owner would then deadlock. The addon waits on the RPC thread, but
  nothing stops a GUI macro from doing it.

**Fix:**

- Pass a pure C++ result (`DocumentCommitResult` plus the captured error) through the future,
  and convert it to a dict inside `wait()` on the waiting thread, under the GIL.
- Reject `postToOwner()` while a `Close` is active.
- Make `wait()` raise `DocumentWouldBlock` on the GUI thread.

### N4: the working tree does not build

`MeshGui` and `FemGui` fail to compile. The mechanical rewrite to
`Gui::requestDocumentRecompute(*getDocument())` landed in classes that have no `getDocument()`:

```
src\Mod\Mesh\Gui\DlgRegularSolidImp.cpp(208): error C3861: 'getDocument': identifier not found
src\Mod\Fem\Gui\TaskFemConstraint.cpp(253): error C3861: 'getDocument': identifier not found
src\Mod\Fem\Gui\TaskFemConstraintInitialTemperature.cpp(112): error C3861: ...
src\Mod\Fem\Gui\TaskCreateElementSet.cpp(571, 580, 591, 600, 864): error C3861: ...
```

Line 864 is in `~TaskCreateElementSet()`. Even once it compiles, it must null-check the
document, as `~TaskPostDataAtPoint()` does.

### N5: the `recompute()` → `recomputeAsync()` rewrite breaks callers

The pass replaced `ActiveDocument.recompute()` with `recomputeAsync()` at 364 Python call
sites in BIM, CAM, Draft, FEM, OpenSCAD, PartDesign and others. C++ GUI modules got the same
treatment via `Gui::requestDocumentRecompute()`. The replacement also rewrote tests
(`bimtests/*`), the doctest examples in `Arch.py`, and a commented-out line in
`SprocketFeature.py`.

The calls that followed the old sync recompute still assume it has finished:

- **Reads straight after the call.** 21 sites read `.Shape`, `.Volume` or `BoundBox`, or call
  `ViewFit`, within three lines. Examples: `TestArchComponent.py:44-47`,
  `TestArchRoof.py:168-172`, `TestArchWall.py:82`, `ShaftFeature.py:172`,
  `importCSG.py:1362`.
- **Transaction commits straight after.**
  [gui_facebinders.py:92](../src/Mod/Draft/draftguitools/gui_facebinders.py),
  [gui_layers.py:94](../src/Mod/Draft/draftguitools/gui_layers.py) and
  [base_femtaskpanel.py:43](../src/Mod/Fem/femtaskpanels/base_femtaskpanel.py) call
  `commitTransaction()` right after. `commitTransaction()` runs on the calling thread with no
  lane check, so it touches the active undo transaction while the lane's recompute may be
  writing to it.
- **Sync calls after async work.** Any sync API after an async recompute fails headless with
  "lane busy". The unchanged line `self.document.recompute()` in `TestArchWall.py:216` and
  `:288` now raises it, because the rewritten BIM *library* code left a recompute running.

**Verified headless (R3):**

- TestArchWall: 1 failure, 5 errors of 17.
- TestArchRoof: "shape is invalid".
- TestArchComponent: the first test leaves a prepared commit open. All 18 `setUp`s fail with
  "document lifecycle changes are unavailable during the prepared commit", and the process
  does not exit.

At `4d8d579503` these call sites were synchronous. CI's `freecad-integration-tests` step runs
`FreeCADCmd -t 0`, which includes these modules, and it was green.

**Fix:** revert the blanket replacement. Convert call sites one by one where the code after the
call does not depend on the result, and keep (or await) the sync form where it does.
Never rewrite tests and docstrings this way.

### N6: the addon's handlers never await the waiter

`dispatch_gui()` awaits only when the GUI task *returns* the waiter itself. The tool handlers
call `commit_native_mutation()` and post-process the return value. `create_object`'s
`_create_object_native_result()` turns any non-dict into
`INVALID_NATIVE_CREATE_OBJECT_RESULT` / "Native commit returned no terminal result".

**Verified in-process in GUI FreeCAD (R4):** the result type was `_AsyncMutationWaiter`, the
handler returned `outcome: 'uncertain', committed: None`, and the mutation was still queued
on the lane. FreeCAD then aborted (N3). The new tests in `freecad-mcp` cover
`CollaborationAPI` in isolation with FreeCAD stubbed, so they cannot see this.

### N7: cancel can deadlock against a running Python feature

`RecomputeHandle.cancel()` → `DocumentRecomputeCoordinator::cancel()` locks `_operationMutex`
on the caller's thread, still holding the GIL. `poll()` holds the same mutex on the owner
thread while the feature runs
([DocumentRecomputeCoordinator.cpp:531, 777](../src/App/DocumentRecomputeCoordinator.cpp)).

**Verified headless:** with a Python feature sleeping 3 s, `h.cancel()` never returned.
`faulthandler` after 25 s showed the main thread in `h.cancel()` and the lane thread in
`execute()`, waiting to re-take the GIL.

This call path is unchanged in this pass, so it is probably pre-existing. L6 now routes
`DocumentCommandHandle::cancel()`, documented as "safe even when the owner thread is stalled",
into the same lock.

**Fix:** never take the coordinator's operation lock from a non-owner thread. Set the flag and
let the owner forward the cancel, as before L6, or release the GIL around the lock.

### N8: change hygiene

- 25 files under `src/Mod` marked `-text` were rewritten from LF to CRLF (Mesh, MeshPart,
  Spreadsheet, TechDraw, PartDesign, Sketcher, Part). Committed as they are, they become
  whole-file rewrites: the diff stat is +17,125/−17,966, against +4,010/−4,851 ignoring line
  endings.
- `src/App/DocumentCommandHandle.h` became double-spaced (each `\r\r\n` turned into two
  newlines).
- `tests/architecture/test_sync_api_ratchet.py` is untracked.
- The `freecad-mcp` pin `452b75c2` is not contained in any local or remote-tracking ref of the
  submodule, so CI and other clones cannot check it out. The X1 fixes exist only as
  uncommitted changes in the submodule, so the pin does not include them.

### N9: never-saved close in `MainWindow`

[MainWindow.cpp:1200-1215](../src/Gui/MainWindow.cpp) calls `gdoc->saveAs()`, which for lane
documents submits an async SaveAs and returns `false`. It then reads `doc->isSaved()`
immediately. That read races the lane thread writing `FileName`. If the write has not landed,
the in-flight save is counted as failed and the user sees "Some documents could not be saved".
If it has landed, the close is deferred, but `saveAs()` never set the pending flag, so nothing
retries it.

## Runtime testing

### Environment

- Windows 11, MSVC 19.44, pixi env (Python 3.11.14, Qt 6.8.3), `build/release`.
- An incremental build of the working tree: `FreeCADApp`, `FreeCADGui`, all gtests and every
  module except `MeshGui` and `FemGui` (N4) built.
- Throwaway `FREECAD_USER_HOME` per run. GUI runs were scripted with a dialog driver, using
  the non-native file dialog.

### R1: gtests

| Binary / filter | Result |
|-----------------|--------|
| `App_tests_run.exe`: lane, handle, command, telemetry, stall, cross-document, RecomputeHandle | **70 / 70 passed** (61 before, plus 9 new) |
| `Gui_tests_run.exe`: responsiveness, presentation, `GuiPythonGate` | **39 / 39 passed** |

### R2: headless (`FreeCADCmd`)

| Test | Result |
|------|--------|
| `recomputeAsync()` first status, 5 runs (H5) | **PASS**: `running` every time |
| `closeDocument()` right after `done()`, 5 runs (H5) | **PASS**: 0 failures |
| `closeAsync()` status after close (M9) | **PASS**: `Completed` / `Close` / "close completed" |
| `saveAsync()` on an unnamed document (M10) | **PASS**: `ValueError: document has no file name; use Save As` |
| `commitCompatibilityMutationAsync()` + `wait()` | **CRASH** 3/3: `Fatal Python error: PyThreadState_Get` (N3) |
| Same, handle dropped without `wait()` | **CRASH** (N3) |
| `RecomputeHandle.cancel()` during a 3 s Python feature | **DEADLOCK**: no return; `faulthandler` stacks (N7) |

### R3: BIM tests, headless

| Module | Result |
|--------|--------|
| `bimtests.TestArchWall` | **FAILED**: 1 failure, 5 errors of 17 ("lane busy", "non-owner thread during an atomic presentation callback") |
| `bimtests.TestArchRoof` | **FAILED**: 1 error of 7 ("shape is invalid") |
| `bimtests.TestArchComponent` | **FAILED**: 18 errors of 18 (prepared commit left open), then the process hung |
| `bimtests.TestArchSpace` | **FAILED**: 3 errors of 5 (same prepared-commit refusal), then the process hung |

TestArchComponent and TestArchSpace print their summary, then never exit. Both were stopped
by the 600 s limit. The remaining modified modules (SectionPlane, Structure, Report) were
queued behind these hangs and not waited for.

### R4: GUI (`FreeCAD.exe`, scripted)

| Test | Result |
|------|--------|
| GUI-thread `doc.recompute()` while a 3 s async recompute runs (H1) | **PASS**: `DocumentWouldBlock` after 0.503 s; 1.514 s with the timeout set to 1500 ms |
| Event-loop latency during an async recompute (5 ms timer) | **PASS** after a warm-up: p99 7.9 ms, max 9.9 ms. The first async recompute of a session stalls the Python timer ~0.5 s once, independent of the admission timeout |
| Close tab → *Save Changes*, saved document (H4) | **FAIL**: file saved, then **FreeCAD quit** (N2) |
| Same, with a second document open | **FAIL**: prompt "Save all changes to document 'Other' before closing?" (N2) |
| Close tab → *Save Changes*, never-saved document (B3) | **Partial**: Save As dialog, file written, document **still open** after 8 s |
| `App.closeDocument()` on the GUI thread | **FAIL**: "close failed"; document still listed; lane dead (N1) |
| `doc.closeAsync()` | **FAIL**: `Failed` / "close failed" (N1) |
| Close tab → *Close Without Saving* | **FAIL**: document still listed (N1) |
| Property editor `Polygon` 6 → 8, then `undo()` (M2) | **FAIL**: value 8, `UndoNames == []`, undo leaves 8. Baseline edit in a transaction: undoable |
| Addon `commit_native_mutation()` + `_create_object_native_result()` on the GUI thread | **FAIL**: waiter returned, handler says "uncertain", then the process aborts (N3, N6) |

### R5: Sketcher GUI suite (M1)

`FreeCAD.exe -t TestSketcherGui` produced no output and had not finished after 900 s; it was
stopped. That is consistent with N1 blocking test teardown, so M1 remains unverified at runtime.

### R6: architecture tier (`tests/architecture`, working tree)

**329 passed, 0 failed** (plus 14 subtests), 14 minutes with `-n 8`. At `4d8d579503` it was
303 of 318 with 15 failures, so the inventories are resynced and H6 is fixed locally. Two
caveats:

- The run includes the untracked `test_sync_api_ratchet.py`, which CI will not see until it is
  added.
- This tier is static. It passes even though the tree does not compile (N4) and the GUI
  cannot close documents (N1).

## Recommended next steps

1. Fix N1 before anything else: admit the lane-marshalled close in `permitsApplicationClose()`,
   and only shut the lane down after a successful close.
2. Rework N2: make close-after-save re-issue the *document* close, not `MainWindow::close()`,
   and clear the pending state on every terminal outcome. Then finish B3: after an async
   Save As, continue the close.
3. Fix N3 by keeping Python objects out of the `std::future`. Reject `postToOwner()` during
   `Close`, and refuse `wait()` on the GUI thread. Then make the addon handlers await (N6),
   push the submodule commits and re-pin.
4. Fix the N4 compile errors, and revert the blanket `recomputeAsync()` rewrite (N5), keeping
   only reviewed call sites.
5. Address M2 (lane `Edit` undo), M3 (locking), M4 (pruning) and N7 (cancel lock).
6. Restore line endings for the 25 `-text` files and `DocumentCommandHandle.h`, add the
   ratchet test to git, and split the fix pass into reviewable commits.
7. Add tests that would have caught these regressions: a GUI-hosted close, close-with-Save, and
   an async-mutation `wait()` round trip on Windows.

## Follow-up: state after the 2026-10-05 pass

The committed fix pass (`55e13f0e83`) already differed from the working tree reviewed above.
This section records each finding against the branch after `FreeCAD-start` was merged in
again (PR #63, main as of 2026-10-04) and the fixes below.

### Findings resolved in `55e13f0e83`

| ID | Resolution |
|----|------------|
| N1 | The `Close` command admits only the thread it marshalled `closeDocument()` onto (`beginMarshalledCloseAdmission()`), and shuts the lane down only after a successful close. Gtest `GuiMarshalledCloseRemovesDocument`. |
| N2 | `PendingLaneCloseKind` records whether a document close or an application close is pending. Save completion re-issues that close, and every terminal state, including `Stalled`, clears the flag. |
| N3 | The future carries the pure C++ `AsyncMutationPayload`. `postToOwner()` rejects work during `Close` and after the document is gone, and `wait()` refuses the GUI thread. Gtests `AsyncMutationWaitReturnsCommitDict` and `AsyncMutationWaitRefusesGuiThread`. |
| N4, N5 | The blanket `recomputeAsync()` rewrite and the `getDocument()` compile errors are not in the commit. |
| N9 | `MainWindow::closeAllDocuments()` judges a pending Save As by the pending flag, not by `FileName`. |
| M2 | The lane `Edit` opens a transaction named `Edit`. Gtest `EditCommandRollsBackOnUnknownObject` changes one property, fails on the next, checks the rollback, then checks `UndoNames` and `undo()`. |
| M3, M4 | Owner-thread writes to `ActiveCommand` take `_mutex`. Terminal snapshots are capped at 64 per lane, and the process archive drops the oldest closed documents past 32. |
| H3 | Automatic orphan cleanup no longer opens a modal or starts an application close. *Skip* marks the document `skipsSaveOnClose`. |
| L3, L4 | The section token includes vertex coordinates and no longer computes `Volume`. Header line endings are restored. |

### Fixed in this pass

- **Build.** The new gtests did not compile with GCC: a static data member in a local class,
  and a `std::string` passed to `runString()`. `GuiCloseDispatcher` moved to namespace scope.
- **Transaction ownership.** The lane `Edit` called `_openTransaction()`, `_commitTransaction()`
  and `_abortTransaction()` directly, which `test_document_transaction_ownership.py` forbids.
  It now goes through the collaboration service (`openMutationTransaction()`,
  `commitApplicationTransaction()`, `abortApplicationTransaction()`), the same per-document
  terminals the commit coordinator owns.
- **N3 error path.** A callback failure propagated through the future reached `wait()` as a
  generic runtime error. `wait()` now restores the original Python exception.
- **N7.** Cancelling from a non-owner thread only recorded the request; preparations that were
  already running were cancelled only after they finished. `poll()` now forwards the cancel
  to them on the owner thread.
  The rework also recorded the flag before its re-entrancy check, so a commit observer's
  rejected `cancel()` still cancelled the outer recompute
  (`commitObserverRejectsReentrantMutationsWithoutBlockingOuterCommit`). An owner-thread
  re-entrant cancel is now rejected before it changes anything.
- **Transaction control during replay (M1).** GUI-thread transaction control raced the lane
  owner replaying a commit's notifications and was refused. Leaving sketch edit opens an
  `AutoTransaction`, so main's new `test_origin_marker_tracks_drawing_tool_state` failed in
  teardown. Only re-entrant calls are refused now: calls on the replaying owner thread, and
  calls inside a blocking notification marshalled to the GUI thread. Other threads wait up
  to 10 s for the replay to finish, servicing marshalled tasks and releasing the GIL.
- **Draft angular dimensions.** The shared SVG geometry helper (L7) dropped `radius` and
  `angle`, which `updateData` still uses: main's arrow chords read `radius`, and the
  `Angle` update reads `angle`. The helper now returns both (`NameError` in
  `TestDraftGui`).
- **Tree status during a lane recompute.** `TreeWidget::onUpdateStatus()` read live object
  state while the lane owner recomputed a sketch: the overlay icon reads `ExternalGeo`. It
  crashed on freed geometry, and `TestSketcherGui` segfaulted in 3 of 5 runs. The update now
  uses the committed presentation or retries once the lane is idle, as it already did during
  undo and redo (0 of 5).
- **Merge with `FreeCAD-start`.** Seven conflicts were resolved. The branch's log calls were
  converted to the `std::format` placeholders that main now uses: 13 printf-style calls would
  have printed a literal `%s`.
- **N8.** Unchanged lines in 16 files had been rewritten from mixed line endings to CRLF.
  They are restored, and the PR diff is now the same with and without `--ignore-cr-at-eol`.
- **Architecture tier.** The tier was red on the branch. The sync-API ratchet still described
  the reverted `recomputeAsync()` rewrite. The AB-21, view-provider and CC-WP05 inventories
  had drifted with main's line numbers, and main removed the AxisMap task-panel transaction
  rows. The inventories were regenerated with their own generators, and the hard-coded
  sites in the tests were remapped by line diff.

### Local pipeline run (Docker)

Every step of `.woodpecker/ci.yml` was run on the final tree in the CI images
(`freecad-ci-deps:24.04`, `freecad-ci-mcp:24.04`, `python:3.12`, `pixi:0.81.0`), with
freecad-mcp at the pinned `1608657`:

| Step | Result |
|------|--------|
| all-submodules-check, mcp-pin-on-main, pixi-lock-check | pass |
| freecad-lint | pass (0 misspellings in 324 changed files) |
| freecad-arch-tests | 329 passed |
| freecad-configure-debug, freecad-debug-build | pass (GCC, `-Wall -Wextra`) |
| freecad-unit-tests | every gtest binary passes (App 1110) |
| freecad-integration-tests | `FreeCADCmd -t 0` passes |
| freecad-e2e | every GUI module passes |
| freecad-mcp-lint, freecad-mcp-unit-tests, freecad-mcp-sidecar-integration | pass (12076 unit) |
| freecad-mcp-load-preflight | `PREFLIGHT_OK` |
| freecad-mcp-core-{collab,sketch,part,rest} | 59 / 256 / 335 / 106 passed |
| freecad-mcp-e2e | 128 passed |

### Not fixed here

- **B4 / N6 (MCP mutations in GUI FreeCAD).** The addon commits `2be76ee`..`8069f11` make
  every typed handler return a pending waiter on the GUI thread. They fail MCP's own gates:
  mypy reports 278 errors, and 59 unit tests fail. They were never on freecad-mcp `main`,
  which the `mcp-pin-on-main` gate requires. PR #62 therefore pins freecad-mcp `main`
  (`1608657`) again, and that work continues on the freecad-mcp branch
  `feature/nonblocking-document-execution`.
  A GUI-thread mutation still raises `DocumentWouldBlock`. That is an explicit refusal, not
  a crash or a wrong result.
- **M1** is still only checked statically. **L2** (pick after an in-place mesh edit) and
  **L7** (duplicated Draft SVG geometry) are unchanged.
