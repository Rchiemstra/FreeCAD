# Change-Aware Saving and Autonomous MCP Editing — Work-in-Progress Handoff

- **Status:** Committed work in progress on a feature branch; **not release-ready**
- **Last updated:** 2026-08-15
- **Parent branch:** `fix/change-aware-save-mcp-autonomy`
- **Nested MCP branch:** `fix/change-aware-save-mcp-autonomy`

### Current published WIP heads

These are the authoritative heads. Do not amend, rebase, or force-push them; land further
work as follow-up commits.

| repository | commit | subject |
| --- | --- | --- |
| parent | `0343b0a49a94f3bdac24de7814f5db90433d6aba` | `fix(gui): make document saving change-aware and observable` |
| nested MCP | `0bd67ad531164baac81302e49ee042dc077b1c4a` | `fix(mcp): make editing and save finalization transaction-safe` |

The parent gitlink for `tools/mcp/freecad-mcp` was advanced from `f5c40fe6` to `0bd67ad5` in
the parent commit above.

**Neither commit has been pushed.** The parent branch has no configured upstream, so both
heads are local-only. Deliberately excluded from staging and still untracked: parent
`results/`, parent `tests/lib/`, and the nested `results_luna_*` logs.

### Baseline for comparison

| reference | commit |
| --- | --- |
| parent base (branch point) | `0b90c1533719486a390a4c7afcf40aa7226f316a` |
| nested base | `f5c40fe679567a63ad16e48ff95e5dce75197fb0` |

## Original pause state (historical — superseded)

> **Historical.** Everything in this section described the state at the original pause, before
> the work was committed. Its claims that no commit exists and that the trees are unstaged were
> true then and are **no longer true**; see "Current published WIP heads" above. The text is
> preserved unchanged as the historical record.

All implementation and review sub-agents were stopped. No commit or push has been made.
The working trees are intentionally left intact and unstaged. The user's running FreeCAD
session and the dirty `HamaAdapter_v3` document were never restarted, closed, saved, or
modified by this work.

Important: the current source tree is a development checkpoint, not a releasable state.
The last authoritative branch-built MCP E2E gate is red, and the newly added MCP
postcondition plumbing does not yet have its matching native C++/Python binding.

The latest read-only checks at the pause point were:

- Parent `git diff --check`: passed, with line-ending warnings only.
- Nested `git diff --check`: passed, with line-ending warnings only.
- All sub-agents: interrupted or completed; none remain active.
- Indexes: unstaged; no partial commit exists.

## Original objective

Deliver one combined release that:

1. Fixes the false “MCP write lane blocked” diagnosis and makes pad, pocket, undo, redo,
   repair, generated mutations, and recovery transaction-safe.
2. Makes `App::Document` the sole authority for persistent file-change state.
3. Makes ordinary saves change-aware and exposes structured save outcomes.
4. Adds per-document mutation readiness, local pause, quarantine, and observable agent state.
5. Keeps camera-only and cache-only activity out of dirty state and autosave.
6. Adds a user-facing Document Changes panel, status indicator, clearer tabs, and safe close
   prompts.
7. Preserves external-file conflict, atomic replacement, backup, and no-clobber policy.
8. Verifies the result through unit, native, GUI, branch-built MCP, authenticated session,
   integration, and E2E gates before committing and pushing.

## Root cause that started the work

The reported problem was not a globally blocked MCP write lane.

The public pad and pocket tools generated Python that called:

- `openTransaction()`
- `commitTransaction()`
- `abortTransaction()`

The production `execute_code` path already ran inside
`Document.commitCompatibilityMutation()`, whose native coordinator owned the one valid
transaction and deliberately rejected nested transaction/history control. A valid pad or
pocket therefore failed deterministically with:

> transaction, undo, and redo control is unavailable during a prepared commit

Evidence showed that unrelated mutations and new-document creation still succeeded after
the first failure. Restarting FreeCAD was neither necessary nor curative and would have put
the dirty user document at risk.

## Work completed so far

### 1. MCP transaction ownership

- Removed public transaction control from pad and pocket templates.
- Routed public pad and pocket tools through typed RPC handlers.
- Routed undo and redo through their existing typed RPC methods rather than generated
  `execute_code`.
- Retired `run_transaction` on every path with a stable retirement result.
- Added a recursive generated-payload policy test forbidding transaction/history control.
- Added structured native rejection, rollback evidence, retryability, and document-local
  quarantine handling.
- Made native collaboration support fail closed when the required runtime API is absent.
- Added readiness admission and postflight handling to mutating `execute_code`.
- Added deferred-recompute native policy plumbing for `repair_references` while preserving
  pre-existing pending recompute work.

### 2. Mutation readiness and autonomy controls

- Added native per-document readiness fields for pending/booked/locked transactions,
  recompute, `mustExecute`, commit barrier, notification replay, poison, and quarantine.
- Added read-only `get_mutation_readiness(doc_name=None)` across the MCP capability,
  generated client, RPC, and addon layers.
- Kept `check_rpc_sync` explicitly transport-only.
- Added document-local quarantine after proven rollback failure.
- Made ordinary semantic/preflight failures retryable when rollback leaves the document
  healthy.
- Added local-only Pause/Resume agent-write control. Reads and local GUI edits remain
  available; remote code cannot unpause itself.
- Added current/last MCP operation reporting and a GUI-thread-safe bridge into the panel.
- Added attention rules so normal transient recompute/transactions do not unnecessarily
  pop open the panel.

### 3. App-owned file-change state

- Added base file states `NotSaved`, `Clean`, and `Modified`.
- Added pending `Model` and `Appearance` categories.
- Added a separate canonical-save-failure overlay.
- Added App-owned state and save-outcome signals.
- Added undo-aware transactional and sticky file-change token channels.
- Added canonical savepoint path binding so directly changing `FileName` cannot produce a
  false `Unchanged` result.
- Preserved sticky/non-transactional changes across abort, undo, redo, and collaboration
  rollback.
- Added dynamic property schema and serialized status tracking for Document,
  DocumentObject, and ViewProvider paths.
- Excluded `NoModify`, transient value changes, nonpersistent data, restoration internals,
  and runtime guard bits where appropriate.
- Kept a blank new document `NotSaved` without treating it as having pending changes.

### 4. Structured save outcomes

- Added `DocumentSaveDisposition` values:
  - `Written`
  - `Unchanged`
  - `CopyWritten`
  - `Failed`
- Added structured outcome fields for intent, canonical path, target path, file-written
  status, resulting state/categories, error code, and message.
- Added native/Python methods:
  - `saveWithOutcome()`
  - `forceSave()`
  - `saveAsWithOutcome()`
  - `saveCopyWithOutcome()`
  - `hasPendingFileChanges()`
  - `getFileChangeState()`
- Preserved the legacy entry points and their basic compatibility behavior.
- Added clean canonical `Unchanged` short-circuiting before metadata, signals, backups,
  thumbnail, camera, or serialization work.
- Made missing canonical files force a write.
- Kept Save Copy from moving the canonical savepoint.
- Added generation capture so mutations occurring after serialization remain dirty.
- Added staged Save As identity rollback for filename, label, UUID, transient directory, and
  metadata.
- Made save-outcome and file-state delivery resilient against throwing observers.
- Added MCP result fields such as `save_disposition`, `file_written`, `unchanged`,
  `canonical_path`, `target_path`, `resulting_clean`, and stable archive evidence.
- Added SHA-256 plus stable stat/ZIP/`Document.xml` evidence before finalize/release.

### 5. GUI and Review Notes

- Added a clickable document-state status control.
- Added the dockable **Document Changes** panel.
- Added active path, Model/Appearance categories, camera-only note, readiness, current/last
  agent operation, and up to 100 session-local history entries.
- Added explicit tab suffixes and canonical-path tooltips.
- Updated the initial close prompt to `Save Changes`, `Close Without Saving`, and `Cancel`,
  including pending categories and a note that camera navigation is excluded.
- Made App file state authoritative for GUI modified state.
- Moved autosave onto persistent file-change signals instead of broad touch/view activity.
- Made active-view camera selection deterministic for real writes.
- Added camera change sensing without dirtying the document.
- Marked Review Note camera-derived `LeaderEnd` and `LeaderHalfExtent` cache properties as
  nonpersistent/`NoModify` and restored their invariant after legacy restore.
- Excluded those properties from shared-presentation revisions.
- Fixed a pre-existing NotificationArea/MainWindow shutdown null dereference and added a
  dedicated process-level regression.
- At the final work step before pause, save-failure close dialogs were changed so `Cancel`
  is the default/escape action and the destructive action reads `Close Without Saving`.
  New behavioral GUI tests were added, but they have not yet been built or run.

### 6. Session E2E process safety

- Reworked the isolated session launcher so test cleanup owns the child process from
  creation rather than reconstructing authority from a PID later.
- Windows uses suspended `CreateProcessW`, an owned Job Object, restricted inherited
  handles, and resume only after job assignment.
- POSIX uses a private session/process group and creation-owned group teardown.
- STOP/EOF, signals, readiness failure, pre-yield failure, manifest mismatch, and auth
  mismatch all clean up through the retained owner.
- Profile deletion remains gated by authenticated identity evidence.
- Independent review found no remaining release-significant process-identity issue in this
  scope.

### 7. Latest unverified edits at the pause point

These edits are present but are newer than the last green full gate:

- Nested pad/pocket was split into apply-time creation and post-recompute validation.
- Nested collaboration plumbing now passes optional `postcondition=`.
- Native support for that keyword is **not implemented yet**, so the current combined tree
  fails closed before a typed pad/pocket callback runs.
- Save Copy now performs a static canonical-alias check and `PropertyXLink` ignores Copy and
  Recovery save notifications. The new App tests have not run.
- Screenshot capture after a committed typed pad/pocket is now best-effort and returns a
  `presentation_warning` instead of reclassifying the committed mutation as failed. Its new
  unit test has not run.
- The two save-failure close-dialog safety changes and tests have not run.

## Test evidence obtained before the latest edits

### Parent native/GUI evidence

- Full branch build: `1116/1116`, exit 0.
- Targeted App collaboration/save suite: `95/95` passed.
- Targeted GUI collaboration/recovery suite: `52/52` passed.
- NotificationArea shutdown regression: `1/1` passed.
- Exact Review Note GUI/camera test: `1/1` passed with clean shutdown.
- Parent diff check: clean apart from line-ending notices.

These results predate the newest Save Copy, postcondition, close-dialog, and screenshot
changes and must not be treated as final evidence.

### Nested MCP non-live evidence

On the last fully reconciled nested tree before the newest postcondition/screenshot edits:

- Architecture/generator/manifest/contract suite: `289/289` passed.
- Full unit selection: `2358 passed`, `1 platform skip`, `1 documented xfail`,
  `129 deselected`, no failures/errors.
- Compileall: passed.
- Ruff: passed.
- Nested diff check: passed apart from line-ending notices.

The postcondition work later ran a focused related suite with `151 passed, 2 skipped`, but
it cannot be considered integrated until the native binding exists.

### Branch-built MCP gates

