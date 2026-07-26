# Non-blocking Geometry Architecture Progress

## Status overview

The 2026-07-24 follow-up read-only implementation review verdict remains **FAIL** for end-to-end
detached geometry. Substantial Priority 1–3 scaffolding is now in place (commit fences, non-blocking
close, FreeCADCmd dispatch, archive deep-copy/repeat-write, builtin registry, geometry progress
show path, hasher revision advance on commit, trusted relative result path/size/digest checks,
Unix process-group cancel, progress-controller manager hooks, atomic hasher merge, typed
`Part::Boolean` codec, and real FreeCADCmd mapped Boolean qualification), but the working tree still
lacks Fillet/Sweep codecs, Windows Job Object cancel, retained-success janitor, production
prepare/commit adapters, and Phases 3–6. No rollout path is ready to enable. Phase 2 is **not**
complete.

| Phase | Plan focus | Actual status |
|---|---|---|
| 1 | Guardrails and scheduler foundation | Mostly scaffolded; fences/transactions/failure-aware generation/slice budget/non-blocking close/FreeCADCmd cancel covered. 30 s acceptance heartbeat and non-cooperative shutdown recovery still incomplete |
| 2 | Archive and isolated worker | In progress; deep OCC copy + repeat-write restore, mapped Boolean/Fillet/Sweep (in-process), hasher revision/atomic merge, nested/shared child maps, long hashed names (FCG1 v4), typed Boolean codec, parent `GeometryWorkerProcess` protocol+decode gates, real FreeCADCmd mapped Boolean + semantic FCStd reopen. Fillet/Sweep codecs and broader protocol fuzz still open |
| 3 | Non-blocking visual pipeline | Not started; GUI-thread tessellation remains. Progress controller is hooked to the manager; 250 ms GUI visibility harness still missing |
| 4 | Native geometry migrations | Unconnected ops (mapped + registry allowlisted) without feature/task-panel adapters |
| 5 | Recompute and legacy compatibility | Not started; `Std_Refresh` still sync; `DetachedDocumentArchive` stub |
| 6 | Qualification and rollout | Not started |

Verified narrow positives (focused Docker validation: **35/35 App**, **44/44 Part**,
**3/3 CrossProcessBooleanTest**, **16/16 GeometryWorkerProcessTest**):

- Commit fences (job/object name/type/incarnation/generation), transaction-wrapped commit, and
  failure-aware generation (no advance on failed commit) with `FeatureTestDetached`.
- Successful detached commit advances document hasher revision; failed commit does not; stale
  hasher deltas are rejected after advance (`commitHasherDelta` / `mergeExactClosure`).
- Nested/shared child `ElementMap` identity preserved through createBundle/write/read; long hashed
  names round-trip with FCG1 v4 hasher threshold restoration.
- Non-blocking legacy close with deferred Document destroy; FreeCADCmd invalidate cancel.
- Manager-owned FreeCADCmd launch + start-failure path; Gui installs process backend.
- Archive `makeElementCopy` deep geometry; repeat-write byte-identical with mark/`_id` restore.
- Builtin `Part::Boolean`/`Fillet`/`Sweep` registry registration at Part module load.
- Geometry progress show bypasses 2 s sequencer delay (`showGeometryProgress`); manager progress/
  state listeners wire `GeometryProgressController`.
- Trusted relative result path + size/digest/jobId checks; Unix process-group terminate/kill.
- Atomic `materializeExactClosure` / `mergeExactClosure` (threshold/revision/value-ID uniqueness +
  rollback); typed Boolean request codec; operands rebound onto one worker hasher.
- Parent-controlled `Gui::GeometryWorkerProcess` mapped Boolean (real FreeCADCmd): fail-closed
  FCGEO/1 protocol, task-aware FCG1 decode before `ReadyToCommit`, explicit serialize failure
  channel, digest-correct malformed FCG rejected, FCStd second-process naming/SID/history reopen.

Those positives are scaffolding checks, not phase acceptance. No production Part/PartDesign feature
implements prepare/commit; Fillet/Sweep still lack typed codecs; Windows Job Object cancel and
installed-tree smoke beyond the Docker build tree remain open; Phases 3–6 are untouched.


## Implementation history

### Step 0 — Baseline scaffold audit

- Confirmed the initial commit added the planned API sketches:
  `GeometryJobManager`, `DocumentRecomputeCoordinator`, `DetachedDocumentArchive`,
  `GeometryWorkerProcess`, `TopoShapeArchive`, and Boolean/Fillet/Sweep task classes.
- Confirmed Phase 3 visual classes were absent and Phase 4 feature/GUI integration was absent.
- The initial progress claim of broad completion was already known to be overstated.

### Step 1 — Phase 1 guardrail changes

Implemented or attempted:

- Added `App::GuiResponsivenessProbe` and limited scoped instrumentation.
- Added document generation changes for property writes, `touch()`, add/remove, undo/redo, and
  close invalidation.
- Removed the coordinator's direct unsupported-object `recomputeFeature()` fallback.
- Added manager coalescing/state changes and `SequencerBar` geometry-progress methods.
- Added narrow scheduler and idle-heartbeat tests.

Review corrections:

- `GeometryJobManager::submit()` stores a queued record but launches no backend.
- `GeometryWorkerProcess` and `GeometryProgressController` have no production call sites.
- `GeometryJobSpec::coalescing`, `RecomputeTargets::forceAll`, and the slicing/async options are
  ignored.
- `cancelCurrentSession()` can invalidate its iterator through its synchronous callback;
  multi-target sessions stale their sibling jobs after the first commit; terminal states can be
  overwritten; object invalidation is not document-scoped; callbacks are not marshalled to the GUI
  thread; and job records are never pruned.
- The heartbeat is started only by its unit test. The duration test is tautological, and the
  heartbeat test measures an idle 80 ms event loop rather than the acceptance workloads.
- The progress methods are disconnected and retain the status bar's 2-second show delay, so they
  do not establish the required 250 ms visibility.

### Step 2 — Narrow Phase 1 compile/test pass

Historical validation compiled `FreeCADApp`/`FreeCADGui` and passed the new filtered tests. The
review reproduced those results, but the earlier label “Phase 1 validated” is withdrawn.

Confirmed regression:

- `Application::canRecomputeRequestOnWorker()` is hard-coded false while
  `queueRecomputeRequest()` executes unsafe requests inline. The old live-document worker still
  exists, and GUI recompute entry points still call synchronous `Document::recompute()`.
- `AsyncRecomputeTest.WorkerSafetyIsCheckedFromRequest` fails.
- `AsyncRecomputeTest.CloseDocumentWaitsForInFlightAsyncRecompute` blocks before its release point
  and does not complete.
- `Document.recomputeAsync()` with the normal empty object list schedules no targets; `forceAll`
  and `checkCycle` are ineffective.

### Step 3 — Phase 2 archive/process changes

Implemented or attempted:

- Added an FCG1 version field, SHA-256 calculation, temporary-file publication, request JSON, and
  FCGEO/1 message parsing/emission.
- Added a registry API and a hasher-delta validation helper.

Review corrections:

- `TopoShapeArchive::createBundle()` restores the source map but assigns the returned null pointer
  to `bundle.elementMap`; archives therefore omit topological naming.
- Hasher IDs, mapped elements, source identities, and the exact-ID closure are not serialized.
  `ElementMapArchiveContext` is unused, global save/restore tables remain, readback uses a null
  hasher, and the restored map is not rebound to the shape.
