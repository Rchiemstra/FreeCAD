# Async LOCKED_ERROR handoff and restart-free stale recovery

This document records the as-implemented async `LOCKED_ERROR` handoff and the
implementation plan for automatic recovery of an agent's own lease. The source
claims were reviewed on 2026-07-31 against committed
`tools/mcp/freecad-mcp` revision `5fc118e`, after the field incident described
below. The superproject may carry a gitlink update, but the referenced submodule
revision itself is clean.

The handoff work described by the existing sequence diagram is implemented.
Restart-free self-recovery is a required follow-up and is **not implemented
yet**. Restarting FreeCAD or deleting a sidecar is not an acceptable normal
recovery path.

## Requirement

> An agent must automatically recover its own timeout-induced `STALE` lease
> without restarting FreeCAD and without involving the user.

"Automatically" means the recovery is performed by the MCP runtime as part of
its own lifecycle. An agent that must notice a lease problem, reason about it,
and call a repair tool has not met this requirement; it has only been given a
better manual workaround.

## Two independent failures block self-recovery

Self-recovery is currently blocked by two distinct defects. The plan must fix
both, and must fix them in the stated order, because the second one prevents
the repair for the first one from running at all.

| ID | Failure | Symptom the agent sees | Root cause |
| --- | --- | --- | --- |
| F1 | Heartbeat starvation | The lease silently becomes `STALE` while a single tool call is still running. | Every MCP tool is synchronous and is awaited inline on the asyncio event loop, so the heartbeat coroutine cannot be scheduled for the whole call. |
| F2 | Identity-registration wedge | `[LEASE_SERVICE_ERROR] live document identity could not be registered` on any selector-based tool, including `save_document`. | The registered live proxy's saved-file identity drifted, and no repair path covers a drifted document that is still leased. |

F2 is not a consequence of F1 and can occur alone, but the two compose badly:
the reconcile RPC that repairs F1 resolves its own selector through the same
registration path that F2 breaks. While a document is wedged by F2, F1 is
unrecoverable in-process even after F1's own gates are relaxed.

### F1: synchronous tools starve the heartbeat

- `_lease_heartbeat_loop` awaits `asyncio.sleep` on the MCP event loop
  (`src/freecad_mcp/server.py:420-426`) at `_LEASE_HEARTBEAT_INTERVAL_S = 10.0`
  seconds with 0.8-1.2 jitter (`src/freecad_mcp/server.py:234`).
- The service marks a lease `STALE` after
  `DEFAULT_STALE_AFTER_SECONDS = 90.0`
  (`addon/FreeCADMCP/document_lease/service.py:48`).
- All 170 `@mcp.tool` functions in `src/freecad_mcp/server.py` are declared
  with `def`, not `async def`. The SDK's
  `FuncMetadata.call_fn_with_arg_validation` ends in `if fn_is_async: return
  await fn(...) else: return fn(...)`, so a synchronous tool body executes
  inline on the event-loop thread and no coroutine on that loop runs until the
  tool returns. This was verified against the installed `mcp` package, not
  inferred.
- The existing offload is not a mitigation in practice.
  `InstrumentedFastMCP.call_tool` only routes `HEAVY_TASK_TOOLS` through
  `experimental.run_task` when the *client* supplies task metadata
  (`src/freecad_mcp/instrumented_server.py:299-303`). Clients that do not opt
  into MCP experimental tasks take the blocking path for every tool, including
  `execute_code` and `save_document`.

The consequence is deterministic, not probabilistic: any single tool call
exceeding 90 seconds guarantees that the caller's own lease goes `STALE`. The
field incident below was a 106-second measurement loop.

### F2: a leased document that drifted has no identity repair

- `_ensure_v2_document` raises `DocumentIdentityError("live document identity
  could not be registered")` whenever `register_live_document_recovery`
  returns `None` (`addon/FreeCADMCP/rpc_server/rpc_server.py:978-1006`).
- `register_live_document_recovery` returns `None` from two branches
  (`addon/FreeCADMCP/document_lease/observer.py:257-332`): registration failed
  and neither `rebind_closed_recovery_document` nor
  `refresh_orphaned_foreign_document_identity` applied, or the post-
  registration `inspect_registered_document` evidence check failed.
- `register_document` raises `IdentityMismatchError` for an already-registered
  proxy whose observed `name`, `comparison_key`, or `file_identity` changed
  (`addon/FreeCADMCP/document_lease/identity.py:225-244`).
- On Windows, `file_identity_for_path` is the NTFS volume serial plus file
  index (`addon/FreeCADMCP/document_lease/identity.py:133-162`). FreeCAD saves
  by atomic replacement, so an ordinary in-place save at an unchanged path
  changes the file index and therefore the file identity.