- Load preflight: passed.
- Core marker: `8 passed`, `5 expected xfail`.
- First E2E run after initial fixes: `104 passed`, `9 failed`.
- After fixing App-vs-GUI file-state authority and execute-code readiness, the fresh
  authoritative rerun produced:
  - `113 collected`
  - `102 passed`
  - `11 failed`
  - `0 errors/skips`
  - FreeCADCmd process exit 0, strict JUnit verdict/Docker gate exit 1
  - runtime `633.692s`

Artifacts are under:

`results/mcp-branch-gates-rerun/`

The recorded source fingerprint for that run is:

- Parent: `0b90c1533719486a390a4c7afcf40aa7226f316a`
- Nested: `f5c40fe679567a63ad16e48ff95e5dce75197fb0`
- Diff hash: `95b0ca32cafa04fcb3698daf6992727560ce8b87`

## Open release blockers

### A. Native structural mutation boundary

This is the immediate E2E blocker.

Current native behavior opens the structural grant only around `operation.apply()`, closes
it, and then performs the authoritative recompute. Real FreeCAD features lazily create
schema during recompute, so the coordinator rejects legitimate work after the grant has
closed.

Observed failures include:

- Datum/Pad lazy schema: `DynamicPropertyOnNewObject` during coordinator recompute.
- Sketch attachment: `PositionBySupport` changes on an existing sketch.
- Assembly grounding: `Placement`/`LinkPlacement` editor-status changes on an existing
  component.
- Recursive removal: teardown changes schema on an existing sketch, followed by a rollback
  that cannot be proven stable.

Required design:

1. Preserve all existing exported ABI signatures and public struct layouts.
2. Add a distinct options/policy API rather than changing an existing by-value ABI.
3. Keep the full structural grant around apply only.
4. Add a narrow coordinator-owned recompute grant that permits lazy schema only on objects
   created by this transaction; it must not permit arbitrary object creation during execute.
5. Add an explicit trusted structural policy for the narrowly required existing-object
   editor-status/schema transitions used by typed/generated FreeCAD operations.
6. Keep default structural callbacks rejecting borrowed arbitrary existing-object schema.
7. Ensure every newly allowed status/schema change is transactionally recorded/restored or
   otherwise included in exact rollback proof.
8. Add tests for commit and rollback of each allowed class, plus retained rejection tests.

### B. Native post-recompute postcondition

The nested API now expects:

```python
Document.commitCompatibilityMutation(
    callback,
    structural=False,
    recompute=True,
    postcondition=None,
    trusted_structural=False,
)
```

The native binding currently accepts only `callback`, `structural`, and `recompute`.

Required behavior:

- Validate the optional zero-argument callable before mutation starts.
- Call it exactly once after eager native recompute, before publication/commit.
- For deferred recompute, call it immediately after apply.
- `True` continues.
- `False` returns `PostconditionFailed` and rolls back.
- A Python exception rolls back first, then restores the original Python exception.
- Old calls that omit the new keywords retain their behavior.
- Old native runtimes reject the new keyword before invoking apply, so MCP fails closed.

### C. Save atomicity and replacement races

The save redesign is not yet release-safe in all filesystem cases.

Open issues:

1. When the legacy `BackupPolicy` preference is false, current code writes directly to the
   canonical file instead of always using same-filesystem temp plus atomic replacement.
2. Save Copy's new alias check is only a preflight check. A destination can be swapped to a
   symlink/hardlink/reparse alias before replacement.
3. Save As no-clobber is checked before serialization but not atomically at replacement, so
   a destination created or changed during the write can be overwritten.
4. Timestamp backup cleanup can throw after the new file is already durable, yet current
   outcome handling may report `Failed` and roll back Save As identity.
5. MCP Save As currently rejects `expected_destination_sha256` instead of preserving the
   existing external-file conflict/CAS contract.
6. A proper replacement primitive must support:
   - always-temporary serialization,
   - overwrite or no-replace mode,
   - expected destination identity/hash when supplied,
   - final canonical-alias validation at the replacement boundary,
   - explicit distinction between replacement success and backup-maintenance warnings.

Required tests include mid-serialization failure with backups disabled, destination creation
or change during Save As, alias swap during Save Copy, matching/mismatching expected hash,
and durable write followed by backup-history cleanup warning.

### D. Failed Save As identity notification

Save As staging currently publishes the attempted filename/label to ordinary observers, then
suppresses corrective notifications during rollback. A consumer can cache the failed
destination even though native identity is restored.

Required behavior:

- Do not expose staged identity through ordinary public notifications.
- On success, publish adopted identity exactly once through an authoritative path.
- On failure, no attempted identity escapes.
- Add a signal-backed cache regression using the same basename in a different directory.

### E. Readiness robustness

Two plan gaps remain:

1. `getMutationReadiness()` throwing, returning a non-mapping, or returning malformed data
   currently falls back to legacy attributes and can report `ready=True`. An advertised but
   unreadable native getter must fail closed as incompatible.
2. The promised event-driven same-request wait is currently only a synchronous recompute plus
   immediate resample. Recompute/replay that clears asynchronously is rejected instead of
   awaiting a bounded stable transition tied to the request cancellation/deadline.

### F. Remaining UX and operation truth

- Build/run the new single- and multi-document failed-save close-dialog tests.
- Build/run the new screenshot-failure-after-commit test.
- Add old template result parity fields for attachment support/map mode if public consumers
  require literal parity.

### G. E2E failure classification

Of the 11 failures in the last authoritative E2E run:

- Seven exposed the real native structural/postcondition problems described above.
- One worker snapshot failure exposed the real Save Copy/`PropertyXLink` dirtying bug; a
  static fix is present but untested.
- Three RPC/worker timing failures occurred under heavy concurrent test/probe load. Worker
  startup rose from about 6–7 seconds to about 14–20 seconds. Rerun these alone after the
  source defects are fixed and with no concurrent probes before changing timeout policy.

## Safe resume plan

### Phase 0 — preserve and reconcile the checkpoint

1. Confirm no implementation agent/process is still active.
2. Record parent and nested status/diff hashes.
3. Do not run formatters or bulk generators until the partial native/nested interface is
   reconciled.
4. Do not touch or restart the user's live FreeCAD process.
5. Continue using isolated FreeCADCmd/GUI processes and fresh profiles only.

### Phase 1 — finish the native structural/postcondition boundary

1. Implement ABI-safe native options/policy entry points.
2. Add Python `postcondition` and `trusted_structural` keywords.
3. Implement the narrow recompute grant and trusted existing-object status/schema policy.
4. Make rollback exact for the newly allowed mutations.
5. Update nested calls so only internal typed/generated structural operations request trusted
   scope.
6. Remove the stale in-callback recompute workaround from generated execute-code once the
   native coordinator owns the complete recompute phase.
7. Run targeted native postcondition/structural tests and the focused nested suites.

### Phase 2 — finish save-core atomicity and conflict handling

1. Introduce one authoritative same-filesystem temp-and-replace primitive.
2. Make backup retention independent of atomic serialization/replacement.
3. Add overwrite/no-replace/CAS/alias-boundary policy.
4. Return `Written` plus warning after durable replacement if only backup cleanup fails.
5. Suppress staged identity notifications and publish successful adoption once.
6. Wire MCP `expected_destination_sha256` into the authoritative replacement contract.
7. Run targeted App save/Save As/Save Copy/link tests.

### Phase 3 — close readiness and UX gaps

1. Fail closed on malformed native readiness.
2. Implement bounded event-driven waiting without a nested GUI event loop.
3. Run the new close-dialog and screenshot-after-commit tests.
4. Rerun the Document Changes, autosave, Review Note, and shutdown tests.

### Phase 4 — targeted branch-built reproduction

With no concurrent probes:

1. Run valid pad and pocket through the native postcondition path.
2. Run invalid open-profile pad, repair, valid pad, valid pocket, undo, redo, and a second
   document.
3. Run sketch attachment, cross-body datum, grounded assembly joint, and recursive deletion.
4. Run Save Copy with an external link and verify the dependent remains clean.
5. Rerun the three load-sensitive RPC/worker failures alone.

### Phase 5 — full acceptance gates

Run in this order and stop on first red layer:

1. Parent and nested `git diff --check`.
2. Nested Ruff/compileall.
3. Nested focused unit suites.
4. Nested architecture/generator/manifest/contracts.
5. Nested full `pytest -m unit`.
6. Incremental parent native build.
7. Targeted App tests.
8. Targeted GUI/recovery tests under Xvfb/fresh profile.
9. NotificationArea shutdown regression.
10. Exact Review Note GUI test.
11. Branch-built MCP load preflight.
12. Branch-built MCP `MARKER=core`.
13. Branch-built MCP `MARKER=e2e`.
14. Authenticated session E2E with the creation-owned supervisor.
15. Full FreeCAD unit, integration, and GUI E2E.
16. `all-submodules-check`.

Docker Compose's stock/conda FreeCAD is not authoritative for the native changes. The MCP
core/e2e authority is the branch-built Woodpecker lane using this checkout's
`build/debug/bin/FreeCADCmd`.

### Phase 6 — final review and delivery

1. Sol performs a fresh final diff, compatibility, rollback, filesystem, and test-evidence
   review.
2. Resolve every P0/P1/release-significant finding and rerun the affected layer.
3. Remove only known current test artifacts.
4. Preserve parent `tests/lib/` because it predates this work and is not ours to delete.
5. Stage explicit intended paths; do not use `git add -A`.
6. Commit and push nested MCP first:

   `fix(mcp): make editing and save finalization transaction-safe`

7. Update the parent gitlink and commit parent native/GUI work:

   `fix(gui): make document saving change-aware and observable`

8. Run final submodule check.
9. Push the parent branch without force-pushing.

## Git and artifact hygiene

Do not stage:

- Parent `results/`
- Parent `tests/lib/`
- Nested `results_luna_*`
- Nested `results_root_lifecycle.xml`
- Nested Luna stdout/stderr logs
- Any ignored historic `results_unit.xml`

Intended untracked source/test files include:

- `src/Gui/DocumentChangesWidget.cpp`
- `src/Gui/DocumentChangesWidget.h`
- `tests/src/Gui/MainWindowShutdown.cpp`
- Nested automation pause/readiness UI/runtime modules
- Nested focused tests for pause, readiness, native payload policy, structural commit result,
  live connection, and session lifecycle

## Operational safety rules for resuming

- Never use a FreeCAD restart as mutation recovery.
- Never close, save, reload, or mutate `HamaAdapter_v3`.
- Never attach branch tests to the user's active FreeCAD profile or process.
- Use fresh `FREECAD_USER_HOME`, data, temp, user config, and system config for every native
  or GUI lane.
- A rollback failure quarantines only that document.
- A healthy readiness result after a semantic failure remains retryable.
- A tool-local failure must not be reported as an addon-global write-lane outage.
- Do not commit or push until native, nested, branch-built E2E, and final review are all green.

## Short status summary

Most of the planned architecture is implemented and had substantial green unit/native/GUI
evidence. The original nested-transaction defect is removed. The remaining release work is
concentrated in two boundaries:

1. completing the native structural recompute/postcondition authority needed by real
   PartDesign, attachment, Assembly, and delete operations; and
2. making the save replacement/CAS/alias behavior truly atomic at the filesystem boundary.