- Archive-controlled lengths are allocated without bounds; the digest omits tag/revision/high-water
  metadata; real snapshots leave revision zero; and `mergeHasherDelta()` validates only a subset of
  state and never appends missing IDs.
- `GeometryWorker.py` is not copied or installed, the registry has no built-in registrations, all
  task `writeArchive()` methods are empty, and no input decoder exists.
- Parent result handling trusts a child-supplied path and file existence, ignores size/digest/job/
  build/message ordering, accepts nonzero normal exit after a result line, and deletes a successful
  workspace immediately after emitting `jobFinished`.
- Cooperative cancellation is not read by the child, timeout can be reported as a crash, only the
  direct process is killed, process-start failure has no completion path, and the controller
  destructor performs a forbidden `waitForFinished(500)`.

### Step 4 — Operation/hasher test sketches

- Same-process Boolean Fuse and Fillet happy paths, SHA hex formatting, checksum corruption, and
  one revision-mismatch case are covered.
- Boolean Section and failed-build handling are incorrect; Sweep is untested; operation code does
  not check cancellation/deadlines or preserve mapped OCC history.
- There are no process, install, protocol, malformed-size, map/child-map/history, exact merge,
  crash, cleanup, FCStd reopen, task-panel, responsiveness-under-load, or downstream naming tests.

## Read-only review record — 2026-07-24

Scope and constraints:

- Reviewed the current dirty working tree based on `14333d5cb0`.
- Did not modify implementation or test files, commit, or interact with the running MCP server or
  FreeCAD instance.
- Used only new, isolated Docker containers for build/test execution. Existing containers were not
  reused, restarted, stopped, or reconfigured.
- Updated `doc/PLAN.md` with the newly verified implementation gates, then updated this file last.

Docker environment:

- Image tag: `127.0.0.1:5001/freecad-ci-deps:24.04`
- Local image ID:
  `sha256:1e0c674e6b58b62e1e8e02d1fdedb4b484e540263959c7e54ed032771e2527a0`
- Available repo digest:
  `sha256:0d3423f22af09202a4b65a8396de5ff498098f77280f501d8048d5f3c782a240`
- Source mount: `D:\code\FreeCAD` to `/code`
- Build directory: existing incremental `/code/build_docker` (`Debug`, Ninja, tests enabled)

Validation performed:

- `ninja -C build_docker App_tests_run Part_tests_run FreeCADGui` — success.
- `App_tests_run --gtest_filter=GeometryJobTest.*:GuiResponsivenessProbeTest.*` —
  **8/8 passed**.
- `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` — **6/6 passed**.
- `AsyncRecomputeTest.WorkerSafetyIsCheckedFromRequest` — **failed** at
  `tests/src/App/AsyncRecompute.cpp:106`.
- `AsyncRecomputeTest.CloseDocumentWaitsForInFlightAsyncRecompute` — **did not complete** and was
  terminated by a 5-second Docker timeout while still inside the test body.
- Broad App run excluding only that known hang — **534 tests run: 519 passed, 2 skipped, 13
  failed**. The worker-safety failure is a confirmed implementation regression. Other failures
  reported unknown C++ exceptions; an isolated rerun identified
  `Materials::MaterialNotFound` for at least one, so those failures were not attributed to this
  implementation without a clean baseline.

Validation limitations:

- The build was incremental, not a clean-from-scratch configure/build.
- No GUI, camera, heartbeat-under-load, process-worker, install-tree, cross-process topology,
  cancellation/tree-kill, crash/fuzz, sanitizer, Windows Job Object, or full Part regression
  qualification was performed.
- Runtime MCP and the existing FreeCAD process remain deliberately unverified. The MCP submodule
  worktree is clean but checked out at `191a1046ee4391273bfddc31cdcf691526087f41`, while the
  superproject records `5894a776c1b437c77e9c369797d37a3f63d8d338`; the review did not alter it.

## Follow-up read-only review record — 2026-07-24

Scope and constraints:

- Reviewed the current dirty working tree based on `14333d5cb0`.
- Did not modify implementation or test files, commit, or interact with the running MCP server or
  FreeCAD instance. Only `doc/PLAN.md` and this progress record were updated.
- Used only new, network-isolated Docker containers. Existing containers were not reused,
  restarted, stopped, or reconfigured.
- `doc/PLAN.md` was updated first; this file is the final filesystem edit of the review.

Docker environment and validation:

- Image: `127.0.0.1:5001/freecad-ci-deps:24.04`
- Image ID: `sha256:1e0c674e6b58b62e1e8e02d1fdedb4b484e540263959c7e54ed032771e2527a0`
- Repo digest: `sha256:0d3423f22af09202a4b65a8396de5ff498098f77280f501d8048d5f3c782a240`
- Source mount: `D:\code\FreeCAD` to `/code`; existing Debug/Ninja build directory:
  `/code/build_docker`.
- CMake regenerated successfully. `App_tests_run` and `Part_tests_run` linked. The first full GUI
  build container was externally SIGKILLed (Docker exit 137) at step 106/224 without a compiler
  error; a fresh container resumed and successfully linked `libFreeCADGui.so`.
- Compiler warnings in reviewed code: signed/unsigned flag comparison at
  `src/App/StringHasher.cpp:975`, and unused cancellation reason at
  `src/Gui/GeometryWorkerProcess.cpp:145`.
- Focused App filter
  `AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`: **16/16 passed**.
- Focused Part filter `NonBlockingGeometryTest.*`: **11/11 passed**.
- Full App: **541 run, 527 passed, 2 skipped, 12 failed**. The failures are the same
  Material/Expression group recorded previously; they were not attributed to this change without a
  clean passing baseline.
- Full Part: **297 run, 190 passed, 107 failed**. Isolated representatives from Attacher, mapped
  Boolean, PropertyTopoShape, and WireJoiner all abort with `Materials::MaterialNotFound`; the
  environment therefore prevents a regression verdict for the broad Part suite.
- The broad Part run created `test.FCStd` and `test2.FCStd` in the bind-mounted repository root.
  Both review-created artifacts were removed after their exact workspace paths were verified.
- `git diff --check` reports trailing whitespace in changed App/Gui hunks, including
  `src/App/Application.cpp`, `Document.cpp`, `Document.h`, `DocumentObject.cpp`, `FeatureTest.h`,
  and `src/Gui/ProgressBar.cpp`.

Confirmed defects:

- **Critical — no production path:** no production object implements the detached prepare/commit
  API, and `Std_Refresh` still executes synchronous `Document::recompute()` when the legacy
  live-document worker is denied (`src/App/DocumentRecomputeCoordinator.cpp`,
  `src/Gui/CommandDoc.cpp`). Phase 1 and every dependent phase remain incomplete.
- **High — naming identity is still incomplete for operations:** high-water seeding, duplicate-ID
  rejection, and mapped Boolean/Fillet/Sweep builders are covered, but production never advances
  the hasher revision on commit, and freeze/write deep-copy/repeatability remain incomplete.
- **High — commit/state-machine contract is incomplete:** graph expansion/sorting/snapshotting run
  synchronously and ignore `allowAsync`/`maxGuiSlice`; different-purpose in-flight requests can be
  dropped; commit checks only incarnation/generation and object ID; there is no transaction,
  fingerprint/digest/current-job/type/name fence, and generation advances even if commit fails
  (`src/App/DocumentRecomputeCoordinator.cpp`, `src/App/GeometryJob.cpp`).
- **High — scheduler semantics are incomplete:** commit fences/transactions/slice budgets remain
  missing (`DocumentRecomputeCoordinator.cpp`). Legacy close is non-blocking with deferred
  Document destruction; FreeCADCmd invalidation cancels processes without waiting. Residual:
  `Document::recompute` still holds the GIL for the full call, so a worker that blocks without
  releasing the GIL can still stall close observers.
