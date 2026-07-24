# Non-blocking Geometry Architecture for FreeCAD

## 1. Target architecture and acceptance contract

All live model and UI state remains on the parent GUI thread. Only detached immutable data crosses a worker boundary.

```text
GUI thread
──────────
Command / task panel / Python proxy
  → DocumentRecomputeCoordinator
  → capture immutable inputs in bounded slices
  → GeometryJobManager
       │
       ├─ verified, cancellable operation → in-process worker
       │
       └─ default / crash-prone operation → FreeCADCmd via QProcess
                                                    │
Worker boundary                                     │
────────────────                                    │
Private TopoShape + parameters                      │
  → OCC boolean / sweep / fillet / tessellation     │
  → mapped TopoShape or chunked render-mesh result  │
  → progress / cancellation / deadline              │
                                                    ↓
GUI thread
──────────
Validate document incarnation + generation + target identity
  → time-sliced StringHasher/ElementMap merge
  → atomic document transaction and Shape swap
  → coalesced tessellation request
  → time-sliced Coin chunk construction
  → atomic scene-graph generation swap
```

Measurable acceptance requirements:

- No OCC geometry construction or `BRepMesh_IncrementalMesh` executes on the GUI thread.
- No parent worker receives an `App::Document*`, `DocumentObject*`, Python object, ViewProvider, Coin node, or widget.
- Each application-controlled GUI callback or commit slice is at most 33 ms.
- A 10 ms Qt heartbeat has no gap greater than 100 ms during calculation, serialization, cancellation, timeout, result import, or visual update.
- Progress becomes visible within 250 ms.
- Camera navigation, painting, window movement, and cancellation remain operational throughout.
- There is no synchronous heavy-operation fallback. Unsupported work fails with an actionable error or uses isolated compatibility.
- Sweep, boolean, fillet, and arbitrary tessellation start process-first. Promotion to an in-process worker requires an explicit operation/OCCT-version allowlist.

## 2. Core job, recompute, and process design

### Core APIs

Add generic, Part-independent scheduling types under `src/App`:

```cpp
struct DocumentRevisionToken {
    Base::Uuid documentUid;
    std::string internalName;
    uint64_t runtimeIncarnation;
    uint64_t modelGeneration;
};

struct ObjectRevisionToken {
    std::string internalName;
    long objectId;
    Base::Type type;
};

enum class GeometryJobPurpose {
    ModelRecompute,
    Preview,
    Tessellation,
    LegacyIsolatedRecompute
};

enum class GeometryBackend {
    FreeCADCmd,
    VerifiedInProcess
};

enum class GeometryJobState {
    Queued,
    Snapshotting,
    Running,
    Cancelling,
    Decoding,
    ReadyToCommit,
    Completed,
    Cancelled,
    TimedOut,
    Crashed,
    Failed,
    Stale,
    DocumentClosed
};

struct GeometryJobSpec {
    GeometryJobKey key;
    DocumentRevisionToken document;
    ObjectRevisionToken target;
    GeometryBackend backend;
    std::chrono::steady_clock::time_point deadline;
    CoalesceMode coalescing;
    std::shared_ptr<const DetachedGeometryTask> task;
};

class DetachedGeometryTask {
public:
    virtual std::string operationType() const = 0;
    virtual uint32_t codecVersion() const = 0;
    virtual GeometryOperationTraits traits() const = 0;
    virtual DetachedGeometryResult run(GeometryWorkerContext&) const = 0;
    virtual void writeArchive(GeometryArchiveWriter&) const = 0;
};

class GeometryJobManager {
public:
    GeometryJobHandle submit(GeometryJobSpec);
    void cancel(GeometryJobId, CancelReason);
    void invalidateDocument(const DocumentRevisionToken&, CancelReason);
    void invalidateObject(const ObjectRevisionToken&, CancelReason);
};

class DocumentRecomputeCoordinator {
public:
    RecomputeHandle request(Document&, RecomputeTargets, RecomputeOptions);
};
```