The implementation is deliberately paused before either unfinished boundary is committed or
deployed.

## Resume log — 2026-08-15

The user resumed the work with `start`. The pause snapshot above remains intact as the
historical handoff; this section records work performed after resumption.

### Completed after resumption

- Reconciled the parent and nested working trees without staging, committing, pushing, or
  touching the user's live FreeCAD process/document.
- Made native readiness fail closed when `getMutationReadiness()` is missing, non-callable,
  throws, returns a non-mapping, or returns malformed/contradictory fields.
- Implemented a dispatcher-owned, event-driven continuation for transient native
  recompute/replay/barrier states. It retains the same logical request, request/session IDs,
  cancellation token, absolute deadline, and same-document ordering; unrelated documents
  may continue. It uses no polling or nested Qt event pump.
- Added focused tests for continuation promotion, ordering, duplicate signals,
  cancellation, timeout, shutdown, and unsupported-observer fail-closed behavior.
- Independently reran the combined readiness, collaboration postcondition bridge,
  structural result truth, continuation, and screenshot-after-commit slice: `57 passed`.

### Corrections in progress

- Independent review found that the first continuation bridge listened to recompute/commit/
  abort callbacks that can occur before native readiness fully unwinds. Native is adding an
  authoritative `BecameStable` Python document-observer callback, and the addon bridge is
  being changed to wake only from that safe boundary.
- The addon bridge is also being corrected to remove an unlocked document-key handoff race
  and to unregister the native document observer on its QObject owner thread with visible,
  retryable failure handling.
- The native structural/postcondition implementation is closing its remaining read-only
  postcondition, cross-document lifecycle, deferred-recompute, exact rollback, and trusted
  status undo/redo invariants before the next incremental App build.
- Atomic-save work has started only in non-overlapping private writer/backup files while
  `Document.cpp` remains owned by the native lane. Integration will wait for a source-freeze
  notice.

### Current gate status

- No new live FreeCAD, GUI, or branch-built MCP process has been launched since resumption.
- No commit or push has been made.
- The next native gate is the smallest previously failing postcondition/structural test,
  followed by `DocumentCollaborationPythonCompatibilityTest.*` if it is green.
- Full acceptance remains blocked until the native structural boundary, stable readiness
  wake, and atomic replacement/CAS work are all implemented and their ordered gates pass.

### Resume checkpoint — second independent review round

The event-driven MCP readiness continuation is now source-complete and independently
reviewed. It wakes only from the native `BecameStable` boundary, keeps the same logical
request/deadline/cancellation identity, preserves per-document ordering, and never calls
`processEvents`. Its Qt observer cleanup is owner-thread-bound, bounded, observable, and
retryable across shutdown races. The latest focused evidence for this slice is `210 passed`,
with the real-Qt module truthfully skipped in the shell that lacked `QCoreApplication`; the
architecture slice remained `188 passed`.

The native structural coordinator closed the first set of postcondition/recompute findings:

- process-scoped target and read-only admission now rejects joined-worker, cross-document,
  lifecycle, and non-owner mutations;
- pending removals are processed or safely requeued before the stable event;
- every canonical, force, Save As, policy, and Save Copy entry is fenced before signals,
  identity staging, failure overlays, or filesystem access during a prepared commit; and
- rollback/status history uses stable object/property identity rather than retained raw
  property pointers.

A fresh independent review then found four final App items, now being corrected before any
build:

1. recovery snapshot writes and recovery-outcome publication must use the same prepared-save
   fence;
2. semantic revision publication needs a document-scoped, coordinator-only grant, and the
   target must remain admitted through reservation commit;
3. status and dynamic metadata on a transaction-owned new object must use transactional file
   tokens so a failed operation can return a clean baseline to clean; and
4. the two newly added document signals must live behind `DocumentP`, while the exact legacy
   `saveToFile(const char*) const` symbol is restored.

The private atomic writer/backup helper also completed an initial implementation and two
read-only design/review passes. Those reviews correctly blocked integration until these
filesystem issues are fixed:

- serialization must use a seekable stream over the creation-owned native handle, not reopen
  the UUID pathname;
- Windows replacement must rename that exact handle through a pinned parent, while POSIX uses
  pinned-parent `openat`/`renameat`/`linkat` operations;
- existing permissions/DACL/basic attributes must be preserved, while unsupported advanced
  Windows metadata fails before replacement instead of being silently lost;
- POSIX backup installation must sync the new directory entry before consuming the recovery
  source; and
- post-replacement backup management must reject a source that aliases the canonical target.

The writer fixes and deterministic boundary-race tests are in progress only in
`DocumentFileWriter`, `BackupPolicy`, their CMake lists, and their focused App tests.
`Document.cpp` integration remains deliberately deferred until both native and writer slices
freeze and pass another independent review.

No App build, branch-built FreeCAD process, live MCP lane, commit, push, or operation against
the user's active FreeCAD session/document has occurred during this checkpoint.

### Resume checkpoint — native authority source-clear

The native structural/postcondition/revision slice is now frozen and independently clear at
the source-review level. The final adversarial review reported no remaining P0/P1 or concrete
P2 in its scope. In particular:

- coordinator-owned prepared target and read-only scopes cannot be released through the
  legacy public begin/end APIs;
- revision reservation and publication stay document-scoped, retain the prepared target
  through publication, and cannot silently report `Committed` if publication fails;
- every post-reservation failure releases the hidden reservation and index lock before
  rollback completion or the resilient stable notification;
- pending object removal is an authoritative native readiness blocker, failed removals are
  retained, each actually drained document receives its own stable wake, and document
  lifetime remains protected through removal callbacks and stable dispatch;
- recovery snapshot writes/outcomes and all canonical, force, Save As, policy, and Save Copy
  entry points are fenced before identity, signals, overlays, or filesystem access during a
  prepared mutation;
- trusted status/schema rollback and committed undo/redo use stable object/property identity,
  including transaction-owned new objects, without retaining raw property pointers; and
- the legacy exported save and compatibility symbols/layouts remain available, while new
  signal/state storage is private or process-lifetime.

The nested MCP readiness schema now requires and validates native `pending_removal`, treats it
as transient-but-blocking, and resumes the same logical request only from the authoritative
stable event. Focused nested evidence for this addition is `83 passed`; diff-check, Ruff, and
compileall were clean. A broader non-live nested checkpoint is running while the filesystem
writer remains under repair.

No App build has started yet because `DocumentFileWriter` and `BackupPolicy` are part of the
same target and are still being corrected after their independent filesystem-identity review.
The outstanding helper work is to retain exact source ownership through backup/discard,
remove destructive pathname TOCTOUs, re-hash after the last deterministic boundary hook, and
preserve or fail closed on all relevant Windows metadata. `Document.cpp` integration remains
deferred until that helper also freezes and passes a fresh independent review.

### Resume checkpoint — native build and focused authority gates

The native structural/postcondition/revision implementation now compiles and its complete
focused authority gate is green in the branch-built CI image:

- `FreeCADApp` and `App_tests_run`: built and linked successfully.
- `DocumentCollaborationPythonCompatibilityTest.*`: `60/60` passed.
- `CollaborationAuthorityRemovalTest.*`: `10/10` passed.
- Combined focused result: `70/70` passed in an isolated container and fresh profile.

The failures encountered before the green rerun were test-precondition defects rather than
production rollback defects: a lazy test feature had not been touched, an intentionally
failed recompute correctly left its object touched until the test established a stable-error
baseline, a transaction test passed a never-attached object whose detach contract returned
null, three tests assumed a zero revision instead of preserving the actual baseline, and a
plain stable-signal connection outlived stack-owned callback state. The fixtures now model
the intended boundaries and use scoped signal connections.

The independently reviewed retained-handle writer/backup helper is also built and green in
its direct native filter: `75` selected, `73` passed, and `2` platform-specific tests skipped.
It retains exact source ownership through serialization, replacement, backup, and discard;
uses atomic no-replace/CAS/alias checks at the final filesystem boundary; and reports Windows
zero-retention durability conservatively.

The latest nested non-live checkpoint is green after readiness-fixture reconciliation:

- architecture/generator/manifest/contracts: `289/289` passed;
- full unit selection: `2422 passed`, `1` platform skip, `1` documented xfail,
  `129` deselected;
- Ruff, compileall, and diff-check: passed.

Atomic writer integration into the public Document save paths is now the active Phase 2
work. No live FreeCAD process, user document, commit, or push was touched by these gates.

### Resume checkpoint — save integration and final nested gates

The public save integration and identity-publication work is now source-frozen and has a
clear independent final review. The integrated behavior now includes:

- retained-handle atomic serialization and replacement for canonical save, Save As, and
  Save Copy;
- native no-clobber and expected-SHA-256 compare-and-swap outcomes;
- truthful `file_written`, durability, warnings, and savepoint/identity adoption semantics;
- exact-property staging that prevents provisional FileName, Label, Uid, TransientDir, Tip,
  and last-modified notifications or accounting from escaping a failed save;
- ordered post-savepoint identity replay on durable Save As, followed by the resilient save
  outcome; and
- a resilient GUI outcome fallback that repairs FileName/Label presentation if a throwing
  legacy listener starves later raw-signal subscribers.

The narrow Uid replay correction matches legacy behavior: the document-level Uid change is
published, a changed TransientDir follows on document and property channels, and a Uid
property signal is emitted only if the UUID bytes actually changed. Its independent rereview
found no remaining P0, P1, or P2 issue.

Latest isolated native evidence on the refrozen tree:

- App save/ABI filter: `79/79` passed;
- prepared-mutation/revision authority filter: `70/70` passed; and
- retained writer/backup filter: `73 passed`, `2` platform skips.

The nested MCP tree is also fully green on the frozen source:

- the three stale generated-contract/authority fixture checks: `3/3` passed;
- generator/architecture reconciliation suite: `280/280` passed;
- full unit selection: `2430 passed`, `129` deselected, `1` expected xfail;
- authoritative Ruff scan: all `1039` files passed; and
- compileall and `git diff --check`: passed.

The two GUI test targets are currently rebuilding against the final App/Gui sources. The
remaining work is the isolated failed-save dialog, GUI shutdown, Review Note, branch-built
MCP core/E2E/session, and full parent acceptance sequence, followed by one last independent
audit and explicit-path staging. The user's live FreeCAD session remains untouched; no
commit or push has occurred.

### Resume checkpoint — final release audit reopened implementation

The GUI target rebuild completed successfully, and the isolated baseline close-dialog and
notification-dock shutdown regressions passed. A subsequent independent adversarial audit,
however, found additional release-significant gaps that were not exercised by the earlier
focused gates. The prior green evidence remains useful as a baseline but is not final
acceptance evidence.

The parent fixes now in progress are:

- exception-safe, irrevocable document-close completion once the collaboration identity has
  entered `Closing`, including per-observer isolation and guaranteed lifetime unregister;
- a conflict-safe, recoverable SHA-256 destination CAS that uses exact strict no-replace
  move-aside/verify/install primitives, retains every competing version on failure, and
  fails before mutation when the required primitive is unavailable (the public wording no
  longer claims an impossible portable single-syscall atomic hash replacement);