- **High — process controller/worker is not trusted or recoverable yet:** manager now owns
  FreeCADCmd dispatch + random workspaces + start-failure completion, but result
  path/size/digest/job/build/message ordering are not validated, registry registrations/codecs/
  input decoding are absent, child cancellation is not consumed, process trees are not owned, and
  `waitForStarted` can block the GUI thread briefly
  (`src/Gui/GeometryWorkerProcess.cpp`, `src/Mod/Part/App/GeometryWorker.cpp`,
  `src/Mod/Part/App/GeometryWorkerRegistry.cpp`).
- **Medium — progress acceptance is not implemented:** manager progress is only stored and has no
  production controller call; the geometry progress bar retains a 2-second minimum and
  `delayedShow()` requires the unrelated legacy sequencer to be running
  (`src/App/GeometryJobManager.cpp`, `src/Gui/ProgressBar.cpp`). The 250 ms requirement is not met
  or tested.
- **Medium — test coverage is not an acceptance workload:** the heartbeat load is a cooperative
  250 ms sleep task, not a 30-second OCC/serialization/import/visual workload; archive tests omit
  nested child maps, repeat-write immutability, cross-process mapped history, malformed protocol,
  crash/cancel/recovery, FCStd reopen, and sanitizers. FreeCADCmd coverage uses a fake App-side
  backend, not an installed-tree FreeCADCmd launch.

Confirmed risks:

- `FrozenTopoShapeBundle` uses shallow `TopoShape` assignment, so the OCC geometry is not a private
  immutable copy (`src/Mod/Part/App/TopoShapeArchive.cpp`).
- `writeArchive(const FrozenTopoShapeBundle&)` mutates private hasher marks and map archive IDs, and
  source state restoration is not protected by RAII around every throwing save operation
  (`src/Mod/Part/App/TopoShapeArchive.cpp`).
- Finished in-process workers that call `setJobState` on their own thread are detached rather than
  joined (cannot join self); destructor still joins any remaining joinable slots.

Unverified items:

- Existing live MCP and FreeCAD behavior was deliberately not exercised. The MCP submodule worktree
  is clean at `191a1046ee4391273bfddc31cdcf691526087f41`, different from the superproject-recorded
  `5894a776c1b437c77e9c369797d37a3f63d8d338`; behavioral equivalence is unverified.
- Real GUI navigation/painting/cancellation, 33/100/250 ms acceptance, install-tree worker launch,
  cross-process naming/history, Windows Job Object behavior, crash/OOM/disk/rename recovery,
  fuzzing, and sanitizer qualification remain unverified.
- Broad App/Part regression safety is unverified until the Docker Material/Expression environment
  has a known passing baseline.

## Next step

Required remaining fixes (priorities still open — do not enable rollout):

1. **Priority 1 residuals:** 30-second OCC/serialization heartbeat acceptance workload;
   non-cooperative deadline/shutdown recovery; `Document::recompute` GIL hold during long work.
2. **Priority 2 residuals:** typed Fillet/Sweep codecs + FreeCADCmd qualification; broader
   hang/crash/OOM/rename recovery matrix; installed-tree smoke outside the Docker build tree.
3. **Priority 3 residuals:** Windows Job Object cancel; retained-success cleanup janitor;
   250 ms geometry-progress visibility harness (controller is wired; GUI timing test still open).
4. **Priority 4 (blocked until 1–3):** production prepare/commit adapters, `Std_Refresh` routing,
   Phases 3–6 (visual pipeline, feature migrations, compatibility, qualification).

Concrete next action for the implementing agent:

- Typed `Part::Fillet` (or Sweep) codec through FreeCADCmd with the same mapped-name/FCStd
  qualification bar as Boolean. Keep production feature adapters and rollout flags disabled.
  Do not mark Phase 2 complete.

## Implementation log (continued)

### Step 5 — Priority 1: restore async worker baseline and lifecycle (in progress)

* Completed work:
  - Restored `Application::canRecomputeRequestOnWorker()` to explicit object/document opt-in
    (deny-by-default via `DocumentObject::canRecomputeOnWorker()` default `false`).
  - `FeatureTest` now explicitly opts into worker recompute so `WorkerSafetyIsCheckedFromRequest`
    matches deny-by-default semantics.
  - Coordinator now expands empty/`forceAll` targets, dependency-orders them, and submits
    **sequentially** (one in-flight job) to avoid multi-target self-invalidation.
  - `cancelCurrentSession()` copies pending IDs before cancel (callback-safe).
  - Manager: terminal states are monotonic; object invalidation is document-incarnation scoped;
    terminal job pruning added; `GeometryCommitScope` advances generation without invalidating
    pending session jobs.
  - Added tests: terminal monotonicity, cancel-from-callback, document-scoped invalidation,
    empty-target expansion.
* Files changed:
  - `src/App/Application.cpp`, `FeatureTest.h`, `GeometryJob.h`, `GeometryJob.cpp`,
    `GeometryJobManager.{h,cpp}`, `DocumentRecomputeCoordinator.{h,cpp}`, `Document.{h,cpp}`,
    `DocumentObject.cpp`, `tests/src/App/TestGeometryJob.cpp`, `tests/src/App/AsyncRecompute.cpp`,
    `doc/PROGRESS.md`
* Tests or validation performed:
  - Docker validation starting (isolated container).
* Current issues or blockers:
  - Awaiting Docker results for `AsyncRecomputeTest.*`, focused GeometryJob tests, and full
    `App_tests_run`.
* Next step:
  - Record Docker evidence; only then continue remaining Priority 1 gaps or Priority 2.

### Step 5b — Focused Docker results

