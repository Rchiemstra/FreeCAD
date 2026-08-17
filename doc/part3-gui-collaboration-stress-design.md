# Part 3 — GUI Collaboration Stress: Local User vs Remote Agent

Status: **Proposed**
Date: 2026-08-16
Branch: `fix/change-aware-save-mcp-autonomy` (parent `2c4c16bb`, nested `0bd67ad5`)
Supersedes: `tests/gui/collaboration_gui_stress.py`

---

## 0. Why this document exists

A user and an MCP agent must work safely in the same live FreeCAD document. Camera rotation,
pan, zoom, selection and tree expansion are **personal** state and must never invalidate an
otherwise valid model mutation. Genuinely overlapping model changes must still produce
**targeted** semantic conflicts.

Every layer below Part 3 is green: Windows `App_tests_run` 929 run / 920 passed / 9 skipped /
0 failed, Linux 926 / 924 / 2 / 0, the explicit 9p save gate (21 checks), and the Linux
cross-process lock-anchor gate (24 checks).

Part 3 Stage A nonetheless failed — **on the harness, not the product**. The existing harness
drives everything through `execute_code`: `openTransaction`/`commitTransaction`, `newDocument`,
`saveCopyWithOutcome`, geometry mutation, and even local pause/resume. It also depends on
pivy/Coin SWIG for camera work. Every one of those mechanisms is retired, fenced by design, or
unavailable. Patching it would only hide the contract violations.

This document defines the replacement architecture. It is the first deliverable of Part 3 and
must be reviewed before production edits land.

### 0.1 Part 3 delivery orchestration

Part 3 work packages are delivered through a mandatory orchestration loop defined in
`doc/part3-orchestrated-review-fix-test-plan.md`. That execution plan augments this ADR; it
does not replace actor, credential, save, recovery, or collaboration contracts stated here.

For every commit-sized work package:

1. Grok 4.5 High performs read-only review (no Fast tier, no automatic model substitution).
2. Main Agent triages findings; accepted findings route to Composer 2.5 for implementation or
   documentation fixes.
3. Docker focused validation and required native platform gates run after every Composer
   change. Documentation-only packages use documentation consistency checks and
   `git diff --check` instead of compile gates.
4. Grok 4.5 High re-reviews the **complete current diff** after validation passes.
5. Commit and push occur only after final `NO_ISSUES`, matching diff fingerprints, and all
   required gates.

The state machine (`BASELINED → REVIEW_REQUIRED → FIX_REQUIRED | TEST_REQUIRED |
FINAL_GATE_REQUIRED → … → COMMIT_READY → PUSH_READY → DONE`), prohibited model substitutions,
and loop-breaker rules in the execution plan are **binding** for Part 3 delivery. No state may
be skipped; `Composer → Commit` and `Tests → Commit` are invalid transitions.

---

## 1. Actor roles and credential boundaries

Three actor **roles** operate with distinct **credential boundaries**. Roles are not trust
domains; credentials do not overlap across boundaries.

| Actor role | Process | Credential | Reachable surfaces |
| --- | --- | --- | --- |
| **LocalUser** | FreeCAD GUI + LocalUserDriver | per-run **local control token** (32-byte secret via environment) | camera, pan, zoom, fit, selection, tree, active view, Pause/Resume agent writes, local model edits for conflict construction |
| **RemoteAgent** | RemoteAgentDriver **child process** | authenticated JSON-RPC session (`handshake_v2` / session manager) | typed CAD mutations, authenticated save/finalize/shutdown, read-only inspection |
| **StressCoordinator** | external parent process | provisions secrets; holds neither control token nor RPC session | launch, preflight, evidence collection, shutdown orchestration — no arbitrary Python inside FreeCAD |

```
StressCoordinator                       (external process)
  ├─ provisions the isolated profile, the per-run auth secret and the control token
  ├─ launches FreeCAD only through   python start_freecad.py
  ├─ RemoteAgentDriver  (child process; no control token in env/argv)
  │       └── JSON-RPC 2.0  POST /jsonrpc :9875   typed verbs only
  └─ LocalUserDriver client ──► 127.0.0.1:<ephemeral> + per-run token
                                              │
FreeCAD GUI process                           ▼
  Mod/Part3LocalDriver/InitGui.py  →  QObject on the Qt owner thread
        camera · pan · zoom · fit · active view · selection · tree ·
        pause/resume · local model edits for conflict scenarios
```