- null-safe GUI save camera/thumbnail selection when no `MainWindow` exists;
- pre-mutation admission, exact rollback, transactional new-object accounting, and one
  presentation revision for serialized ViewProvider property-status changes;
- the canonical failure overlay for invalid expected-hash policy requests; and
- post-durable exception handling that can only add maintenance warnings and can never
  reclassify installed bytes as unwritten or roll back an adopted Save As identity.

The nested MCP audit found six P1 groups: typed mutation callbacks and many leaf functions
still recompute inside the broad apply phase before the coordinator's authoritative
recompute; reference repair reports pre-authoritative state; execute-code and 64 active
generated templates contain duplicate/bypassing recomputes; 14 tools advertised as typed
gateways still route through generated scripts; sketch attachment and Assembly solve omit
their required structural trust; and `recompute_and_wait` bypasses readiness/quarantine
admission. It also found stale Save As CAS documentation and fail-open finalization of
contradictory native outcome envelopes. The earlier Document Changes dock-name concern was
retracted after verifying the actual dock object name, so no change is planned there.

Implementation is split into non-overlapping frozen-source lanes. No new build will be
accepted until the close/save, writer CAS, GUI status, and nested recompute-order slices have
each frozen and passed review. After that, App/Gui will be rebuilt, the focused tests will be
rerun on the new binaries, the public typed/generated routes will be migrated, and the full
branch-built MCP acceptance sequence will run. The user's live FreeCAD session remains
untouched; no commit or push has occurred.

## User-requested pause checkpoint — 2026-08-15 (historical — superseded)

> **Historical.** This section records the state at the second pause. Its statements that no
> commit exists, that both repositories are unstaged, and that the branches sit at the base
> commits were true then and are **no longer true**; see "Current published WIP heads" at the
> top. The text is preserved unchanged as the historical record.

The user requested another stop while the final adversarial repair round was still in
progress. Work stopped immediately. No new full nested test, parent build, FreeCAD process,
commit, push, staging operation, or artifact cleanup was started after that request.

### Immediate operational state

- All active sub-agents were interrupted: `gui_status_fix`, `parent_final_fix_review`, and
  `writer_recovery_cleanup_final`.
- `nested_execute_template_final` had already completed and frozen before the pause.
- `docker ps` showed no running containers at the checkpoint.
- The user's live FreeCAD process, profile, and dirty `HamaAdapter_v3` document remain
  untouched. They were not saved, closed, reloaded, restarted, or used for a test.
- No commit or push exists. Both repositories remain unstaged.
- Parent branch/HEAD remain `fix/change-aware-save-mcp-autonomy` at
  `0b90c1533719486a390a4c7afcf40aa7226f316a`.
- Nested branch/HEAD remain `fix/change-aware-save-mcp-autonomy` at
  `f5c40fe679567a63ad16e48ff95e5dce75197fb0`.
- Current short-status counts are:
  - parent: `68` unstaged tracked modifications and `9` untracked entries;
  - nested: `205` unstaged tracked modifications, `1` intentional deletion, and `30`
    untracked entries.
- The nested deletion is the intentional retirement of
  `src/freecad_mcp/templates/diagnostics/run_transaction.py.txt`.

### Work completed in the latest round

#### Native close, save truth, and GUI status

- Document close was changed to complete irrevocably after publishing `Closing`, with
  per-observer isolation and preallocated collaboration tombstone state. A failed tombstone
  reservation now occurs before any irreversible identity/revision transition.
- Expected-hash-without-overwrite Save As now uses the canonical failure overlay.
- Post-durable backup, version, recovery-warning, and outcome-promotion failures are
  structured as warnings and cannot intentionally roll back an installed Save As identity
  or erase `fileWritten=true`.
- Save Copy now prebuilds its committed message before replacement, uses the emitted failure
  message for the legacy exception path, and avoids post-emission outcome copies.
- A failed-replacement promotion fault checkpoint/test was added so result and resilient
  observer must retain the original durability error and `fileWritten=true` under injected
  `bad_alloc`.
- MainWindow-less active-view lookup now has a null-safe document-view fallback for
  thumbnail/camera save paths.
- ViewProvider property-status handling now has stable-identity rollback and private exact
  status restoration. The bulk `setPropertyStatus` path was converted to two passes: capture
  every change, preflight the complete set before any assignment, then apply/account once.
  The independent source rereview cleared this all-or-nothing bulk patch.
- The GUI status lane is source-ready, but its requested fresh combined App/Gui build was
  deliberately held and never started.

#### Native signal resilience and ABI work

- Lifecycle and authoritative save/file-state notifications use resilient per-subscriber
  delivery so one throwing observer cannot starve later core observers.
- An initial FastSignals change that replaced `vector<packed_function>` with a different
  element type was correctly rejected as an old-binary ABI break and was removed.
- The current design retains the exact historical `signal_impl` member order and
  `vector<packed_function>` representation. Resilient traversal lazily replaces a slot with
  another `packed_function` forwarder, while the original callable is retained by a weak
  process-lifetime side registry keyed by signal and slot ID.
- User-callable copies happen outside the signal mutex, are serialized by a per-slot
  authority mutex, retain historical copy/throw behavior for ordinary base emission, and
  allow copy constructors to re-enter `num_slots`/connect/disconnect.
- Resilient traversal now stops after reporting a registry/lock failure that occurred before
  traversal progress, avoiding an infinite loop at irrevocable close.
- Regressions were added for a throwing callable copy, copy-constructor signal re-entry,
  later-listener delivery, and ordinary historical base-copy behavior after resilient
  stabilization.

This signal source has **not** received the final requested independent verdict and has not
been compiled in the current form. Resume must treat it as open until both happen.

#### Writer/CAS work at interruption

- The retained-handle writer already contained strict move-aside/verify/install CAS,
  immediate irreversible `fileWritten` truth, recovery copies for POSIX source-leaf
  substitution, and conservative platform durability reporting.
- The interrupted final writer lane added source for two more recovery seams:
  - after a failed CAS guard restore succeeds but before the restored canonical entry is
    inspected; and
  - after successful CAS installation but before the displaced guard is inspected.
  These paths are intended to materialize a named, hash-verified copy from the retained exact
  predecessor handle when the public name has been swapped or removed.
- It also changed POSIX automatic destructor/explicit discard behavior to fail closed rather
  than perform an unsafe `fstatat`-then-`unlinkat` on a public name. The header now documents
  that abandoned POSIX serialization may remain as named recovery evidence because POSIX has
  no portable inode-conditional unlink.
- Direct tests for fail-closed displaced discard/source swaps are present, and the latest
  targeted `git diff --check` over writer/save/signal/GUI-status files was clean apart from
  existing line-ending notices.

The writer lane was interrupted mid-task. Do **not** assume it is complete. In particular,
the new CAS restore/post-install callbacks were visible in source, but their deterministic
tests were not yet present when inspected after the stop.

#### Nested MCP recompute and typed routes

- Public delete, sketch, body, constraint, expression, spreadsheet, and Assembly mutations
  now route through typed connection methods exactly once instead of `execute_code`.
- Generic mutating execute no longer recomputes inside the broad apply phase:
  - `recompute="none"` performs no adapter/native recompute;
  - `recompute="target"` uses exactly one coordinator-owned native recompute and a native
    postcondition;
  - unsupported multi-document recompute scope fails before executing code.
- Signed generated code now supports an authenticated native-post-recompute continuation.
  Fifty-nine active template tails were migrated, and direct `.recompute()` calls were
  removed from all generated templates. Derived result inspection happens in the read-only
  postcondition phase.
- `recompute_and_wait` now uses readiness/quarantine admission; reference repair is split
  into apply and post-recompute validation; unsupported Gmsh/FEM/two-phase boundaries fail
  before mutation.
- A static architecture gate prohibits template-local recompute except explicit compatibility
  allowlists.
- Latest focused nested evidence from the frozen template lane:
  - Python 3.12 focused selection: `339 passed`;
  - Python 3.11 architecture/phase-14 selection: `28 passed`;
  - selected Ruff: green;
  - Python 3.11 and 3.12 compileall: green;
  - lane-scoped diff check: green.

The full nested unit gate was about to be started when the user paused; it was **not**
started. The older `2430 passed` full-unit result predates the latest typed-route/template
changes and is baseline evidence only.

### Known open release blockers at this pause

1. **Two unsafe POSIX link fallbacks remain in the writer.** Independent review found
   pathname proof followed by `unlinkat` in:
   - ordinary NoReplace portable link fallback (current area around
     `DocumentFileWriter.cpp:1940-1964`); and
   - displaced-lease/BackupPolicy portable link fallback (current area around
     `DocumentFileWriter.cpp:2183-2222`).

   A same-UID namespace swap can make either unlink delete a foreign entry. Portable safe
   choices are to leave the source name and report it unconsumed/recoverable after a
   successful link, or fail before linking when strict no-replace rename is unavailable.
   Another userspace identity check is not sufficient.

2. **The interrupted CAS evidence patch needs completion and tests.** Add deterministic
   restore-before-inspection and install-before-guard-inspection swaps. Both must preserve
   the foreign public entry and leave the exact predecessor at a different reported path
   that survives result/lease destruction.

3. **FastSignals needs final independent ABI/concurrency review and compilation.** Preserve
   the historical static-library storage/method contract for already-built extensions. Run
   the new copy-throw/re-entry/base-emission tests after the current source is reviewed.

4. **The latest Document outcome patches are source-only.** The pessimizing-move compile
   issue found during review was corrected by direct prvalue returns on capture branches,
   but the source has not been rebuilt. The two remaining `return std::move(outcome)` sites
   are lambda returns of an enclosing captured lvalue, not NRVO candidates.

5. **No current parent binary is authoritative.** Earlier `79/79`, `70/70`, and writer
   results predate the final close/CAS/status/signal/outcome changes. A completely fresh
   combined build and affected tests are required.

6. **Final release review and full acceptance remain outstanding.** No branch-built MCP
   core/E2E/session rerun, full parent suite, submodule check, staging, commit, or push has
   occurred.

### Exact safe resume sequence from this checkpoint

1. Confirm again that no agent, Docker build, FreeCAD test process, or live-session test is
   active. Do not touch the user's FreeCAD process/document.
2. Inspect the interrupted writer diff before editing. Finish the two remaining POSIX
   proof/link/unlink paths without pathname deletion, then add/finish the two CAS named-
   evidence regressions. Run targeted diff check and request a fresh independent writer
   review.
3. Independently review the current FastSignals side-registry/authority-mutex design for
   old-binary storage compatibility, traversal progress, concurrent copy semantics, and
   teardown lifetime. Fix any finding before building.
4. Rerun targeted static checks on Document outcome, collaboration tombstone, GUI bulk
   status, writer, FastSignals, and their tests. Freeze all parent source.
5. Only after that freeze, launch one fresh isolated combined build of:
   - `App_tests_run`
   - `Gui_tests_run`
   - `GuiShutdown_tests_run`
6. Run the smallest new regressions first: lifecycle throwing/copy/re-entry, tombstone
   allocation, failed outcome promotion, post-durable save truth, bulk ViewProvider status,
   MainWindow-less save, every CAS recovery/cleanup test, then the broader App/GUI filters.