* Tests or validation performed (Docker container `freecad-nbgeom-p1fix`):
  - `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **14/14 PASSED**
  - Confirmed previously failing/hanging cases now pass:
    - `AsyncRecomputeTest.CloseDocumentWaitsForInFlightAsyncRecompute`
    - `AsyncRecomputeTest.WorkerSafetyIsCheckedFromRequest`
    - `AsyncRecomputeTest.EmptyRecomputeAsyncExpandsTouchedTargets`
* Current issues or blockers:
  - Full `App_tests_run` still in progress.
* Next step:
  - Record full App suite result; triage any remaining baseline failures.

### Step 5c — Full App suite Docker evidence

* Tests or validation performed (Docker container `freecad-nbgeom-apptest`):
  - Full `App_tests_run`: **539 tests, 525 passed, 2 skipped, 12 failed**
  - All Priority-1 geometry/async suites green in the full run:
    - `AsyncRecomputeTest.*` (3/3)
    - `GeometryJobTest.*` (9/9)
    - `GuiResponsivenessProbeTest.*` (2/2)
  - Confirmed regression from review is gone (`WorkerSafetyIsCheckedFromRequest` and
    `CloseDocumentWaitsForInFlightAsyncRecompute` both pass).
  - Remaining 12 failures isolated with `GTEST_CATCH_EXCEPTIONS=0`:
    - `DocumentObjectTest.getSubObjectList` → `Materials::MaterialNotFound` (environment/data)
    - `ExpressionParserTest.*` / `PropertyExpressionEngineTest.*` → `Base::TypeError` in SetUp
      (same class of baseline failures noted in the 2026-07-24 review; not attributed to this
      geometry-job change set)
* Current issues or blockers (Priority 1 remaining):
  - `GeometryJobManager::submit()` still does not launch FreeCADCmd / in-process backends.
  - Production callers do not drive `GeometryProgressController`; SequencerBar geometry progress
    is not proven to become visible within 250 ms under load.
  - No 30-second heartbeat/cancellation workload test yet for the 33/100/250 ms contracts.
  - Callbacks are not yet marshalled onto the GUI thread via a dedicated hop helper.
* Next step (still Priority 1):
  - Add a Docker heartbeat-under-synthetic-load test, then wire a minimal job executor path that
    can complete a queued `DetachedGeometryTask` without live-document access (in-process only when
    allowlisted; otherwise leave queued for process backend). Do not start Priority 2 until that
    executor + load test evidence is recorded.

### Step 5d — Minimal in-process executor + heartbeat-under-load

* Completed work:
  - `GeometryJobManager` launches allowlisted `VerifiedInProcess` tasks on a worker thread with a
    private temp dir and cooperative cancel flag (no live document/GUI pointers).
  - Terminal delivery marshals to the GUI thread when `MainThreadSignalConfig` hooks exist.
  - Added `AllowlistedInProcessExecutorCompletes` and `HeartbeatUnderSyntheticLoad` tests.
* Files changed:
  - `src/App/GeometryJobManager.{h,cpp}`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-p1exec`):
  - Focused filter `AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **16/16 PASSED** (includes in-process executor + heartbeat-under-load <100 ms gap / <33 ms
    slice).
* Current issues or blockers:
  - FreeCADCmd process backend still not launched from the manager (queued until Gui process
    controller is wired).
  - Production SequencerBar geometry progress still not driven by job updates (250 ms visibility
    not proven in UI).
  - Full App suite still has 12 baseline Material/Expression failures unrelated to this work.
* Next step:
  - Begin Priority 2 transport fixes starting with lossless `TopoShapeArchive::createBundle()`
    ElementMap capture and hasher closure serialization; keep process-backend wiring as the bridge
    from Priority 1.

### Step 6 — Priority 2: lossless ElementMap + hasher closure

* Completed work:
  - Fixed `TopoShapeArchive::createBundle()` so ElementMap is privately cloned (no null assignment
    from the double-`resetElementMap` bug) and rebound onto a private hasher clone.
  - Added `App::StringHasherClosure` capture/merge APIs with exact-ID append, collision, and
    revision checks; `mergeHasherDelta()` now actually appends missing IDs.
  - Added `ElementMapArchiveContext`-aware `beforeSave`/`restore` paths so FCG1 transport does not
    rely on process-global document save tables.
  - Bumped FCG1 to v3 with serialized hasher closure, authenticated digest over metadata+payloads,
    and bounded section/blob lengths.
  - Extended Part NonBlocking tests for map round-trip, append, collision, and oversized rejection.
* Files changed:
  - `src/App/StringHasher.{h,cpp}`, `src/App/ElementMap.{h,cpp}`
  - `src/Mod/Part/App/TopoShapeArchive.{h,cpp}`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-p2fix2`):
  - `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **10/10 PASSED**
    (includes ElementMapSurvivesArchiveRoundTrip, HasherDeltaAppendsMissingIds,
    HasherDeltaCollisionRejected, OversizedSectionRejected).
* Current issues or blockers:
  - Priority 2 still open: FreeCADCmd process backend, installed `GeometryWorker.py`, retained
    artifacts, process-tree cancel, task encode/decode, registry registrations.
* Next step:
  - Defect-first review of Step 6 transport slice; then continue Priority 2 process-backend /
    install / artifact lifetime work.

### Step 6b — Review fixes for freeze mutation + atomic merge

* Review findings addressed:
  - [P1] `createBundle` now snapshots/restores hasher marks and ElementMap `_id`s so freeze does
    not leave live document save state mutated.
  - [P1] `mergeExactClosure` validate-then-commit: collision/missing-related reject before any
    append; multi-entry collision test asserts size unchanged.
  - [P2] Clone failure sets `FrozenTopoShapeBundle::valid=false` / `errorCode`; `writeArchive`
    rejects invalid bundles.
  - [P3] `highWaterId` now uses `lastID()` instead of `size()`.
* Files changed:
  - `src/App/StringHasher.{h,cpp}`, `src/App/ElementMap.{h,cpp}`
  - `src/Mod/Part/App/TopoShapeArchive.{h,cpp}`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-p2review2`):
  - `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **11/11 PASSED**
    (includes mark-preservation check in ElementMapSurvivesArchiveRoundTrip and
    HasherDeltaCollisionLeavesPriorAppendsUncommitted).
* Current issues or blockers:
  - Priority 2 still open: FreeCADCmd process backend, installed `GeometryWorker.py`, retained
    artifacts, process-tree cancel, task encode/decode, registry registrations.
* Next step:
  - Continue Priority 2 process-backend wiring: install `GeometryWorker.py`, register built-in
    ops, keep result artifacts until decode/commit, and avoid destructor `waitForFinished`.

### Step 7 — Result workspace ownership + high-water/duplicate hasher gates

* Completed work:
  - `GeometryJobManager` retains in-process workspaces through terminal delivery and deletes them
    only via `releaseJobArtifacts()`, prune, or destructor. Temp dirs use a per-launch stamp.
  - `SleepTask` now creates a real artifact file; GUI-hop test installs `MainThreadSignalConfig`
    hooks and asserts the path is readable inside the deferred callback, then gone after release.
  - `StringHasher::mergeExactClosure` rejects duplicate IDs and IDs above `highWaterId` before any
    mutation; successful merges call `reserveHighWater` so `lastID()`/new allocations skip gaps.
  - `TopoShapeArchive::{createBundle,readArchive}` seed empty-entry private hashers when high-water
    is non-zero.
* Files changed:
  - `src/App/GeometryJobManager.{h,cpp}`, `src/App/StringHasher.{h,cpp}`
  - `src/Mod/Part/App/TopoShapeArchive.cpp`
  - `tests/src/App/TestGeometryJob.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step7`, `/code/build_docker`):
  - Build: `ninja -C /code/build_docker App_tests_run Part_tests_run` → success
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **17/17 PASSED** (includes `ResultWorkspaceSurvivesQueuedGuiCallback`)
  - Focused Part filter (twice):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **14/14 PASSED**
    (includes `HasherHighWaterGapReservesFutureIds`, `HasherRejectsIdAboveHighWater`,
    `HasherDuplicateIdsLeaveCanonicalUnchanged`)
* Current issues or blockers:
  - Multi-observer callbacks still overwrite; Running-job cancel flag is not reliably set on
    supersede/invalidation; FreeCADCmd, mapped OCC history, and commit fences remain open.
* Next step:
  - See revised `## Next step` above (joined observers + cancel-flag propagation first).

### Step 8 — Multi-observer fan-out + cancel-flag on supersede/invalidate

* Completed work:
  - `JobRecord` stores `std::vector<JobCallback>`; terminal delivery fans out to every observer.
  - Late `registerCallback` after terminal still notifies the new observer.
  - Supersede, document invalidation, and object invalidation call `requestCancelFlag()` so Running
    in-process workers observe cooperative cancel.
* Files changed:
  - `src/App/GeometryJobManager.{h,cpp}`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step8`, `/code/build_docker`):
  - `ninja -C /code/build_docker App_tests_run` → success
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **19/19 PASSED** (includes `JoinedObserversAllReceiveTerminal`,
    `SupersedeSetsCancelFlagForRunningInProcessJob`)
  - Focused Part filter (re-check):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **14/14 PASSED**
* Current issues or blockers:
  - Coalescing modes and parameter-aware join identity remain unimplemented; FreeCADCmd, mapped OCC
    history, and commit fences remain open.
* Next step:
  - See revised `## Next step` above (parameter-aware join + coalescing modes).