Add an optional two-phase contract to `App::DocumentObject`:

```cpp
virtual std::optional<PreparedDetachedRecompute>
prepareDetachedRecompute(const SnapshotContext&) const;

virtual DocumentObjectExecReturn*
commitDetachedRecompute(const DetachedGeometryResult&, CommitContext&);
```

The default is unsupported for heavy asynchronous execution. Deprecate the current broad `canRecomputeOnWorker()` contract and change its default to `false`.

### Document lifecycle and generations

Each `App::Document` receives:

- A persistent document UUID.
- A non-reusable runtime incarnation, changed after close/reopen even when the name or pointer is reused.
- A monotonically increasing model generation.
- One GUI-thread-affine `DocumentRecomputeCoordinator`.

Generation advances on property changes, add/remove, `touch()`, undo/redo, external transaction changes, and structural link changes. Do not reuse `DocumentMutationAuthority` fencing generations; those represent MCP ownership rather than model revision.

Job commit requires all of:

- Document UUID, runtime incarnation, and generation still match.
- Object name, ID, and type still identify the target.
- The job is still the coordinator’s current job.
- Input shape fingerprints and parameter digest still match.
- Document is neither closing nor restoring.
- No other recompute commit is active.

A `GeometryCommitScope` suppresses self-invalidation while installing a result, then advances the generation once and schedules downstream dependencies. Result signals are queued and batched; they must not invoke tessellation inline.

### Recompute state machine

Replace the monolithic GUI-thread `Document::recompute()` loop with a coordinator state machine:

1. Capture the dependency graph as plain object IDs and links in GUI slices capped at 8 ms.
2. Topologically sort the copied graph away from the live document.
3. Process dependent features in order; do not capture a downstream feature until its inputs have committed.
4. Permit only one model-recompute session per document.
5. Join identical requests and union dirty roots for simultaneous full recompute requests.
6. Open an undo transaction only around a valid final commit. Cancellation, timeout, crash, stale output, or snapshot failure creates no transaction.
7. Batch dependent-object touching and signal delivery in bounded GUI slices.

Internal GUI callers use `RecomputeHandle` asynchronously. Add `Document.recomputeAsync()` to Python.

For compatibility, existing `Document.recompute()` behaves as follows:

- In GUI mode, submit asynchronously and wait using a nested `QEventLoop`, not `processEvents()`. Painting, navigation, progress, and cancellation remain enabled; mutations of the same document are deferred or invalidate the request.
- In headless mode, synchronously wait for the same coordinator because there is no GUI event loop.
- Internal commands and task panels must not use this compatibility wait.

### Coalescing and scheduling

Use `(document incarnation, target object ID, purpose, preview channel)` as the coalescing key.

- Identical same-generation jobs share one result.
- Preview and tessellation are latest-write-wins.
- A newer generation cancels the active older job and replaces its pending request.
- A final feature calculation cancels previews for that feature.
- Full recomputes join one per-document session.
- Priority is final model calculation, preview model calculation, coarse tessellation, then detailed tessellation.
- Default to one global OCC-heavy child process to avoid CPU and memory starvation. Run it below normal OS priority and reserve at least two logical CPUs for the GUI/OS where supported.
- Never force-terminate an in-process thread.

### Deadlines and cancellation

Default GUI deadlines:

| Job | Deadline |
|---|---:|
| Live preview | 10 seconds |
| Coarse tessellation | 15 seconds |
| Detailed tessellation | 60 seconds |
| Final sweep/boolean/fillet | 120 seconds |
| Full recompute session | 10 minutes |

Expose bounded preferences from 1 to 3600 seconds; GUI jobs cannot select an infinite deadline.

For process jobs:

1. Send cooperative cancellation.
2. After 250 ms, call `QProcess::terminate()`.
3. After another 750 ms, call `QProcess::kill()`.
4. Kill the complete Windows Job Object or Unix process group.
5. Never use `waitForStarted()` or `waitForFinished()` on the GUI thread.

