# Document Collaboration Architecture Decision Record

## Status

Accepted for `CC-WP01` on branch `fix/change-aware-save-mcp-autonomy`.

This ADR freezes the target architecture, public API direction, and worker
protocol used by `CC-WP02` through `CC-WP15`. It is normative for later work
packages, but it does not claim that every target component already exists.
The source-backed migration baseline is the companion
[document collaboration ingress inventory](document-collaboration-ingress-inventory.md).
Later packages may update an inventory row's migration state, but changing an
architectural decision in this file requires a new superseding ADR.

## Scope

This decision covers supported live model mutations entering through App,
Python, GUI, modules, MCP, undo/redo, recovery, save-related mutation, and
recompute. It also fixes the ownership boundary for lightweight preparation,
heavy OCC work, recompute scheduling, final commit, evidence-system
authorization, and evidence-system terminal verdict publication.

The architecture is:

```text
local GUI model intent ─┐
remote MCP typed intent ├─> DocumentCollaborationService
supported App/Python ───┘             |
                           semantic dependency capture
                                      |
                         +------------+------------+
                         |                         |
               PreparedEditExecutor       GeometryJobManager
               lightweight values         isolated FreeCADCmd/OCC
                         |                         |
                         +------------+------------+
                                      |
                        immediate revision/lifecycle revalidation
                                      |
                         DocumentCommitCoordinator
                         sole live transaction owner
                                      |
                                App::Document
```

Personal camera, pan, zoom, selection, preselection, tree expansion, scroll,
active view, edit focus, workbench, and overlays stay in GUI actor state. They
are not model intent and do not participate in model conflict detection.

## Invariants

1. `DocumentCollaborationService` (DCS) is the supported model-intent facade.
   Callers declare typed intent; they do not acquire transaction authority.
2. `DocumentCommitCoordinator` (DCC) is the only owner that may open, commit,
   abort, or roll back a live model transaction for supported ingress. Public
   compatibility methods delegate to DCS/DCC instead of exposing a second
   transaction owner.
3. A DCC commit revalidates the document instance, lifecycle epoch, declared
   semantic read/write/publication sets, and expected revisions immediately
   before changing live state. A stale result never commits.
4. Detached workers never receive `App::Document*`, `DocumentObject*`, GUI
   pointers, transaction handles, or authority grants. Worker payloads and
   results are pointer-free bounded values.
5. The parent-side trusted adapter declares dependencies, validates worker
   output, and constructs `PreparedEdit`. Workers never choose authoritative
   semantic dependencies or publish live revisions.
6. `PreparedEditExecutor` is restricted to trusted lightweight/value work.
   Heavy or untrusted OCC work uses `GeometryJobManager` and defaults to an
   isolated `FreeCADCmd` process. There is no hidden synchronous heavy fallback
   on the GUI thread.
7. Full-document recompute is scheduled by one per-document
   `DocumentRecomputeCoordinator`. It captures immutable dependency-ordered
   work, schedules downstream work only after upstream commit, rejects stale
   output, and commits derived results through DCC without creating new user
   undo entries.
8. A failed recompute feature remains touched/error and receives no partial
   result. Successfully committed upstream features may remain committed.
9. Save while unresolved recompute exists returns structured
   `RECOMPUTE_PENDING` and does not overwrite canonical FCStd. After successful
   completion, the first save is `Written` and the next unchanged save is
   `Unchanged`.
10. Unsupported cross-document atomic mutation is rejected unless a typed
    adapter explicitly implements its atomicity, dependency, rollback, and
    publication contract.
11. Compatibility sessions for legacy long-lived GUI task-panel transactions
    are coordinator-owned. Competing model commits conservatively return
    `Busy` until the session ends.
12. The static ingress gate must have zero unclassified production callers.
    Adding a direct transaction, history, or recompute caller requires an
    inventory classification and its architecture test in the same change.

## Ownership

| Concern | Single owner | Responsibilities | Delivered by |
|---|---|---|---|
| Model-intent admission | `DocumentCollaborationService` | Typed intent, lifecycle pinning, semantic capture, preparation policy, compatibility delegation | Existing facade; preserved by all packages |
| Live transaction and publication | `DocumentCommitCoordinator` | Coordinator grant, transaction open/commit/abort, rollback, final revalidation, exactly-once publication | CC-WP02–CC-WP05 |
| Lightweight detached preparation | App-owned `PreparedEditExecutor` | Trusted bounded value calculations only | CC-WP06 restriction |
| Heavy geometry jobs | App-owned `GeometryJobManager` | Bounded queue, lifecycle, cancellation, deadline, coalescing, progress, isolated worker dispatch | CC-WP06–CC-WP09 |
| Worker process | Installed `FreeCADCmd` entry point | Decode trusted request, run bounded OCC operation, encode result; no live document access | CC-WP07–CC-WP09 |
| Recompute scheduling | Per-document `DocumentRecomputeCoordinator` | Dependency ordering, immutable capture, detached execution, cancellation, stale rejection, partial-failure policy | CC-WP10–CC-WP12 |
| GUI personal state | GUI actor context | Camera/selection/tree/view state only; revision- and dirty-neutral | Existing behavior; regression gates |
| Evidence lifecycle | `EvidenceRunner` | Ordered schema-44 lifecycle and the decision to request terminal finalization | Current code; descriptive decision |
| Final verdict publication | `finalization.finalize()` | Terminal authorization recheck and the sole create-once write of `final-verdict.json` | Current code; descriptive decision |