### Step 9 — Honor coalescing modes + parameter-aware join identity

* Completed work:
  - `submit()` aligns `purpose` from the coalescing key **before** deadline selection.
  - Joins/replacements follow `CoalesceMode`: `None` (no join/cancel), `LatestWins` (always
    replace), `SingleInstance`/`Union` (join only on identical task identity; otherwise replace;
    older generation does not displace a newer active job).
  - Task identity is `operationType` + `codecVersion` + `parameterDigest()` (new virtual on
    `DetachedGeometryTask`).
* Files changed:
  - `src/App/GeometryJob.h`, `src/App/GeometryJobManager.cpp`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step9`, `/code/build_docker`):
  - `ninja -C /code/build_docker App_tests_run` → success
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **22/22 PASSED** (includes `DifferentParametersDoNotJoinUnderSingleInstance`,
    `CoalesceNoneDoesNotJoinOrCancel`, `CoalesceLatestWinsCancelsEvenIdenticalModelJob`)
  - Focused Part filter:
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **14/14 PASSED**
* Current issues or blockers:
  - Production geometry ops still return empty `parameterDigest()`; FreeCADCmd, mapped OCC history,
    thread bounds, and commit fences remain open.
* Next step:
  - See revised `## Next step` above (operation parameter digests).

### Step 10 — Operation parameter digests for join identity

* Completed work:
  - Added `TopoShapeArchive::fingerprintBundle()` (tag/high-water/revision/map size/OCC HashCode/
    bounding box).
  - `BooleanGeometryOperation`, `FilletGeometryOperation`, and `SweepGeometryOperation` override
    `parameterDigest()` with type/operand/parameter fingerprints.
* Files changed:
  - `src/Mod/Part/App/TopoShapeArchive.{h,cpp}`
  - `src/Mod/Part/App/BooleanGeometryOperation.{h,cpp}`
  - `src/Mod/Part/App/FilletGeometryOperation.{h,cpp}`
  - `src/Mod/Part/App/SweepGeometryOperation.{h,cpp}`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step9`, `/code/build_docker`):
  - Focused Part filter (twice):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **17/17 PASSED**
    (includes `BooleanDifferentOperandsHaveDistinctDigests`,
    `BooleanDifferentOperandsDoNotJoinUnderSingleInstance`,
    `FilletDifferentRadiiHaveDistinctDigests`)
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **22/22 PASSED**
* Current issues or blockers:
  - Fingerprints use HashCode+bbox (not a content checksum); residual collision risk remains.
  - Mapped OCC history on op results, FreeCADCmd, thread bounds, and commit fences remain open.
* Next step:
  - See revised `## Next step` above (mapped Boolean/Fillet/Sweep history).

### Step 11 — Mapped Boolean OCC history

* Completed work:
  - `BooleanGeometryOperation` now materializes tagged operands and calls
    `TopoShape::makeElementBoolean` (Fuse/Cut/Common/Section) instead of raw
    `FCBRepAlgoAPI_*` + `setShape`.
  - Result freeze rejects invalid bundles; cooperative cancel checked around compute.
* Files changed:
  - `src/Mod/Part/App/BooleanGeometryOperation.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step11`, `/code/build_docker`):
  - `ninja -C /code/build_docker Part_tests_run` → success
  - Focused Part filter (twice):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **18/18 PASSED**
    (includes `BooleanFusePreservesMappedElementHistory`)
  - Focused App filter:
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **22/22 PASSED**
* Current issues or blockers:
  - Fillet/Sweep still raw; FreeCADCmd, thread bounds, commit fences remain open.
  - Test asserts map size ≥ 10, not full name-equality against a golden history list.
* Next step:
  - See revised `## Next step` above (mapped Fillet).

### Step 12 — Mapped Fillet OCC history

* Completed work:
  - `FilletGeometryOperation` now uses `BRepFilletAPI_MakeFillet` + `TopoShape::makeElementShape`
    with `OpCodes::Fillet` (same pattern as `FeatureFillet`), preserving per-edge radii.
* Files changed:
  - `src/Mod/Part/App/FilletGeometryOperation.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step11`, `/code/build_docker`):
  - Focused Part filter (twice):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **19/19 PASSED**
    (includes `FilletPreservesMappedElementHistory`)
  - Focused App filter → **22/22 PASSED**
* Current issues or blockers:
  - Sweep still raw; FreeCADCmd, thread bounds, commit fences remain open.
* Next step:
  - See revised `## Next step` above (mapped Sweep).

### Step 13 — Mapped Sweep OCC history

* Completed work:
  - `SweepGeometryOperation` now materializes tagged spine/profile operands and calls
    `TopoShape::makeElementPipeShell` with `OpCodes::Sweep` (same pattern as `Part::Sweep`),
    instead of raw `BRepOffsetAPI_MakePipeShell` + `TopoShape(shape)`.
* Files changed:
  - `src/Mod/Part/App/SweepGeometryOperation.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step13`, `/code/build_docker`):
  - `ninja -C /code/build_docker Part_tests_run` → success
  - Focused Part filter (twice):
    `Part_tests_run --gtest_filter=NonBlockingGeometryTest.*` → **20/20 PASSED**
    (includes `SweepPreservesMappedElementHistory`)
  - Focused App filter → **22/22 PASSED**
* Current issues or blockers:
  - Map-size assertions only (not golden name lists); Frenet/transition modes not exposed on the
    detached sweep op yet.
  - Unbounded in-process threads, FreeCADCmd, commit fences, and archive deep-copy/repeat-write
    remain open.
* Next step:
  - See revised `## Next step` above (bounded in-process worker pool).

### Step 14 — Bounded in-process worker pool + reclaim

* Completed work:
  - Replaced per-job retained `std::thread` list growth with `WorkerSlot` + finished flags.
  - Cap concurrent unfinished in-process workers at `MaxConcurrentInProcessWorkers` (2); queue
    excess as `Queued` and dispatch when a slot frees.
  - `reclaimFinishedWorkers()` joins finished workers outside the mutex; detaches if reclaim runs
    on the finishing worker itself (cannot join self).
  - Exposed `retainedWorkerThreadCount` / `peakUnfinishedWorkerThreadCount` for tests.
* Files changed:
  - `src/App/GeometryJobManager.{h,cpp}`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step14`, `/code/build_docker`):
  - `ninja -C /code/build_docker App_tests_run` → success
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **23/23 PASSED** (includes `InProcessWorkersAreBoundedAndReclaimed`, 32 jobs)
  - Focused Part filter → **20/20 PASSED**
* Current issues or blockers:
  - FreeCADCmd jobs still never launch; commit fences / non-waiting close / archive deep-copy remain.
  - Detached self-finish path skips join (documented residual).
* Next step:
  - See revised `## Next step` above (FreeCADCmd manager-owned launch).

### Step 15 — Manager-owned FreeCADCmd process backend launch

* Completed work:
  - Added injectable `GeometryProcessLaunchRequest` / process backend hooks on `GeometryJobManager`.
  - FreeCADCmd dispatch creates a random manager-owned workspace, marks `Running`, invokes the
    backend; start failure completes as `Failed`/`ProcessStartFailed` and cleans the workspace.
  - Cancel of running FreeCADCmd jobs invokes the process cancel hook (`Cancelling`).
  - Gui registers `GeometryWorkerProcess::installManagerBackend()` at GUI startup; process accepts
    manager workspaces and retains them until `releaseJobArtifacts()`; maps `ReadyToCommit` →
    `Completed` for observers until commit fencing lands.
  - App tests cover launch+start-failure, launch+cancel hook, and no-backend still-queued behavior.