An in-process operation is eligible only if automated stress tests prove cooperative cancellation completes within one second. A missed deadline removes that operation/OCCT build from the allowlist.

### Process protocol

Launch one trusted worker per unsafe job:

```text
FreeCADCmd --safe-mode <installed Part/GeometryWorker.py>
```

The installed script imports `Part` and calls a private native `Part._runGeometryWorker()` entry point. No path, Python source, module name, or output location supplied by the request is executed.

Workspace:

```text
<UserCache>/GeometryJobs/<random-job-id>/
  owner.lock
  request.json.tmp → request.json
  input.fcg
  result.fcg.tmp → result.fcg
  worker.log
```

`request.json` contains protocol, FreeCAD/OCC build fingerprint, job ID, operation ID/version, document and object stamps, generation, deadline, typed scalar parameters, input size, checksum, and relative result path.

Bulk geometry uses a versioned `FCG1` archive with:

- Binary/BinTools geometry.
- `TopoShape::Tag`.
- Element-map version.
- Explicit `ElementMap` data, including child maps.
- Referenced `StringHasher` closure and its high-water ID.
- Source-object identities and mapped subelement references.
- Per-section lengths, limits, and SHA-256 checksums.

Control messages use prefixed JSON Lines so ordinary console output is ignored:

```text
FCGEO/1 {"type":"hello", ...}
FCGEO/1 {"type":"progress","phase":"boolean.build","fraction":0.42}
FCGEO/1 {"type":"heartbeat", ...}
FCGEO/1 {"type":"result","path":"result.fcg","size":...,"sha256":"..."}
FCGEO/1 {"type":"error","code":"occ_error","message":"..."}
```

Progress is limited to 20 messages per second. The child reads `cancel` messages on a control thread and exposes the atomic flag and monotonic deadline through an OCC `Message_ProgressIndicator`.

The child writes and closes `result.fcg.tmp`, atomically renames it, and only then emits `result`. Crash exit, timeout, protocol mismatch, malformed or oversized data, checksum failure, missing final artifact, or partial rename is failure with no commit.

### Topological naming across processes

Raw BREP is forbidden because it loses `ElementMap` and OCC-generated history.

- Snapshot capture pins the source geometry and copies map/hasher values into an immutable `FrozenTopoShapeBundle` in bounded GUI slices.
- Binary shape serialization and deep materialization happen off-thread from that frozen bundle.
- Published document shapes are treated as immutable. Any algorithm that calls `BRepTools::Clean()` or may mutate triangulation first makes a private copy.
- The child restores all input shapes against one cloned `StringHasher`.
- Boolean, sweep, and fillet calculators use existing `makeShapeWithElementMap`/mapped `TopoShape` paths while the OCC builder still exposes `Generated()` and `Modified()`.
- The child serializes the completed output `TopoShape`, map, history, and exact-ID hasher delta before destroying the OCC builder.
- Parent decoding and checksum validation occur off-thread.
- The GUI thread time-slices exact-ID hasher-delta application. Existing IDs must match byte-for-byte; any collision or revision mismatch rejects the result.
- The output map is rebound to the canonical document hasher, and the complete prepared `TopoShape` is swapped into `PropertyPartShape` without reconstructing or retagging history.
- IDs appended before an exceptional commit failure remain unreachable and are removed during normal hasher compaction.

### Isolated compatibility for unported features and Python proxies

Add `DetachedDocumentArchive` for a target feature’s transitive dependency closure.

- Capture ordinary property values and shape snapshots incrementally.
- Load the archive into a temporary child-process `App::Document` on that process’s main thread.
- Run the unported feature or Python proxy there.
- Return only declared output properties and complete `TopoShapeArchive` results.
- Reject structural add/remove side effects, GUI access, external process state, and nonserializable proxy state.
- A Python proxy may opt in with a bounded `prepareDetachedRecompute()` hook returning only value parameters, property references, and an operation identifier.
- Failure to snapshot or replay an unported proxy produces an actionable compatibility error. It never falls back to parent-GUI execution.