No global singleton owns geometry jobs. `App::Application` owns
`GeometryJobManager`, as it already owns application-lifetime collaboration
services such as `PreparedEditExecutor`.

## Public APIs

The target public surface is deliberately small:

- `Application::geometryJobManager()` returns the App-owned manager.
- Pointer-free `GeometryJobId`, `GeometryJobState`, request, status, progress,
  cancellation, deadline, coalescing, and result types describe geometry work.
- Preparation policy is one of `Inline`, `DetachedInProcess`, or
  `IsolatedProcess`. Heavy or untrusted geometry selects `IsolatedProcess`.
- Each document owns a `DocumentRecomputeCoordinator` and exposes a
  pointer-free `RecomputeHandle`.
- Python exposes `Document.recomputeAsync(...)` with status, progress,
  cancellation, and completion access.
- Existing synchronous `Document.recompute()` delegates to the same
  coordinator. Headless mode blocks on the handle; GUI mode uses a responsive
  compatibility wait. Neither path starts a second recompute implementation.
- Existing transaction/history/recompute public APIs remain source-compatible
  where required, but their supported mutation behavior delegates through
  DCS/DCC. Low-level transaction primitives and coordinator grants are private.

Compatibility does not mean silent acceptance. Unsupported unsafe features,
unresolved cross-document dependencies, unserializable proxies, undeclared
structural effects, or GUI access from an isolated recompute fail explicitly
and remain touched.

## Worker protocol

`GeometryJobManager` and the installed `FreeCADCmd` worker communicate through
a versioned FCG snapshot/result archive:

- Requests contain a protocol version, job ID, operation type, preparation
  policy, deadline, build fingerprint, bounded section table, digests, and only
  the immutable geometry/value inputs required by the adapter.
- Results echo the protocol version and job ID and contain only operation
  payloads, exact element/history mapping, progress/diagnostic metadata,
  digests, and the worker build fingerprint.
- The trusted parent adapter owns semantic dependency declarations and rejects
  wrong job IDs, wrong digests, version/build mismatch, undeclared output, and
  malformed, truncated, traversing, or oversized archives.
- Publication is atomic. Per-job workspaces are private, bounded, and cleaned
  by the startup janitor. Heartbeat, cancellation, deadline, crash, hang, OOM,
  and worker-tree termination have distinct structured outcomes.
- Windows uses Job Object termination; Unix uses process-group termination.
  A worker result is never a live commit and never contains authority material.
- Boolean, Sweep, and Fillet use process-first native adapters. There is no
  synchronous heavy OCC fallback.

## Schema-44 authorization

The immutable diagnostic package is the single authorization authority.
`review-authorization.json`, its signature, and the reviewer key are captured
from the immutable prerequisite set and validated against the execution
binding. The writable output directory must never carry a second authorization
copy.

Writable output may contain the immutable authorization hash, signature hash,
and binding needed to prove which authorization governed the run. Those are
references, not a second authorization source. A consumer must reject missing,
changed, expired, multiply located, or mismatched authorization rather than
falling back to writable output. This separation keeps the package inventory
immutable and makes create-once output publication compatible with exact
authorization identity.

This decision resolves the former incompatible topology in which legacy
`review_authorization()` expected writable-output authorization while schema-44
`authorization_prerequisites()` expected immutable-package authorization.
There is one authority and one location; legacy consumers must use the captured
immutable prerequisite or its bound hashes.

## Terminal verdict

`EvidenceRunner` is the single owner of terminal lifecycle decisions.
`finalization.finalize()` is the sole publisher of `final-verdict.json`.
Timeout, executor, cleanup, Docker-control, and child-reconciliation paths
return structured outcomes to `EvidenceRunner`; they never publish a verdict.

`EvidenceRunner` may request finalization only after immutable outer execution,
child reconciliation, and ledger validation have produced one final candidate.
`finalization.finalize()` then rechecks the immutable terminal authorization
and create-once publishes exactly one `PASS` or `FAIL`. An interruption before
publication leaves no final verdict; it never synthesizes `PASS`.

This is descriptive of the current tracked evidence-system code, specifically
`EvidenceRunner.run()` and `finalization.finalize()`. It records existing
ownership and is not a migration assigned to a later CC work package.

## Exceptions

The sole permitted direct low-level transaction exceptions are narrowly scoped
bootstrap/restore internals that establish a document before supported runtime
ingress can be admitted, plus rollback performed inside DCC under its private
grant. An exception:

1. is named by concrete file and symbol in the ingress inventory;
2. is unreachable as a general public mutation API;
3. cannot publish collaboration revisions or expose intermediate state;
4. ends before the document becomes live/admissible; and
5. has a test or static gate preventing its scope from widening.

File loading, recovery metadata parsing, and object restoration do not receive
a blanket exception. Any mutation after live identity/admission exists routes
through DCS/DCC. Undo, redo, normal recompute, save-related model mutation, GUI
commands, Python calls, and module commands are supported ingress and are not
bootstrap/restore exceptions.

## Prohibited source integration

`origin/feature/non-blocking-geometry-scaffolding` at concept reference
`be9de15c20` is read-only design input. No CC work package may merge it,
cherry-pick it, or copy it wholesale. Applicable concepts and tests must be
rederived against the current DCS/DCC, revision, lifecycle, save, and MCP
architecture. This prohibition includes partial history integration intended
to disguise a merge or cherry-pick; only reviewed, package-scoped source edits
are allowed.