* Files changed:
  - `src/App/GeometryJobManager.{h,cpp}`
  - `src/Gui/GeometryWorkerProcess.{h,cpp}`
  - `src/Gui/Application.cpp`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step15` / `step15b`, `/code/build_docker`):
  - `ninja FreeCADApp FreeCADGui App_tests_run Part_tests_run` → success
  - Focused App filter (twice):
    `App_tests_run --gtest_filter=AsyncRecomputeTest.*:GeometryJobTest.*:GuiResponsivenessProbeTest.*`
    → **26/26 PASSED** (includes FreeCADCmd launch/start-failure/cancel/no-backend tests)
  - Focused Part filter (twice) → **20/20 PASSED**
* Current issues or blockers:
  - No installed-tree FreeCADCmd smoke yet; registry/codecs/protocol validation still absent.
  - Document/object invalidation does not cancel running FreeCADCmd processes.
  - `waitForStarted` can briefly block the GUI thread; close may still wait on workers.
  - Commit fences / production adapters / archive deep-copy remain open.
* Next step:
  - See revised `## Next step` above (invalidate/close must cancel FreeCADCmd processes).

### Step 16 — Invalidate/close cancels FreeCADCmd processes (non-blocking)

* Completed work:
  - `invalidateDocument` / `invalidateObject` invoke the registered process cancel hook for running
    FreeCADCmd jobs, still marking `DocumentClosed`/`Stale` immediately without waiting on QProcess.
  - Added focused tests for document-close and object-remove cancel-hook delivery.
* Files changed:
  - `src/App/GeometryJobManager.cpp`
  - `tests/src/App/TestGeometryJob.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step16`, `/code/build_docker`):
  - `ninja FreeCADApp App_tests_run` → success
  - Focused App filter (twice) → **28/28 PASSED**
  - Focused Part filter (twice) → **20/20 PASSED**
* Current issues or blockers:
  - Legacy live-document async close still waits.
  - Commit fences / installed FreeCADCmd protocol / production adapters remain.
* Next step:
  - See revised `## Next step` above (non-waiting document close).

### Step 17 — Non-blocking legacy closeDocument (deferred destroy)

* Completed work:
  - Removed the `_recomputeDocumentsInProgress` wait from `cancelRecomputeRequestsForDocument`.
  - `closeDocument` defers Document destruction into `_recomputePendingDestroy` when a legacy
    worker is still in-flight; the worker destroys the Document after finishing.
  - `FeatureTestAsyncBlocker` releases the GIL while waiting so close signals/Python teardown
    cannot deadlock against `Document::recompute`'s full-call GIL lock.
  - Replaced `CloseDocumentWaitsForInFlightAsyncRecompute` with
    `CloseDocumentDoesNotWaitForInFlightAsyncRecompute`.
* Files changed:
  - `src/App/Application.{h,cpp}`
  - `src/App/FeatureTest.cpp`
  - `tests/src/App/AsyncRecompute.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step17b`, `/code/build_docker`):
  - `ninja FreeCADApp App_tests_run` → success
  - `AsyncRecomputeTest.*` → **3/3 PASSED**
  - Focused App filter (twice) → **28/28 PASSED**
  - Focused Part filter (twice) → **20/20 PASSED**
* Current issues or blockers:
  - Commit fences / failure-aware generation / GUI slice budgets still missing.
  - Full-call GIL in `Document::recompute` remains a residual stall risk for non-cooperative work.
  - Installed FreeCADCmd protocol, production adapters, archive deep-copy remain open.
* Next step:
  - See revised `## Next step` above (coordinator commit fences).

### Step 18 — Commit fences, transactions, failure-aware generation

* Completed work:
  - Added `CommitFence` / `commitFenceMatches` / `makeGeometryInputFingerprint`.
  - Coordinator validates fences before commit; wraps commit in a document transaction; advances
    generation only via `GeometryCommitScope::markSucceeded()`.
  - Honors `maxGuiSlice` for planning/snapshot bursts (defers remaining targets).
  - Added `FeatureTestDetached` adapter + AsyncRecompute fence/fail/success generation tests.
* Files changed:
  - `src/App/GeometryJob.{h,cpp}`, `DocumentRecomputeCoordinator.{h,cpp}`, `FeatureTest.{h,cpp}`,
    `Application.cpp`, `tests/src/App/AsyncRecompute.cpp`, `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step18` / `step19b`):
  - Focused App filter (twice) → **31/31 PASSED**
* Current issues or blockers:
  - 30 s heartbeat acceptance and full non-cooperative shutdown still open.
* Next step:
  - Continue Priority 2/3 residuals (see `## Next step`).

### Step 19 — Archive deep-copy/repeat-write, builtin registry, geometry progress show

* Completed work:
  - `createBundle` uses `makeElementCopy` for private OCC geometry.
  - `writeArchive` snapshots/restores hasher marks and map archive IDs (repeat-write byte-identical).
  - `GeometryWorkerRegistry::registerBuiltins()` for Boolean/Fillet/Sweep; called from Part module init.
  - `ProgressBar::showGeometryProgress()` bypasses 2 s sequencer delay for geometry jobs.
* Files changed:
  - `src/Mod/Part/App/TopoShapeArchive.cpp`, `GeometryWorkerRegistry.{h,cpp}`, `AppPart.cpp`
  - `src/Gui/ProgressBar.{h,cpp}`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step19b`, `/code/build_docker`):
  - Focused App filter (twice) → **31/31 PASSED**
  - Focused Part filter (twice) → **22/22 PASSED** (includes RepeatWrite + BuiltinWorkerRegistry)
* Current issues or blockers:
  - See revised `## Next step` (hasher revision on commit; FreeCADCmd protocol; P4 blocked).
* Next step:
  - See revised `## Next step` above.

### Step 20 — Hasher revision on commit, trusted result path, progress hooks

* Completed work:
  - `mergeExactClosure` compares against the live hasher revision when `expectedRevision == 0`
    (still allows bootstrap onto a fresh revision-0 hasher).
  - `GeometryCommitScope` advances the document hasher revision only on successful commit.
  - `TopoShapeArchive::commitHasherDelta` merges then advances; stale re-apply is rejected.
  - FreeCADCmd result messages require a workspace-relative path + size + digest (+ optional
    jobId); absolute/`..` paths and size/digest mismatches fail closed.
  - Worker idle/success paths publish under `result.fcg` with real size/sha256.
  - Unix workers are placed in their own process group; cancel/timeout signal the group.
  - `GeometryJobManager` progress/state listeners; `GeometryProgressController::installManagerHooks`
    from Gui startup; `isTrustedRelativeResultPath` helper.
* Files changed:
  - `src/App/StringHasher.cpp`, `GeometryJob.{h,cpp}`, `GeometryJobManager.{h,cpp}`
  - `src/Mod/Part/App/TopoShapeArchive.{h,cpp}`, `GeometryWorker.cpp`
  - `src/Gui/GeometryWorkerProcess.{h,cpp}`, `GeometryProgressController.{h,cpp}`, `Application.cpp`
  - `tests/src/App/AsyncRecompute.cpp`, `TestGeometryJob.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`, `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step20b`, `/code/build_docker`):
  - Focused App filter → **33/33 PASSED**
  - Focused Part filter → **23/23 PASSED** (includes `StaleHasherRevisionRejectedAfterAdvance`)