## 3. Tessellation and ViewProvider pipeline

Extract tessellation from `ViewProviderPartExt::setupCoinGeometry()` into GUI-independent Part code:

```cpp
struct VisualTessellationParameters {
    double deviation;
    double angularDeflection;
    bool normalsFromUV;
    VisualLod lod;
};

struct VisualMeshChunk {
    uint32_t sequence;
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<int32_t> triangleIndices;
    std::vector<FaceRun> faceRuns;
    std::vector<int32_t> lineIndices;
    std::vector<ElementId> edgeIds;
    std::vector<ElementId> vertexIds;
};

class VisualTessellator {
public:
    VisualMeshResult run(const FrozenTopoShapeBundle&,
                         const VisualTessellationParameters&,
                         GeometryWorkerContext&);
};
```

`VisualTessellator` performs `BRepMesh_IncrementalMesh`, face/edge traversal, and normal generation on private geometry. Arbitrary BRep meshing is process-first.

Add:

- `PartGui::VisualUpdateScheduler`: latest-wins tessellation scheduling and lifetime/generation validation.
- `PartGui::VisualCommitScheduler`: main-thread-only, time-sliced Coin work.
- `PartGui::VisualCommitTask`: owns one staging generation.
- `PartGui::SoBrepGeometryGeneration` and `SoBrepGeometryChunk`: element-aware chunked scene graph.

Visual behavior:

- `Gui::Document::slotChangedObject()` and `ViewProviderPartExt::updateData()` only invalidate and submit; they never tessellate.
- Keep the last complete mesh visible. For a first render, display a lightweight bounding box until coarse LOD arrives.
- Generate coarse LOD first, capped at 50,000 triangles, then detailed chunks.
- Worker artifact chunks are capped at 25,000 triangles or 1 MiB.
- GUI Coin writes use adaptive subchunks starting at 64 KiB, never exceeding 256 KiB or 8 ms per event-loop turn.
- Each Coin/VBO node owns only one bounded chunk, preventing a later monolithic GPU upload.
- Build a staging generation, then swap it into an `SoSwitch` atomically.
- Face, edge, and vertex chunks carry global element IDs. Picking, highlighting, material assignment, and selection translate local chunk IDs back to `FaceN`, `EdgeN`, and `VertexN`.
- Appearance-only changes use a separate generation and update materials in bounded slices without remeshing.
- Hidden objects cancel visual work; showing them requests the latest generation.
- Preview, task-preview, and committed-shape channels have independent generations.
- A cancelled, stale, crashed, or malformed visual result leaves the prior complete mesh intact.

## 4. Phased implementation and migration

### Phase 1 — Guardrails and scheduler foundation

- Add document UUID/incarnation/generation tracking, thread-affinity assertions, `GeometryJobManager`, `DocumentRecomputeCoordinator`, handles, progress state, coalescing, deadlines, and cancellation.
- Replace the current live-document async worker. No worker may call `Document::recompute()` or `recomputeFeature()`.
- Change worker safety to explicit opt-in.
- Add a GUI heartbeat and callback-duration probe used by tests and debug builds.
- Add event-driven progress UI to `SequencerBar`; do not use `qApp->processEvents()`.

### Phase 2 — Archive and isolated worker

- Implement immutable `StringHasherSnapshot`, explicit `ElementMapArchiveContext`, `FrozenTopoShapeBundle`, and `TopoShapeArchive`.
- Implement the trusted FreeCADCmd worker registry and QProcess controller.
- Add atomic result publication, process-tree ownership, deadlines, crash handling, checksum validation, workspace RAII, and startup cleanup.
- Qualify archive round trips and topological history before migrating any command.