7. Run nested full unit, architecture/generator/contracts, Ruff, compileall, and diff-check
   on the final nested tree. Reconcile only intentional generated fixtures.
8. Continue the ordered branch-built MCP core/E2E/authenticated-session and full parent
   acceptance gates from the existing Phase 5 plan. Stop on the first red layer.
9. Request one final independent release audit. Stage explicit intended source paths only;
   do not use `git add -A`.

### Artifact and staging warning at this pause

Do not stage or delete indiscriminately:

- parent `results/`;
- parent `tests/lib/`;
- nested `results_luna_*` logs/stdout/stderr files currently visible as untracked;
- historic ignored result XML/log artifacts.

The handoff file itself remains intentionally untracked until final explicit-path staging.
No cleanup was performed during this pause because recovery/test artifacts and pre-existing
files must not be guessed at or deleted.

## Resume checkpoint — writer POSIX link fallbacks (blocker 1)

Work resumed from the pause checkpoint above. Resume steps 1 and 2 are done; nothing else
from the ordered sequence has been started.

### Step 1 — checkpoint reconciled, nothing active

Verified before any edit:

- `docker ps -a` showed no running container; the most recent, `fc-gui-targets-rebuild`,
  had exited 0.
- No FreeCAD process was present on the host.
- Parent branch/HEAD unchanged: `fix/change-aware-save-mcp-autonomy` at
  `0b90c1533719486a390a4c7afcf40aa7226f316a`.
- Nested branch/HEAD unchanged: `fix/change-aware-save-mcp-autonomy` at
  `f5c40fe679567a63ad16e48ff95e5dce75197fb0`.
- Parent short status: `68` tracked modifications, `9` untracked entries — an exact match
  for the pause snapshot.
- Nested short status: `206` non-untracked entries (`205` modifications plus the intentional
  `run_transaction.py.txt` deletion) and `30` untracked entries — also an exact match.

No agent, build, FreeCAD process, or live-session test was started, and the user's session
and `HamaAdapter_v3` were not touched.

### Step 2 — blocker 1 closed at source level

Both flagged sites still contained the reviewed-unsafe pattern: a pathname `fstatat` identity
proof immediately followed by an `unlinkat` on that same pathname. The file already stated the
correct rule in `NativeFile::discardExact()` and `cleanupOwnedFile()` — POSIX has no
inode-conditional unlink, so those paths fail closed and leave the proved owned name as
recovery evidence. The two link fallbacks contradicted that rule; they now follow it.

**Ordinary NoReplace portable link fallback** (`replaceOwnedFile`, POSIX branch):

- After a successful `linkat`, the destination is proved to name the exact retained handle.
  `linkat` resolves its source by pathname, so a substituted leaf could otherwise have
  installed a foreign inode under the canonical name; that case now returns `ESTALE` and
  reports no installation.
- The source `unlinkat` and its adjacent proof were removed entirely. A successful fallback
  returns installed with `sourceConsumed` false.
- `DocumentFileWriter::commit()` gained an `else if (!replacementSourceConsumed)` branch that
  emits a warning naming the retained serialization path and why it was kept. The already
  existing `replacementError` warning branch is unchanged, and `markTransferred` is still
  skipped when the source is not consumed, so cleanup state stays `Owned` and the POSIX
  destructor keeps performing no namespace mutation.

**Displaced-lease / BackupPolicy portable link fallback**
(`installDisplacedFileLeaseNoReplace`, POSIX branch):

- The same post-`linkat` destination identity proof was added before `installed` is reported.
- The entire proof/`unlinkat`/`markTransferred` block was removed. The flow is now link,
  prove, checkpoint, directory flush, re-prove the durable destination, then report
  `installed=true`, `durabilityVerified=true`, `sourceConsumed=false` with a non-fatal reason
  in `error`.
- `BackupPolicy` needed no new branch: its existing
  `installed && durabilityVerified && !sourceConsumed` case already emits "the backup was
  installed, but the displaced snapshot name remains" and already sets
  `installationDurable=false`, which suppresses history pruning. That is the intended
  conservative outcome on a filesystem without strict no-replace rename.

Supporting changes:

- `DisplacedFileLeaseOperationResult` is now documented: an installation can be durable while
  `sourceConsumed` is false, and `error` then carries a non-fatal retention reason.
- `DisplacedFileLeaseCheckpoint::BeforeSourceUnlink` and
  `::AfterSourceUnlinkBeforeDirectoryFlush` became unreachable and were removed, along with
  their two `BackupPolicy` switch cases. `BackupPolicyTestCheckpoint` keeps both values
  because the non-lease compatibility path still emits them.
- Two POSIX writer tests asserted the deleted behavior and were rewritten:
  - `PreUnlinkSourceSwapNeverDeletesForeignEntry` →
    `PortableLinkFallbackInstallsBackupAndRetainsSourceName`, which forces the fallback and
    asserts the backup is created, the source is reported unconsumed with the retention
    warning, and the displaced name still holds the old bytes.
  - `PostUnlinkCandidateSwapIsNotReportedOrPrunedAsDurable` →
    `PortableLinkFallbackCandidateSwapIsNotReportedOrPrunedAsDurable`, which swaps the
    candidate at `AfterLinkBeforeDirectoryFlush` and asserts the post-flush identity proof
    refuses the install, preserves the foreign entry, the known-good history, and the source.
- The two `BackupPolicy` tests that use the unlink checkpoints
  (`PreConsumptionDurabilityFailureRetainsSourceAndHistory`,
  `PostConsumptionDurabilityFailureDoesNotPruneHistory`) call `applyAfterReplacement` without
  a lease, so they exercise the legacy compatibility path and are unaffected.

Known behavioral consequence, accepted deliberately: on a filesystem with neither
`renameat2(RENAME_NOREPLACE)` nor `renameatx_np(RENAME_EXCL)`, each save now leaves its
displaced snapshot name behind as reported evidence and does not prune backup history. The
reviewed alternative — failing before linking — would lose the backup entirely, so retention
is the safer of the two portable choices.

### Verification status for this slice

- `git diff --check` over the touched tracked files: clean apart from the existing
  LF/CRLF notices.
- Trailing-whitespace scan of the three untracked writer/test files: clean.
- Grep confirms no `unlinkat` or `::unlink(` call remains in `DocumentFileWriter.cpp`; the
  four remaining occurrences are explanatory comments.
- **Not compiled and not run.** These are POSIX-only branches and the host is Windows; they
  can only be built and exercised in the branch-built Linux image, which per the resume plan
  waits for the full parent source freeze.

### Files touched in this checkpoint

- `src/App/DocumentFileWriter.cpp` (untracked)
- `src/App/DocumentFileWriter.h` (untracked)
- `src/App/BackupPolicy.cpp`
- `tests/src/App/DocumentFileWriter.cpp` (untracked)

No commit, push, staging, build, FreeCAD process, or MCP lane was started.

## Resume checkpoint — CAS named-evidence regressions (blocker 2)

Blocker 2 asked for the two deterministic CAS swap regressions that were missing when the
writer lane was interrupted. Inspection first confirmed the *source* seams were already
complete and correctly shaped, so no production change was needed for this blocker:

- `afterCompareAndSwapGuardRestoreBeforeInspection` fires after the strict restore primitive
  reports success and before the canonical entry is inspected. Both a throwing hook and a
  non-throwing name swap reach `materializeAfterRestoreInspectionFailure`, which relinquishes
  path cleanup and publishes a hash-verified `.cas-restored-predecessor-recovery` copy.
- `afterCompareAndSwapInstallBeforeGuardInspection` fires after the install is irreversible
  and before the obsolete guard is inspected. A guard that no longer proves ownership is left
  untouched and the predecessor is published as
  `.cas-post-install-predecessor-recovery`.
- `createVerifiedRecoveryCopy` deliberately never requires the *source pathname* to still
  name the source; it hashes and copies through the retained handle and only requires the new
  sibling and the pinned parent to be owned. That is exactly what makes both seams survivable.

One suspected defect was investigated and dismissed: a stale guard-path warning after the
restore seam. It cannot occur, because `guardRecoveryWarning` and `temporaryRecoveryWarning`
are already moved-from at the guard-move step, and `publishPredecessorRecovery` swaps the
recovery warning into the same `warningsBeforeCompareAndSwap` slot the guard warning
occupied. No source change was made.

### Regressions added

Both are cross-platform (`pinnedPathStillOwned` and all recovery-copy machinery are
identity-based on Windows and POSIX alike) and both assert the full blocker 2 contract:
the foreign public entry is preserved, the exact predecessor is reported at a *different*
path, and that path survives result and lease destruction.

- `CompareAndSwapRestoreSwapKeepsForeignEntryAndNamesPredecessor` — drives the guard-validation
  fault so the guard is restored into the canonical name, then renames that canonical name
  away and drops a foreign file there before inspection. Asserts `DESTINATION_CHANGED` with
  `fileWritten` false, the foreign occupant intact at the canonical name, the predecessor
  readable at the reported recovery path (which is neither the canonical nor the consumed
  guard name), the serialized temporary still intact, and the guard name gone.
- `CompareAndSwapInstallGuardSwapKeepsForeignEntryAndNamesPredecessor` — lets the CAS install
  succeed, then swaps the obsolete guard name before inspection. Asserts the commit still
  succeeds with the serialized bytes installed, the foreign guard occupant left exactly as
  the swap left it, the predecessor readable at the reported recovery path, and both the
  "guard ownership could not be proved" and recovery-path warnings present.

The existing `CompareAndSwapPostInstallGuardInspectionIsBestEffort` remains the injected-throw
variant of the second seam; the new test is the deterministic name-swap variant it lacked.

### Verification status for this slice

- `git diff --check` on the touched tracked file: clean apart from the LF/CRLF notices.
- No line exceeds the project's 100-column limit; no trailing whitespace; no duplicate test
  names in the file.
- **Not compiled and not run**, for the same reason as the previous slice.

Files touched: `tests/src/App/DocumentFileWriter.cpp` only (untracked).

## Resume checkpoint — line-ending corruption (new blocker, not previously recorded)

Reviewing the FastSignals diff exposed a defect that affects the whole release, not just that
slice, and that none of the earlier `git diff --check` passes could see.

### What was wrong

`core.autocrlf` is `true`, and most FreeCAD blobs are stored LF. But eight modified files have
blobs stored **CRLF** (vendored or historically Windows-committed). Git deliberately skips
CRLF→LF normalization when the index blob already contains CRLF, so for those eight files the
working tree is compared byte-for-byte. Editing them injected lone-LF terminators, producing
mixed line endings and enormous phantom diffs:

| file | raw diff (+/-) | real content diff (+/-) |
| --- | --- | --- |
| `src/App/Document.cpp` | 2867 / 589 | 2493 / 215 |
| `src/App/Document.h` | 343 / 63 | 283 / 3 |
| `src/App/DocumentObject.cpp` | 96 / 30 | 74 / 8 |
| `src/3rdParty/.../signal_impl.h` | 230 / 16 | 215 / 1 |
| `src/3rdParty/.../signal.h` | 16 / 4 | 12 / 0 |
| `src/Gui/ViewProviderDocumentObject.h` | 8 / 3 | 5 / 0 |
| `src/3rdParty/.../signal_impl.cpp` | 54 / 54 | **none** |
| `src/App/DocumentObserverPython.h` | 5 / 5 | **none** |