* Current issues or blockers:
  - Nested child maps, FreeCADCmd codecs/installed smoke, Windows Job Object, 250 ms GUI harness,
    and P4 adapters remain open.
* Next step:
  - See revised `## Next step` above.

### Step 21 — Nested/shared child maps + long hashed names (FCG1 v4)

* Completed work:
  - Part tests prove nested/shared child `ElementMap` identity survives createBundle/write/read.
  - Long hashed `StringID`s round-trip via `#id` mapped names + SID refs; FCG1 bumped to **v4** to
    persist hasher threshold so `Option::Hashable` lookups resolve after restore.
  - `StringHasherClosure` carries `threshold`; `captureClosure` / `mergeExactClosure` apply it.
* Files changed:
  - `src/App/StringHasher.{h,cpp}`, `src/Mod/Part/App/TopoShapeArchive.cpp`
  - `tests/src/Mod/Part/App/TestNonBlockingGeometry.cpp`, `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-step21`, `/code/build_docker`):
  - Focused Part filter → **25/25 PASSED**
  - Focused App filter → **33/33 PASSED**
* Current issues or blockers:
  - Cross-process FreeCADCmd mapped ops, FCStd reopen, codecs/Job Object/janitor/smoke, and P4
    adapters remain open.
* Next step:
  - See revised `## Next step` above.

### Step 22 — Lossless cross-process mapped Part::Boolean (FreeCADCmd)

* Completed work:
  - Hardened `materializeExactClosure` (fresh bootstrap) vs `mergeExactClosure` (canonical):
    validate-then-commit; threshold must match on canonical merge; preflight ID + `(data,postfix)`
    uniqueness; rollback inserts/threshold/revision/high-water on unexpected insert failure.
  - `App::GeometryRequestWorkspace` stages archives via `.tmp` rename and publishes `request.json`
    last; `GeometryWorkerProcess` calls `task->writeArchive` before publish.
  - Typed Boolean codec v1 (`writeArchive` / `decodeFromRequest`) with size/SHA-256, trusted
    relative paths, codec/operation/BooleanType checks, and shared-hasher operand rebind.
  - Worker emits control lines via `QJsonDocument`; parent requires hello-before-progress/result,
    single terminal, trusted path/size/digest; `readArchive` is output-atomic.
  - `GeometryWorker.py` invokes `main()` under FreeCAD `runFile` (`__name__` ≠ `__main__`) and
    resolves request path via `--pass`.
  - Real FreeCADCmd tests: mapped Boolean round-trip, FCStd second-process reopen, recovery after
    bad request, irreconcilable hasher threshold collision before OCC. No in-process `task.run()`
    fallback for cross-process qualification.
* Files changed (representative):
  - `src/App/StringHasher.{h,cpp}`, `GeometryRequestWorkspace.{h,cpp}`, `CMakeLists.txt`
  - `src/Mod/Part/App/{BooleanGeometryOperation,TopoShapeArchive,GeometryWorker,GeometryWorkerRegistry}.*`
  - `src/Mod/Part/GeometryWorker.py`
  - `src/Gui/GeometryWorkerProcess.{h,cpp}`
  - `tests/src/Mod/Part/App/{TestNonBlockingGeometry,TestCrossProcessBoolean}.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `freecad-nbgeom-bool-final`, `/code/build_docker`):
  - `ninja FreeCADApp Part FreeCADCmd PartScripts App_tests_run Part_tests_run FreeCADGui`
  - App filter `GeometryJobTest.*:GuiResponsivenessProbeTest.*:AsyncRecomputeTest.*` → **33/33 PASSED**
  - Part filter `NonBlockingGeometryTest.*:CrossProcessBooleanTest.*` → **37/37 PASSED**
    (34 NonBlocking + 3 CrossProcess; includes real FreeCADCmd + FCStd reopen)
* Remaining gaps (do not mark Phase 2 complete):
  - **P1:** 30 s heartbeat acceptance; non-cooperative shutdown recovery; GIL hold.
  - **P2:** Fillet/Sweep typed codecs + FreeCADCmd qualification; broader crash/rename recovery;
    install-tree smoke beyond build_docker.
  - **P3:** Windows Job Object; retained-success janitor; 250 ms GUI progress harness.
  - **P4:** production Part/PartDesign adapters / rollout (blocked).
* Next step:
  - See revised `## Next step` above.

### Step 23 — Decode-before-ReadyToCommit + parent GeometryWorkerProcess qualification

* Completed work:
  - Task-aware result decode: `DetachedGeometryTask::decodeResultArchive`; Boolean uses bounded
    `TopoShapeArchive` structural decode; parent transitions `Decoding` → `ReadyToCommit` only
    after success; digest-correct malformed FCG never reaches ReadyToCommit/Completed.
  - Fail-closed `GeometryWorkerProcess` FCGEO/1: single hello first with required protocol/version;
    strict JSON types for progress/result/error/jobId; protocol-failed latch (no recovery from a
    later valid-looking result); completion uses explicit protocol state instead of error-code
    string exclusion lists.
  - Explicit `GeometryArchiveWriteResult` from `writeArchive`; staging failure blocks
    `request.json`, launches no child, returns `RequestSerializeFailed`; no Boolean-specific
    file-existence inference.
  - NUL-safe hasher value-key identity for closure preflight; `rebindBundleToHasher` snapshot/
    restore of SID marks and map archive IDs (source unchanged on success and failure).
  - Hashed SID FCStd persistence: `saveStream` base64-encodes hashed/binary payloads (restore
    already expected base64); fixes digest corruption that broke Hashable plaintext re-resolve.
  - Python `StringHasher.getID(txt, base64=False, hashable=False)` exposes Option::Hashable.
  - Gui integration tests exercise real `GeometryWorkerProcess` + FreeCADCmd; CrossProcess FCStd
    reopen asserts threshold, Hashable SID, map reachability, SID resolution, and source-tag
    history (`:H1:`/`:H2:` / recursive `getElementHistory`).
* Files changed (representative):
  - `src/App/{GeometryJob.h,GeometryRequestWorkspace.*,StringHasher.*,StringHasherPyImp.cpp}`
  - `src/Gui/GeometryWorkerProcess.{h,cpp}`
  - `src/Mod/Part/App/{BooleanGeometryOperation.*,TopoShapeArchive.cpp}`
  - `tests/src/Gui/TestGeometryWorkerProcess.cpp`, `tests/src/Gui/CMakeLists.txt`
  - `tests/src/Mod/Part/App/{TestCrossProcessBoolean,TestNonBlockingGeometry}.cpp`
  - `tests/src/App/StringHasher.cpp`
  - `doc/PROGRESS.md`
* Tests or validation performed (Docker `/code/build_docker`):
  - `ninja FreeCADApp Part FreeCADCmd PartScripts FreeCADGui App_tests_run Part_tests_run Gui_tests_run`
  - App `GeometryJobTest.*:GuiResponsivenessProbeTest.*:AsyncRecomputeTest.*` → **33/33 PASSED**
  - App `StringHasherTest.SaveDocFile*` → **2/2 PASSED**
  - Part `NonBlockingGeometryTest.*` → **37/37 PASSED**
  - Part `CrossProcessBooleanTest.*` → **3/3 PASSED** (child QProcess + FCStd semantic reopen)
  - Gui `GeometryWorkerProcessTest.*` → **7/7 PASSED** (protocol fail-closed, malformed-FCG
    reject before ReadyToCommit, **parent-controlled real FreeCADCmd** → ReadyToCommit)