### Phase 3 — Non-blocking visual pipeline

- Move OCC meshing and mesh extraction out of `ViewProviderPartExt`.
- Introduce process-first tessellation, progressive LOD, element-aware chunks, and bounded Coin commits.
- Migrate normal ViewProvider updates and preview updates.
- Disable synchronous live preview for features that do not yet provide a detached adapter.

### Phase 4 — Native geometry migrations

Boolean:

- Snapshot operands, operation type, tolerance/fuzzy parameters, refinement policy, mapped identities, and target tag.
- Refactor `FCBRepAlgoAPI_Cut/Fuse/Common` usage into `Part::BooleanGeometryOperation`.
- Migrate Part BOPTools and PartDesign Boolean task changes to coalesced preview requests.
- Commit properties and mapped output shape once, on the GUI thread.

Fillet:

- Snapshot base shape, mapped edge selections, radii, variable-radius data, and refinement policy.
- Move `BRepFilletAPI_MakeFillet` into `Part::FilletGeometryOperation`.
- Keep fillets process-only until OCC exposes and passes bounded cooperative cancellation.
- Radius/edge UI changes replace the pending preview rather than calling `recomputeFeature()`.

Sweep/Pipe:

- Snapshot spine, ordered profiles, mapped subelements, transition mode, Frenet/binormal settings, solid flag, and PartDesign additive/subtractive context.
- Move `BRepOffsetAPI_MakePipeShell` into `Part::SweepGeometryOperation`.
- Preserve the section-order validation that protects OCC from invalid input.
- Run final PartDesign additive/subtractive boolean as part of the same child job so history is produced in one result archive.

Task-panel behavior:

- Live changes submit a debounced, latest-wins preview after 150 ms.
- OK submits an immediate final job and leaves the panel responsive with visible progress and Cancel.
- The panel closes only after a valid commit.
- Failure leaves the panel open and retains the last valid shape.
- Cancel aborts the task transaction and worker job.
- Part commands that do not create an object until acceptance create and populate it in the successful commit transaction.
- Existing PartDesign task transactions may remain open while waiting, but same-document model mutations are disabled; navigation and cancellation remain available.

### Phase 5 — Recompute and legacy compatibility

- Route `Std_Refresh`, `Std_Recompute`, tree recompute, `Command::updateActive()`, Python recompute, task finalization, and direct feature recompute through `DocumentRecomputeCoordinator`.
- Process dependency-ordered migrated features through detached jobs.
- Use `DetachedDocumentArchive` for eligible unported native features and Python proxies.
- Reject unsupported heavy synchronous execution rather than invoking it on the GUI thread.
- Ensure only successful final commits create undo entries.

### Phase 6 — Qualification and rollout

- Keep all new behavior behind development flags until geometry, visual, crash, and heartbeat tests pass together.
- Enable the process-first path by default only after sweep, boolean, fillet, recompute, and tessellation all meet the 33/100 ms contract.
- Maintain an initially empty in-process allowlist for migrated geometry operations.
- Promote an operation only for an exact operation/OCCT/build combination after cancellation, race, fuzz, and long-running stress qualification.
- Record job durations, cancellation latency, GUI slice duration, heartbeat gaps, crash exits, stale-result counts, and worker cleanup failures without recording model data.

## 5. Exact files and classes

### Core App and recompute

Modify:

- [Application.h](D:/code/FreeCAD/src/App/Application.h:100) and [Application.cpp](D:/code/FreeCAD/src/App/Application.cpp:825): remove live-document worker execution and own the new manager/coordinators.
- [Document.h](D:/code/FreeCAD/src/App/Document.h:109), [Document.cpp](D:/code/FreeCAD/src/App/Document.cpp:2850), and [DocumentP.h](D:/code/FreeCAD/src/App/private/DocumentP.h:72): revision tokens, incarnation, sliced recompute state, commit scope, and close invalidation.
- [DocumentObject.h](D:/code/FreeCAD/src/App/DocumentObject.h:1298) and [DocumentObject.cpp](D:/code/FreeCAD/src/App/DocumentObject.cpp:216): detached preparation/commit contract and touch invalidation.
- [Property.cpp](D:/code/FreeCAD/src/App/Property.cpp:304): generation notification before external writes.
- [DocumentPyImp.cpp](D:/code/FreeCAD/src/App/DocumentPyImp.cpp:738) and `Document.xml`: `recomputeAsync()` and GUI compatibility wait.