The last two had *no content change whatsoever* — they were pure line-ending churn sitting in
the release diff. `Document.cpp` alone carried roughly 374 phantom insertions and deletions.
This would have made the final release audit unreliable and polluted a vendored third-party
dependency against upstream.

### What was done

All eight files were normalized back to CRLF at byte level (insert `\r` before any `\n` not
already preceded by one; nothing else touched). Verified three ways:

- the raw `--numstat` after normalization is now **exactly equal** to the content-only
  `--ignore-cr-at-eol --numstat` recorded before it;
- the content-only `--numstat` is **unchanged** before and after, proving no content moved;
- no lone-LF terminator remains in any of the eight files.

`signal_impl.cpp` and `DocumentObserverPython.h` dropped out of the diff entirely, taking the
parent tracked-modification count from `68` to `66`.

### Standing rule for the rest of this work

Before editing any file, check whether its blob is CRLF; if so, re-normalize after editing and
confirm raw `--numstat` still equals content-only `--numstat`. A plain `git diff --check` does
**not** catch this — it reports only whitespace errors, not terminator churn.

## Resume checkpoint — FastSignals review (blocker 3)

With the diff finally readable (`signal.h` +12, `signal_impl.h` +215/-1, `signal_impl.cpp`
byte-identical to HEAD), the resilient-emission design was reviewed against the four concerns
the pause checkpoint named.

### Cleared

- **Old-binary storage compatibility.** Member order and the
  `std::vector<packed_function>` representation are unchanged. `m_functions` gained only
  `mutable`, which is a compile-time storage qualifier and does not affect layout, size, or
  offsets. `signal_impl.cpp` is unchanged, so `add`/`remove`/`remove_all`/`get_next_slot`/
  `count` keep their exact historical behavior.
- **Traversal progress.** `invoke_resilient` advances `expectedIndex`/`nextId` before any
  throwing operation, so a failed slot is reported through the handler and skipped rather than
  retried; the explicit no-progress check breaks the loop when the failure happened before
  traversal advanced. The `nextId` computation is arithmetically identical to the historical
  `get_next_slot`.
- **Concurrent copy semantics.** `copyMutex` is held only across the user-callable clone and
  never across the invocation, so a callable may re-enter `num_slots`/`connect`/`disconnect`
  without deadlocking. Lock order is consistently registry → signal, and `copyMutex` is only
  ever taken with both released.
- **Address recycling.** Initially suspected: the registry is keyed by
  `{const void* signal, uint64_t slotId}`, `signal_impl` addresses are recyclable, and
  `m_nextId` restarts at `1` for every new signal, so keys collide readily. This is *not*
  exploitable, because `signal::operator()` and `emit_resilient` both copy the
  `shared_ptr<signal_impl>` for the duration of emission. No authority can outlive its
  `signal_impl`, so a recycled address always finds an expired weak reference. Recorded as a
  residual design sharpness, not a defect.

### Defect found and fixed

Registry entries were **never erased when a signal was destroyed**. They are removed only when
a later lookup happens to hit the same key and finds it expired. `signal_impl` is not templated
on the signature, so it cannot reach the per-signature registries, and adding a member or
virtual to let it do so would break exactly the layout compatibility this design exists to
preserve. The result was monotonic growth of dead entries over the process lifetime — a real
leak in a long CAD session that opens and closes many documents.

Fixed with an amortized sweep under the lock already held: `prune_expired_locked()` erases
expired weak references once the map exceeds a threshold, then re-arms the threshold at twice
the surviving live count (floor 64). No layout, ABI, or lock-ordering change.

Also documented the one genuinely unsupported re-entry: a user copy constructor that
resiliently re-emits the *same slot of the same signal* would deadlock, because `copyMutex` is
not recursive.

### Status

Source-complete and reviewed; **still uncompiled** at the time of writing. The combined build
below is its first compilation.

## Resume checkpoint — combined freeze build

With the parent source frozen, resume step 5 was started: one fresh isolated build of
`App_tests_run`, `Gui_tests_run`, and `GuiShutdown_tests_run` in the branch CI image
`127.0.0.1:5001/freecad-ci-deps:24.04`, container `fc-freeze-combined-build`, ccache volume
`freecad-ci-ccache`, against `build/debug`.

This build is also the first real compilation of the writer POSIX link-fallback changes from
blocker 1. Those branches are `#ifndef FC_OS_WIN32`, so they could not be compiled on the
Windows host at all; the Linux CI image is the first environment that reaches them.

### Build result — green

`App_tests_run`, `Gui_tests_run`, and `GuiShutdown_tests_run` all compiled and linked with
**zero errors or warnings** (455 targets). This is the first successful compilation of:

- the FastSignals resilient-emission design plus the new `prune_expired_locked()` sweep;
- the blocker 1 POSIX link-fallback rewrite, whose `#ifndef FC_OS_WIN32` branches the Windows
  host could never reach; and
- the blocker 4 Document outcome patches, including the pessimizing-move correction.

Blockers 3, 4, and 5 are therefore closed as far as compilation goes.

## Resume checkpoint — first red layer: App writer filter

`App_tests_run --gtest_filter="DocumentFileWriterTest.*:BackupPolicyTest.*"` gives
**76 passed, 2 skipped, 11 failed**.

### All four new tests pass

The work from blockers 1 and 2 is confirmed on real Linux POSIX semantics:

- `PortableLinkFallbackInstallsBackupAndRetainsSourceName` — passed
- `PortableLinkFallbackCandidateSwapIsNotReportedOrPrunedAsDurable` — passed
- `CompareAndSwapRestoreSwapKeepsForeignEntryAndNamesPredecessor` — passed
- `CompareAndSwapInstallGuardSwapKeepsForeignEntryAndNamesPredecessor` — passed

### The 11 failures are stale expectations, not new regressions

Ten of them track a deliberate, documented behavior change made during the *interrupted*
writer lane — POSIX `discardExact()`/`cleanupOwnedFile()` now fail closed and never unlink a
pathname — which was never compiled or tested at the time, so its tests were never updated.

**Group A — abandoned temporary now survives (7 tests).** All fail on the identical assertion
`fs::exists(temporary)` expected false, actual true:
`AbandonedSerializationNeverTouchesDestination`,
`NoReplaceRejectsDestinationCreatedAfterSerialization`,
`CompareAndSwapRejectsSwapAtReplacementPrimitive`,
`CompareAndSwapGuardMoveNeverClobbersCollision`,
`CompareAndSwapUnsupportedNoReplaceFailsBeforeMutation`,
`CompareAndSwapAuthorityFailureIsPreMutationAndSpecific`,
`CompareAndSwapDurabilityFailureIsPreMutationAndSpecific`.

**Group B — POSIX discard now fails closed (1 test).**
`RetainedLeaseDiscardRemovesOnlyOwnedSnapshot` expects consumption, no warnings, and removal;
it now gets non-consumption, a warning, and a surviving snapshot.

**Group C — successful CAS now retains its predecessor (1 test).**
`CompareAndSwapAcceptsMatchingDestinationHash` expects an empty `displacedFile` and no lease,
but a successful CAS deliberately reports the retained predecessor so `BackupPolicy` can
install it. `CompareAndSwapRetainsExactMovedPredecessorForBackup` asserts that newer contract
and passes.

**Group D — stale warning wording (1 test).**
`CompareAndSwapPostInstallGuardInspectionIsBestEffort` greps for "guard ownership inspection
failed", which is now only the `catch (...)` fallback text; the injected `std::runtime_error`
path reports "guard ownership could not be proved at '<path>' (<diagnostic>)".

**`BackupPolicyTest.TimestampTargetHasNoExtension` — see the dedicated bisect section below.**
This entry was recorded twice with a wrong conclusion (first "pre-existing" by unsound
reasoning, then "regression" on partial evidence). The measured answer is a pre-existing
latent defect that only fires when the temporary directory path contains a `.`; the branch did
not cause it.

### Open design decision

Group A means every failed save on POSIX now leaves a UUID-named temporary behind permanently.
That is safe but it is litter, and the plan never recorded it as an accepted user-visible
change. No test in groups A–D has been edited.

## Resume checkpoint — O_TMPFILE attempted and found unusable here

The chosen fix was unnamed serialization: an abandoned file with no directory entry needs no
cleanup at all, because closing the descriptor releases the inode. It was implemented:

- `NativeFile::createUnnamedTemporary()` opens `O_TMPFILE` in the pinned destination directory
  and returns null when unsupported;
- `NativeFile::materializeAt()` links from the retained descriptor via `/proc/self/fd/N` with
  `AT_SYMLINK_FOLLOW`, so no pathname is ever resolved and no substitution window exists;
- `reserveSerializationFile()` prefers unnamed and falls back to the historical named sibling;
- `replaceOwnedFile()` installs an unnamed file directly (`NoReplace` collides through
  `linkat`'s own `EEXIST`; `Replace` materializes to a private name that the immediately
  following `renameat` consumes); and
- the pre-boundary "temporary path changed" proof is skipped as unnecessary, not merely
  bypassed, when there is no pathname.

It compiles cleanly and is correct where supported. **It does not help in this environment.**

A direct probe shows `O_TMPFILE` returning `ENOTSUP (95)` on every filesystem available:

| mount | filesystem | O_TMPFILE |
| --- | --- | --- |
| `/` (container temp dirs) | `overlay` | `ENOTSUP` |
| `/tmp` | `overlay` | `ENOTSUP` |
| repo bind mount | `9p` | `ENOTSUP` |

The writer filter is therefore **unchanged at 76 passed / 2 skipped / 11 failed** — byte for
byte the same failures — because the named fallback is always taken.

Consequences that must drive the next decision:

1. The litter problem is untouched in the only environment currently available, and would also
   remain on macOS, which has no `O_TMPFILE` at all.
2. The new unnamed install branch cannot be exercised by any test in this lane, so it is new,
   unvalidated code in the most safety-critical path — exactly what the release gates exist to
   prevent. Keeping it requires either accepting unvalidated code or finding an ext4/XFS/tmpfs
   test surface; reverting it keeps the tree honest and testable.

The remaining realistic choices for the fallback path — which is the one that actually runs —
are still to update the stale tests to accept retention, or to permit an identity-proved unlink
of our own `O_EXCL`-created UUID temporary (never of a canonical or backup name).

**Resolved:** O_TMPFILE was reverted in full (unnamed creation, `materializeAt`, the unnamed
install branch, and the `isNamed()` guard). Recorded for a future release: **tmpfs does support
`O_TMPFILE`**, verified with `docker run --tmpfs`, so a capable lane is available whenever the
unnamed create/materialize/install/failure paths are given real coverage.

## Bisect — `BackupPolicyTest.TimestampTargetHasNoExtension`

Run on both commits in the same image, with `TZ=UTC`, `LANG=LC_ALL=C.UTF-8`, the same test
command, and the same procedure (`git archive` of the commit, submodule trees copied in,
identical cmake configure). `tests::TempDirectory` removes its tree in its destructor, so the
literal post-process listing is necessarily empty; the artifact listings below are reconstructed
from a complete `strace` of file syscalls, which gives the exact directory contents at the
moment of assertion.