- A narrow repair for exactly that case exists, but
  `refresh_local_recovery_document_identity` refuses any record that is not
  `USER_INTERVENED` or `UNLOCKED_DIRTY`
  (`addon/FreeCADMCP/document_lease/service.py:3843-3878`, "saved-file
  identity can refresh only after takeover"). A `STALE`, `LOCKED_IDLE`, or
  `LOCKED_ERROR` record is therefore stranded.
- The blast radius is larger than one document. The session-uuid selector
  branch calls `_ensure_v2_document` on every open document while scanning
  (`addon/FreeCADMCP/rpc_server/rpc_server.py:1039-1043`) and lets the
  exception escape rather than skipping the candidate, so one wedged document
  breaks selector resolution for all of them.

### Field incident, 2026-07-31

An agent made a correct geometry edit and then ran a wall-clearance probe at a
0.005 mm step. The probe took 106 seconds, exceeded the 90-second stale
threshold while holding the event loop, and killed its own lease while the
document was still dirty. The user then pressed `Ctrl+S` to protect the live
geometry. That content-changing, unscoped GUI save put the work on disk but was
also genuine local intervention: it changed the saved-file identity and
baseline outside the stale owner's authenticated flow. Later selector-based
recovery failed with `[LEASE_SERVICE_ERROR] live document identity could not be
registered`, and cleanup required deleting
`HamaAdapter.FCStd.freecad-mcp.lock` and restarting FreeCAD, in that order.

This is the exact outcome the requirement forbids. It is also the second
occurrence of the same identity wedge, which is why F2 is treated as a
first-class defect here rather than as a footnote to F1.

## Current implementation status

| Area | Status | Current behavior | Evidence |
| --- | --- | --- | --- |
| Async dirty `LOCKED_ERROR` handoff | Implemented | An eligible dirty error lease can be authorized, prepared, claimed, and acknowledged through the continuation workflow below. | `rpc_server.py` handoff continuation; suite below |
| Heartbeat loop | Implemented but starvable | Heartbeats are scheduled on the same event loop that runs every synchronous tool body, so any long tool suspends them entirely. | `server.py:234`, `server.py:420-426` |
| Expiry watchdog | Implemented | FreeCAD independently changes an expired lease to `STALE` and persists that fenced state. It deliberately does not silently delete the lease. | `service.py:48` |
| Credential retention across `STALE` | Implemented, and correct | `STALE` is absent from `_REVOCATION_ERROR_CODES`, so a stale heartbeat result does not discard the MCP-side credential. Self-recovery is therefore possible without any new secret exchange. | `lease_manager.py:180-188`, `lease_manager.py:428-497` |
| Exact-owner stale reconcile, service layer | Implemented, dirty-capable | `LeaseService.reconcile_stale` accepts a dirty record; it requires the live modified flag to equal `record.dirty` and the fresh baseline to equal the persisted baseline. It does not require a save after the last mutation. | `service.py:4284-4322`, `service.py:848-928` |
| Exact-owner stale reconcile, RPC layer | Implemented, but blocks dirty work | `lease_reconcile` adds two gates the service does not: a saved verified baseline must exist, and `last_verified_save_revision` must be at least `last_mutation_revision`. The second gate rejects precisely the dirty case that matters. | `rpc_server.py:4041-4053` |
| Reconcile reachability under F2 | Broken | `lease_reconcile`'s prepare phase resolves its own selector through `_live_document_from_selector`, so a document wedged by F2 cannot be reconciled by any means. | `rpc_server.py:4026` |
| Identity refresh for a leased document | Missing | `refresh_local_recovery_document_identity` is restricted to `USER_INTERVENED` and `UNLOCKED_DIRTY`. `STALE`, `LOCKED_IDLE`, and `LOCKED_ERROR` records have no in-place-save identity repair. | `service.py:3843-3878` |
| Automatic recovery orchestration | Missing | `FreeCADClient.reconcile_document_lease` exists but has no caller anywhere in `src/`. Nothing detects a self-inflicted `STALE` state and repairs it. | `freecad_client.py:1146-1170` |
| Save while stale | Unsafe as normal recovery | An unscoped GUI save is treated as local user intervention and can rotate ownership. Saving first also changes the file identity and the baseline, which converts an F1-only failure into F1 plus F2. | `observer.py:558-568`, `service.py:3843-3878` |
| Restart-free requirement | Not met | Reacquisition conflicts with the persisted stale lease, and remote force release is intentionally unavailable. This is why the flow still ends in a FreeCAD restart. | Field incident above |

### Existing handoff review

- The focused lease/handoff suite passes: 225 tests across
  `test_rpc_dirty_adoption.py`, `test_rpc_request_idempotency.py`,
  `test_lease_manager.py`, `test_mcp_rpc_v2_lifecycle.py`, and
  `test_document_lease_v2_service.py`.
- The implementation checkpoint passed `compileall`. A clean submodule diff is
  not treated as additional evidence; the committed revision and focused test
  result are the relevant provenance.
- The default handoff authorization hook always returns `true`; there is no
  FreeCAD confirmation popup. The `denied` continuation state remains as a
  defensive/testable branch.
- Handoff eligibility is based on an eligible local dirty `LOCKED_ERROR`
  record and its recovery evidence; the service does not enforce that the
  replacement `agent_id` or MCP runtime differs from the recorded owner.
- `cancel_request` is currently registered as a model-facing MCP tool. The
  lower-level client/RPC docstrings that still call it unexposed are stale
  comments, not runtime behavior.
- Exact continuation states are `pending_authorization`, `authorizing`,
  `hashing`, `claiming`, `claim_committed`, `claiming_uncertain`, `claimable`,
  `cancelled`, `failed`, `denied`, and `claimed`.
- The credential vault is process-local. Its constructor still accepts the
  legacy defaults of 256 entries and a 600-second TTL, but neither limit evicts
  an unacknowledged credential. Such an entry remains until custody is
  acknowledged, it is explicitly discarded, or the FreeCAD process restarts.
  A claimable continuation can therefore observe a missing entry after restart
  or explicit discard, but not from TTL or capacity expiry.
- `invoke_v2` finalizes the original acquisition handler before MCP-local
  credential storage and the separate custody acknowledgement. The replay
  cache never stores a raw acquisition token.
- Stale reconcile already has refusal-path coverage:
  `tests/test_rpc_stale_reconcile.py` covers the two-GUI-check hash window,
  same-size/same-mtime tampering, unstable hash capture, and a sidecar
  authority change during hashing. `test_document_lease_v2_service.py:3489`
  covers exact-owner reconcile of a *saved* lease. There is no test for a
  dirty exact-owner reconcile, none for a drifted identity, and none for
  automatic orchestration.

## Required outcome

An MCP runtime must recover its own lease without closing or restarting
FreeCAD when all of the following remain true:

- The lease became `STALE` only because its heartbeat expired.
- The runtime presents the exact lease ID, generation, token, agent identity,
  and MCP instance identity recorded by FreeCAD.
- The document identity, sidecar authority, recorded dirty state, and on-disk
  baseline when one exists still agree with the live document. A never-saved
  document must satisfy the in-memory continuity proof in D5 instead.
- No local user action, competing claimant, ownership rotation, or newer
  generation has intervened.

Successful recovery must atomically change `STALE` to `LOCKED_IDLE`, refresh
the heartbeat deadline, and preserve the same credential, generation, dirty
state, recovery snapshot, and save baseline. A dirty document must not need to
be saved merely to prove that the exact owner is allowed to continue.

A live document must also remain addressable across a baseline-preserving
in-place rewrite. Where the saved-file identity is the only thing that changed,
where the registered live proxy object is unchanged, where the document name
and canonical path are unchanged, and where the content still matches the
accepted baseline, registration must repair itself and report the repair rather
than failing every subsequent selector-based call.

A content-changing save performed outside the authenticated typed lease flow
is user intervention, not identity-only drift. It must remain a takeover or
explicit recovery case. In the field incident, pressing `Ctrl+S` after the
lease was already `STALE` changed the file and crossed this boundary. The new
flow prevents that circular workaround by recovering first and then saving
through the normal typed lease path; it does not silently reclaim ownership
after an out-of-band content-changing save.

Recovery must fail closed for `USER_INTERVENED`, a foreign owner, a token or
generation mismatch, a changed document identity, a changed saved file, or
inconsistent in-memory and sidecar records. Those cases require an explicit
handoff or local recovery decision; they must never be relabelled as a simple
timeout recovery.

### Non-goals

These are deliberately excluded so the recovery path stays narrow:

- No takeover, no replacement owner, no fencing-token rotation.
- No sidecar deletion and no suppression of recovery evidence.
- No implicit save, and no clearing of a dirty flag, to satisfy a gate.
- No agent-facing "repair my lease" tool as the primary mechanism. A
  diagnostic tool may exist, but the requirement is met only by automatic
  in-runtime recovery.
- No relaxation of a `Save As`, reload, or replacement-proxy rebind into the
  automatic identity-refresh path.

## New plan: restart-free exact-owner self-recovery

The solution is layered. Each layer is independently useful, and each one
reduces how often the next layer has to run.

1. **Do not go stale.** Run blocking tool bodies off the asyncio event-loop
   thread so scheduled heartbeats keep the lease alive during long calls.
2. **Stay addressable.** Repair the narrow in-place-save identity drift
   automatically for any leased state, so the document can still be resolved
   by selector. Without this, layer 3 cannot even be invoked.
3. **Recover the lease.** Treat a genuine heartbeat race as recoverable: the
   MCP lease manager uses its retained exact credential to request one guarded
   reconcile, including for a dirty document.
4. **Do it without being asked.** Drive layers 2 and 3 from the MCP runtime's
   own lifecycle, not from an agent noticing an error.

The recovery transitions are:

```text
LOCKED_* -- heartbeat deadline expires --> STALE
STALE + exact owner + unchanged authority + no intervention
    -- guarded compare-and-swap --> LOCKED_IDLE
STALE + any ownership or document mismatch
    -- no mutation --> explicit recovery/handoff error

registered proxy unchanged + name unchanged + path unchanged
    + only file_identity changed + on-disk baseline still accepted
    -- exact-proxy identity refresh --> registration succeeds
any other identity change
    -- no repair --> DocumentIdentityError, explicit rebind required
```

Both paths are narrower than takeover. Neither creates a replacement owner,
rotates the fencing token, suppresses evidence, clears dirty state, or removes
a sidecar. They restore liveness only when the existing owner proves
continuity.

## Design decisions

| ID | Decision | Consequence |
| --- | --- | --- |
| D1 | Prevention and recovery are separate requirements. | Moving blocking tools off the event loop handles the normal case; exact-owner reconcile still handles watchdog and scheduling races. Neither substitutes for the other. |
| D2 | Exact-owner recovery is not takeover. | The lease ID, generation, token, agent ID, and MCP instance ID must all match. Recovery retains them and performs only the guarded `STALE -> LOCKED_IDLE` transition. |
| D3 | The service owns the safety invariant. | RPC may collect and normalize evidence, but it must not add a stricter saved-after-last-mutation gate that contradicts dirty recovery already accepted by `LeaseService.reconcile_stale`. |
| D4 | Baseline-preserving identity drift is repairable; content-changing out-of-band saves are not. | An atomic rewrite with the same proxy, name, path, size, mtime, and SHA-256 may refresh `file_identity`. A changed hash, `Save As`, reload, or replacement proxy remains user intervention and fails closed. |
| D5 | Never-saved dirty documents use in-memory continuity evidence. | They have no disk baseline. Recovery must instead require the same registered proxy and session UUID, exact credential, unchanged document name, matching dirty state and mutation revision, intact recovery snapshot, and no observer-recorded user intervention. This case receives its own tests and cannot fall through a `baseline is None` branch accidentally. |
| D6 | Cancellation has a dedicated control lane. | General synchronous tools use one serialized worker lane. `cancel_request` and other explicitly allowlisted bounded control operations use a separate single-worker control lane, so they never queue behind a long `execute_code`. Heartbeats remain on the event loop. |
| D7 | Automatic recovery does not imply blind mutation replay. | The runtime may retry only when the rejected request is proven not to have started. Otherwise it restores the lease and returns a stable retryable result. |
| D8 | Recovery is serialized per document and observable without secrets. | Heartbeat, post-tool, and pre-operation triggers share one async recovery lock and stable reason codes; logs and model-facing results never contain a raw token. |
| D9 | One coordinator owns integration and the progression record. | Cursor subagents may work in isolated worktrees, but each production file has one owner per wave. Only the coordinator merges branches, resolves overlap, and updates this document's progress states. |

## Implementation plan

### Phase 1: keep heartbeats schedulable

- Route synchronous MCP tool execution through a worker thread from the
  project-owned server wrapper, `InstrumentedFastMCP._call_registered_tool` in
  `src/freecad_mcp/instrumented_server.py`. Do not patch the installed MCP
  package and do not rewrite 170 tool signatures.
- Keep the existing `HEAVY_TASK_TOOLS`/`experimental.run_task` branch as-is;
  it is an optimization for clients that opt in, not the fix. The worker-thread
  path must be unconditional so that clients without task metadata are covered.
- Keep async tools on the event loop and keep FreeCAD GUI work on its existing
  GUI-thread dispatch path.
- Use one serialized worker for ordinary synchronous tools and a separate
  single-worker control lane for `cancel_request` and other explicitly
  allowlisted bounded control operations. A cancellation request must not sit
  behind the long-running tool it is intended to cancel. Control-lane code must
  not perform unbounded work or synchronously wait on the FreeCAD GUI thread.
- Preserve context variables, request cancellation, exception mapping, and
  request-finalization behavior across the worker boundary. `contextvars` must
  be propagated explicitly; `bind_context` telemetry and
  `document_lock.get_request_identity` both depend on it.
- Bound concurrency so the change does not turn a serialized tool stream into
  parallel GUI-thread contention. Serial execution with a non-blocking loop is
  the goal, not parallelism.
- Add a timing test proving that heartbeats continue while a synchronous tool
  runs longer than the configured lease timeout.

### Phase 2: repair in-place-save identity drift automatically

This phase is new and is a prerequisite for Phase 4's orchestration, because
`lease_reconcile` resolves its own selector through the path that fails here.

- Extend the exact-proxy refresh so a leased record can be repaired, not only
  a `USER_INTERVENED` or `UNLOCKED_DIRTY` one. The safe predicate is: same
  registered proxy object, same document name, same comparison key, only
  `file_identity` changed, and the on-disk file still matches the record's
  accepted baseline by size, mtime, and SHA-256.
- Apply the repair from `register_live_document_recovery` so it covers every
  selector-based call, alongside the existing `rebind_closed_recovery_document`
  and `refresh_orphaned_foreign_document_identity` branches.
- Refuse and keep failing closed when the name or path changed, when the proxy
  object is not the registered one, when the content hash no longer matches the
  accepted baseline, or when no baseline exists. `Save As`, reload, and
  replacement-proxy rebinds keep their explicit workflows.
- Treat a content-changing out-of-band save as user intervention. Phase 2
  repairs only a baseline-preserving atomic rewrite; it does not adopt new file
  content or recover a lease after `Ctrl+S` changed that content while stale.
- Stop one wedged document from poisoning unrelated ones: the session-uuid
  scan in `_live_document_from_selector` must skip a candidate that cannot be
  registered instead of letting the exception escape the loop, while still
  returning the precise identity error when the *selected* document is the one
  that failed.
- Emit a structured, token-free record of every automatic identity refresh.
  A silent identity repair is as hard to diagnose as a silent failure.

### Phase 3: allow dirty exact-owner stale recovery

- The service layer already permits this. `LeaseService.reconcile_stale`
  requires `validation.document_modified == record.dirty` and an exactly
  matching baseline; it does not require a post-mutation save. No service
  change is needed for the dirty case.
- Remove the RPC-only gate at `rpc_server.py:4041-4053` that requires
  `last_verified_save_revision >= last_mutation_revision`. That gate is what
  rejects unsaved work, and it has no counterpart in the service contract it
  fronts.
- Keep the requirement that a *saved* document has a verified baseline, keep
  both GUI-phase identity checks, keep the off-GUI hash between them, and keep
  `_assert_mutation_file_metadata_unchanged`.
- Implement the never-saved rule from D5 explicitly. With no canonical path or
  disk baseline, require the same registered proxy and session UUID, unchanged
  name, exact credential, matching dirty state and mutation revision, intact
  recovery snapshot, and no observer-recorded user intervention.
- Commit `STALE -> LOCKED_IDLE` with a compare-and-swap and a refreshed
  heartbeat deadline while retaining recovery evidence and dirty metadata.
- Make the operation idempotent: repeating it with the same valid credential
  after success returns the already-recovered lease rather than a state error.

### Phase 4: automatic MCP recovery orchestration

- Give `FreeCADClient.reconcile_document_lease` a caller. Today it has none in
  `src/`, which is the single clearest reason the requirement is unmet.
- Retain the credential after a timeout-state error. This already holds:
  `STALE` is not in `_REVOCATION_ERROR_CODES`. Add a regression test so a
  future edit cannot quietly add it and disable self-recovery.
- Trigger reconciliation from three places, all inside the runtime:
  1. `_lease_heartbeat_once`, when a batch result reports a held lease as
     `STALE`;
  2. immediately after a tool call returns, in the instrumented `call_tool`
     wrapper, when the call outlasted the stale threshold;
  3. lazily before the next protected operation, if neither of the above ran.
- Serialize recovery per document with an async lock so the heartbeat task and
  the tool-completion path cannot both reconcile the same lease.
- Bound and back off. A refused reconcile must not retry in a tight loop; a
  refusal that proves ownership loss must stop attempts entirely.
- Do not blindly replay a mutation. Retry only when the response proves that
  the rejected operation never started; otherwise recover the lease and return
  a precise retryable result.

### Phase 5: improve status and recovery guidance

- Report whether stale recovery was attempted, succeeded, refused, or remains
  unnecessary, using stable structured reason codes.
- Make the identity failure self-describing. `live document identity could not
  be registered` names neither the document, nor which of the two `None`
  branches fired, nor what changed. It should report the drifted field and
  whether an automatic refresh was attempted and refused.
- Remove restart/delete-sidecar advice for an eligible exact-owner heartbeat
  timeout.
- Keep local recovery UI for genuine user intervention and abandoned owners,
  clearly separate from automatic exact-owner recovery.
- Ensure a normal GUI save is never a prerequisite for timeout recovery.

### Phase 6: verify failure and race boundaries

- Cover clean and dirty exact-owner stale recovery in service, RPC, client,
  and MCP integration tests. The dirty case currently has no test at any layer.
- Cover automatic identity refresh: accept an atomic in-place save under a
  live lease; refuse a `Save As`, a replaced proxy, a changed content hash, and
  a missing baseline.
- Cover the recoverable composite failure end to end: perform a
  baseline-preserving atomic rewrite, exceed the timeout, confirm identity and
  lease recovery without agent action, and only then save through the typed
  lease path.
- Cover the field-incident boundary separately: a content-changing `Ctrl+S`
  after the lease is stale must remain user intervention and must not be
  reclaimed automatically.
- Use an accelerated timeout to test a synchronous call longer than the lease
  TTL while confirming continued heartbeat progress.
- Force a watchdog race after the final heartbeat and confirm automatic
  recovery before the next protected action.
- Verify refusal after a local save/takeover, foreign token, newer generation,
  changed document identity, changed disk baseline, conflicting sidecar, or
  real owner-process death.
- Verify that cancellation, request idempotency, handoff continuations, and
  credential redaction retain their existing behavior across the new worker
  boundary.

### Phase 7: release and operational validation

- Run the focused lease/handoff suite, new stale-recovery tests, `compileall`,
  and whitespace checks.
- Perform a real FreeCAD smoke test: make an unsaved edit, run a tool beyond
  the timeout, observe recovery, continue editing, save through the typed
  lease path, and confirm the document remains valid on disk.
- Re-run the safe replacement for the field-incident workflow: a >90 s probe
  on a dirty document, automatic exact-owner recovery, then `save_document`,
  with no restart, GUI save, or sidecar deletion.
- Record recovery attempts and outcomes without tokens so a future incident
  can be diagnosed from health output rather than inferred from GUI state.

## Progression

The table distinguishes analysis and planning already completed from code that
still has to be implemented. A row moves to `Complete` only when its exit
criterion is demonstrated.

| ID | Work item | Progress | `/multitask` lane | Exit criterion |
| --- | --- | --- | --- | --- |
| P0 | Reproduce and locate the timeout failure | Complete | Coordinator | The event-loop starvation, watchdog transition, missing automatic reconcile call, and dirty-baseline gate are identified with file-level evidence. |
| P1 | Define the restart-free recovery contract | Complete | Coordinator | This document states the eligible transitions, preserved state, non-goals, and fail-closed boundaries. |
| P1a | Locate the identity-registration wedge | Complete | S2/S3 review | The two `register_live_document_recovery` `None` branches, the NTFS file-index drift on atomic save, and the leased-state exclusion in `refresh_local_recovery_document_identity` are identified. |
| P1b | Resolve design and multitask decisions | Complete | Coordinator | D1-D9 define save boundaries, never-saved recovery, cancellation isolation, ownership, merge order, and maximum useful concurrency. |
| P2 | Isolate synchronous tools from the heartbeat event loop | Not started | S1 | A non-task-aware client can run a tool beyond the lease TTL while heartbeats and the independent cancellation lane remain responsive. |
| P3 | Baseline-preserving in-place identity refresh | Not started | S2 | A live document remains resolvable across a content-identical atomic rewrite, while changed content, `Save As`, or a replaced proxy still fails closed. |
| P3a | Stop one wedged document poisoning selector resolution | Not started | S3 | An unregisterable document does not break resolution of a different, healthy document. |
| P4 | Implement dirty exact-owner `STALE` recovery | Not started | S3 | The same credential recovers saved and never-saved unsaved work to `LOCKED_IDLE` without saving, takeover, or token rotation. |
| P5 | Add automatic MCP recovery orchestration | Not started | S4 | Heartbeat, post-tool, and pre-operation paths reconcile an eligible stale lease with no agent tool call and no user action. |
| P6 | Update status, diagnostics, and GUI guidance | Not started | S5 | Eligible timeouts no longer instruct the user to restart FreeCAD or delete a sidecar, and identity errors name what drifted. |
| P7 | Add boundary, race, and regression tests | Not started | S1-S6 | All success/refusal cases, the safe field-workflow regression, and all 225 existing focused tests pass deterministically. |
| P8 | Validate in a real dirty FreeCAD document | Not started | One live validator | The agent edits, exceeds the timeout, self-recovers, continues, and saves successfully in one FreeCAD session with no restart. |

### Cursor `/multitask` concurrency budget

Cursor has published support for
[up to eight agents in parallel on one prompt](https://cursor.com/changelog/2-0),
and [`/multitask` can split a plan across async
subagents](https://cursor.com/changelog/04-24-26). The `/multitask` announcement
does not publish a separate, higher numeric subagent limit. This plan therefore
uses a conservative maximum of **eight active agents total: one coordinator
plus seven worker subagents**. If the account or Cursor UI exposes fewer
concurrent slots, reduce each wave to that lower limit without changing the
dependency order.

`7 (MAX)` below means all seven worker slots are useful and may be launched in
the same `/multitask` wave. It is not a direction to add artificial agents to a
serial or shared-state step.

### Subagent workstreams

| Lane | Responsibility | Exclusive production-file ownership in the wave |
| --- | --- | --- |
| S1 | Event-loop offload, serialized general worker, separate cancellation/control worker | `src/freecad_mcp/instrumented_server.py` and its focused tests |
| S2 | Exact-proxy identity refresh and in-memory continuity evidence | `addon/FreeCADMCP/document_lease/observer.py`, `identity.py`, and identity-specific service code |
| S3 | RPC reconcile gates, selector-scan isolation, saved/never-saved evidence normalization | `addon/FreeCADMCP/rpc_server/rpc_server.py` and focused RPC tests |
| S4 | Heartbeat, post-tool, and pre-operation automatic recovery orchestration | `src/freecad_mcp/lease_manager.py`, `freecad_client.py`, `server.py`, and focused client tests |
| S5 | Structured status, reason codes, health output, and local GUI guidance | Status/UI modules assigned by the coordinator, including `lock_indicator.py` when needed |
| S6 | New end-to-end and cross-layer regression tests | New integration-test files only; no production files and no test file already owned by S1-S5 |
| S7 | Security, race, and fencing review | Read-only; reports findings to the coordinator and never edits another lane's files |

Interfaces from D1-D9 are frozen for the first build wave. A subagent that
needs to change another lane's contract reports the request instead of editing
that lane's files.

### `/multitask` waves and maximum subagents

| Wave | Work | Maximum worker subagents | Start condition | Finish condition |
| --- | --- | --- | --- | --- |
| M0 | Independent source/design verification by S1-S7 | **7 (MAX)** | Current committed source available | Findings reconciled into D1-D9; this wave is complete. |
| M1 | Isolated implementation by S1-S6 plus continuous read-only review by S7 | **7 (MAX)** | D1-D9 frozen and each lane has an isolated worktree | Every lane supplies a scoped diff and focused test result; no shared production file was edited by two lanes. |
| M2 | Parallel verification on the coordinator's integrated branch: runtime, identity, reconcile, orchestration, handoff regression, static checks, and security review | **7 (MAX)** | Coordinator has merged M1 in the order below | All seven reports agree on the same integrated commit. Test agents do not patch failures. |
| M3 | Cross-layer fixes discovered by M2 | **2** | M2 failures are grouped by owning file | Focused failures pass and the coordinator has resolved all overlapping changes. |
| M4 | Real FreeCAD dirty-document smoke test | **1** | Automated suite is green | One validator completes the P8 workflow without restart, GUI save, or sidecar deletion. |
| M5 | Final diff, documentation, and progression audit | **2** | M4 passes | One code auditor and one documentation auditor report clean results; coordinator marks completed rows. |

M1 and M2 are the two places where this plan intentionally uses the maximum
seven subagents. M4 intentionally does not: multiple agents must not control
the same FreeCAD process, document, lease, or sidecar concurrently.

### Merge order and coordination rules

1. The coordinator creates or confirms isolated worktrees before M1 and gives
   every subagent its lane, exact file ownership, and acceptance tests.
2. Merge S1, then S2, S3, S4, S5, and finally S6. S7 is a review gate and has
   no branch to merge.
3. After each merge, run that lane's focused tests on the integrated branch.
   M2 begins only after all six implementation lanes are integrated.
4. M2 agents are read-only test/review workers. They report commands, commit,
   failures, and artifacts; the coordinator assigns any code fix in M3.
5. Only the coordinator updates progression states or resolves merge
   conflicts. Subagents must not mark their own work `Complete`.
6. Only one agent may operate the live FreeCAD GUI during M4.

## Acceptance criteria

The requirement is complete only when all of these statements are true:

- A synchronous MCP call lasting longer than 90 seconds does not starve lease
  heartbeats, for any client, regardless of whether it opts into MCP tasks.
- If scheduling or a watchdog race still produces `STALE`, the owning runtime
  recovers it automatically, with no agent tool call and no GUI action.
- A baseline-preserving atomic in-place rewrite never renders a live document
  unaddressable, and `save_document` on a recovered dirty document succeeds.
- A content-changing out-of-band save remains user intervention and is not
  automatically reclaimed by the former owner.
- Unsaved in-memory changes, dirty metadata, recovery evidence, generation,
  and fencing token survive recovery unchanged.
- The recovered agent can continue with a protected operation and then save
  using the normal typed lease flow.
- Recovery never deletes a sidecar and never converts genuine user
  intervention or competing ownership into agent ownership.
- No documented recovery procedure for an eligible self-inflicted timeout
  mentions restarting FreeCAD or deleting a `.freecad-mcp.lock` sidecar.
- The existing 225 focused async `LOCKED_ERROR` handoff, cancellation,
  idempotency, and credential-redaction tests remain green.

## Known risks

- **Worker-thread migration is the largest behavioral change.** Moving 170
  synchronous tools off the event loop touches cancellation, `contextvars`
  propagation, and telemetry binding at once. It should land before the
  recovery phases and be validated on its own, because a context-propagation
  bug there would silently break lease attribution rather than fail loudly.
- **Relaxing the RPC dirty gate widens the reconcile surface.** The gate is
  removable because the service enforces the real invariants, but that must be
  demonstrated by test, not assumed from reading `reconcile_stale`.
- **Automatic identity refresh must not become a general rebind.** The
  predicate is deliberately narrow. If it starts accepting name or path
  changes, a `Save As` could be mistaken for continuity of the original file.
- **Recovery loops.** Three trigger points plus a per-document lock is enough
  to cause repeated attempts against a permanently refused lease. Back-off and
  a terminal refusal state are required, not optional polish.

## Target self-recovery sequence (not yet implemented)

This diagram describes the intended end state of phases 1 to 5. It is a design
target, not a record of current behavior. The agent appears only at the start
and the end: everything between the two is runtime lifecycle work.

```mermaid
sequenceDiagram
    autonumber

    actor Agent
    participant MCP as MCP Server
    participant W as Tool Worker Thread
    participant HB as Heartbeat Task
    participant LM as MCP Lease Manager
    participant RPC as FreeCAD RPC
    participant IDS as Identity Service
    participant LS as Lease Service

    Note over Agent,LS: Phase 1 - a long tool no longer blocks the event loop

    Agent->>MCP: Long protected tool call
    MCP->>W: Run the synchronous body off the event loop
    loop Every ~10 s while the tool runs
        HB->>RPC: lease_heartbeat_batch(held credentials)
        RPC->>LS: Refresh deadlines
        LS-->>HB: LOCKED_* renewed
    end
    W-->>MCP: Tool result

    Note over HB,LS: Normal case ends here: the lease never went STALE

    opt Watchdog still wins a scheduling race
        LS->>LS: Deadline expired, persist LOCKED_* -> STALE

        alt Heartbeat observes the stale lease first
            HB->>RPC: lease_heartbeat_batch
            LS-->>HB: state=STALE for a held lease
            HB->>LM: apply_heartbeat_response
            Note over LM: STALE is not a revocation code,<br/>the exact credential is retained
            HB->>HB: Take the per-document recovery lock
        else The tool returns first
            MCP->>MCP: Call outlasted the stale threshold
            MCP->>MCP: Take the per-document recovery lock
        else Neither ran and a later call arrives
            Agent->>MCP: Next protected tool call
            MCP->>MCP: Lazy pre-operation recovery check
        end

        Note over MCP,LS: Exactly one holder proceeds; the others observe<br/>the recovered lease and skip

        MCP->>RPC: reconcile_document_lease(retained credential)
        RPC->>IDS: Resolve selector, register live proxy

        alt Only file_identity drifted on the same proxy
            Note over IDS: Phase 2 - atomic in-place save changed<br/>the NTFS file index at an unchanged path
            IDS->>IDS: Verify same proxy, same name, same path,<br/>on-disk file still matches the accepted baseline
            IDS-->>RPC: Refreshed identity, refresh recorded
        else Name, path, or proxy changed, or content no longer matches
            IDS-->>RPC: DocumentIdentityError naming the drifted field
            RPC-->>MCP: Fail closed, explicit rebind required
            MCP-->>Agent: Precise identity error, no restart advice
        else Identity is unchanged
            IDS-->>RPC: Registered identity
        end

        RPC->>LS: authorize(credential, allowed_states=STALE)

        alt Foreign owner, token/generation mismatch, or USER_INTERVENED
            LS-->>RPC: Authorization refused
            RPC-->>MCP: Terminal refusal with a stable reason code
            MCP->>LM: Stop retrying this lease
            MCP-->>Agent: Handoff or local recovery required
        else Exact owner
            LS-->>RPC: STALE record
            RPC->>RPC: Hash the saved file off the GUI thread
            RPC->>LS: Second GUI-thread identity and authority check

            alt Baseline or authority changed during hashing
                LS-->>RPC: Mismatch
                RPC-->>MCP: Remains STALE, no mutation
                MCP-->>Agent: Retryable or terminal per reason code
            else Evidence is exact
                Note over RPC,LS: Phase 3 - the dirty case is allowed here.<br/>document_modified must equal record.dirty,<br/>no post-mutation save is required
                RPC->>LS: reconcile_stale(credential, validation)
                LS->>LS: CAS STALE -> LOCKED_IDLE,<br/>refresh deadline, retain generation,<br/>token, dirty state, and evidence
                LS-->>RPC: Recovered lease
                RPC-->>MCP: Recovered
                MCP->>HB: Resume normal heartbeats
            end
        end
    end

    Agent->>MCP: save_document(selector)
    MCP-->>Agent: Saved through the normal typed lease flow
```

## Existing as-implemented LOCKED_ERROR handoff sequence

```mermaid
sequenceDiagram
    autonumber

    actor Agent
    participant MCP as MCP Server
    participant LM as MCP Lease Manager
    participant RPC as FreeCAD RPC
    participant IR as Inflight Request Registry
    participant HC as Handoff Continuation
    participant GUI as FreeCAD GUI Thread
    participant LS as Lease Service
    participant Vault as Credential Vault

    Note over Agent,Vault: Entry and synchronous adoption/acquisition

    Agent->>MCP: adopt_dirty_document(selector)
    Note over MCP,LM: The same successful custody/redaction path also applies<br/>to acquire_document_lock success responses
    MCP->>RPC: invoke_v2(adopt_dirty_document, acquisition_request_id,<br/>live_request_ids, authenticated)
    RPC->>IR: Register live invoke_v2 request
    RPC->>GUI: Resolve selector and inspect document

    alt Selector is invalid, missing, or ambiguous
        GUI-->>RPC: Resolution failure
        RPC->>IR: Finalize request as failed
        RPC-->>MCP: Terminal error, no credential
        MCP-->>Agent: Failure, no credential

    else Document is not dirty or the adoption precondition is false
        GUI-->>RPC: Nothing eligible to adopt
        RPC->>IR: Finalize request as failed
        RPC-->>MCP: Terminal precondition failure, no credential
        MCP-->>Agent: Tool failure, no lease acquired

    else Document is locked in a non-handoff-eligible state
        GUI-->>RPC: Lock conflict or ownership denial
        RPC->>IR: Finalize request as denied/conflict
        RPC-->>MCP: Terminal denial, no credential
        MCP-->>Agent: Lock not acquired

    else Unlocked eligible document
        Note over GUI,LS: adopt uses begin_dirty_adoption for a dirty document,<br/>acquire uses begin_acquisition for a clean document
        GUI->>LS: Begin ACQUIRING reservation(document identity,<br/>acquisition_request_id, live_request_ids)

        alt Reservation or fencing check rejects acquisition
            LS-->>GUI: Conflict or denial
            GUI-->>RPC: Acquisition not started
            RPC->>IR: Finalize request as failed/denied
            RPC-->>MCP: Terminal result, no credential
            MCP-->>Agent: Adoption/acquisition failed

        else ACQUIRING reservation succeeds
            LS-->>GUI: ACQUIRING reservation
            GUI-->>RPC: Reservation established
            RPC->>RPC: Hash saved-file baseline off GUI thread

            alt Baseline capture fails
                RPC->>GUI: Roll back reservation
                GUI->>LS: abort_acquisition by exact CAS

                alt Exact rollback succeeds
                    LS-->>GUI: Reservation released
                    GUI-->>RPC: Terminal failure, no credential
                else Exact rollback fails
                    LS-->>GUI: Rollback failure
                    Note over GUI,LS: Keep the fence/recovery artifact,<br/>return the stricter rollback error
                    GUI-->>RPC: Terminal recovery failure, no credential
                end
                RPC->>IR: Finalize request as failed
                RPC-->>MCP: Failure, no credential
                MCP-->>Agent: Adoption/acquisition failed

            else Baseline capture succeeds
                RPC->>GUI: Final identity/dirty-state revalidation<br/>and recovery snapshot
                GUI->>LS: Record snapshot and promote ACQUIRING to LOCKED_IDLE

                alt Snapshot, revalidation, or promotion fails
                    GUI->>GUI: Failure detected
                    GUI->>LS: abort_acquisition by exact CAS

                    alt Exact rollback succeeds
                        LS-->>GUI: Reservation released
                        GUI-->>RPC: Terminal failure, no credential
                    else Exact rollback fails
                        LS-->>GUI: Rollback failure
                        Note over GUI,LS: Keep the sidecar and recovery artifact
                        GUI-->>RPC: Terminal recovery failure, no credential
                    end
                    RPC->>IR: Finalize request as failed
                    RPC-->>MCP: Failure, no credential
                    MCP-->>Agent: Adoption/acquisition failed

                else Promotion succeeds
                    LS-->>GUI: Lease metadata and private credential
                    GUI-->>RPC: Successful acquisition result
                    RPC->>Vault: Escrow credential until MCP custody

                    alt Escrow store fails after ownership change
                        Vault-->>RPC: Store failure
                        RPC->>IR: Finalize handler as failed
                        RPC-->>MCP: Failure, token never public
                        MCP-->>Agent: Credential unavailable, recovery required

                    else Escrow succeeds
                        Vault-->>RPC: Credential escrowed
                        RPC->>IR: Finalize acquisition handler as completed
                        Note over RPC,IR: Replay entry is token-free,<br/>the private claim is repeatable until acknowledgement
                        RPC-->>MCP: Private lease result
                        MCP->>LM: Store credential locally

                        alt Local credential storage fails
                            LM-->>MCP: Storage failure
                            Note over MCP,RPC: MCP sends no custody acknowledgement
                            Note over RPC,Vault: Escrow remains until acknowledgement,<br/>explicit discard, or process restart
                            MCP-->>Agent: credential_stored=false, token absent

                        else Local credential storage succeeds
                            LM-->>MCP: Credential stored
                            MCP->>RPC: Acknowledge credential custody

                            alt Custody acknowledgement succeeds
                                RPC->>Vault: Delete acknowledged credential
                                Vault-->>RPC: Deleted
                                RPC-->>MCP: acknowledged=true
                                MCP-->>Agent: success=true, credential_stored=true,<br/>lease metadata retained, token redacted/absent

                            else Custody acknowledgement fails
                                RPC-->>MCP: Retryable acknowledgement failure
                                Note over LM,Vault: MCP holds the credential,<br/>escrow copy remains until acknowledgement succeeds
                                MCP-->>Agent: Custody stored, cleanup pending,<br/>token redacted/absent
                            end
                        end
                    end
                end
            end
        end

    else Eligible local dirty LOCKED_ERROR lease requires handoff
        GUI-->>RPC: LOCKED_ERROR handoff required
        RPC->>HC: Start handoff continuation(request_id,<br/>document identity, old ownership)
        RPC->>IR: Finalize detect handler with process_pinned=true
        IR-->>RPC: Terminal failed-status tombstone retained,<br/>control cancellation sees a completed tombstone
        RPC-->>MCP: success=false, error_code=LOCKED_ERROR_HANDOFF_PENDING,<br/>request_id, handoff_pending=true, confirmation_pending=false
        MCP-->>Agent: Non-error CONDITION_FALSE tool result,<br/>success=true, pending=true, credential_stored=false,<br/>no token, poll by request_id

        Note over Agent,GUI: No FreeCAD confirmation popup

        HC->>GUI: Resolve selector, verify dirty state,<br/>and call automatic authorization hook
        Note over HC,GUI: The current default hook always returns true

        alt Defensive authorization hook returns false
            GUI-->>HC: Denied
            HC->>HC: Mark terminal denied, no credential

        else Initial identity or dirty-state validation fails
            GUI-->>HC: Validation failure
            HC->>HC: Mark terminal failed, no credential

        else Initial validation succeeds
            GUI-->>HC: Identity and dirty state valid
            HC->>HC: Capture file baseline off GUI thread

            alt Baseline capture fails
                HC->>HC: Mark terminal failed, no credential

            else Baseline capture succeeds
                HC->>GUI: Final live-document revalidation,<br/>cancel gate, ownership CAS, and immediate escrow

                alt Claim GUI phase times out with uncertain completion
                    GUI-->>HC: completion_uncertain
                    HC->>HC: If escrow is not already claimable,<br/>mark claiming_uncertain (not cancellable)
                    Note over HC,Vault: A late GUI completion may still fail,<br/>or may CAS and escrow the credential

                else Document changed, disappeared, or is no longer eligible
                    GUI-->>HC: Final validation failure
                    HC->>HC: Mark terminal failed, no credential

                else Final revalidation succeeds
                    GUI-->>HC: Document still valid
                    GUI->>HC: begin_claim atomic cancellation gate

                    alt Cancellation won before begin_claim
                        HC->>HC: Mark terminal cancelled, no credential

                    else Claim gate won
                        HC->>HC: Mark claim_committed<br/>(irreversible cancellation boundary)
                        Note over HC,LS: A hang here is not cancellable and does not imply<br/>that an escrowed credential already exists
                        GUI->>LS: CAS rotate LOCKED_ERROR ownership

                        alt Ownership CAS fails
                            LS-->>GUI: CAS failure
                            GUI-->>HC: Claim failure
                            HC->>HC: Mark terminal failed, no credential

                        else Ownership CAS succeeds
                            LS-->>GUI: New lease metadata and private credential
                            GUI->>Vault: Escrow credential immediately

                            alt Escrow store fails after CAS
                                Vault-->>GUI: Store failure
                                GUI-->>HC: Recovery-required failure
                                HC->>HC: Mark recovery-required terminal failure,<br/>ownership already rotated, no claimable credential

                            else Escrow succeeds
                                Vault-->>GUI: Credential escrowed
                                GUI-->>HC: Claim phase completed
                                HC->>HC: Mark claimable
                            end
                        end
                    end
                end
            end
        end
    end

    opt Agent polls after receiving the pending handoff result
        Note over Agent,HC: Status polling after a pending handoff

        loop Until the claim is custodied or a terminal/recovery outcome is known
            Agent->>MCP: get_request_status(request_id)
            MCP->>RPC: Query continuation status
            RPC->>HC: Read authoritative continuation state

            alt pending_authorization, authorizing, hashing, or claiming
                HC-->>RPC: Pre-claim continuation, no credential
                RPC-->>MCP: success=true, state=running,<br/>result_claimable=false
                MCP-->>Agent: Continue polling or cancel

            else claim_committed before CAS/escrow completes
                HC-->>RPC: claim_committed,<br/>credential not yet guaranteed
                RPC-->>MCP: success=true, state=claim_committed,<br/>result_claimable=false
                MCP-->>Agent: Continue polling, do not assume escrow exists

            else claiming_uncertain after a GUI timeout
                HC-->>RPC: claiming_uncertain,<br/>late CAS/escrow outcome unknown
                RPC-->>MCP: success=true, state=running,<br/>completion_uncertain=true,<br/>result_claimable=false
                MCP-->>Agent: Continue polling, cancellation is not allowed

            else claimable with a live vault entry
                HC-->>RPC: continuation state=claimable
                Vault-->>RPC: claimable=true
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=true
                MCP-->>Agent: Call claim_acquisition_result(request_id)

            else claimable continuation but vault entry is missing
                HC-->>RPC: continuation state=claimable
                Vault-->>RPC: claimable=false
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=false
                MCP-->>Agent: Call claim once to surface the<br/>credential-unavailable recovery failure

            else terminal cancelled
                HC-->>RPC: cancelled, no credential
                RPC-->>MCP: success=true, state=cancelled
                MCP-->>Agent: Handoff cancelled

            else terminal denied
                HC-->>RPC: denied with details, no credential
                RPC-->>MCP: success=true, state=failed,<br/>handoff_continuation.state=denied
                MCP-->>Agent: Handoff denied, no claim advice

            else terminal failed
                HC-->>RPC: failed with details, no credential
                RPC-->>MCP: success=true, state=failed,<br/>handoff_continuation.state=failed
                MCP-->>Agent: Handoff failed, no claim advice

            else claimed
                HC-->>RPC: continuation state=claimed,<br/>credential already taken into custody
                RPC-->>MCP: success=true, state=completed,<br/>result_claimable=false
                MCP-->>Agent: Lease already stored, no token returned
            end
        end
    end

    opt Agent calls cancel_request at any point after the pending result
        Note over Agent,HC: Cancellation is resolved from continuation state,<br/>not from the inflight tombstone alone

        Agent->>MCP: cancel_request(request_id)
        MCP->>RPC: Cancel invoke_v2/handoff request
        RPC->>IR: request_cancel(request_id)
        IR-->>RPC: requested, completed tombstone, or unknown
        RPC->>HC: Cancel continuation or inspect terminal state

        alt Continuation exists and is cancellable before begin_claim
            HC-->>RPC: cancelled
            Note over RPC,IR: Continuation result overrides tombstone status
            RPC-->>MCP: success=true, handoff_cancelled=true
            MCP-->>Agent: Cancelled, no credential

        else Continuation is claim_committed, claiming_uncertain, or claimable
            HC-->>RPC: not_cancellable with current state
            Note over RPC,IR: Return REQUEST_NOT_CANCELLABLE for every IR status,<br/>including a completed tombstone
            RPC-->>MCP: success=false, error_code=REQUEST_NOT_CANCELLABLE,<br/>neutral irreversible/terminal wording

            alt Current state is claim_committed
                MCP-->>Agent: Continue polling, escrow may not exist yet
            else Current state is claiming_uncertain
                MCP-->>Agent: Continue polling, late CAS/escrow is unresolved
            else Current state is claimable
                MCP-->>Agent: Claim the available acquisition result
            end

        else Continuation is terminal failed
            HC-->>RPC: terminal_failed(details)
            RPC-->>MCP: Return actual failure, not not_cancellable
            MCP-->>Agent: Failure details, no credential to claim

        else Continuation is terminal denied
            HC-->>RPC: terminal_denied(details)
            RPC-->>MCP: Return actual denial, not not_cancellable
            MCP-->>Agent: Denial details, no credential to claim

        else Continuation is already cancelled
            HC-->>RPC: terminal_cancelled
            RPC-->>MCP: Idempotent cancelled result
            MCP-->>Agent: Already cancelled

        else Continuation is already claimed/completed
            HC-->>RPC: terminal_completed
            RPC-->>MCP: Request already completed, cancellation impossible
            MCP-->>Agent: Lease already in custody, no token returned

        else No handoff continuation exists
            HC-->>RPC: not_found
            RPC-->>MCP: Use ordinary inflight cancellation result
            MCP-->>Agent: requested, completed, or unknown as applicable
        end
    end

    opt Agent calls claim_acquisition_result
        Note over Agent,Vault: Claim and one-time credential custody

        Agent->>MCP: claim_acquisition_result(request_id)
        MCP->>RPC: Claim private credential
        RPC->>HC: Inspect authoritative continuation state

        alt Continuation is pending_authorization, authorizing, hashing,<br/>claiming, claim_committed, or claiming_uncertain
            HC-->>RPC: Not claimable yet
            RPC-->>MCP: success=false, pending=true,<br/>error_code=ACQUISITION_CLAIM_PENDING,<br/>no credential
            MCP-->>Agent: Tool failure advises continued polling

        else Continuation is cancelled
            HC-->>RPC: cancelled
            RPC-->>MCP: Terminal cancelled, no credential
            MCP-->>Agent: Nothing to claim

        else Continuation is failed or denied
            HC-->>RPC: Actual terminal state and details
            RPC-->>MCP: Terminal failure/denial, no credential
            MCP-->>Agent: Nothing to claim

        else Continuation is already claimed
            HC-->>RPC: Already claimed
            RPC-->>MCP: success=true, already_claimed=true,<br/>no private token
            MCP-->>Agent: Lease already stored, token absent

        else Continuation is claimable
            HC-->>RPC: Claimable
            RPC->>Vault: Read escrowed credential

            alt Credential is missing after explicit discard or process restart
                Vault-->>RPC: Missing
                RPC->>HC: Mark credential unavailable
                RPC-->>MCP: Terminal credential-unavailable failure
                MCP-->>Agent: No token, ownership may require recovery

            else Credential is available
                Vault-->>RPC: Lease metadata and private token
                Note over RPC,Vault: The claim is a repeatable private peek until acknowledgement
                RPC-->>MCP: Private credential result
                MCP->>LM: Store credential locally

                alt Local credential storage fails
                    LM-->>MCP: Storage failure
                    Note over MCP,RPC: MCP sends no custody acknowledgement
                    Note over RPC,Vault: Keep escrow until acknowledgement,<br/>explicit discard, or process restart;<br/>never expose token publicly
                    MCP-->>Agent: credential_stored=false, token absent

                else Local credential storage succeeds
                    LM-->>MCP: Credential stored
                    MCP->>RPC: Acknowledge custody

                    alt Acknowledgement succeeds
                        RPC->>Vault: Delete acknowledged credential
                        Vault-->>RPC: Deleted
                        RPC->>HC: Mark claimed
                        RPC-->>MCP: Custody complete
                        MCP-->>Agent: Handoff completed, credential_stored=true,<br/>lease metadata retained, token redacted/absent

                    else Acknowledgement fails
                        RPC-->>MCP: Retryable acknowledgement failure
                        Note over LM,Vault: MCP holds token,<br/>escrow copy remains until acknowledgement succeeds
                        MCP-->>Agent: Custody stored, cleanup pending,<br/>token redacted/absent
                    end
                end
            end
        end
    end

    opt Independent create_document lifecycle
        Note over Agent,LS: create_document must use the same acquisition fencing identity

        Agent->>MCP: create_document(name)
        MCP->>RPC: invoke_v2(create_document, acquisition_request_id,<br/>live_request_ids, authenticated)
        RPC->>IR: Register live create request
        RPC->>GUI: Reject an already-open name, otherwise create document
        GUI->>LS: begin_acquisition(document_id,<br/>acquisition_request_id, live_request_ids)

        alt Stale same-MCP ACQUIRING owner is not in live_request_ids
            LS->>LS: Fence interrupted request immediately
            LS-->>GUI: New ACQUIRING reservation
            Note over GUI,Vault: Create recovery snapshot, promote, then use the common<br/>escrow, local custody, acknowledgement, and redaction path

        else Same-MCP acquisition request is still live
            LS-->>GUI: Do not steal, return conflict/pending owner
            GUI-->>RPC: Creation/acquisition not completed
            RPC-->>MCP: Non-success result, no credential
            MCP-->>Agent: Existing request remains authoritative

        else Foreign owner or non-fenceable lock state exists
            LS-->>GUI: Lock conflict/denial
            GUI->>LS: Attempt exact reservation rollback
            GUI-->>RPC: Creation/acquisition failed
            RPC-->>MCP: Terminal result, no credential
            MCP-->>Agent: New document is normally closed if rollback succeeds,<br/>a failed rollback deliberately retains the open fenced document for recovery

        else No conflicting acquisition exists
            LS-->>GUI: ACQUIRING reservation
            GUI->>GUI: Create recovery snapshot
            GUI->>LS: complete_acquisition to LOCKED_IDLE
            Note over GUI,Vault: Continue through the common escrow, local custody,<br/>acknowledgement, and public token redaction path
        end
    end
```