Add:

- `src/App/GeometryJob.{h,cpp}`
- `src/App/GeometryJobManager.{h,cpp}`
- `src/App/DocumentRecomputeCoordinator.{h,cpp}`
- `src/App/DetachedDocumentArchive.{h,cpp}`

### Process transport and naming

Modify:

- [StringHasher.h](D:/code/FreeCAD/src/App/StringHasher.h:632) and `StringHasher.cpp`: immutable snapshot, revision/high-water tracking, exact-ID delta validation and resumable merge.
- [ElementMap.h](D:/code/FreeCAD/src/App/ElementMap.h:80) and `ElementMap.cpp`: explicit archive context; transport must not use the current process-global save/restore tables.
- [TopoShape.h](D:/code/FreeCAD/src/Mod/Part/App/TopoShape.h:293), `TopoShape.cpp`, and [TopoShapeExpansion.cpp](D:/code/FreeCAD/src/Mod/Part/App/TopoShapeExpansion.cpp:292): frozen capture and prepared canonical-hasher import.
- [AppPartPy.cpp](D:/code/FreeCAD/src/Mod/Part/App/AppPartPy.cpp): private trusted worker entry point.

Add:

- `src/Mod/Part/App/TopoShapeArchive.{h,cpp}`
- `src/Mod/Part/App/GeometryWorker.{h,cpp}`
- `src/Mod/Part/App/GeometryWorkerRegistry.{h,cpp}`
- `src/Mod/Part/GeometryWorker.py`
- `src/Gui/GeometryWorkerProcess.{h,cpp}`
- `src/Gui/GeometryProgressController.{h,cpp}`

Update App, Gui, Part App, and Part Gui CMake/install lists.

### Geometry features and callers

Modify operation implementations:

- [PartFeatures.cpp](D:/code/FreeCAD/src/Mod/Part/App/PartFeatures.cpp:303) for `Part::Sweep`.
- [FeaturePartBoolean.cpp](D:/code/FreeCAD/src/Mod/Part/App/FeaturePartBoolean.cpp:120) and `FCBRepAlgoAPI_BooleanOperation.*`.
- [FeatureFillet.cpp](D:/code/FreeCAD/src/Mod/Part/App/FeatureFillet.cpp:48).
- [FeaturePipe.cpp](D:/code/FreeCAD/src/Mod/PartDesign/App/FeaturePipe.cpp:122).
- [PartDesign FeatureBoolean.cpp](D:/code/FreeCAD/src/Mod/PartDesign/App/FeatureBoolean.cpp:67).
- [PartDesign FeatureFillet.cpp](D:/code/FreeCAD/src/Mod/PartDesign/App/FeatureFillet.cpp:77).

Add `BooleanGeometryOperation`, `FilletGeometryOperation`, and `SweepGeometryOperation` under `src/Mod/Part/App`.

Modify GUI callers:

- [Part Command.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/Command.cpp:1397), [TaskSweep.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/TaskSweep.cpp:305), [DlgBooleanOperation.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/DlgBooleanOperation.cpp:375), and [DlgFilletEdges.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/DlgFilletEdges.cpp:1034).
- [PartDesign Command.cpp](D:/code/FreeCAD/src/Mod/PartDesign/Gui/Command.cpp:603), [TaskFeatureParameters.cpp](D:/code/FreeCAD/src/Mod/PartDesign/Gui/TaskFeatureParameters.cpp:168), `TaskPipeParameters.cpp`, `TaskBooleanParameters.cpp`, and `TaskFilletParameters.cpp`.
- [CommandDoc.cpp](D:/code/FreeCAD/src/Gui/CommandDoc.cpp:1789), [CommandFeat.cpp](D:/code/FreeCAD/src/Gui/CommandFeat.cpp:47), [Gui Command.cpp](D:/code/FreeCAD/src/Gui/Command.cpp:1012), and [Tree.cpp](D:/code/FreeCAD/src/Gui/Tree.cpp:1472).

### Visual pipeline

Modify:

- [ViewProviderExt.h](D:/code/FreeCAD/src/Mod/Part/Gui/ViewProviderExt.h:182) and [ViewProviderExt.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/ViewProviderExt.cpp:1034).
- `ViewProviderPreviewExtension.{h,cpp}` and [PreviewUpdateScheduler.cpp](D:/code/FreeCAD/src/Mod/Part/Gui/PreviewUpdateScheduler.cpp:35).
- [SoFCShapeObject.h](D:/code/FreeCAD/src/Mod/Part/Gui/SoFCShapeObject.h:46) and `SoFCShapeObject.cpp`.
- `SoBrepFaceSet.{h,cpp}`, `SoBrepEdgeSet.{h,cpp}`, and `SoBrepPointSet.{h,cpp}`.
- [PartDesign ViewProvider.cpp](D:/code/FreeCAD/src/Mod/PartDesign/Gui/ViewProvider.cpp:213).

Add:

- `src/Mod/Part/App/VisualTessellation.{h,cpp}`
- `src/Mod/Part/Gui/VisualUpdateScheduler.{h,cpp}`
- `src/Mod/Part/Gui/VisualCommitScheduler.{h,cpp}`

## 6. Safety invariants

1. The parent live `App::Document` and every object/property are accessed only on their owning GUI thread.
2. No live document is moved into a `QThread`.
3. Worker payloads own or pin immutable data and contain no live model or GUI pointers.
4. Published shapes are immutable; mutating OCC algorithms operate on private copies.
5. Exactly one model recompute session and one commit can exist per document.
6. Every result is fenced by document incarnation, model generation, object identity, input fingerprint, parameter digest, and job identity.
7. Document close invalidates the incarnation before object destruction and never waits for a worker.
8. Late callbacks resolve stable IDs through the manager; they never dereference captured raw pointers.
9. Transactions exist only around valid GUI-thread commits.
10. Topological history is created and serialized in the worker; raw BREP never crosses the boundary as a complete result.
11. Coin and Qt objects are created, updated, and destroyed only on the GUI thread.
12. Coin work, notification delivery, graph planning, hasher merging, and cleanup are time-sliced.
13. In-process execution is deny-by-default and cannot be force-terminated.
14. Crash, timeout, cancel, stale output, decode failure, or disk failure leaves the previous shape and previous complete visual generation intact.
15. Rollback may use process-only execution or disable an operation; it may not restore synchronous GUI-thread OCC.

## 7. Test plan, risks, and rollback

### Unit tests

- Scheduler state transitions, priorities, identical-request joining, latest-wins replacement, and callback exactly once.
- Generation changes for property writes, add/remove, touch, undo/redo, and structural links.
- Document/object delete and recreate with reused names or addresses.
- Internal commits do not self-invalidate; observer mutations schedule a later generation.
- Archive round trips for nested element maps, shared hashers, long hashed names, placements, tags, and child maps.
- Exact-ID hasher-delta merge, collision rejection, revision mismatch, and interrupted merge.
- Sweep, boolean, fillet, and tessellation calculator equivalence against current geometry.
- Cancellation and deadline checks at every cooperative progress point.
- Mesh positions, normals, face/edge/vertex ordering, reversed faces, free edges, compounds, and null shapes.
- Published input shape triangulation remains unchanged.