Only the LocalUser side (GUI process and coordinator's LocalUserDriver client) holds the local
control token. The StressCoordinator provisions it but does not use it for remote operations.

### 1.1 RemoteAgentDriver

Executes in a **separate child process** spawned by the StressCoordinator. That child process's
environment and `argv` **never** receive the local control token. RemoteAgentDriver speaks only
to the JSON-RPC listener on port 9875; it cannot reach the local control channel. WP04 must
prove the child process lacks the token (process environment inspection or equivalent).

Speaks only supported typed JSON-RPC. It may create and close disposable documents, create
model objects, set properties, create and edit sketches and features, recompute under native
coordinator ownership, undo and redo, save, Save Copy, query mutation readiness, capture
semantic revisions, submit conflict-aware mutations, and inspect documents read-only.

It **must not** use `execute_code` for document lifecycle, transactions, history, saves, model
mutations, recompute or conflict construction. `execute_code` remains a diagnostic
compatibility escape hatch and is absent from the Part 3 acceptance path. A static architecture
test enforces this (§11.6).

### 1.2 LocalUserDriver

Test-only infrastructure loaded from the isolated profile as `Mod/Part3LocalDriver`. It runs
its work on the Qt owner thread and performs camera rotation, pan, zoom, fit, active-view
switching, selection changes and clearing, tree expansion and collapse, local **Pause Agent
Writes** and **Resume Agent Writes**, and the local model edits used to construct conflicts.

It is not an MCP capability, is never registered with the JSON-RPC listener, and exposes no
method reachable by a remote client.

### 1.3 StressCoordinator

Launches FreeCAD only through `python start_freecad.py`, creates the isolated profile and
disposable directories, spawns **RemoteAgentDriver as a child process** (§1.1), starts the
LocalUserDriver client, interleaves local GUI activity with remote typed mutations, collects
evidence, validates shutdown, and never attaches to an unknown existing session. It executes no
arbitrary Python inside FreeCAD.

---

## 2. Trust boundaries

| Surface | Reachable by | Authentication |
| --- | --- | --- |
| Typed CAD mutations (`create_document`, `close_document`, `create_object`, `edit_object`, `recompute_document`, `undo`, `redo`, `body_create`, `sketch_*`, `pad_feature`, `pocket_feature`) | remote | plain JSON-RPC; actor identity from authenticated session when session exists |
| `begin_checked_edit`, `commit_checked_property`, `cancel_checked_edit` | remote | **`handshake_v2` session required**; actor derived from session, not caller |
| `save_document`, `save_document_as`, `finalize_document_edit`, `save_document_copy`, `shutdown_rpc_server` | remote | **`handshake_v2` session required** |
| Camera, pan, zoom, fit, selection, tree, active view | **local only** | control token |
| Pause / Resume agent writes | **local only** | control token |
| Local model edit for conflict construction | **local only** | control token |

### 2.1 The local control channel

A `ThreadingHTTPServer` bound to `127.0.0.1:0` — an ephemeral port, never a fixed one. It
requires a 32-byte per-run token passed to the GUI process through the environment and compared
in constant time. On startup it writes its chosen port and a readiness marker atomically to
`<run>/control/endpoint.json`; the coordinator reads that file rather than guessing.

Every request carries an `operation_id`. Every response acknowledges that same `operation_id`
and reports the observed result. **Synchronisation is by acknowledgement only — no sleeps.**

Because resume is reachable only through this channel, the existing invariant proved by
`test_remote_rpc_cannot_resume_local_pause` remains true: no remote RPC can unpause itself.

### 2.2 Exactly-once mutating RPC semantics

Mutating typed RPCs that may be retried after a lost response carry a **server-enforced
operation identity** in the canonical request payload. The server stores the terminal result
for each `(session, operation_id)` pair.

1. **First application** — the mutation executes once; the terminal result is stored.
2. **Duplicate id, equal canonical payload** — the server returns the stored terminal result
   without re-executing the mutation.
3. **Duplicate id, different canonical payload** — the server fails with an explicit protocol
   conflict; it does not apply either payload twice.

The local control channel's `operation_id` is acknowledgement-only for synchronisation. It does
not substitute for server-side operation identity on typed JSON-RPC mutations. Unauthenticated
surfaces cannot mint or override operation identity.

### 2.3 Why an unknown MCP session cannot be reused

`MCP_RPC_PORT = 9875` is a module constant in the launcher with no flag or environment
override, and `REUSE_DISABLED_REASON` records why reuse is refused outright: *no read-only
identity method exists to prove which session is listening*. `ping` returns only `True`,
`handshake_v2` is an authenticated operation, and identifying a session through `execute_code`
would mean running code inside a process of unknown provenance.

The coordinator therefore **refuses to start** when port 9875 already answers, rather than
attaching to a session it does not own. Isolation is asserted by comparing
`FreeCAD.getUserAppDataDir()` against the isolated path **before any document is touched**.

### 2.4 Isolation of the user's environment

`python start_freecad.py` is not a neutral launcher: `_ensure_mcp_addon_installed()` replaces
`Mod/FreeCADMCP` in every user-data directory under `%APPDATA%\FreeCAD`, and
`_ensure_mcp_auto_start()` rewrites `freecad_mcp_settings.json`. Redirecting the data base
contains all of it:

```
APPDATA / XDG_DATA_HOME  -> <run>/profile
FREECAD_USER_HOME        -> <run>/profile/FreeCAD
FREECAD_USER_DATA        -> <run>/profile/FreeCAD
FREECAD_USER_TEMP        -> <run>/profile/FreeCAD/temp
FREECAD_REPO             -> <repo root>
```

The one-level offset is required: the launcher installs into `$APPDATA/FreeCAD/Mod` while
FreeCAD reads `FREECAD_USER_HOME` verbatim.

`HamaAdapter_v3` is never opened, saved, closed, reloaded or mutated. The user's normal FreeCAD
profile is never used.

### 2.5 Authenticated RPC v2 provisioning

`initialize_rpc_v2_session()` leaves `session_manager` as `None` unless the profile carries both
`profile_instance_id` and `auth_secret_file`. Without it, every authenticated verb — including
canonical save — returns `LEASE_PROTOCOL_REQUIRED`. The mandated launcher does not write those
keys.

The coordinator therefore provisions them into the isolated profile using the existing
mechanism (`scripts/setup_isolated_profile.py`, `doc/isolated-instance.md`), and the
RemoteAgentDriver obtains its session with the existing client-side helpers
(`_shared/protocol/handshake_request.build_handshake_request_from_manifest`,
`src/freecad_mcp/server_ops/manifest_auth.py`) — no new protocol.

**Verification step:** confirm `_ensure_mcp_auto_start()` merges rather than replaces those
keys. If it replaces them, write the settings after the launcher's ensure step and re-verify
before creating any document. This is resolved during setup, never at stress time.

---

## 3. Operation ownership

### 3.1 Document identity and lifecycle authority

Part 3 selectors, edit sessions, prepared edits, and operation results key on **document
identity**, not on a bare name string.

- **Authority** — `Uid` plus instance/lifecycle epoch from `App::Document` (the stable identity
  for a live document instance). The document **name** is diagnostic only; it is not sufficient
  authority for session binding or conflict accounting.
- **Lifecycle epoch** — document open, close, and reopen advance the lifecycle epoch. A reopened
  document at the same pathname is a new instance for Part 3 contracts even if the name string
  matches.
- **Invalidation** — close or reopen invalidates all prior `session_id` values, prepared edits,
  stored operation results, and revision bindings targeting the old instance. Stale selectors
  receive an explicit hard refusal, not silent retargeting to a reused name.
- **Quarantine** — document-local quarantine keys on `(Name, id(document), Uid)` as in §13;
  identity authority governs whether a session is still valid, not merely whether the name string
  matches.

### 3.2 Remote undo and redo safety

Remote `undo` and `redo` are reusable typed RPCs, but Part 3 requires **history-head binding**
before any remote history action is admitted:

- The client supplies an **expected history head** or **operation identity** observed at
  preparation time (via session state or a preceding read).
- If a **local transaction** has intervened since that observation — for example, a LocalUser
  model edit through `commitCompatibilityMutation` — the server refuses the history action
  with an explicit safe refusal. It **never** silently undoes user work.
- Lost-response retry of an admitted history action follows §2.2 exactly-once rules for equal
  and unequal payload replay.

### 3.3 Coordinator and agent ownership

- **FreeCAD owns document lifecycle.** Creation, opening and closing happen through typed RPC
  that reaches `App::Document`, never through generated code.
- **The native coordinator owns transactions and recompute.** `commitCompatibilityMutation`
  and the prepared-edit path open exactly one native transaction, apply, recompute the affected
  graph, run the postcondition, and publish or roll back.
- **The agent never opens its own transaction.** `openTransaction`, `commitTransaction`,
  `abortTransaction`, `setTransactionMode`, `undo` and `redo` through generated payloads remain
  forbidden, as `tests/test_native_owned_payload_policy.py` already enforces for templates.
- **The local GUI driver never bypasses semantic conflict accounting.** Local model edits go
  through `Document.commitCompatibilityMutation(...)`, so a user edit advances revisions exactly
  as a GUI command does. Raw property assignment is not used.

---

## 4. View-state classification

This reuses the classification already ruled in
`doc/freecad_document_collaboration_plan.md` §3 and proved by
`tests/src/Gui/CollaborationViewIsolation.cpp`.

| Class | Contents | Revisioned | Dirties the file | Owner |
| --- | --- | --- | --- | --- |
| **Model state** | parametric values, expressions, constraints, placements, shapes | yes — `ObjectModel`, `ObjectProperty` | yes — `DocumentFileChange::Model` | App |
| **Structural state** | object incarnation, names, dynamic-property schema, Body/Group membership and order, Tip, link edges | yes — `ObjectExistence`, `ObjectStructure`, `DocumentStructure` | yes — Model | App |
| **Shared presentation** | deliberately shared visibility, colour, transparency, display mode | serialised GUI compatibility path | yes — `DocumentFileChange::Appearance` | Gui |
| **Personal view state** | camera, projection, pan/zoom, selection, preselection, tree expansion and scroll, active document/view/workbench, edit focus, temporary overlays | **no** | **no** | Gui, actor-scoped |

Unknown persisted properties fail closed into model, structural or shared-presentation
handling — never into personal state.

`App::DocumentFileChange` states this directly: *"Persistent change categories. These
deliberately exclude session-only view activity."* Camera activity is reported through
`Gui::Document::signalCameraActivity` and surfaces as the *"View changed locally. The file is
unchanged."* note, without ever marking the document dirty.

**The Part 3 proof reads `Document.getFileChangeState()`** before and after every personal
view action and requires `pending_changes == []` and `has_pending_file_changes is False`,
while `get_semantic_revisions(...)` returns an unchanged revision vector.

---

## 5. Recompute contract

**Decision: `execute_code` keeps `recompute="none"` as its default.** It is a
compatibility/diagnostic escape hatch; implicit recompute would duplicate coordinator-owned
recompute, move validation into the broad apply phase, and hide missing operation contracts.
The public default is defined in `operations/core_ops/execute_ops.py:19`,
`execute_options.py:19` and `generated/.../tools_core_execute.py:79`. It does not change.

`operations/core_ops/run_code.py:22` defaults to `"target"`. That is the **internal** generated
typed-template path, where coordinator-owned recompute is correct. The asymmetry is
intentional and is documented rather than aligned.

The wedge recorded in the handoff — a geometry mutation committing truthfully but leaving the
object `Touched`, so the next mutation is refused `retryable: False` — is dissolved by using
typed APIs: `run_cad_mutation(..., native_recompute=True)` gives coordinator-owned `target`
recompute. Part 3 never depends on implicit recompute.

Rules enforced:

1. Every typed mutation declares `recompute = none` or `recompute = target`, in a new
   `rpc_server/recompute_policy.py` registry. `run_cad_mutation` asserts the declaration matches
   the `native_recompute=` it was handed.
2. Geometry-producing public operations normally use coordinator-owned `target`.
3. Read-only operations never recompute.
4. Multi-document recompute stays unsupported —
   `UNSUPPORTED_NATIVE_RECOMPUTE_SCOPE`.
5. No template or typed leaf recomputes inside the broad apply callback.
   `tests/test_generated_recompute_architecture.py` already proves this and is reused.
6. Result inspection that depends on derived geometry happens in the native post-recompute
   postcondition phase, behind the `# __FREECAD_MCP_NATIVE_POST_RECOMPUTE__` boundary.

---

## 6. Conflict construction

Conflicts are arbitrated by **edit sessions**, not by racing clients. The JSON-RPC listener is
single-threaded and mutations are marshalled onto the GUI thread, so two clients writing at once
are serialised by construction — racing them proves nothing while reporting success.

Native protocol on `App::Document`:

```
session  = beginEditSession()          # actor derived from authenticated RPC session
snapshot = snapshotForEdit(session_id, revision_keys)
prepared = prepareEdit(session_id, operation_id, "App.CollaborativeSetProperty", args, provenance)
result   = commitEdit(session_id, prepared)
```

**Actor identity** is bound to the authenticated JSON-RPC session (`handshake_v2` / session
manager), not to a caller-supplied string. Unauthenticated surfaces cannot mint or override actor
identity. Part 3 wire methods do not accept `actor_id` as a client-chosen parameter.

Revision keys are dicts with a **`kind`** member — `ObjectProperty`, `ObjectExistence`,
`ObjectModel`, `ObjectStructure`, `DocumentStructure`, `UnknownModelMutation` — plus `subject`
and, for `ObjectProperty`, `property_name`. The old harness omitted `kind`; that is the
`revision key 'kind' must be a string` failure.

### 6.1 Same-property conflict

1. The remote agent opens a checked edit and snapshots `StressBox.AlphaValue` at revision r0.
2. The **local user** changes the same property through the legitimate GUI/model path → r1.
3. The agent commits the edit prepared against r0.
4. Expected: a targeted property/dependency conflict naming the changed semantic key, with
   `expected_revisions` and `current_revisions`.
5. Expected: no global write-lane outage; other documents and other properties stay writable.
6. Expected: no quarantine, because rollback is healthy.

Holding the session across calls is what makes the refusal **native**. Comparing revisions in
Python would be a weaker test that could pass while the coordinator was broken.

### 6.2 Independent mutation

1. The local user changes `StressBox.AlphaValue`.
2. The remote agent changes `SecondBox.BetaValue`.
3. Both must land exactly once, because dependency analysis finds no overlap.

Both scenarios get deterministic unit/integration coverage before the full stress run.

---

## 7. Pause and resume semantics

Pause state is process-local, in memory, and not per-document. `automation_pause` holds it;
`dispatch()` gates every remote write through `admit_remote_write()`.

The LocalUserDriver drives **only** the real Qt control:
`getMainWindow().findChild(QCheckBox, "pauseAgentWrites").setChecked(True)`, which runs
`_on_pause_toggled` exactly as a human click does.

**No acceptance fallback.** Part 3 acceptance and stress must drive pause and resume through
this checkbox path (`pauseAgentWrites` / human-equivalent toggle). A missing checkbox is a **hard
setup failure** — the stage aborts during preflight. Direct `automation_pause` module calls,
remote RPC, or any non-checkbox pause API are **prohibited** in the acceptance path and must
not substitute for the real GUI control.

Proven behaviour:

- an operation admitted before the pause may finish (`pause_after_current`);
- later remote writes receive `AUTOMATION_PAUSED`;
- reads remain available — the 43 methods in `_READ_ONLY_REMOTE_METHODS` are never gated;
- **no remote RPC can resume writes**;
- a local Resume action restores admission;
- the next remote typed mutation succeeds;
- document behaviour stays observable throughout.

---

## 8. Evidence schema

Written to `<run>/evidence/evidence.json`, extending the existing shape.

```jsonc
{
  "schema_version": 2,
  "started_utc": "...", "finished_utc": "...",
  "environment": {
    "executable", "binary_fingerprint": { "FreeCADApp.dll": {"mtime","size","sha256"}, ... },
    "git": {"parent_commit","nested_commit","branch","dirty"},
    "isolated_profile", "isolated_mod_dir", "documents_dir",
    "mcp_host", "mcp_port", "control_endpoint",
    "launcher", "python", "freecad_pid", "freecad_version",
    "reported_user_app_data", "isolation_verified": true,
    "auth": {"v2_session": true, "profile_instance_id": "..."}
  },
  "stage": "A" | "B" | "C",
  "cycles": [
    {
      "index": 0,
      "local_actions":  [{"operation_id","action","ack_utc","view_state_changed": true}],
      "remote_actions": [{"operation_id","method","result_envelope","committed_once": true}],
      "revisions_before": [...], "revisions_after": [...],
      "file_change_state_before": {...}, "file_change_state_after": {...},
      "readiness": {...}
    }
  ],
  "saves": [{"index","disposition","file_written","durability_verified",
             "sha256_before","sha256_after","truthful": true}],
  "conflicts":    {"same_property": {...}, "independent_property": {...}},
  "pause_resume": {"pause": {...}, "refused": {...}, "resume": {...}, "after": {...}},
  "shutdown": {
    "requested_utc", "rpc_admission_closed_utc", "worker_shutdown_utc",
    "documents_closed_utc", "listener_shutdown_utc", "window_closed_utc",
    "process_exit_utc", "deadline_seconds": 60, "forced": false,
    "stalled_stage": null
  },
  "artifacts": {"documents": [...], "lock_anchors": [...], "unexplained": []},
  "checks": [{"name","passed","detail"}],
  "failed_checks": [...],
  "verdict": "PASSED" | "FAILED"
}
```

Machine-readable verdict line on stdout: `PART3_RESULT: PASSED|FAILED`, matching the existing
`9P_GATE_RESULT:` and `LOCK_ANCHOR_GATE_RESULT:` convention. Exit codes: 0 pass, 1 fail,
2 fatal preflight.

`lock_anchors` is a **separate class** from `unexplained`. Per contract R27 a surviving
`<destination>.FreeCAD-save.lock` is never evidence of a held lock, a crashed save or a leaked
artifact, and generic leftover checks must exclude it **explicitly**, not incidentally.

---

## 9. Graceful-shutdown contract

Forced termination is not an accepted successful result.

1. `close_document` for every disposable document (typed RPC).
2. `shutdown_rpc_server()` (typed, authenticated) — RPC admission closes.
3. The local driver acknowledges, then closes the main window from the Qt owner thread.
4. The coordinator waits for process exit against a documented deadline of **60 seconds**.

Each transition is timestamped into `evidence.shutdown`. If the process does not exit, the
coordinator classifies the stage at which shutdown stopped, collects the launcher log,
`stress.log`, the last local acknowledgements and thread state, and marks the stage **failed**.
Only then may it force-kill. A `MiniDumpWriteDump` watchdog is built only if an unexplained
hang reproduces. A forced kill never yields a green run.

---

## 10. Compatibility and rollback plan

- `execute_code` is unchanged, keeps its `recompute="none"` default, and remains available as a
  diagnostic escape hatch. Nothing in this design narrows it.
- The four new wire methods are additive. Removing them restores the previous surface exactly;
  no existing method changes shape.
- No frozen layer is touched: DocumentFileWriter, BackupPolicy, the NT pinned-parent rename,
  Windows read-only replacement, recovery-lease semantics, the Q1 destruction boundary,
  MainThreadSignal storage, the repository-owned launcher and the 9p save path are all
  untouched, except for one export annotation described in §11.8 that is inert in production
  builds.
- `Mod/Part3LocalDriver` exists only inside a disposable isolated profile. It is never installed
  into a user profile and is not shipped.
- Rollback is `git revert` of the Part 3 commits; the branch returns to `2c4c16bb` behaviour
  with no residue, because nothing outside `tests/`, `doc/` and the additive nested methods
  changes.

### 10.1 Future non-blocking geometry compatibility

Detached geometry may become an execution backend in a later work stream. Part 3 must remain
compatible with that direction **without** merging or cherry-picking
`feature/non-blocking-geometry-scaffolding` wholesale. That branch is not merged in Part 3.

Invariants for the future geometry path:

- **`DocumentCommitCoordinator` remains the sole final live-document commit owner.** Part 3
  must not introduce a second transaction authority or a parallel commit path for live documents.
- **Semantic read-set revision validation** — future geometry results must be validated against
  semantic read/write revision sets, not only a global `modelGeneration` fence. Personal view
  activity and unrelated model changes remain eligible as in §4 and §6.
- **`MainThreadSignal` hook storage** — the exported out-of-line hook storage used by the
  non-blocking geometry work must not regress; Part 3 changes must not break that contract.

Obtaining non-blocking geometry behaviour in Part 3 is through explicit, reviewed product
changes in later work packages — not through wholesale merge of the scaffolding branch.

---

## 11. Implementation plan

### 11.1 Typed API inventory

| operation | current | existing typed RPC | coordinator-owned | recompute | replacement | coverage |
| --- | --- | --- | --- | --- | --- | --- |
| create document | `execute_code` | `create_document` | yes | none | reuse | contract snapshot |
| close document | — | `close_document` | yes | none | reuse | contract snapshot |
| create model object | `execute_code` | `create_object`, `body_create` | yes | target | reuse | typed gateway tests |
| set property | `execute_code` | `edit_object` | yes | target | reuse | typed gateway tests |
| create/edit sketch | `execute_code` | `sketch_create`/`_attach`/`_add_geometry`/`_add_constraint` | yes | target | reuse | typed gateway tests |
| Pad / Pocket | `execute_code` | `pad_feature`, `pocket_feature` | yes | target | reuse | typed gateway tests |
| recompute | `_doc.recompute()` | `recompute_document`, `recompute_and_wait` | yes | target | reuse | existing |
| save | `saveWithOutcome()` in code | `save_document` | yes | none | reuse (+ auth) | contract snapshot |
| unchanged save | same | `save_document` → `unchanged` | yes | none | reuse | 9p gate + new |
| **Save Copy** | `saveCopyWithOutcome()` in code | **none** | yes | none | **add `save_document_copy`** | new unit + stress |
| undo / redo | `execute_code` | `undo`, `redo` | yes | target | reuse (+ expected history head / op identity per §3.2) | existing + new safety tests |
| **capture revisions** | `snapshotForEdit` in code | **none** | yes | never | **add `get_semantic_revisions`** | new unit |
| **targeted conflict** | edit sessions in code | **none** | yes | target | **add `begin_checked_edit` + `commit_checked_property`** | new unit + stress |
| independent mutation | same | same | yes | target | same pair | new unit + stress |
| readiness | `get_mutation_readiness` | yes | n/a | never | reuse | existing |
| **pause / resume** | remote `execute_code` | **must not exist** | n/a | n/a | **LocalUserDriver only** | new |
| **camera / pan / zoom** | pivy in `execute_code` | **must not exist** | n/a | n/a | **LocalUserDriver only** | new GUI tests |
| **selection / tree** | remote `execute_code` | remote verbs exist but are the wrong actor | n/a | n/a | **LocalUserDriver only** | new GUI tests |
| screenshot | `saveImage` in code | `get_active_screenshot` | n/a | never | local driver `saveImage` | existing |
| process shutdown | `taskkill /F` | `shutdown_rpc_server` | n/a | n/a | ordered graceful sequence | new |

### 11.2 New typed methods (nested repo)

| method | shape | auth | recompute |
| --- | --- | --- | --- |
| `save_document_copy(selector, destination, overwrite=False)` | mirrors `save_document_as`, wraps `saveCopyWithOutcome`, asserts the canonical savepoint did **not** move | authenticated | none |
| `get_semantic_revisions(doc_selector, revision_keys)` | read-only; `doc_selector` carries Uid/instance/lifecycle identity; `[{kind, subject, property_name, revision}]` | plain | never |
| `begin_checked_edit(doc_selector, revision_keys)` | `beginEditSession` (actor from authenticated session) + `snapshotForEdit` → `{session_id, revisions}` | authenticated | none |
| `commit_checked_property(session_id, doc_selector, object_name, property_name, value_type, value, operation_id)` | `prepareEdit` + `commitEdit`; refusal → `-32001 DOCUMENT_CONFLICT` with `changed_semantic_keys`, `expected_revisions`, `current_revisions`; `operation_id` per §2.2 | authenticated | target |
| `cancel_checked_edit(session_id, reason)` | `cancelEdit` | authenticated | none |

Each is bound in `rpc_server_ops/facade_bindings.py`, added to `_READ_ONLY_REMOTE_METHODS` or
`AUTHENTICATED_METHODS` as appropriate, wrapped by the existing admission / postflight /
quarantine scaffolding, and reflected in `tests/fixtures/freecad_rpc_contract_snapshot.json`.
There is deliberately **no** broad "run stress action" RPC. MCP-tool exposure is added for
`save_document_copy` only; the session primitives stay wire-level, and that limitation is
recorded here.

### 11.3 Camera and view control without pivy

pivy/Coin SWIG is **not** a Part 3 dependency. `View3DInventorPy` already exposes everything
needed, with no SWIG object crossing the boundary:

| Semantic action | Backend |
| --- | --- |
| rotate by yaw/pitch, or to a `Base.Rotation` | `setCameraOrientation` / `getCameraOrientation` |
| pan by a normalised delta | `viewPosition(Base.Placement, steps, duration_ms)` — also reads the camera back |
| zoom by a factor | `zoomIn` / `zoomOut` / `boxZoom` |
| fit all | `fitAll(factor)` |
| exact save/restore | `getCamera()` / `setCamera(str)` (plain Inventor text) |
| named views | `viewTop` … `viewTrimetric`, `viewDefaultOrientation` |
| select object/subelement | `Gui.Selection.addSelection`, `getSelectionEx` |
| clear selection | `Gui.Selection.clearSelection` |
| activate document/view | `Gui.setActiveDocument`, `FreeCADGui.getMainWindow()` MDI activation |
| expand/collapse tree item | `Gui.Document.toggleTreeItem(obj, mod, subName)`, `mod ∈ {0,1,2,3}` |
| screenshot | `view.saveImage(path, w, h, "Current")` |

Only `getCameraNode`, `getSceneGraph`, `dumpNode` and the `*CallbackPivy` variants need SWIG,
and none is used. Animation is disabled (`duration_ms = 0`) so every action is deterministic.

All actions execute on the Qt owner thread, report completion by explicit acknowledgement,
never mutate the App model, never change semantic revisions, never set Model dirty, and
preserve the personal-view classification of §4.

Focused GUI tests run **before** any stress loop:

1. **Rotate** — camera state changes, model revisions unchanged, Model dirty false.
2. **Pan/zoom** — view state changes, model revisions unchanged.
3. **Selection and tree** — GUI state changes, model revisions unchanged, and a save
   immediately afterwards still reports `Unchanged`.
4. **Concurrent remote mutation** — the local view action completes, the remote typed mutation
   commits exactly once, and no document-wide conflict occurs.

`tests/src/Gui/CollaborationViewIsolation.cpp` already proves camera, selection and tree do not
advance App revisions; it is extended with the `getFileChangeState()` dimension rather than
duplicated.

### 11.4 Local driver layout

```
tests/gui/part3/local_driver/
  InitGui.py          # sys.path bootstrap + QTimer.singleShot(0, _install)
  control_channel.py  # 127.0.0.1:0, per-run token, atomic endpoint.json
  driver.py           # QObject on the Qt owner thread, queued signal, op-id acks
  actions.py          # the semantic view/selection/tree/pause action set
```

`InitGui.py` binds helpers as default arguments, because `FreeCADGuiInit.py` executes it with
bare `exec(code)` where globals and locals are separate mappings — the same pattern the MCP
`InitGui.py` already uses. The `QObject` mirrors `rpc_server/gui_dispatcher_qt.py` (queued
signal plus a captured owner-thread identity).

### 11.5 Coordinator layout

```
tests/gui/part3/
  remote_agent_driver.py   # child-process typed JSON-RPC only; no control token; JsonRpcClient
  local_user_driver.py     # coordinator-side client of the control channel (holds token)
  stress_coordinator.py    # provisioning, launch, child spawn, preflight, stages, shutdown, evidence
  scenarios.py             # Stage A/B/C cycle definitions
  evidence.py              # schema + writer
  test_part3_architecture.py
```

`stress_coordinator.py` spawns `remote_agent_driver.py` in a child process whose environment
and `argv` never include the local control token (§1.1). WP04 proves this isolation.

`JsonRpcClient` from `tools/launcher/start_freecad_impl.py` is reused rather than
reimplemented: it validates the protocol version, the response id, the single-member
result/error rule and the retired `410 Gone`, none of which the old private client did.

`tests/gui/collaboration_gui_stress.py` is **deleted** in the same commit, so no
retired-API harness remains runnable or mistakable for a gate.

### 11.6 Static architecture gate

`tests/gui/part3/test_part3_architecture.py`, AST-based over the whole package:

- no call to `execute_code` or `execute_code_async`;
- no `openTransaction`/`commitTransaction`/`abortTransaction`/`setTransactionMode` in any call
  or string literal;
- no `newDocument`, `saveAs`, `saveWithOutcome`, `saveCopyWithOutcome`, `undo`, `redo` or
  `recompute` reached through a code-string parameter;
- every RemoteAgentDriver RPC method name is in an allowlist of typed verbs;
- no `pivy`, `coin`, `getCameraNode` or `getSceneGraph` reference;
- no `time.sleep` in any synchronisation path.

It mirrors `tests/test_public_typed_gateway_operations.py::test_public_typed_mutation_calls_rpc_once_and_never_execute_code`
and the `FORBIDDEN_CALLS` policy of `tests/test_native_owned_payload_policy.py`.

### 11.7 Recompute policy registry

`rpc_server/recompute_policy.py`: a `RecomputePolicy` enum and one declaration per typed
mutation wire method. Architecture tests prove every method classified `mutation` in
`generated/capabilities/gateway_dispatch.json` has exactly one declaration, that read-only
methods declare nothing, and that the public `execute_code` default is still `"none"`.
Declaring the policy in the capability manifests themselves is a recorded follow-up; it would
touch 17 manifests and the generator, and is not required to make Part 3 honest.

### 11.8 Windows lock-anchor proof

`App::Internal::DocumentFileLock` is declared without an export annotation, so
`save_lock_probe` cannot link on Windows. The fix adds no public API to production builds:

- `src/App/DocumentFileWriter.h:249` → `class APP_DOCUMENTFILEWRITER_EXPORT DocumentFileLock`.
  That macro expands to nothing in production and to `AppExport` only under
  `FREECAD_DOCUMENTFILEWRITER_TEST_API`, which `src/App/CMakeLists.txt` sets PRIVATE under
  `ENABLE_DEVELOPER_TESTS` — exactly as `DocumentFileWriter` itself already does.
- `tests/filesystem/CMakeLists.txt` →
  `target_compile_definitions(save_lock_probe PRIVATE FREECAD_DOCUMENTFILEWRITER_TEST_API)`.

Then the same acknowledged A → B → C sequence runs on Windows and must prove: B cannot acquire
while A holds; B acquires after A releases; C later coordinates through the same persistent
pathname; abrupt process exit releases the kernel lock; the zero-byte pathname survives; and no
two processes ever report ownership simultaneously.

### 11.9 Commit sequence

```
docs:  define the Part 3 local-user/remote-agent architecture
feat(mcp):  complete typed lifecycle operations for acceptance
feat(mcp):  declare an explicit recompute policy for every typed mutation
test(mcp):  forbid acceptance mutations through execute_code
feat(gui):  add the local GUI stress controller
feat(gui):  add the Part 3 stress coordinator and typed agent driver
test(gui):  prove personal view state never touches model state
test(app):  prove Windows cross-process save locking
fix(gui):   complete graceful owned-session shutdown
```

Nested commits and push first, then the parent gitlink, then the parent. No amend, no rebase,
no force push, no pull request. `results/`, `tests/lib/`, evidence directories and the research
report are never committed.

---

## 12. Gates

Stop at the first unexplained red layer.

**Nested MCP** — focused typed-operation tests; `test_native_owned_payload_policy`;
`test_generated_recompute_architecture`; `test_architecture_policy`;
`test_freecad_rpc_contract_snapshot`; `test_mcp_tool_registry_contract_snapshot`;
`test_capability_manifest_generator`; `test_automation_pause`; full `pytest -m unit`;
`ci/lint_python.py` with Ruff; `compileall`; `git diff --check`.

**Windows** — focused LocalUserDriver GUI tests; focused typed mutation tests; conflict and
independence tests; pause/resume tests; the Windows lock-anchor gate; complete isolated
`App_tests_run` via `tests/filesystem/run_app_tests_isolated.sh`; `Gui_tests_run`; launcher
tests; real readiness; occupied-port refusal.

**Linux** — affected focused App/MCP/GUI tests; complete `App_tests_run` because
`DocumentFileWriter.h` changes; the explicit 9p gate; the Linux lock-anchor recheck because
`tests/filesystem/CMakeLists.txt` changes. **Rebuild Gui as well as App** — building
`App_tests_run` alone leaves a stale `FreeCADGui.dll`.

**Repository** — `git diff --check`, line-ending integrity, gitlink consistency, no unintended
artifacts, no stale generated fixtures.

---

## 13. Staged stress

Every GUI instance starts through `python start_freecad.py` with an isolated profile, isolated
data and Mod paths, a disposable document directory, port 9875 confirmed free, the tracked
launcher, recorded binary hashes, and no session reuse.

| Stage | View/mutation cycles | Save cycles |
| --- | --- | --- |
| A | 10 | 5 |
| B | 50 | 20 |
| C | 500 | 100 |

Each stage covers camera rotation, pan, zoom, selection, tree expansion and collapse,
active-view switching, typed model mutations, recompute, save, `Unchanged` save, Save Copy,
undo, redo, two documents, the same-property conflict, the independent-property success, and
local Pause/Resume.

Required invariants:

- personal view activity never changes model semantic revisions;
- personal view activity never sets Model dirty;
- camera changes never invalidate unrelated model commits;
- remote mutations commit exactly once per §2.2 server-enforced operation identity;
- remote undo/redo refuse when a local transaction has intervened (§3.2);
- the native coordinator owns transaction and recompute;
- no global write-lane outage follows a healthy error;
- quarantine stays document-local, keyed on `(Name, id(document), Uid)`;
- real overlap produces a targeted conflict;
- independent edits remain eligible;
- save dispositions are truthful against the observed SHA-256;
- the canonical `.FCStd` stays ordinarily readable;
- files validate as ZIP archives containing `Document.xml`;
- no unexplained temporary or displaced artifacts remain;
- persistent lock anchors are classified separately per R27;
- graceful shutdown completes without forced termination.

No larger stage is attempted after an unexplained failure.

---

## 14. Risks

- **v2 auth provisioning through the mandated launcher** is the highest-risk unknown (§2.5).
  Resolved during setup, verified before any document is created.
- **Contract-snapshot churn.** Five new wire methods force a regenerated
  `freecad_rpc_contract_snapshot.json` and a bumped tool count — expected, and covered by the
  generator's byte-equality tests.
- **Windows `Gui_tests_run`** historically had `.pyd` discovery problems. If the C++
  view-isolation additions cannot run there, they still run on Linux, and the Python-level
  focused GUI checks cover Windows.
- **pivy is in fact built here** (`build/release/Mod/pivy/_coin.pyd`) and is most likely
  disabled by `check_bundled_pivy()` poisoning `sys.modules["pivy"]`. Irrelevant by design —
  Part 3 must not depend on it either way.

---

## 15. Verification

1. `pytest tests/gui/part3/test_part3_architecture.py` — the acceptance path is free of
   `execute_code`, transactions, history and pivy.
2. Nested `pytest -m unit` plus the architecture, contract and generator gates.
3. `run_app_tests_isolated.sh` on Windows and in the Linux container; `Gui_tests_run`.
4. `python tests/filesystem/save_lock_anchor_gate.py build/release/bin/save_lock_probe` on
   Windows — 24 checks.
5. `bash tests/filesystem/run_9p_save_compat_gate.sh <FreeCADCmd> <dir under build/>` — 21 checks.
6. `python tests/gui/part3/stress_coordinator.py --stage a`, then `--stage b`; each must end in
   `PART3_RESULT: PASSED` with an `evidence.json` whose `verdict` is `PASSED`, whose shutdown
   record shows process exit inside the deadline with `forced: false`, and whose artifact scan
   shows nothing but the documents and their lock anchors.