### Result: both commits PASS

| commit | TMPDIR | result |
| --- | --- | --- |
| base `0b90c153` | `/basetmp` | **PASSED** |
| head `0343b0a4` | `/headtmp` | **PASSED** |

Base artifacts:

```
/basetmp/fc_backup_policy_0/source.fcstd
/basetmp/fc_backup_policy_0/target
/basetmp/fc_backup_policy_0/target.2026-08-15.FCBak
rename("…/target", "…/target.2026-08-15.FCBak") = 0
rename("…/source.fcstd", "…/target")            = 0
```

Head artifacts (the legacy rename is now a link+unlink through `atomicInstallNoReplace`, which
is the only behavioural change in the whole `BackupPolicy.cpp` diff):

```
/headtmp/fc_backup_policy_0/source.fcstd
/headtmp/fc_backup_policy_0/target
/headtmp/fc_backup_policy_0/target.2026-08-15.FCBak
/headtmp/fc_backup_policy_0/target.FreeCAD-save.lock
link("…/target", "…/target.2026-08-15.FCBak")   = 0
unlink("…/target")                              = 0
rename("…/source.fcstd", "…/target")            = 0
```

### Actual root cause: a dot in the temporary directory path

The earlier failures were produced with `TMPDIR=$(mktemp -d)`, which yields `/tmp/tmp.XXXXXXXX`
— a path containing a `.`. The bisect scripts used `/basetmp` and `/headtmp`, which do not.
That difference, not the commit, decides the result:

| binary | TMPDIR | result |
| --- | --- | --- |
| working tree | `/nodot` | **PASSED** |
| working tree | `/has.dot` | **FAILED** |

The trace under a dotted TMPDIR shows the backup being written to the wrong directory entirely:

```
link("/tmp/tmp.pbQUWBuJg3/fc_backup_policy_0/target", "/tmp/tmp.2026-08-15.FCBak") = 0
```

`applyTimeStamp` computes its backup base as

```cpp
std::string ext = fi.extension();
if (!ext.empty()) {
    bn = fi.filePath().substr(0, fi.filePath().length() - ext.length());
```

For a target with **no filename extension** inside a directory whose name contains a `.`,
`Base::FileInfo::extension()` reports everything after the last dot **in the whole path**
(`pbQUWBuJg3/fc_backup_policy_0/target`). The `else` branch is therefore never taken, `bn`
truncates the path at the last dot to `/tmp/tmp.`, and the backup lands outside the document's
directory.

This code is byte-identical at base and head. It is a **pre-existing latent defect**, not
caused by this branch, and it is genuinely user-facing: saving an extensionless document from a
folder containing a dot writes its backup into the wrong directory. It should be filed and
fixed separately, against `Base::FileInfo::extension()` or `applyTimeStamp`'s use of it — not
as part of this release.

### Consequence for the gate

With a dot-free `TMPDIR`, and with the EphemeralPartial cleanup from step 5 in place, the
writer filter is now **9 failures**, down from 11:

- step 5 fixed `AbandonedSerializationNeverTouchesDestination`;
- a dot-free `TMPDIR` removes `BackupPolicyTest.TimestampTargetHasNoExtension`.

All nine remaining failures are `DocumentFileWriterTest` cases, and eight of them are decided
by the two open contract questions:

| test | decided by |
| --- | --- |
| `NoReplaceRejectsDestinationCreatedAfterSerialization` | Q1 |
| `CompareAndSwapRejectsSwapAtReplacementPrimitive` | Q1 |
| `CompareAndSwapGuardMoveNeverClobbersCollision` | Q1 |
| `CompareAndSwapUnsupportedNoReplaceFailsBeforeMutation` | Q1 |
| `CompareAndSwapAuthorityFailureIsPreMutationAndSpecific` | Q1 |
| `CompareAndSwapDurabilityFailureIsPreMutationAndSpecific` | Q1 |
| `CompareAndSwapAcceptsMatchingDestinationHash` | Q2 |
| `RetainedLeaseDiscardRemovesOnlyOwnedSnapshot` | Q2 |
| `CompareAndSwapPostInstallGuardInspectionIsBestEffort` | neither — stale warning wording only |

In each Q1 case the writer deliberately calls `relinquishPathCleanup()` on the serialized
temporary so it survives as recovery evidence, which is exactly the `VerifiedSerialization`
question.

## Resume checkpoint — contract approved, writer filter green

Both contract questions were ruled on: **Q1 retain and report**, **Q2 permit with an identity
proof**. Both are implemented and the full reconciliation is recorded in
`doc/save-artifact-contract.md` §9.

- `markVerifiedSerialization()` clears the `EphemeralPartial` mark at the serialized-baseline
  hash, so verified bytes can never be removed by cleanup; the failure warning now describes
  them as the document being saved rather than as an unremovable temporary.
- `discardDisplacedCanonicalExact()` gives the `numberOfFiles == 0` discard its own entry
  point, unreachable from generic cleanup, with an identity proof that retains and reports on
  failure.

Of the original eleven, only **two were production defects**; seven were expectations the
approved contract shows to be wrong, one was an unrelated pre-existing bug, and one was a
wording drift.

### Gate results

- `DocumentFileWriterTest.*` + `BackupPolicyTest.*`: **87 passed, 2 platform skips, 0 failed.**
- Full `App_tests_run`: **922 tests, 919 passed, 2 skips, 1 failed.**

### New finding — `RenameProperty.updateExpressionDifferentDocument` is a regression

Surfaced by the broader App filter and bisected the same way as the timestamp test, with the
same image, locale, timezone and command:

| commit | `RenameProperty.*` |
| --- | --- |
| base `0b90c153` | **14/14 passed** |
| working tree | 13 passed, **1 failed** (`updateExpressionDifferentDocument`) |

It fails in isolation, so it is not test-order pollution. The test lives in
`tests/src/App/Property.cpp`, which this branch does not modify, but the branch does modify
`src/App/Property.cpp` and `src/App/Property.h`. This is a **genuine regression introduced by
this work** and is the next thing to fix; it is not yet root-caused.

Per the ordered-gate rule this is the first red layer of the broader parent sequence, so the
nested, branch-built MCP, authenticated-session and full acceptance gates have **not** been
started.

## Root cause — `RenameProperty.updateExpressionDifferentDocument`

### Correction: the earlier bisect was invalid

The bisect that "proved" a regression compared **different filesystems**. Base was built and
run at `/basetree2`, which is container-local **overlayfs**; head was run from the repo root,
which is the **9p** bind mount. Same-filesystem runs of the *current* tree show:

| filesystem | result |
| --- | --- |
| overlayfs (`/tmp`, `mktemp -d`) | **PASSES**, including `--gtest_repeat=5` and the whole `RenameProperty.*` suite |
| 9p (anywhere under the repo mount) | **FAILS** |

The test is not order-dependent, not polluted by a pre-existing `test.FCStd`, and not affected
by `HOME`/`FREECAD_USER_HOME`. It uses relative paths (`saveAs("test.FCStd")`), so it saves
into the current working directory — which is why running the suite from the repo root put it
on 9p.

### The actual failure

The escaping exception is `Base::FileException` from `Document.cpp:5116`, carrying
`REPLACEMENT_VERIFICATION_FAILED` — "The installed destination does not match the serialized
temporary file". At the check in `DocumentFileWriter::commit()`:

```
destinationAfter.exists    = false
destinationAfter.identity  = ""
temporary->identity()      = "133:34902897112163405"
```

The destination does not resolve at all immediately after the replacement rename.

### Mechanism, isolated to a single primitive

Reproduced with no FreeCAD code at all. Renaming a file **while holding an open descriptor on
it** makes the destination invisible to a subsequent `stat` on 9p:

| filesystem | hold fd across rename | destination visible after rename |
| --- | --- | --- |
| 9p | no | **yes** |
| 9p | **yes** | **no** — `ENOENT`, still failing after 1s |
| overlayfs | no | yes |
| overlayfs | yes | yes |

Overwrite-vs-create and dirfd-vs-path make no difference; the retained descriptor is the only
variable. This is precisely the writer's retained-handle design (contract R22), so on 9p the
post-replacement identity proof (R23) can never succeed.

Base does not hit this because its legacy save closes the temporary before renaming.

### Severity: worse than a failed test

On 9p the bytes are installed correctly but the save is reported as failed:

- `doc.FCStd` afterwards is the **new** 1192-byte FCStd zip — the save physically succeeded;
- `doc.FCStd.<uuid>.displaced` holds the exact previous version;
- yet `saveAs` throws and `Document.FileName` is rolled back to `''`.

A user on such a mount is told the save failed while the file on disk is correct and current.
They may re-save repeatedly, accumulating a `.displaced` copy each time, or quit believing
work was lost that was in fact written.

### Why this stops here

This is **not** a rename/expression defect, so the prescribed narrowing (Property.cpp/.h
restore, then the notification chain through PropertyContainer/Document/transaction) does not
apply — the divergence is not in that chain at all. The failing expectation is correct and has
not been touched.

The smallest production invariant to fix lives in `DocumentFileWriter::commit()`, which is
**frozen** by the current constraints, so no change has been made. It also contaminates an
authoritative gate: the branch-built MCP lane runs with the repo on 9p, so any test that saves
into the repo tree fails there for this reason.

## Resume checkpoint — Windows rebuild for GUI stress — 2026-08-16

Part 3 (interactive GUI stress) must run against **this branch**, so the Windows Release tree
had to be rebuilt first. `build/release/bin/FreeCADApp.dll` and `FreeCADGui.dll` were dated
10 Aug 2026 while every branch commit is 15–16 Aug 2026, so they could not contain
`DocumentFileWriter.cpp`, which is a new file on this branch.

Four build attempts, three distinct causes. Two were environment, one was a real defect.

### 1. `moc.exe` exits `0xC0000135` (environment)

MSVC was activated through `vcvars64.bat` but the pixi environment was not, so `moc.exe` and
`rcc.exe` could not find their Qt DLLs. `0xC0000135` is `STATUS_DLL_NOT_FOUND`, reported by
ninja as exit code `3221225781`.

**Both** activations are required, and for different reasons:

* `vcvars64.bat` — MSVC needs `INCLUDE`/`LIB` in the environment. The generated Ninja rules do
  not pass the Windows SDK include directories with `-I`.
* `pixi run` — puts the pixi environment on `PATH` so the Qt tools can load their DLLs.

Building as `pixi run cmake --build build\release -j 8 -- -k 0` from a vcvars shell satisfies
both. This cause was diagnosed once, then reintroduced by a later script that called
`cmake.exe` directly; it is recorded here so it is not rediscovered a third time.

### 2. `LNK1104: cannot open file 'bin\FreeCADGui.dll'` (environment)

A FreeCAD process held the DLL open during the link. It was not started by this work and was
not touched; it exited on its own and the link then succeeded. Note that an in-place rebuild
leaves `build/release` transiently inconsistent — `FreeCADApp.dll` can be relinked while
`FreeCADGui.dll` is still the old one. **Do not launch FreeCAD from a partially rebuilt tree**;
that App/Gui pair is mismatched.