* Remaining gaps (do not mark Phase 2 complete):
  - **P1:** 30 s heartbeat; non-cooperative shutdown; GIL hold.
  - **P2:** Fillet/Sweep codecs; broader crash/rename; install-tree smoke.
  - **P3:** Windows Job Object; janitor; 250 ms GUI harness.
  - **P4:** production adapters/rollout (blocked).
* Next step:
  - See revised `## Next step` above.

### Step 24 — Boolean/FCStd/protocol closeout (wire + side-effect safety)

* Completed work:
  - Corrected Python `StringHasher.getID` base64/binary/hashable contract: `base64=True` always
    sets `Option::Binary`; `hashable` is never implied; malformed base64 raises `ValueError`.
  - FCGEO/1 `jobId` is a canonical decimal JSON string covering the full `uint64_t` domain
    (max `18446744073709551615`); parent and worker parse/validate strictly. Optional
    `executionTime` must be finite, non-negative, and ≤ 86400 when present. Error terminals
    require non-empty code/message and matching `jobId`.
  - `GeometryRequestWorkspace` clears stale `request.json` on construct/failure; `writeBytes`
    accepts null+size0, rejects null+size>0 and oversized sections. Manager-path serialize
    failure surfaces `RequestSerializeFailed` (monotonic terminal; no child launch). Callback
    coverage in `ManagerPathRequestSerializeFailedNoLaunch` is **late registration only** (after
    the job is already terminal), not early registration while launch is blocked.
  - Rebind success/failure tests assert source marks, archive IDs, hasher metadata, and shared
    child pointer identity (`sharedRefs >= 2`). FCStd same-process reopen asserts hasher
    ownership, recursive map/SID closure, hashed SID map reference, and history tags.
  - `TopoShapeArchive` range-checks int64→long and rejects `highWaterId > LONG_MAX` without
    mutating the caller's bundle; `writeArchive` captures marked closure from the live hasher
    after rebind; `ElementMap::getChildElements` skips null child-map slots.
  - Nested/shared rebind + repeat-write side-effect checks live in
    `NestedAndSharedChildMapsSurviveArchiveRoundTrip`.

### Step 24 correction — bad-path closeout (2026-07-26)

* Root-cause fixes (verified in code):
  1. **JSON `size` parsing:** `requireJsonInt64` rejects with an exclusive 2^63 bound and
     enforces the trusted 512 MiB maximum before any `qint64` cast; protocol failures latch
     (`ProtocolResultSizeInt64OverflowFailsClosed`, `ProtocolResultSizeNullWrongJsonType`,
     `ProtocolResultSizeHugeExponentFailsClosed`).
  2. **Stale workspace cleanup:** `clearPublishedRequest` / `clearStagedFile` / `writeFileAtomic`
     fail closed when an existing path cannot be removed or replaced; Boolean no longer issues
     unchecked `QFile::remove` for operands/request; `markFailed` uses best-effort request
     removal only (no recursive failure overwrite).
  3. **Staging bounds:** `stageFileAtomic` checks source size before streaming (chunked copy,
     no `readAll()` up to 512 MiB).
  4. **Live closure capture:** removed stale `hasherSnapshot` fallback on capture failure;
     `setTestForceClosureCaptureFailure` restores marks/IDs and leaves no published `.tmp`; the
     fault seam is declaration/definition guarded and compiled only with
     `ENABLE_DEVELOPER_TESTS`.
  5. **`ElementMap` null slots:** `getAll`/`find`/`hasChildElementMap` null-safe; orphan
     `childElements` entries pruned after `addChildElements`.
  6. **Cross-process recovery test:** bad Boolean request carries jobId `42`; worker returns
     `MissingOperandArchive` with matching terminal `jobId` (not `missing_job_id`).
  7. **Early worker errors:** parent sets `FCGEO_LAUNCHED_JOB_ID` for child processes so
     pre-parse failures still emit a matching `jobId` when launched from `GeometryWorkerProcess`
     (`CrossProcessBooleanTest.EarlyWorkerErrorIncludesLaunchedJobId`,
     `PythonMissingArgHonorsLaunchedJobId`). The missing-argument test invokes real FreeCADCmd
     without a request; the missing-binding test runs an isolated copy of the Python wrapper with
     a stub `Part` module. Former probe filenames are ordinary request paths, covered by
     `ProbeRequestFilenamesAreOrdinaryPaths`. Request `jobId` must match the launched ID before
     workspace/OCC work (`job_id_mismatch`).
  8. **LP64 honesty:** single `TopoShapeArchive::int64ToLongChecked` implementation used by FCG1
     decode and unit tests; FCG1 decode narrowing integration tests **skip** on LP64
     (Docker x86_64 `127.0.0.1:5001/freecad-ci-deps:24.04`); `highWaterId > LONG_MAX` rejection
     still runs on LP64.
  9. **Workspace ownership:** documented idempotent manager/worker cleanup; serialize failures
     keep `RequestSerializeFailed` (manager does not overwrite with `ProcessStartFailed`).

* Tests or validation performed (Docker `127.0.0.1:5001/freecad-ci-deps:24.04`, LP64 `/code/build_docker`,
  GCC 13.3, Qt 6.11.1, Debug):
  - Incremental Ninja build of `FreeCADApp Part FreeCADCmd PartScripts FreeCADGui App_tests_run
    Part_tests_run Gui_tests_run` against the CMake-qualified `/code/build_docker` graph → success
  - App `GeometryJobTest.*:GuiResponsivenessProbeTest.*:AsyncRecomputeTest.*` → **35/35 PASSED**
  - App `StringHasherTest.SaveDocFile*:StringHasherTest.*Python*` → **8/8 PASSED**
  - App `GeometryRequestWorkspaceTest.*` → **5/5 PASSED**
  - Part `NonBlockingGeometryTest.*:CrossProcessBooleanTest.*:TopoShapeArchiveInt64ToLongTest.*`
    → **51 PASSED**, **4 SKIPPED** (LP64 narrowing decode; not falsely passed)
  - Gui `GeometryWorkerProcessTest.*` → **25/25 PASSED**
  - Part `NestedAndSharedChildMapsSurviveArchiveRoundTrip` ×20 in one deterministic
    `QT_HASH_SEED=0` process plus five fresh default-randomized processes → **120/120 PASSED**
  - Part `CrossProcessBooleanTest.RecoveryAfterBadRequestThenValidBoolean` → **PASSED**
    (`MissingOperandArchive`, hello-first, single error terminal, no `result.fcg`, then good path)
  - Part `CrossProcessBooleanTest.EarlyWorkerErrorIncludesLaunchedJobId` → **PASSED**
    (`request_file_not_found` with matching `jobId` from `FCGEO_LAUNCHED_JOB_ID`)
  - UBSan float-cast-overflow demo: **not run** (`FREECAD_USE_SANITIZER_UBSAN=OFF` in this tree)
  - `git diff --ignore-cr-at-eol --check` on Step 24 correction sources → clean (no conflict markers)
  - `tools/mcp/freecad-mcp` → untouched; **no git commit** on this closeout

* Remaining gaps (Phase 2 still **not** complete):
  - **P1:** 30 s heartbeat; non-cooperative shutdown; GIL hold.
  - **P2:** Fillet/Sweep codecs; broader crash/rename; install-tree smoke; LLP64/32-bit-long
    decode qualification on a non-LP64 builder.
  - **P3:** Windows Job Object; janitor; 250 ms GUI harness.
  - **P4:** production adapters/rollout (blocked).
* Next step:
  - Continue Phase 2 bad-path hardening / Fillet-Sweep codecs per `doc/PLAN.md` (not started here).

---