### Integration and topology tests

- Cross-process sweep, boolean, and fillet preserve geometry, mapped names, `getElementHistory()`, and FCStd save/reopen results.
- A returned mapped shape can feed a subsequent mapped operation without hasher warnings or naming loss.
- Edit inputs while a job runs; the old result never commits and the newest request eventually does.
- Two recompute requests for one document never overlap.
- Independent documents remain usable while one has a long job.
- Only a successful result creates one undo transaction.
- Task-panel preview cancellation and final acceptance preserve existing UX and transactions.
- Eligible Python proxy recompute succeeds in an isolated snapshot; GUI-dependent or structurally mutating proxies fail without parent execution.

### Responsiveness and visual tests

- Run a 10 ms Qt heartbeat during a synthetic 30-second OCC job and multi-million-triangle visual update.
- Assert maximum heartbeat gap below 100 ms and every instrumented GUI slice below 33 ms.
- Send repeated camera-navigation events and assert processing latency below 100 ms.
- Submit 100 rapid preview or visual invalidations and observe exactly one latest-generation commit.
- Verify progressive coarse-to-detailed rendering while navigation remains responsive.
- Verify split-face, split-edge, and split-vertex picking, highlighting, and materials across chunks.
- Confirm every Coin mutation occurs on the Qt thread.
- Confirm stale or failed visual generations never replace the last complete mesh.

### Cancellation, timeout, crash, and cleanup tests

- Cooperative cancel completes an allowlisted in-process job within one second.
- Cancel a non-cooperative fillet and verify process termination within 1.5 seconds.
- Simulate child hang, abort/access violation, OOM exit, malformed protocol, bad checksum, oversized output, truncated result, disk full, and kill during atomic rename.
- Close the document or remove the feature during snapshot, execution, decoding, naming merge, document commit, and Coin commit.
- Verify no use-after-close, no stale transaction, no partial visual, and no orphan progress item.
- After every failure mode, submit another job and verify successful recovery.
- Verify temporary workspace cleanup, Windows locked-file retry, and startup janitor behavior.

### Principal risks and mitigations

- Topological naming mismatch: require archive equivalence and downstream-history tests before rollout; reject rather than reconstruct.
- Python/add-on compatibility: use isolated compatibility only for snapshot-safe outputs; provide diagnostics and an explicit adapter API.
- Coin/VBO driver stalls: enforce per-node chunk limits and progressive staging; never create one monolithic buffer.
- Memory amplification from old/coarse/staging meshes: apply per-document memory budgets and evict detailed staging before the last valid/coarse generation.
- Process startup and archive overhead: debounce previews, coalesce requests, and allow later promotion of qualified operations.
- CPU starvation despite process isolation: one heavy process by default, below-normal priority, and reserved CPU capacity.
- Nested Python compatibility waits: restrict same-document mutation and test reentrancy; internal GUI callers remain fully asynchronous.

### Rollback

Maintain independent feature switches for:

- Process geometry backend.
- In-process allowlist.
- Detached recompute coordinator.
- Progressive tessellation.
- Chunked Coin commit.
- Isolated legacy compatibility.

A safe rollback routes all supported work to process-only, retains the previous visual/bounding-box placeholder, or disables the affected operation with an explanatory error. No rollback path may execute heavy OCC or tessellation synchronously on the GUI thread.

### Locked assumptions

- Responsiveness budget is 33 ms per GUI slice and 100 ms maximum heartbeat gap.
- Sweep, boolean, fillet, and arbitrary tessellation ship process-first.
- Unported features use isolated compatibility where snapshot-safe; there is no synchronous fallback.
- The parent’s live documents, Python proxies, ViewProviders, Coin nodes, and widgets remain on the GUI thread.
- Child-process temporary documents are allowed because they are isolated copies on the child’s own main thread.
- No phase is enabled by default until the combined geometry, visual, crash-recovery, and responsiveness suite passes.