### 3. `objidl.h(13629): error C2059: syntax error: 'string'` (real defect, fixed)

`tests/src/App/CMakeLists.txt` defines `DATADIR="${CMAKE_SOURCE_DIR}/data"` for
`App_tests_run`. The Windows SDK declares `enum tagDATADIR { ... } DATADIR;` in `objidl.h`,
which `<windows.h>` pulls in. With the macro defined, the typedef name expands to a string
literal and the SDK header fails to parse.

This only affects `tests/src/App/DocumentFileWriter.cpp` — it is the only file in that target
that includes `<windows.h>`, and it never uses `DATADIR`. `src/App/DocumentFileWriter.cpp`
includes the same headers and compiles because only the *test* target defines the macro.

Fixed by `#undef DATADIR` ahead of the Windows includes in that translation unit. This is a
Windows-only compile fix and changes no test expectation, so it does not breach the writer
freeze. It was invisible on Linux, which is where the writer lanes had been green.

## `start_freecad.py` is not a neutral launcher — isolation is mandatory

The constraint is that every GUI process starts through `python start_freecad.py`. Read before
use, that launcher does three things that collide with the constraint forbidding any contact
with the user's session or profile:

* `_ensure_mcp_addon_installed()` replaces `Mod/FreeCADMCP` in **every** user-data directory
  under `%APPDATA%\FreeCAD`, via `_remove_install()`, which deletes.
* `_ensure_mcp_auto_start()` rewrites `freecad_mcp_settings.json` to force
  `auto_start_rpc = True`.
* `_reuse_existing_mcp_if_possible()` attaches to and drives an MCP session that is already
  listening, instead of starting its own.

Run plainly against the real profile, the mandated launcher would therefore both modify the
user's profile and reuse the user's session.

**Resolution.** `_freecad_user_data_base()` resolves from `APPDATA` on Windows
(`XDG_DATA_HOME` elsewhere). Pointing that at a throwaway directory redirects the addon
install, the Mod directory, and the settings file into the isolated profile. No launcher change
is needed, and the harness verifies the redirect held by comparing
`FreeCAD.getUserAppDataDir()` against the isolated path.

**Residual risk, not solvable by environment.** `MCP_RPC_PORT = 9875` is a module constant in
the launcher with no flag or environment override, so an isolated instance still binds the port
a real session would use. The harness therefore **refuses to start** when that port is already
answering, rather than attaching to a session it does not own.

### Harness

`tests/gui/collaboration_gui_stress.py` drives the run and writes `evidence.json` plus a log
and screenshots. It asserts against the documented save contract rather than inferring it:
`saveWithOutcome()` / `saveCopyWithOutcome()` report `save_disposition`
(`written` / `unchanged` / `copy_written`), `file_written` and `durability_verified`, and every
reported disposition is cross-checked against the observed SHA-256 of the file on disk. A save
that reports `unchanged` while the bytes change — or `written` while they do not — fails the
gate.

## Conflicts are arbitrated by edit sessions, not by racing clients

The first draft of the GUI stress harness tested "same-property conflict" by having two
threads write the same property at the same time. That test was wrong and would have produced
a **false green**. Two facts make it meaningless:

* `JsonRpcListener` extends `SimpleXMLRPCServer` (no `ThreadingMixIn`) and is driven by a
  single `serve_forever()` thread (`rpc_server/server_lifecycle.py:145`), so concurrent RPC
  calls are queued, not run together.
* Mutations are marshalled onto the GUI thread anyway.

Two clients writing at once are therefore serialised by construction and can never collide.
Racing them proves nothing while reporting success.

The legacy lease surface cannot be used to test this either: every method in
`rpc_server/methods/lease_methods.py` — `acquire_document_lock`, `release_document_lock`,
`get_document_lock`, `list_document_locks`, `heartbeat_document_lock` and the rest — now
returns `_legacy_lease_authority_removed()`. Lease authority lives in the native layer.

Arbitration is done by the native edit-session protocol on `App::Document`:

    session = doc.beginEditSession(actor_id)      -> {'session_id': ...}
    doc.snapshotForEdit(session_id, revision_keys)
    prepared = doc.prepareEdit(session_id, operation_id, operation_type, arguments, provenance)
    result   = doc.commitEdit(session_id, prepared)

with `operation_type = "App.CollaborativeSetProperty"` (registered automatically by
`DocumentCollaborationService.cpp:195`) and arguments `object`, `property`, `value_type`
(`integer` / `float` / `string`), `value`.

A commit is refused when it is built on a snapshot another actor has since superseded. That
makes both properties testable **deterministically**, with no threads at all:

* *same property* — two sessions snapshot the same revision, both prepare, both commit. The
  first must commit, the second must be refused, and the losing value must not be observable
  afterwards (exactly-once).
* *independent properties* — two sessions snapshot different properties and both commit. Both
  must succeed and both values must land.

The harness now drives exactly that. The interleaved view/mutation phase is described
accurately as interleaved rather than simultaneous, for the same single-threaded-listener
reason.

## XML-RPC is retired; `start_freecad.py`'s readiness wait is broken

The first GUI stress attempt failed with `MCP RPC became ready: FAIL — timeout or early exit`
after burning the full 240 s. FreeCAD was in fact running the whole time and the MCP server was
listening. The probe was wrong, not the server.

A direct request to the default XML-RPC endpoint shows why:

    xmlrpc.client.ProtocolError: <ProtocolError for localhost:9875/RPC2: 410 Gone>

`transport/listener.py:296` (`_handle_xmlrpc_retired_post`) answers every XML-RPC POST with
`410 Gone`, `Deprecation: true`, and `Link: </jsonrpc>; rel="successor-version"`. The transport
is now **JSON-RPC 2.0 at `POST /jsonrpc`**. Verified against a live instance:

    {"jsonrpc":"2.0","id":1,"method":"ping","params":{}}    -> {"result":true}
    {"jsonrpc":"2.0","id":1,"method":"execute_code",
     "params":{"code":"print('x=1')"}}                      -> {"result":{"success":true,...}}

Both named (`{"code": ...}`) and positional (`[...]`) params are accepted. `ping` needs no
identity headers.

### This is a defect in `start_freecad.py`, not only in the harness

`_ping_mcp_rpc()`, `_mcp_rpc_proxy()`, `_mcp_rpc_process_id()` and
`_reuse_existing_mcp_if_possible()` all still construct `xmlrpc.client.ServerProxy`. Against
the current server every one of them fails, with two consequences:

* **The launcher always reports failure for a healthy server.** It waits the full
  `--mcp-timeout` (default 120 s), then prints "MCP RPC server did not respond…" and returns 1,
  even though FreeCAD is up and serving. In `--wait` mode it goes further and *terminates*
  FreeCAD on that false negative.
* **The session-reuse path can no longer trigger.** `_reuse_existing_mcp_if_possible()` cannot
  ping, so it always returns False. That accidentally removes the risk of the launcher driving
  a session it did not start — but by breakage, not by design, and it would come back the
  moment the launcher is ported to JSON-RPC.

This is recorded as a follow-up; the launcher has **not** been changed, because it is outside
the current scope and is a shared entry point.

The harness therefore passes `--no-wait-for-mcp` and waits for readiness itself over JSON-RPC.

### Two harness defects found by the same failure

* The launcher's output was captured to `subprocess.PIPE` and never drained. That discarded the
  diagnostics and risks blocking the child once the pipe buffer fills. It is now written to
  `launcher.log` in the evidence directory.
* Shutdown terminated the *launcher*, not FreeCAD. The launcher spawns FreeCAD (often through
  `pixi run`) and returns, so the GUI survived — the first smoke run leaked a live FreeCAD
  process (pid 31832) holding port 9875. Shutdown now kills the PID the RPC server reported and
  warns if anything is still answering afterwards.

## The writer had never been run on Windows, and it does not work there

Every writer gate reported green so far was **Linux-only**. The Windows Release build finally
made it possible to run the same lanes natively, and the result is:

    DocumentFileWriterTest.* + BackupPolicyTest.*   92 ran, 41 passed, 47 failed   (Windows)
    same filter                                     89 passed, 2 skipped           (Linux)

All 47 failures were `DocumentFileWriterTest`; **no** `BackupPolicyTest` failed. A plain
`Document.saveAs()` also failed outright, so nothing could be saved on Windows at all.

### Root cause 1 — the diagnostic was being destroyed (fixed)

The failure surfaced only as:

    OSError: File replacement failed with an unknown exception

`commit()` ended with `catch (const std::invalid_argument&)`, `catch (const std::exception&)`
and `catch (...)`. `Base::Exception` derives from `Base::BaseClass`, **not** from
`std::exception`, so every FreeCAD exception — including the SEH translations — skipped the
typed handlers and landed in the catch-all, which discards the message. Adding
`catch (const Base::Exception&)` immediately revealed the real error.

### Root cause 2 — a 1 MiB buffer on a 1 MiB stack (fixed)

    REPLACEMENT_PREFLIGHT_FAILED: SEH exception of type: 3221225725

`3221225725` is `0xC00000FD`, `STATUS_STACK_OVERFLOW`. `hashOnce()` and `copyAndHash()` each
declared

    std::array<char, ioBufferSize> buffer {};   // ioBufferSize == 1024 * 1024

as a **stack local**. The default thread stack is 1 MiB on Windows and 8 MiB on Linux, so this
overflowed the stack before the first read on Windows and was invisible on Linux — the platform
difference is the entire reason the Linux gates stayed green. Both buffers are now
heap-allocated (`std::vector<char>`). After the fix no SEH exception remains anywhere in the
lane.

### Still failing — 44 tests, and saving still does not work

    DocumentFileWriterTest.* + BackupPolicyTest.*   92 ran, 42 passed, 6 skipped, 44 failed

`saveAs()` now fails with a real, specific error instead of a crash:

    Unable to atomically replace the destination: The parameter is incorrect.

`ERROR_INVALID_PARAMETER` from `SetFileInformationByHandle(..., FileRenameInfo, ...)` in the
Windows replacement primitive. Ruled out so far:

* not path length — reproduces identically at `C:\fcprobe\d\p.FCStd`
* not missing `DELETE` on the source handle — the temporary is opened `GENERIC_READ |
  GENERIC_WRITE | DELETE` (line 597)
* not the `RootDirectory` handle's access mask — the pinned parent is opened
  `FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_READ_ATTRIBUTES` with `FILE_FLAG_BACKUP_SEMANTICS`
* **not** the missing terminating-null slack in the `FILE_RENAME_INFO` buffer. That was tried
  (`+ sizeof(wchar_t)` on all three allocation sites), made no difference to either the probe or
  the pass count, and was reverted rather than left in as an unverified change.

A stray `<name>.FCStd.FreeCAD-save.lock` of zero bytes is left behind on every failed save.

### Consequence for the plan

Part 3 cannot run. The GUI stress is built around saving, and saving does not work on Windows.
This is not a harness problem: it reproduces in three lines of `FreeCADCmd`, with no GUI and no
MCP involved. Closing it is a genuine body of Windows work against currently frozen code, which
is a scope decision rather than a mechanical fix.
