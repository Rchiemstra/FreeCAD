# Parallel recompute M0: contracts, runbook and measured serial baseline

Research date: 2026-10-30. Inspected source/plan commit:
`e1446662caadf9ca2e2335fd81ee4eed460f01a4`, branch
`research/parallel-recompute-m0`. This is the source in this clone, not a build
identity. The plan cites an earlier product audit at `d7b0d2ffa7fce1ac21a5a9fdabdfd901fea434d8`;
its measurements have not been repeated here.

**The M0 exit gate is not satisfied.** The initial static audit used no
FreeCAD/OCC executable; the local serial measurements added later are recorded
in section 7. Sections 1–7 used no compiler, CMake build, package installation,
upstream run or race detector; section 8 resolves py-spy into an isolated run
cache for profiling. Nothing under `src/` changes. No production scheduling
or parallel feature execution is introduced. All four live target methods need
serialization. Their proposed detached kernels remain unqualified.

`tools/parallel_recompute_m0_probe.py` is a FreeCAD serial measurement probe,
with local execution and summarization helpers under `tools/`. It changes temporary in-memory
documents and writes generated fixtures/results to an explicit output directory;
it never saves over an existing input FCStd. Its measured behavior and limitations
are recorded in section 7. Do not treat it as a race detector,
an adapter, or a geometry/naming equivalence oracle.

## 1. Deliverables and present status

This expands the three M0 slices and exit criteria in
`doc/parallel-recompute-plan.md:597–622`. “Needs build” includes the indicated
future native instrumentation/test implementation, which the research-only
scope excludes. A specification is not an implemented or passing test.

| M0 deliverable | Doable now? | Status / evidence or remaining work |
| --- | --- | --- |
| Enumerate M0 work, qualification template and negative admission cases | Static | **Done:** this table and sections 2–4. |
| Candidate input/output/invalidation inventory: exact Box, Cut, Fuse, Common; naming, caches, callbacks and OCC settings | Static audit; runtime closure needed | **Done (static):** section 2. Complete *qualified* contracts still **need build**, dependency instrumentation and kernel coverage. |
| Opt-in phase timing, reproducible fixture capture, build/DAG/resource identity | Local serial probe; native instrumentation needed | **Done (local serial):** validated probe, restored synthetic fixtures, local build manifest, coarse DAG and process RSS (section 7). Native phase spans, production property graph, concurrency and aggregate memory traces still need implementation/build. |
| Detached Box/Boolean geometry harness and negative contract tests, with unchanged production scheduling | Static harness/test specification | **Partial (Box test-only):** qualification template, negative admission tests, and a test-only owner publication of one seeded Box in `tests/src/Mod/Part/App/DetachedKernelQualification.*`. That step compares the thread kernel with the owner recompute and with the same step applied to the process shape. It is not production publication. Production `Document::recompute` scheduling is unchanged. The M0.2b subsection records the same test-only harness for Cut, Fuse and Common. Those kernels stay not detached. |
| Compare serial/thread/process costs, wide/narrow and incremental workloads, two narrow controls | Local serial measurements; detached harness needed | **Partial (Box venues):** section 7 is the serial baseline. The M0.2a subsection records owner, worker-thread and isolated-process Box costs. Paid time on every venue is that venue's end-to-end wall, from before capture through commit. The process wall is partitioned in the subsection, and there is no persistent worker to amortise it. The section 4 wide and narrow figures are the earlier owner-thread serial traces, not this venue method. Boolean costs are in the M0.2b subsection. That text is input to the owner, not a venue decision. Thread qualification stays open. |
| Working instrumented race checks, including cold/error/cancel cases | Static gate design | **M0.3a:** FreeCAD TSan runs exist. Startup was a reported race, then the probe was removed. Boolean **fails** on the `Part::SignalException` static. Box is **blocked**: no FreeCAD-side race, and OCC is not instrumented. No suppressions. Thread adapters stay off. Cold/error/cancel TSan is still open. **M0.3b:** serial cold restore, failed feature, and mid-recompute cancel (results, publications visible inside the observer, undo/redo signal traces) are pinned on the `RecomputeHandle` path in `tests/src/App/RecomputePublicationContracts.cpp`. Lane PartialFailure/Cancelled finalization is a known defect, `FIX-LANE-TERMINAL-FINALIZE`, not that contract. |
| Pinned upstream and ordinary/macro/add-on compatibility baseline fixtures | Local headless baseline; pinned upstream still needed | **Done (local baseline):** all nine source fixtures restore/recompute in all/noop (section 7), with genuine non-null proxies and no missing add-on dependencies reported. BIM SVG/text output remains GUI-only. **M0.3b:** seven small serial documents are copied with the Test module to `Mod/Test/TestData/parallel-recompute` and asserted by `ParallelRecomputeBaseline`, including a forced rebuild of volumes and box bounds. Element names and topology equivalence stay outstanding. Bidirectional compatibility/naming/macro runs still need the pinned upstream build; final large user corpus/add-on version selection remains **blocked** on owner input. |
| Existing native App/Part/Gui baseline suites | Local supplied binaries | **Done:** 42 App + 8 Part + 1 Gui tests pass, zero failures/errors/skips, exact expected names present (section 7). These do not implement the new detached/race/publication qualification tests. |
| Exact sibling publication, broad conflict invalidation and existing client visibility | Static assertions/test placement | **Done (serial pin, M0.3b):** `tests/src/App/RecomputePublicationContracts.cpp`. An exact publication invalidates prepared clients that cover that key. A disjoint exact publication leaves the other client valid. A legacy recompute publishes `unknownModelMutation` and informs existing clients; covered contracts conflict. Generic serialization is unchanged. |
| Callback order A/B/Z, PR #114, terminal failure/cancel presentation, undo/rollback | Static source comparison/test matrix | **Done (serial pin, M0.3b):** siblings, chain, A/B/Z readiness, and the PR #114 same-value Length/Width path, including `PartPresentationCaptureTest.UnchangedDatumExtentsPreserveRevisionFence`. Changed callbacks already see the new value and its journal entry. Handle PartialFailure and Cancelled emit `signalRecomputedObject`, `signalRecomputed`, and one presentation callback. The lane skips those for the same states (`FIX-LANE-TERMINAL-FINALIZE`, section 5 item 5). Caller-transaction undo restores property values; a failed feature's error bit, touch, and recompute-log diagnostic are not undone. |
| Safe process transport/lifetime, justified cost/venue and credible end-to-end gain supporting M1 | Static acceptance conditions | **Needs build** and harness implementation; conditional process-only pilot **blocked** on owner decision if thread qualification cannot be achieved. |

## 2. Feature contracts from this source

### Classification and admission boundary

The first targets are **exact** native `Part::Box`, `Part::Cut`, `Part::Fuse` and
`Part::Common` (plan lines 288 and 635). `Part::Boolean` is an abstract base;
`MultiFuse`, `MultiCommon`, PartDesign, Assembly and FeaturePython are not extra
pilot admissions. This inventory includes the inherited recompute and setter
paths, not just the body of each `execute()`.

Classifications:

- **Safe-in-parallel (S):** an immutable, privately owned value calculation;
  copying three doubles or using a frozen enum does not require a shared model.
  This does not qualify the OCC call which consumes those values.
- **Needs-serialization (N):** live property/object/document access with effects,
  callbacks, shared mutable state or owner/interpreter affinity. Capture and
  commit occur on the owner, in existing semantic order.
- **Unknown (U):** safety cannot be established from these sources alone,
  particularly library internals and independence of copied geometry. Exclude
  from concurrent admission until the instrumented gate passes.

| Exact target | Live `recompute()/execute()` classification | Proposed private calculation | Kernel classification now |
| --- | --- | --- | --- |
| `Part::Box` | **N**: Shape setter, status, attachment extension and notifications | Captured L/W/H/tolerance → newly built unplaced raw shape or diagnostic | **U**: specific OCC build/cold paths not instrumented |
| `Part::Cut` | **N**: inherited Boolean contract below, CUT opcode | Private operands + frozen policy → raw CUT result + replayable history | **U**: existing builder has process-wide signal changes; replacement path unqualified |
| `Part::Fuse` | **N**: same contract, FUSE opcode | Same, FUSE | **U**: same |
| `Part::Common` | **N**: same contract, COMMON opcode | Same, COMMON | **U**: same |

**Counts, by exact type:** live contracts S=0, N=4, U=0; proposed detached
kernel qualifications S=0, N=0, U=4. These are two different counts, not eight
feature types. Unknown internal effects do not weaken the known N classification
of the live methods. There are zero currently admitted thread adapters.

### Box: own properties, linked state and failure effects

| Access / effect | Evidence (`src/` paths relative to repository root) | Class / required boundary |
| --- | --- | --- |
| Read `Length`, `Width`, `Height`; test in L/W/H order against `Precision::Confusion()` | `src/Mod/Part/App/FeaturePartBox.cpp:54–70` | **S** only after owner freezes internal-unit doubles and tolerance; live getters stay owner-side. |
| Construct local `BRepPrimAPI_MakeBox` and take Shape | `src/Mod/Part/App/FeaturePartBox.cpp:72–77` | **U** for same-process concurrency until instrumented OCC qualification. No input topology in this kernel. |
| Write own `Shape` with `resetElementMap=false`, then call Primitive/base execute | `src/Mod/Part/App/FeaturePartBox.cpp:76–77`; `src/Mod/Part/App/PrimitiveFeature.cpp:83–85`; `src/Mod/Part/App/PartFeature.cpp:124–127` | **N**. Base execute touches Shape again and executes extensions. Old Shape/map/tag and statuses are inputs to publication, not discardable output scratch. |
| Own `Placement` read during Shape change; update Shape's transform under recomputing status; otherwise Shape change may set Placement | `src/Mod/Part/App/PartFeature.cpp:1542–1577` | **N**. Commit must reproduce the recomputing status. A naked Shape setter outside that scope can change the property write set. |
| Inherited Shape change can write own `_ElementMapVersion` and update other objects' subelement links | `src/App/GeoFeature.cpp:252–284`; `src/App/PropertyLinks.cpp:282–308,1649–1670,1696–1710` | **N**. Guard the version and incoming element-reference closure; a target-Shape-only effect set is not generally complete, even with no attachment/expression on the Box. |
| Dimension setters immediately invoke recompute; restored Shape `User1` flag can invoke it again | `src/Mod/Part/App/FeaturePartBox.cpp:228–245` | **N**. Time edits as well as the later document recompute; reject migration/User1 and recursive speculative preparation. |
| Built-in AttachExtension; touched mapping can reposition and rewrite support/offset state | `src/Mod/Part/App/PrimitiveFeature.cpp:70–73`; `src/Mod/Part/App/AttachExtension.cpp:371–428,470–486` | **N**. Reject active/pending/base attachment, support links and extra extensions; inactive extension still follows the owner phase. |
| Error returns before Shape setter for L/W/H too small; OCC exception translated | `src/Mod/Part/App/FeaturePartBox.cpp:60–81`; `src/Mod/Part/App/PartFeature.cpp:109–120` | **N** for error bookkeeping; preserve old Shape on these early failures. Do not claim all later extension/callback failures are write-free. |

For the narrow Box instance there are no explicit operand links, material copy,
application preference lookup, Python call or signal-handler wrapper in the
kernel body at lines 54–81. These are **absence statements about that body**;
inherited setters/recompute still access application/document state and emit
signals, as detailed below. Construction initializes ShapeMaterial using the
material manager (`PartFeature.cpp:95–99`), so creating temporary App objects in
a worker would reintroduce it. The worker must not create an App document/object.

Candidate Box read set: exact type/schema/extensions, L/W/H, Placement, old Shape
and naming state, extension mapping/support/base-attachment configuration and
pending status; `_ElementMapVersion` and registered incoming element references;
object identity/existence/model and document identity/epoch,
structure, force/freeze/touch/recompute state; configuration/unknown-mutation
guards. Expression bindings, parents requiring transform resolution, dynamic
properties, proxies, migration flags and mutating observers make it ineligible.
Freeze admission without executing expressions or attachment early.

General live success effects include target Shape (tag/map/location), conditional
`_ElementMapVersion` and linked subelement-reference updates, plus model/error/
touched/execution bookkeeping and owner notification/transaction/settlement.
To retain the plan's **narrow** Shape-only publication, require a settled,
unchanging `_ElementMapVersion` and no incoming element-reference rewrites.
Initially reject nonempty registered subelement-reference sets (query
`PropertyLinkBase::getElementReferences` on owner at
`src/App/PropertyLinks.cpp:269–279`) until their exact effects are audited.
Preserve these workflows through serial fallback and compatibility tests. That
query itself accesses global state and is not worker-safe. Any unexpected
version/link/other-object effect rejects admission or requires a reviewed expanded
contract. No Placement or ShapeMaterial write is admitted for this Box subset.
Early modelling failure has no Shape effect; owner diagnostic/model effects
remain. Equality includes saved references and naming, not just volume.

### Cut, Fuse and Common: common native Boolean contract

The three classes inherit `Boolean::execute()`. Their dispatch differences are
`src/Mod/Part/App/FeaturePartCut.cpp:38–46`,
`src/Mod/Part/App/FeaturePartFuse.cpp:56–65` and
`src/Mod/Part/App/FeaturePartCommon.cpp:52–60`. The latter's comment says
“section”, but the code constructs `FCBRepAlgoAPI_Common`; use the code, not that
comment, as the operation contract.

| Access / effect | Evidence | Class / required boundary |
| --- | --- | --- |
| Read own `Base`, `Tool`, later `Refine`; mustExecute reads link/touched state | `src/Mod/Part/App/FeaturePartBoolean.cpp:102–112,126–145,171–173` | **N** live; frozen operation kind/refine/fuzzy/check policy values are **S**. |
| Resolve operand shape, links, placement/transforms and subobject owner, preserving input naming | `src/Mod/Part/App/FeaturePartBoolean.cpp:135–145`; `src/Mod/Part/App/PartFeature.cpp:981–1004,1129–1186,1302–1365` | **N** owner capture. Start with direct native same-document Part features, no Links/groups/scaling/external references; capture actual resolved inputs, not just link identity. |
| Shape retrieval takes GIL, can construct/use Python wrappers and invoke subobject/link implementations | `src/Mod/Part/App/PartFeature.cpp:997–1025` | **N**. “Native Boolean” does not mean its live input acquisition avoids Python/GIL. |
| Input Shape getter mutates cache and possibly Tag, even though it returns const | `src/Mod/Part/App/PropertyTopoShape.cpp:113–132` | **N**. A property read trace alone will miss these writes; capture/warm only on owner. |
| General shape resolution can add `_ShapeCache` dynamic property to a source/owner, connect its early-change signal, write/clear its cache map | `src/Mod/Part/App/PartFeature.cpp:1031,1277–1288`; `src/Mod/Part/App/PropertyTopoShape.cpp:1145–1176,1215–1240` | **N**, concrete possible **cross-object/schema writes during an apparent read**. Not every direct Part operand takes these branches; cold/group/link negative tests must cover them. Never allow in worker. |
| On error, read linked objects' `Label` and shape types for diagnostic | `src/Mod/Part/App/FeaturePartBoolean.cpp:150–164` | **N** capture Label/error input revisions too, or translate diagnostics on owner from validated inputs. |
| Read `CheckModel` through `App::GetApplication()` parameter hierarchy during execute; run BRepCheck | `src/Mod/Part/App/FeaturePartBoolean.cpp:47–59,167` | **N** preference capture; **U** for detached validation library internals. Do not reread parameters in a worker. |
| `RefineModel` preference initializes Refine at construction, not each execute | `src/Mod/Part/App/FeaturePartBoolean.cpp:63–70,91–99,171–173` | **N** construction; freeze the **property** for recompute, distinguish from CheckModel's execute-time preference. |
| Build `FCBRepAlgoAPI_*`; wrappers enable inner parallelism and non-destructive mode, use global fuzzy factor for scaled bounding-box tolerance | `src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:41–85`; `src/Mod/Part/App/FCBRepAlgoAPI_Cut.cpp:37–40` (Fuse/Common likewise lines 37–40) | **N** for global policy, **U** for kernel sharing. CPU affinity alone does not make `SetRunParallel(true)` an inner-one policy. |
| Make topology naming/history maps, optional refine, then write own Shape | `src/Mod/Part/App/FeaturePartBoolean.cpp:169–174`; `src/Mod/Part/App/TopoShapeExpansion.cpp:4484–4499,5087–5118` | **N** naming/owner integration. Refine=false initially; returning a raw BRep alone loses required history. |
| Conditional own `ShapeMaterial` write using Base's material and default material UUID; manager call can itself mutate shared material | `src/Mod/Part/App/FeaturePartBoolean.cpp:175`; `src/Mod/Part/App/PartFeature.cpp:139–156`; `src/Mod/Material/App/MaterialManager.cpp:184–217` | **N**. Success effect set cannot be only Shape. Freeze Base and target material values/default identity; preserve conditional write and notifications on owner. |
| `History` declared transient/hidden/output but not assigned by binary Boolean execute | `src/Mod/Part/App/FeaturePartBoolean.cpp:82–89,120–185`; `src/Mod/Part/App/FeaturePartBoolean.h:48` | **N** guard unchanged property. TNP history **inside Shape** is distinct from this property; do not invent a History output. |
| Early failure returns / Base exceptions / catch-all diagnostics | `src/Mod/Part/App/FeaturePartBoolean.cpp:129–185` | **N** owner error handling. Early missing/null/failed/invalid results precede Shape setter. A failure after Shape/material setters or during extension execution can have provisional effects; compare serial rollback/partial-success semantics. |

Candidate Boolean reads add to the Box/common framework: link identities and
transitive shape/transform sources, each operand Shape/placement/TNP map/hasher,
Base/Tool Label for error paths, own old Shape/naming/Placement/Refine and
ShapeMaterial, direct Base ShapeMaterial, default material/appearance identity
and relevant preferences, Boolean fuzzy factor, OCC policy/tolerances and exact
library build. Some preferences have no existing revision key: snapshot plus
generation/invalidation coverage must be implemented, not assumed. Unknown
extension, cache-producing source closure, expression/attachment or callback
effects force serial. Refine=false does not exclude naming risk.

Candidate success effects: target Shape and naming/tag/location, conditional
target ShapeMaterial, inherited `_ElementMapVersion`/incoming element-reference
updates, target model/error/touch bookkeeping; owner-only transaction, revision,
notifications and downstream settlement. The same initial version/reference
restrictions as Box are necessary for a narrow target-only footprint. Other
properties including History must remain unchanged under that contract. General live
execution may additionally create/change source caches; that cannot fit a
target-only adapter contract without an owner capture phase that preserves its
observable behavior. A warm cache is not evidence that the cold path is absent.

### Shared/document/global effects common to recompute

| Shared resource | Concrete evidence and read/write behavior | Classification / consequence |
| --- | --- | --- |
| Document recompute envelope | `src/App/Document.cpp:8035–8099` executes input expressions, feature recompute, then output expressions; updates recompute diagnostics/error state. `src/App/DocumentObject.cpp:163–205` validates links and changes Recompute/RecomputeExtension bits, then executes extensions. | **N**. Keep phases in existing order; no parallel live recompute. Expressions and extra extensions excluded initially. |
| Property publication, transactions, signals | `src/App/Property.cpp:374–437`: touch and setters publish mutations and call father change handlers/property signals. `src/App/DocumentObject.cpp:1199–1218,1391–1403`: object/document before/changed notifications. `src/App/Document.cpp:2017–2030` delivers deferred notifications. | **N**. Workers must emit no model signal. Owner callbacks run after validated publication; arbitrary callbacks can write other objects or reenter, expanding effects beyond the feature body. |
| Application flags and static partial-document warning | `src/App/DocumentObject.cpp:1328–1352` reads GetApplication closing/restoring/fine-grained mode and document PartialDoc; writes static `warnedDoc` on partial-document warning path. | **N**. Reject partial restore/document modes; freeze/admit flags on owner. Even unrelated documents do not isolate this static. |
| Python observers / GIL / signal marshalling | `src/App/DocumentObserverPython.cpp:434–481` takes GIL and calls Python for object-change/recompute/document callbacks. `src/App/MainThreadSignal.h:306–309,406–409` releases GIL while marshalling/waiting. | **N**. GIL serializes interpreter work, not OCC caches or C++ globals. Avoid owner/GIL/GUI wait inversion; record acquisition waits separately. |
| **Process-wide signal policy** | `src/Mod/Part/App/FeaturePartBoolean.cpp:123–124` creates **Base** SignalException on GNU/Linux; `src/Base/Exception.cpp:733–747` installs/restores SIGSEGV handler. Every wrapper Build creates **Part** SignalException (`src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:105–108`); `src/Mod/Part/App/SignalException.cpp:60,144–146,187–197,210–232` writes static modes and installs/restores process signal handlers. | **N**, high priority blocker. Unsynchronized mode read/write and nested handler restoration can race even on fully private shapes. A different document or per-shape mutex is insufficient. Thread harness must reject this wrapper until a separately reviewed safe policy preserves error semantics; instrument signal-policy access explicitly. |
| **Global Boolean fuzzy tolerance** | `src/Mod/Part/App/FuzzyHelper.cpp:31–49` has mutable `BooleanFuzzy`, plain read/write and scoped save/set/call/restore. Wrapper reads it at `FCBRepAlgoAPI_BooleanOperation.cpp:83–85`. | **N**. Freeze/copy explicit tolerance and forbid global writes for duration. A worker must not use `withBooleanFuzzy`; its exception path also skips restore. |
| TShape, cache and naming aliasing | `src/Mod/Part/App/TopoShapeExpansion.cpp:294–303` copies underlying shape, Hasher, cache, parent cache and element map references. `TopoShapeCache.h:59–77,79–102` stores mutable shared maps/locations/ancestry. `TopoShapeExpansion.cpp:144–152` mutates cache in const initCache. | **N** if shared; **U** until independent capture proven. Shallow TopoShape/TopoDS copies do not isolate a computation. Deep-copy geometry and prove subshape disjointness, not just different top-level handles. |
| Document StringHasher and element maps | `src/Mod/Part/App/PropertyTopoShape.cpp:66–105` assigns/re-tags maps, obtains document hasher, hashes child maps; `src/App/StringHasher.cpp:262–298` looks up and allocates `lastID()+1` then inserts; `TopoShapeExpansion.cpp:929–957,1781` propagates/uses source hashers. | **N**. No lock is apparent in these hasher paths. Even adding a mutex would not establish deterministic ID allocation/order. Owner-only deterministic TNP integration is required. |
| **Incoming element references / global registry** | `src/App/GeoFeature.cpp:252–284` reacts to Shape changes, conditionally writes `_ElementMapVersion` and calls PropertyLinkBase::updateElementReferences. `src/App/PropertyLinks.cpp:72,282–308,493–494,544–551,1696–1710` uses/updates static `_ElementRefMap` and can rewrite/notify a link property on a different object. | **N**. This is intrinsic naming work, not an arbitrary add-on observer. `src/App/Document.cpp:2861–2863` shows isPerformingTransaction means undo/rollback; an ordinary open transaction does not suppress this path. Audit the reverse-reference closure or reject it for initial admission. |
| Release-only random duplicate repair | `src/App/ElementMap.cpp:756–779` has static random_device/mt19937/distribution and mutates generator outside FC_DEBUG. | **N**. Exclude duplicate-name repair from thread kernels. Test optimized/TSan builds too; debug-only checking misses this branch. |
| Shared material manager/default appearance | `src/Mod/Material/App/MaterialManager.cpp:62–70,81–105,141–179,184–217`: shared manager, constructor writes static external-interface policy, default preferences, random-color static generator and mutable material appearance properties. | **N**. Initialization locking is not a certificate for defaultMaterial's other writes. Keep manager access and material copy owner-only; freeze/reject RandomColor and external-interface policy until their semantics are qualified. |
| Other shape-resolution statics | `src/Mod/Part/App/PartFeature.cpp:1105–1111` lazily sets static fallback plane topology during general shape acquisition (inside its GIL scope). | **N**. Retain owner/GIL affinity; do not move the getter to detached compute. Not reached by every native operand. |
| OCC defaults/inner pool, refine implementation | `src/Mod/Part/App/TopoShape.cpp:1668–1678` BOP checking calls SetParallelMode and SetRunParallel; `src/Mod/Part/App/TopoShapeExpansion.cpp:6307–6324` general Boolean helper calls OSD_Parallel::SetUseOcctThreads, ProgressIndicator and global Sequencer. `src/Mod/Part/App/modelRefine.cpp:426–429,839–843,1194–1198` returns static classifier objects. | **N** for global policy/progress changes; **U** for library internals/static classifier mutability. These helpers are separate paths, not evidence that every native binary Boolean execute uses them. Do not substitute them as a detached shortcut. Refine stays excluded until audited. |

OCC libraries, allocator/messenger/progress internals and the prior P1–P4 raw
artifacts are not present as inspected dependencies here. Thus no claim is made
that this static source inventory closes their global-state contracts. The
existing dedicated operation is a useful capture reference:
`src/Mod/Part/App/CollaborativeBooleanOperation.cpp:1213–1256` deep-copies geometry
and checks source/subshape TShape partners, but its raw snapshots deliberately
omit document naming state (lines 1528–1535), and its builder still enables inner
parallelism (line 1033). It is not a drop-in native-feature contract or a race
qualification for this pilot.

### Invalidation, publication and owner ordering

At capture **and** commit validate document instance/epoch, object identity and
exact type/schema/extensions, union of property read/write revisions, operand
closure identities/model/Shape/placement/material/error-label revisions, old
output naming and `_ElementMapVersion` state, incoming element-reference sets,
structure/existence, relevant configuration generation,
unknown-mutation guard, and force/freeze/skip/admission state. Changes must reject
or recapture under existing retry rules. No early attachment/expression writes,
silent revision replacement, or “same command” conflict exemption.

For siblings A and B require disjoint W sets and no W/R intersections in either
direction. Read/read sharing is only logical: each worker needs private topology,
caches and naming data. Owner capture that writes a source cache must itself be
tracked and must not invalidate a previously captured sibling unnoticed.

The current compatibility fence is justified by the source:
`src/App/GenericIsolatedRecompute.cpp:2483–2496` explicitly describes broad
UnknownModelMutation effects and sets refreshRevisionFenceAfterEachCommit. Keep
this unchanged. Proposed tests in the existing coordinator/generic/commit suites:

1. Capture A/B from one revision, commit A's exact effects; B stays valid only
   when disjoint. A real edit to any B read/write key rejects B. Exercise each
   declared property and configuration, not only Length.
2. Publish a broad compatibility mutation/structure change between capture and
   commit; B must reject. Subscribe an existing broad prepared client and an
   exact client before A's exact commit: both see the publication; clients whose
   contract covers A are invalidated, including broad captures. Merely querying
   an exact key afterward is insufficient evidence of stream compatibility.
3. On success, compare actual setters, direct cache writes and postconditions
   with declared effects; material writes are conditional, History unchanged.
   On failure compare the serial retained Shape/material/error/touch state and
   earlier unrelated commits, with/without an open caller transaction.
4. For A→B plus independent Z, record authoritative serial **execute/commit**
   trace, then reverse detached completion with latches. Preserve A/B/Z order
   when serial readiness chooses it. Separately compare deferred final
   signalRecomputedObject order; that signal is not a compute timer. Test callback
   readback, reentrant/mutating observers, undo/redo/save/restore.
5. Retain `PartPresentationCaptureTest.UnchangedDatumExtentsPreserveRevisionFence`
   at `tests/src/Gui/DocumentPresentationCache.cpp:386`. Unchanged datum refresh
   keeps the fence; real size changes reject; presentation Busy retry reuses the
   same payload once and cancellation does not apply it.

These are required new assertions, not claims about existing tests passing.
Use the current tests in `tests/src/App/DocumentRecomputeCoordinator.cpp`,
`GenericIsolatedRecompute.cpp`, `DocumentCommitCoordinator.cpp`,
`tests/src/Mod/Part/App/ShapeCacheIsolatedRecompute.cpp` and the GUI suite as hosts.
No generic fence may relax until these tests and trace coverage pass.

## 3. Race gate before production parallel code

### A. Serial access-contract recorder (debug/test-only proposal)

First detect *latent conflicts* in existing serial recompute. This can be built
without changing the scheduler. It does not detect actual simultaneous accesses
and cannot substitute for TSan.

| Instrumentation point | Record / assertion |
| --- | --- |
| `Document::_recomputeFeature` (`Document.cpp:8035`) and `DocumentObject::recompute/executeExtensions` (`DocumentObject.cpp:163,193`) | RAII scope containing stable document instance/epoch, feature identity, command/recompute ID, phase and thread. Separate input-expression, native execute, extension, output-expression, settlement and callback phases. Nested scopes retain the initiating feature and callback origin. |
| Typed getters, not only getPropertyByName | Hook `PropertyFloat::getValue` (`PropertyStandard.cpp:1169`), Bool (2350), String (1682), Material (2874), Placement (`PropertyGeo.cpp:555`), Link/LinkSub/list getters (`PropertyLinks.cpp:813,1478,2647`), Shape getValue/getShape/getComplexData (`PropertyTopoShape.cpp:108–132`). Include quantity inheritance and inline getters; audit any bypass rather than claiming universal coverage. Log property revision and read identity without invoking getters recursively. |
| Property/object write routes | Hook **before** `Property::aboutToSetValue` (`Property.cpp:430`), touch/purge/status, `hasSetValue` (409), object touch/setTouched/status/error, dynamic property add/remove, document add/remove/link/schema/transaction/publication APIs. Log pre/post revision and actual effect; snapshots of all properties/status/schema before/after catch missing hooks but cannot replace event coverage. |
| Writes that bypass property setters | Explicit probes in `PropertyPartShape::getShape`, `TopoShape::initCache`, Tag/Hasher assignment, `PropertyShapeCache::get/setShape/slotChanged`, StringHasher inserts/clear and element-map/random-repair paths. Record topology/cache/map/hasher identities and ownership. A write in a const getter is still a write. |
| Reverse-reference updates | Trace `_ElementRefMap` lookup/register/unregister, GeoFeature::updateElementReference and every PropertyLinkBase-derived reference updater, including shadow lists, link subnames and onUpdateElementReference. Treat cross-object reference rewrites as declared model effects, not generic downstream touch bookkeeping. |
| Ambient state and callbacks | Probe parameter lookup/update (App accessors/ParameterGrp), FuzzyHelper read/write, both signal wrappers, OCC policy setters, material manager calls and signal/Python callback entry. Classify global reads/writes; callbacks keep their own nested origin and actual writes. |

Assign stable identities on owner before tracing; use per-thread bounded buffers
and a TLS recursion guard. Do not obtain identities through Python wrappers or
warm geometry caches while logging. A buffer overflow, uninstrumented accessor
on an admitted path, unknown callback/effect, or missing terminal scope is a
coverage failure. Run tracing separately from optimized timing to avoid counting
its overhead as feature cost.

Collect cold and warm traces for the exact native types, error cases and the
negative eligibility matrix: proxies/subclasses/extra extensions/dynamic schema,
expressions, attachment/base attachment, shared sources, Links/groups/scaling,
partial restore, migration/User1, Refine=true, custom material/default preference
change, fine-grained mode, callback reentrancy and duplicate naming.

Gate assertions: every read is in the declared frozen/owner set; every write has
an owner, phase and declared effect. A cross-object write during compute fails;
source cache mutation during owner capture is flagged for explicit contract
review, not blanket-exempted as “only a cache”. Serial dependency settlement may
touch downstream objects, but only in its own audited owner phase. Compute has
zero live App object/property/document/interpreter/registry access. Compare
sibling trace sets to the conflict rule and compare post-state with serial.

### B. Instrumented detached-kernel stress job (test-only proposal)

Implement a native test-only harness linked to relevant instrumented FreeCAD
Part/Base/App code and **instrumented OCC**, with private geometry and no live
feature calls in worker threads. Use exact admitted kernel/build policy. A
raw-OCC shortcut must first match serial fuzzy tolerance, non-destructive mode,
compound behavior, diagnostics and history semantics; it cannot silently omit
the signal-wrapper problem. Until that is resolved, the existing FCB wrapper
is a negative case, not an admitted thread kernel.

The repository has `FREECAD_USE_SANITIZER_TSAN` support in
`cMake/FreeCAD_Helpers/SetupSanitizers.cmake:37–42,72–110`; this only instruments
targets built with it, not an externally supplied OCC. CI must record compiler,
runtime/link map, exact FreeCAD/OCC SHAs, TBB setting and instrumented library
hashes. No configure/build command is run by this research task.

Before a useful run, a small deliberate data-race positive control must produce
a TSan finding/nonzero status, and a synchronized control must complete normally.
`unexpected memory mapping`, loader/startup failures, empty/skipped cases,
uninstrumented relevant kernels, or broad suppressions fail qualification. Do
not use ASan/UBSan, matching geometry or a zero-length log as a race pass.

Then rendezvous 2 and 4 detached calculations on a barrier; hold first result,
release siblings in reverse order. Require a maximum-active count >=2 (or exactly
1 in the serial control), actual compute interval overlap, bounded retained
results and deterministic owner integration. No sleeps to manufacture overlap.
Use new private captures per job, including two readers of one logical source;
assert all subshape TShape handles, geometry ownership, caches, maps and hashers
are independent. Top-level `IsPartner()==false` alone is insufficient.

Stress matrix: first kernel in a fresh process and repeated warm runs; valid
boxes and each Boolean; shared-source capture; degenerate/invalid/null/error and
compound operands; cancellation requested before/after kernel and while held;
stale input/config, close/reopen/late result, failed apply and process worker
death/malformed payload. Perform 100 repetitions per supported case at 1/2/4
CPU-token configurations; bound total active tokens and payload bytes. Long OCC
calls need not cancel immediately. Reject/admit cases explicitly; no skipped
native case may count as qualification. Process safety includes archive schema,
size limits, checksums, identity/epoch, crash/cancel cleanup and exactly-once
apply; a process venue is not a thread-safety result.

### C. Required CI result

Five-line gate definition:

1. Serial recorder covers every admitted path; zero undeclared/cross-object compute effects or live worker model accesses.
2. Working TSan positive/safe controls plus instrumented FreeCAD/OCC kernel coverage; startup/coverage failure is red.
3. Latch-proven private-kernel overlap at bounded 2/4 tokens; zero unresolved relevant races, including cold/error/cancel runs.
4. Serial-equivalent outputs/naming/errors/undo/callback/publication traces, broad conflicts rejected and process lifetime/transport checks green.
5. Nonempty expected case manifest, zero skips, exit 0, complete XML/trace artifacts; otherwise keep thread adapters disabled.

The existing `ci/woodpecker/check-test-results.py:42–66` rejects zero executed
tests but counts a partially skipped suite as nonempty; its integration checker
also permits some skips (lines 133–141). A qualification wrapper must additionally
match the **full expected** test names/counts and require zero skips, and must not
inherit an unrelated process-crash quarantine. Add explicit tests that a missing
case, skip, truncated XML and TSan startup error make that wrapper fail.

## 4. Measurement runbook (local serial results in section 7)

### Fixtures and sizes

Synthetic recipes are implemented in the probe; all dimensions below are mm.
They are new reproducible controls, not reconstructions of unavailable P4
scripts. Record *restored* runtime object/type counts, property dependency graph,
actual executed subset and serial fraction for every case.

| Case / sizes | Exact recipe / purpose |
| --- | --- |
| `box:2`, `box:512` | Independent 10×20×30 Boxes, X=60i. Correctness/overhead, never a performance justification. |
| `cut:N`, `fuse:N`, `common:N`; N=24,128,512 | Per branch two Boxes: 40×30×20 at (60i,0,0), 10×10×30 at (60i+15,10,-5); one exact native binary Boolean, Refine=false. 3N feature objects before any runtime cache/schema changes. |
| `pair:N`; N=24,128,512 | Above Cut plus third 8×12×20 Box at (60i+36,9,0); Fuse(Cut,lug), Refine=false. 5N feature objects; 120/640/2560 before runtime additions. Two-stage wide DAG matching the shape of the plan's Boolean workload. |
| `chain:50`, `chain:200` | One Body, N PartDesign::AdditiveBox features (10×10×1), z=0.9j, hence touching/overlapping solids. Auxiliary Body/Origin/datum objects counted at runtime. **Narrow negative control 1**. No claim these are qualified types. |
| `bodies:20`, `bodies:100`, `bodies:500` | N Bodies, one 10×10×1 AdditiveBox each, X=20i; SubtractiveCylinder r=2 h=2 centered at (20i+5,5,-0.5). Count all auxiliaries at runtime, not just 2N features. Independent histories/serial-fraction diagnostic. |
| `data/examples/PartDesignExample.FCStd` | 28 saved XML objects. **Narrow negative control 2**, unchanged real history. |
| `tests/src/Mod/PartDesign/App/TestModels/TwoLengthsPadWithExpression.FCStd` | 10 saved objects; expression-barrier control. |
| Same directory: `TwoLengthsPadCrossObjectRef.FCStd`, `ModelFromV021.FCStd` | 12 and 13 saved objects; cross-reference/restore compatibility controls. |
| `src/Mod/Sketcher/SketcherTests/TestSketchCarbonCopyReverseMapping.FCStd` | 24 saved objects; Sketcher serial control. |
| `data/examples/AssemblyExample.FCStd` | 54 saved objects; coupled native solve + real proxies. Pin/install paths in the future build environment, require real restored joint proxies. |
| `data/examples/EngineBlock.FCStd`, `BIMExample.FCStd`, `FEMExample.FCStd` | 36, 361 and 43 saved objects respectively; larger restore/import/add-on coverage. Missing Draft/Arch/FEM dependencies **fail that fixture**, never substitute stubs. |
| User/imported large documents | Owner-selected, versioned and hashed, at least one wider and one narrow model; record eligibility, total topology, input/output archive bytes and live/aggregate memory. Corpus selection pending. |

Saved object counts above came only from reading zipped Document.xml at this
source commit. Full hashes, byte counts and type counts were written to
`../logs/m0-static/fixtures.json`. Counts are **not** a FreeCAD restore result:
P4's expression/cross-reference/migration/carbon-copy runtime counts differ from
these saved counts. Record any restored/migrated objects before comparing graphs.
For reproducibility, run `sha256sum` on the listed archives and save the manifest
with each future run; unchanged repository paths at another SHA are insufficient.

Add a heavy imported/spline/shared-topology Boolean fixture before deciding a
venue. The analytic Box operands can favor unrealistically cheap captures. Do
not report synthetic primitive or modeled DAG speedup as whole-document benefit.

### What to time and retain

Use monotonic wall clock and process/thread CPU clocks. One run ID links all
phases to command/recompute ID, feature/object identity, expected revision and
final presentation revision. Timing instrumentation must be opt-in with its own
off/on overhead control, outside the trace-heavy contract job.

| Metric | Boundaries / required implementation |
| --- | --- |
| Build/load/cold startup | Trusted CI build manifest: full fork and upstream SHA, RelWithDebInfo configuration/cache, compiler, library hashes, OCC/Python/TBB/Qt/add-on versions and runtime linkage. Separately time process start→ready, FCStd restore and first dirty recompute; initialization/restore can already warm geometry. |
| Dirty plan and owner preparation | Request accepted→dependency/eligibility planning; owner capture start→end including link resolution, cache creation, deep copy, naming snapshot and revision fence. `GenericIsolatedRecompute.cpp` request/preparation and `DocumentRecomputeCoordinator.cpp:375` scheduleReady are hook sites. Retain production property edges and touched/clean/blocked/force/skip/freeze state. Probe OutList is explicitly coarse diagnostics only. |
| Transfer/queue/startup | Archive encode/write/read/decode bytes/time, process spawn→ready, queue residence, worker reservation. Instrument `GeometryProcessBackend.cpp:663–689` process/start gate and geometry archive encode/decode paths; distinguish cold process from reused process if reuse exists. Do not call a fresh-process launch a warm worker. |
| Native calculation | Immediately around raw kernel and history construction separately; instrument test-only detached entry and live serial `Document::_recomputeFeature`/execute for the reference. Split expressions/extensions/material/naming from kernel where possible. Final recompute callback intervals are not native duration. |
| Validation/commit/settlement | Revision validation, presentation admission wait, apply, TNP/material integration, transaction/rollback, publication/notification, dependency settlement. `DocumentCommitCoordinator.cpp:520` commitRecompute and its apply/validation paths are hosts. |
| Presentation / end-to-end | Headless: request→terminal settlement. GUI: ingress→terminal complete **visible** revision swap, including presentation capture/preparation/apply. Hook `Gui/DocumentPresentationCapture.cpp:27`, `DocumentPresentationCache.cpp:125–132` publication and scheduler apply. Record navigation latency while kernels held. Headless duration is not GUI end-to-end duration. |
| GIL and actual overlap | Acquisition request→acquired and held intervals around capture/Python/callback sites; worker start/end timestamps, active count, CPU seconds, scheduling/commit order. Define compute overlap by intervals/latches, never pool capacity or process count. |
| Memory and budgets | On dedicated Linux runner sample parent+live descendant `/proc/*/status` VmRSS at 10 ms plus per-process high-water marks/cgroup memory. Report max of simultaneous **sum**, per-process peaks, cgroup peak and sampling interval; sum of separate process maxima is not aggregate peak. Track live payload estimates/bytes and retained completions. Also record `/usr/bin/time -v` peak as process diagnostic, not aggregate memory. |

For serial baseline, the supplied probe records creation/restore, edit/touch,
document recompute wall and process CPU time, counts, coarse graph, dirty set and
outside-timer geometry summaries. **Phase spans, GIL waits, exact property DAG,
aggregate RSS, GUI presentation and native overlap are null/unavailable**, not
zero. `action_total_ns` includes dirty-set enumeration and Python overhead;
`recompute_ns` excludes it. Box dimension assignment executes native work at edit
time, so quote `mutation_ns` and action total alongside recompute. No performance
result is valid without invalid/error/unsettled checks and full serial oracle.

### Commands that can run with an existing RelWithDebInfo build

Run only from this clone, with build artifacts **supplied by CI in an allowed
workspace path**. The commands below are future commands, not commands already
executed. Do not point them at either prohibited FreeCAD checkout. No package
install or build step is part of this runbook.

```bash
cd /home/rch/Documents/code/agents/IDLE-S4/repo
export M0_REPO="$PWD"
export M0_BUILD="$PWD/../logs/ci-artifacts/RelWithDebInfo"
export M0_CMD="$M0_BUILD/bin/FreeCADCmd"
export M0_BUILD_SHA="$(git rev-parse HEAD)"
export M0_RUN="$PWD/../logs/m0-runtime/$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$M0_RUN"
export M0_PROBE="$M0_RUN/m0-probe.FCMacro"
cp tools/parallel_recompute_m0_probe.py "$M0_PROBE"
test -x "$M0_CMD"
git rev-parse HEAD > "$M0_RUN/source-sha.txt"
sha256sum "$M0_CMD" data/examples/*.FCStd \
  tests/src/Mod/PartDesign/App/TestModels/*.FCStd \
  src/Mod/Sketcher/SketcherTests/TestSketchCarbonCopyReverseMapping.FCStd \
  > "$M0_RUN/input-hashes.txt"
cp "$M0_BUILD/build-manifest.json" "$M0_RUN/build-manifest.json"
ldd "$M0_CMD" > "$M0_RUN/linked-libraries.txt"
lscpu > "$M0_RUN/cpu.txt"
cat /proc/meminfo /proc/loadavg > "$M0_RUN/host-before.txt"
```

The manifest filename is an artifact contract for CI, not a file claimed to
exist now. **Verify its built source SHA matches M0_BUILD_SHA and the binary
hash**; an environment variable or nonempty Version string is not proof. Capture
module library linkage as well (Part/App/OCC/TBB), not only the small launcher.
If the binary lacks a revision field, require trusted build provenance and exact
artifact hashes; do not silently accept an unverifiable build.

The `.FCMacro` copy is intentional: `src/App/Application.cpp:3391–3403` executes
macros but first **imports** `.py` arguments, which would skip this probe's main
guard. Standard input is closed to finish any CLI console phase. Each invocation
uses fresh explicit user/system config files in its output directory (options at
`src/App/Application.cpp:2720–2721`), so preferences are archived without writing
the user's normal configuration. Use the same versioned config copies for any
nondefault-policy comparison. The probe also records CheckModel, default Refine,
RandomColor and external material policy; none is toggled during timing.

Choose four available logical CPU IDs on the dedicated runner as M0_CPUSET (for
example 0–3 only when allowed). CPU affinity contains runnable CPUs; it does
**not** configure OCC's internal pool. Fix documented OCC/TBB/application policy
before each measured process and record actual thread CPU usage. Existing native
Boolean wrappers request inner parallelism, so the baseline must be labelled
with its observed/default inner policy. Comparing it to a detached inner-one
kernel requires a separate same-policy serial control.

```bash
export M0_CPUSET=0-3
for case in box:2 box:512 cut:24 cut:128 cut:512 fuse:24 fuse:128 fuse:512 \
            common:24 common:128 common:512 pair:24 pair:128 pair:512 \
            chain:50 chain:200 bodies:20 bodies:100 bodies:500; do
  for scenario in all edit noop; do
    export M0_CASE="$case" M0_SCENARIO="$scenario"
    export M0_OUT="$M0_RUN/${case/:/-}-$scenario"
    export M0_WARMUPS=5 M0_REPETITIONS=30
    mkdir -p "$M0_OUT"
    /usr/bin/time -v -o "$M0_OUT/time.txt" \
      taskset -c "$M0_CPUSET" "$M0_CMD" -u "$M0_OUT/user.cfg" -s "$M0_OUT/system.cfg" \
      "$M0_PROBE" < /dev/null > "$M0_OUT/console.log" 2>&1
  done
done
```

The probe writes `samples.jsonl` with a final completed record; check that all
30 measured samples exist, no native errors/proxy failures occurred and restored
object counts agree with the fixture recipe. FreeCADCmd may log a script
exception without a useful shell failure: a zero process exit alone cannot pass.
If a recipe/API does not work on the supplied build, retain logs and fix the
research probe before measuring; do not filter that case out of the manifest.

Run native source fixtures similarly (all/noop); file mode never saves the input:

```bash
export M0_CASE=file M0_SCENARIO=all
export M0_FILE="$M0_REPO/data/examples/PartDesignExample.FCStd"
export M0_OUT="$M0_RUN/partdesign-example-all"
mkdir -p "$M0_OUT"
/usr/bin/time -v -o "$M0_OUT/time.txt" \
  taskset -c "$M0_CPUSET" "$M0_CMD" -u "$M0_OUT/user.cfg" -s "$M0_OUT/system.cfg" \
  "$M0_PROBE" < /dev/null > "$M0_OUT/console.log" 2>&1
```

Repeat for every fixture in the table. A file incremental run must set
`M0_SCENARIO=edit M0_EDIT=ObjectName:NumericProperty` using a property chosen and
recorded from that restored fixture; no invented generic “edit first object”.
Include one leaf edit and one common driver (spreadsheet/sketch dimension) where
applicable. Spreadsheet edits require a dedicated fixture-specific macro using
its actual cell/constraint, not the numeric-property probe.

For cold-after-restore series, use an already generated fixture, **30 fresh
processes**, M0_WARMUPS=0, M0_REPETITIONS=1. Do not regenerate/warm the fixture in
the timed process:

```bash
export M0_CASE=file M0_SCENARIO=all M0_WARMUPS=0 M0_REPETITIONS=1
export M0_FILE="$M0_RUN/pair-24-all/fixture.FCStd"
for sample in $(seq 1 30); do
  export M0_OUT="$M0_RUN/pair-24-cold-$sample"
  mkdir -p "$M0_OUT"
  /usr/bin/time -v -o "$M0_OUT/time.txt" \
    taskset -c "$M0_CPUSET" "$M0_CMD" -u "$M0_OUT/user.cfg" -s "$M0_OUT/system.cfg" \
    "$M0_PROBE" < /dev/null > "$M0_OUT/console.log" 2>&1
done
```

This is fresh-process/first-recompute-after-restore, not proof that restore did
not warm OCC. A native detached harness must separately time truly first kernel
initialization. Keep process lifetime total, restore and recompute separate.

Existing native baseline suites can run when supplied, without pretending they
implement the new gate. Use the actual `tests/` artifact paths:

```bash
export M0_TESTS="$M0_BUILD/tests"
"$M0_TESTS/App_tests_run" \
  --gtest_filter='DocumentRecomputeCoordinatorTest.*:GenericIsolatedRecomputeTest.*:NativeRecomputeRequestCaptureTest.*:DocumentCommitCoordinator*' \
  --gtest_output="xml:$M0_RUN/app.xml" > "$M0_RUN/app.log" 2>&1
"$M0_TESTS/Part_tests_run" \
  --gtest_filter='ShapeCacheIsolatedRecomputeTest.*' \
  --gtest_output="xml:$M0_RUN/part.xml" > "$M0_RUN/part.log" 2>&1
QT_QPA_PLATFORM=xcb xvfb-run -a "$M0_TESTS/Gui_tests_run" \
  --gtest_filter='PartPresentationCaptureTest.UnchangedDatumExtentsPreserveRevisionFence' \
  --gtest_output="xml:$M0_RUN/datum.xml" > "$M0_RUN/datum.log" 2>&1
```

Require exit 0, present expected testcase names, no errors/skips and complete
reports. The M0.1 phase recorder exists. The M0.2a Box qualification tests are
`DetachedKernelQualification*` in `Part_tests_run`. Publication, Boolean and
race tests do not yet exist. No invented worker-count CLI in this document is
runnable. The current probe has no `parallel=true` switch.

### Measurements needing the future native harness

Implement the instrumented harness from section 3 and timing hooks before using
these configurations; the current script only obtains serial reference data:

- Same input archives/captures, native serial control; detached threads at 1/2/4
  outer jobs with inner-one; existing geometry processes at 1/2/4 admitted jobs
  within global budget, each inner-one. One job with 1/2/4 inner allowance is the
  narrow-frontier comparison, never outer=4 and inner=4. Record unsupported
  settings/capacities; changing a default queue size is not evidence of a run.
- Reuse the current process protocol/worker; record startup/decode/encode costs
  and end-to-end wall/memory. No new worker executable. If a process is launched
  per request, measure that fact. Raw BRep-only process results cannot pass the
  native naming/history contract without a qualified transport/integration path.
- Use 5 warmups + 30 measured repetitions per case/configuration in 3 separate
  sessions. Within each session alternate controls and configurations with a
  recorded fixed permutation; keep raw samples, order, load and failures. Report
  median/p10/p90/p95, median absolute deviation and a paired-bootstrap 95%
  speedup interval over sessions; do not discard outliers silently. Stop and
  record resource rejection before crossing the provisional 2 GiB detached
  allowance. Begin at 2 outer/inner-one, <=4 total CPU tokens and <=2 retained
  payloads per document; measure baseline memory before admission.
- Serialize heavy performance/sanitizer jobs, with competing CI load recorded.
  Sanitizer timings are not optimized performance. Record uninstrumented versus
  timing-enabled overhead. Report elapsed end-to-end critical path, serial
  fraction, capture/copy/transfer/TNP/commit/presentation bottlenecks and actual
  overlap; summing per-feature durations double-counts overlap.
- On thread race failure, thread result is **not qualified** regardless of
  speedup. A process-only proposal requires completed isolation/transport tests
  and explicit owner decision. Retain owner serial fallback for unsupported
  types/instances; do not benchmark by silently omitting their work.

There is no end-to-end parallel document recompute to benchmark in M0. Detached
harness costs can justify a conditional M1 experiment and an optimistic bounded
cost model, not demonstrate M1's real speedup. Measure that after M1 overlap and
commit integration exists. Proposed >=1.20x median benefit and <=5% narrow/small
regression thresholds remain owner-confirmed *after* noise/capture data; no
threshold has passed here.

### M0.2a Box venue costs

Measured locally on 2026-10-31 from this branch's RelWithDebInfo build, after
the FreeCAD-start merge. This is not the 5-warmup / 30-repetition protocol
above. Every venue uses one warmup and the median of the same three kept
repetitions. Paid is the end-to-end wall on every venue. It starts before
capture and stops after commit. The owner wall is `Document::recompute`.
The process wall is the isolated job through `waitIsolated`. The thread wall
is the freeze, the worker thread, and the test-only owner Shape apply.
Kernel results stay alive until after the timed region; volume checks run
after the loops. Inputs are frozen from live document boxes with a
rotated placement, not fabricated identity placements.

Owner and process phase columns are sums of per-object M0.1 spans. They are
not a partition of wall time. The thread venue has no phase recorder: capture
is the freeze, execute is the raw MakeBox kernel, and commit is the test-only
owner `Shape` apply. Those three clocks sit inside the same thread wall.
The Transfer column is only the isolated-process decode
span. The thread row is 0 there because that span does not exist on the
thread venue; it is not a measured decode.

Paid is that end-to-end wall for the owner, the thread, and the process.
`GeometryProcessBackend.cpp` `runChecked` starts one
`FreeCADCmd` per job and returns only after that process has exited;
`dispatch` does not keep it. `Document.h` records the same fact for
`RecomputeVenue::Isolated`. There is no persistent worker, so startup cannot
be amortised and is not a separate column.

The harness freezes Length, Width, Height, `Precision::Confusion` and
Placement, then runs the MakeBox kernel on one `std::thread`. It does not
call `Part::Box::execute`. The process venue is the existing isolated worker:
`RecomputeVenue::Isolated` through `GenericIsolatedRecompute` and
`GeometryProcessBackend`, which spawns the sibling `FreeCADCmd`. That worker
runs `Part::Box::execute` on an archived object. There is no Box-only
process. Production scheduling is unchanged.

Publication covered here is the test-only owner step: a seeded element map
and the rotated placement applied to the thread kernel's raw shape. That
published result matches the owner recompute. The same step applied to the
process shape matches it as well. The isolated archive stores the OCC brep,
so the process result's own element map is empty. This is not production
publication. Boolean venue costs are in the next subsection. Sibling publication remains later.

| Venue | Workload | Capture ms | Execute ms | Transfer ms | Commit ms | Wall ms | Paid ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| owner | box:1 | 0.006 | 0.076 | 0 | 0.103 | 2.356 | 2.356 |
| thread | box:1 | 0.000 | 0.045 | 0 | 0.012 | 0.119 | 0.119 |
| process | box:1 | 0.746 | 0.134 | 0.364 | 0.105 | 1551.825 | 1551.825 |
| owner | box:32 | 0.438 | 4.493 | 0 | 7.769 | 81.786 | 81.786 |
| thread | box:32 | 0.001 | 0.778 | 0 | 0.170 | 1.185 | 1.185 |

The thread rows are a later same-day sample, after that wall was widened to include capture and the owner Shape apply. Owner and process rows, and the partition below, are the earlier sample.

The process wall below is the repetition whose wall is the median, not the
median of each column. Its pieces are taken from the phase trace and from
1 ms observations of the job directory and the worker pid. They sum to the
wall.

| Process box:1 component | ms |
| --- | ---: |
| Owner capture | 0.787 |
| Parent binary fingerprint | 598.573 |
| Request archive write | 0.000 |
| Spawn, until the worker pid exists | 65.339 |
| Worker startup, pid until first heartbeat | 662.540 |
| Part import, archive restore, result write | 161.763 |
| Worker execute | 0.134 |
| Process teardown, result file until exit | 60.468 |
| Result decode | 0.335 |
| Commit | 0.105 |
| Settle after commit | 1.782 |
| Sum | 1551.825 |

The parent fingerprint is `geometryWorkerBuildFingerprint` of `FreeCADCmd`
and `libFreeCADApp.so`, from the end of capture until the job directory
exists. A direct call of that function took 592.6 ms. The worker runs it
again after `Application::init` and before its first heartbeat
(`GeometryWorkerMain.cpp`). Pid-to-heartbeat is 662.5 ms. The worker's
`/proc/<pid>/io` `rchar` passes 32 MB 259 ms after the pid appears and 80 MB
at 549 ms, with the heartbeat at 662 ms, so that interval is the hash plus
interpreter startup. Empty `FreeCADCmd -c "import FreeCAD"` is 96.1 ms and
`-c "import Part"` is 171.4 ms. Those whole-process probes are not added into
the wall. `Part.so` is mapped 1.8 ms after the heartbeat. Heartbeat to
`result.fcg` is 161.9 ms, of which the execute span is 0.134 ms and the
remaining 161.763 ms is the import, the archived document restore, and the
result write. Teardown is `result.fcg` until the
pid disappears. Decode is the Transfer span. The request file appears in the
same 1 ms sample as the workspace, so the archive write is below that sample.

`box:32` is the in-test wide sample (independent 10×20×30 boxes, translated
and rotated, X=60i). Its owner DAG width was 32 and its longest path was 0.
The section 4 workloads below are the earlier owner-thread serial traces.
They were not remeasured with this venue method. A per-feature process was
not repeated for them.

| Workload | Objects | Width | Longest path | Capture ms | Execute ms | Commit ms | Wall ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| box:2 | 2 | 2 | 0 | 0.018 | 0.171 | 0.246 | 2.72 |
| box:512 | 512 | 512 | 0 | 9.60 | 72.5 | 360 | 1905 |
| chain:50 | 59 | 8 | 50 | 1.16 | 813 | 826 | 964 |
| chain:200 | 209 | 8 | 200 | 6.59 | 8835 | 8950 | 9566 |
| PartDesignExample.FCStd | 28 | 7 | 21 | 0.627 | 305 | 310 | 375 |

Owner, process and the test-only publication matched on topology (1 solid,
6 faces, 12 edges, 8 vertices, volume 6000), located and unlocated vertices,
rotated placement, status and ShapeMaterial. The owner recompute and the
test-only publication matched on the seeded element map. The process result's
own element map was empty. An early Length failure preserved the old volume,
the seeded map on the owner, and the diagnostic "Length of box too small" on
both the owner and the isolated process. The raw kernel map was empty until
the test-only owner step.

Interpretation: a wide Box graph is owner overhead, not OCC. `box:512` in the
earlier serial probe spends about 72 ms in execute spans and about 1.9 s of
wall. A narrow PartDesign chain is the opposite: `chain:200` execute spans
are about 8.8 s of a 9.6 s wall, with longest path 200. One isolated Box call
has a 1551.825 ms wall, and that is the per-feature process cost. About
599 ms is the parent fingerprint and about 663 ms is worker startup, which is
interpreter startup plus the same fingerprint; Part import and restore are
about 162 ms; teardown is about 60 ms; Box execute is 0.134 ms. The thread
kernel is the same geometry as the owner kernel and was not run concurrently
with other OCC work. It stays **U**. These numbers do not justify an M1
speedup claim. Boolean kernels, TShape and cache sharing, OCC globals and the
owner callbacks that fire today are in the M0.2b subsection. The kernel stays
**U**. Callback order across features remains later. TSan is the M0.3a subsection.

### M0.2b Boolean venues

Measured locally on 2026-10-31 from this branch's RelWithDebInfo build, during
`Part_tests_run`. One warmup, median of three. Paid wall starts before capture
and stops after the owner publication of the named shape and the conditional
material copy. The owner wall is `Document::recompute`. The thread execute
clock, inside the `std::thread`, is the raw `FCBRepAlgoAPI_*` constructor,
`BRepCheck`, and `makeElementShape`. The thread wall adds the operand freeze
before that thread and the test-only owner publication after join. Each chain
step runs in its own `std::thread`. The next step's base is the owner-published
shape (`Shape.setValue`, then `getTopoShape`), the same read the serial chain
uses. The raw kernel map is not that read. The process wall is
`waitIsolated`. Wide process (`cut:16`) was not spawned. Production scheduling
is unchanged. `cut:16` width was 16. `chain:8` width was 1 and its longest
path was 7. The heavy solid is a 48-side prism, 50 faces. Phase columns are
sums of M0.1 spans, not a partition of wall. Execute share is execute divided
by wall.

| Venue | Workload | Capture ms | Execute ms | Transfer ms | Commit ms | Wall ms | Execute share |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| owner | cut:1 | 0.018 | 5.512 | 0 | 5.604 | 5.960 | 0.925 |
| thread | cut:1 | 0.324 | 5.388 | 0 | 0.213 | 6.044 | 0.891 |
| process | cut:1 | 3.004 | 7.604 | 1.432 | 0.407 | 4720.549 | 0.002 |
| owner | cut:16 | 0.259 | 96.561 | 0 | 98.666 | 132.571 | 0.728 |
| thread | cut:16 | 4.553 | 86.648 | 0 | 1.645 | 93.183 | 0.930 |
| owner | chain:8 | 0.148 | 185.108 | 0 | 186.792 | 202.891 | 0.912 |
| thread | chain:8 | 1.315 | 178.798 | 0 | 24.550 | 207.240 | 0.863 |
| owner | heavy:1 | 0.033 | 81.738 | 0 | 82.137 | 84.782 | 0.964 |
| thread | heavy:1 | 4.597 | 55.092 | 0 | 0.573 | 59.377 | 0.928 |
| process | heavy:1 | 8.043 | 76.724 | 2.898 | 0.716 | 4798.369 | 0.016 |

On `cut:1` the owner paid wall is lower (5.960 ms versus 6.044 ms). The thread
paid wall is lower on `cut:16` and `heavy:1`. On `chain:8` the owner paid wall
is lower (202.891 ms versus 207.240 ms). The thread execute, 178.798 ms, is
close to the owner's 185.108 ms, and the paid wall includes the publication
between steps (commit 24.550 ms). That is the crossover in this set. A per-call
process does not win.

The `cut:1` process partition is the repetition whose wall is the median.
A persistent worker was not built. The M0.2a costs a cached fingerprint and a
warm Part process would not repeat are the parent fingerprint, worker
startup, Part import, teardown and the per-job spawn. On this run those sum
to 1558.874 ms (the M0.2a box sum of the same categories was 1548.683 ms).
Subtracting them leaves 3161.675 ms.

| Process cut:1 component | ms |
| --- | ---: |
| Owner capture | 2.829 |
| Parent binary fingerprint | 604.220 |
| Request archive write | 0.000 |
| Spawn, until the worker pid exists | 64.190 |
| Worker startup, pid until first heartbeat | 676.618 |
| Part import, archive restore, result write | 162.387 |
| Worker execute | 7.228 |
| Process teardown, result file until exit | 51.459 |
| Result decode | 1.551 |
| Commit | 0.447 |
| Settle after pid exit, decode and commit | 3149.620 |
| Sum / wall | 4720.549 |

The same repetition records the published `result-*` file 1.930 ms after
process exit (`exit_to_published_ms=1.930`). Time from that file until
`waitIsolated` returns is 3149.688 ms. The settle after subtracting decode and
commit is 3149.620 ms and stays `settle_cause=unresolved`. This run does not
time `GeometryProcessBackend.cpp` lines 812–838 as that residual. A Box settle
on the same definition was 1.395 ms. If a persistent worker still paid the
unresolved interval, it would not beat the thread or the owner. If a warm
worker did not pay it, the remainder is capture + execute + decode + commit,
12.055 ms, which is still above both the owner's 5.960 ms and the thread's
6.044 ms on `cut:1`.

The harness deep-copies operands with `BRepBuilderAPI_Copy`. A shallow
`TopoDS_Shape` copy is a partner of the live shape; the deep copy is not,
including subshapes. After the thread joins, the live operands are still
`IsSame`, their `TShape` id lists, element-map text and serialized BREP
(`BRepTools::Write`, no triangles or normals, format version 1) are unchanged,
and the result is not a partner of either live operand. `SetNonDestructive`
may share `TShape`s with the copies the algorithm received.

The worker snapshots `BOPAlgo_Options::GetParallelMode`,
`OSD_Parallel::ToUseOcctThreads`, `FuzzyHelper::getBooleanFuzzy`,
`Precision::Confusion`, the CheckModel preference,
`OSD_ThreadPool::DefaultPool` `NbThreads` / `NbDefaultThreadsToLaunch` /
`IsInUse`, and the full `sigaction` (handler, flags and mask membership) of
`SIGFPE`, `SIGHUP`, `SIGINT`, `SIGQUIT`, `SIGILL`, `SIGBUS`, `SIGSYS` and
`SIGSEGV` (`SignalException.cpp:173–174`) immediately before and after the
raw Boolean, `BRepCheck` and `makeElementShape`. In `Part_tests_run` that
window changed none of them. The pool was 16 threads, default launch 16, and
not in use. In a process whose first Boolean `Build` is this worker, `SIGSEGV`
changes and the other seven do not; later builds in that process change none.
`FCBRepAlgoAPI_BooleanOperation.cpp:43–44` and `:64–65` set per-operation
`myRunParallel` and non-destructive mode, not `BOPAlgo_Options::SetParallelMode`.
`Build` reads `BooleanFuzzy` (`FuzzyHelper.cpp:31`, wrapper lines 83–85) and
does not write it. `Precision::Confusion` is constexpr `1e-7` in this OCC 7.9
(`Precision.hxx:165`). `TopoShape.cpp:1677–1678` and
`TopoShapeExpansion.cpp:6307–6323` are not on binary `Boolean::execute`.
`Part::SignalException` (`FCBRepAlgoAPI_BooleanOperation.cpp:107`,
`SignalException.cpp:214–233`) unsets its mode and then calls
`installSegfaultHandler`, which does not put the previous `SIGSEGV` handler
back. Owner execute also installs `Base::SignalException`
(`FeaturePartBoolean.cpp:123–124`, `Base/Exception.cpp:733–747`), which saves
and restores its handler on that path.

During owner `Document::recompute` of a settled Cut, the test observer
recorded `signalBeforeRecompute`, `signalRecomputed`, `signalRecomputedObject`,
`signalChangedObject`, `signalBeforeChangeObject`, `feature.signalChanged` and
`feature.signalBeforeChange`. `signalBeforeChange` and the operand boxes'
`signalChanged` were connected and did not fire, because those boxes were not
touched. The detached kernel fired none of them. The test-only owner `Shape`
apply fired object and document change signals again.

Clean Cut, Fuse and Common with `Refine=false` and direct native same-document
`Part::Feature` operands are admitted and not detached. A non-feature, an
unknown extension, active attachment, or a source closure outside that native
set is refused. Pending rows still include capture/commit invalidation
(including a target `ShapeMaterial` change before commit), callback write sets,
both signal wrappers, OCC `TShape` internals, `StringHasher`, the material
manager, failure after the Shape setter, `BRepCheck`, Refine, the
`BooleanFuzzy` read during `Build`, and owner-side re-naming for a process
venue. Those rows still force detached false.
An edge tool fails with "Boolean operation failed" and keeps the previous
shape, map, History size 0 and Placement. A CheckModel-invalid result fails
with "Resulting shape is invalid" before `Shape.setValue`. History stays
empty. Publication starts from a distinct stale solid. The material-copy
branch copies a library Base material when the target is still the default.
The preserve branch keeps a different library material already set on the
Boolean. Owner naming (`makeElementShape`) applied to frozen operand inputs
reproduces the seeded `Qual` names. `PropertyPartShape::Save`
(`src/Mod/Part/App/PropertyTopoShape.cpp:408-411`) omits element maps while
`isExporting()`, so the child does not receive those seeds. The restored
process shape carries a map the child built during its own execute. That map
is not empty, and it does not contain `Qual`. Seeded-name equivalence is not
shown on the process result. A process venue therefore requires owner-side
re-naming. That is a requirement for M1 and stays pending.

Each measured cost repetition is kept only when that venue's result succeeded,
passed validation, and matched the serial result with the harness comparator
(topology, and, for an in-process result, the canonical element map). A failed
or invalid thread result does not enter the table or the crossover.

**Input to the owner, not a decision.** On `cut:16` and `heavy:1` the thread
paid wall is lower. On `cut:1` and `chain:8` the owner paid wall is lower. The
kernel stays **U**. This slice does not schedule Boolean execute off the
owner. A per-call process does not win. A persistent worker plus a cached
fingerprint is not shown to beat either in-process venue while the 3149.620 ms
after pid exit remains unresolved, and the 12.055 ms that would remain without
that interval still does not beat `cut:1`. No M1 speedup is claimed.

### M0.3a race runs

Measured locally on 2026-10-31. Tree `build/tsan` only. `build/relWithDebInfo` was not reconfigured. Production scheduling is unchanged. The new test is `concurrentFrozenKernelsWithOwnerRecompute` in `tests/src/Mod/Part/App/DetachedKernelQualificationTest.cpp`. No production change. The Boolean finding is described and left open.

#### Build

Pixi clang 21.1.8, Ninja, CMake 4.4.3. From the repository, inside the pixi environment (`CONDA_PREFIX` set, `CFLAGS` and `CXXFLAGS` empty):

```
cmake -S . -B build/tsan -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$CONDA_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$CONDA_PREFIX/bin/clang++" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_JOB_POOL_COMPILE=compile_jobs \
  -DCMAKE_JOB_POOL_LINK=link_jobs \
  -DCMAKE_PROJECT_INCLUDE="$HOME/Documents/code/agents/PRC/logs/m0-3a/preload-linguist.cmake" \
  -DBUILD_WITH_CONDA=ON \
  -DBUILD_GUI=OFF \
  -DBUILD_FEM_NETGEN=ON \
  -DENABLE_DEVELOPER_TESTS=ON \
  -DFREECAD_LIBPACK_USE=OFF \
  -DFREECAD_USE_EXTERNAL_SMESH=ON \
  -DFREECAD_USE_EXTERNAL_JSON=ON \
  -DFREECAD_USE_PCH=OFF \
  -DFREECAD_USE_PCL=ON \
  -DFREECAD_USE_PYBIND11=ON \
  -DFREECAD_VERSION_INCLUDE_GIT=OFF \
  -DFREECAD_USE_SANITIZER_TSAN=ON \
  -DOCCT_CMAKE_FALLBACK=ON \
  -DQT_HOST_PATH="$CONDA_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$CONDA_PREFIX" \
  -DCMAKE_PREFIX_PATH="$CONDA_PREFIX" \
  -DOCC_INCLUDE_DIR="$CONDA_PREFIX/include/opencascade" \
  -DOCC_LIBRARY_DIR="$CONDA_PREFIX/lib" \
  -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1 -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -DNDEBUG" \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=$HOME/Documents/code/agents/PRC/logs/m0-3a/ld-mold-tsan.sh -fsanitize=thread" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fuse-ld=$HOME/Documents/code/agents/PRC/logs/m0-3a/ld-mold-tsan.sh -fsanitize=thread" \
  -DCMAKE_MODULE_LINKER_FLAGS="-fuse-ld=$HOME/Documents/code/agents/PRC/logs/m0-3a/ld-mold-tsan.sh -fsanitize=thread"
cmake --build build/tsan --target Part_tests_run FreeCADMainCmd -j 6
cp -a build/tsan/bin/FreeCADCmd build/tsan/tests/FreeCADCmd
```

`FreeCADMainCmd` is the sibling `FreeCADCmd` the harness spawns. The backend looks for it next to the test executable, so the copy is required. `BUILD_GUI=OFF` keeps in-tree Coin out of this binary. App translations still call `qt_add_translation`, and LinguistTools is otherwise searched only when GUI is on, so `preload-linguist.cmake` is `find_package(Qt6 REQUIRED COMPONENTS LinguistTools)`. FreeCAD appends `-Wl,--no-undefined` to shared libraries. Clang does not link the TSan runtime into those libraries; the executable exports it (`--dynamic-list` on `libclang_rt.tsan-x86_64.a`). mold then rejects the link. `ld-mold-tsan.sh` drops only `--no-undefined` and execs pixi `ld.mold`.

Generated FreeCAD compile lines contain `-fsanitize=thread -g -O1 -fno-omit-frame-pointer` and RelWithDebInfo `-O1 -g -DNDEBUG`. They do not contain `-O2`. Checked on `src/Mod/Part/App/SignalException.cpp` and `src/Base/Exception.cpp`. `__tsan_init` is defined in `tests/Part_tests_run` and undefined in `lib/libFreeCADBase.so`.

#### Coverage

Not rebuilt and not instrumented: pixi OCC 8.0.1, Qt 6.11.2, Python 3.13.15, gtest 1.18.0. Coin is not in the link (`BUILD_GUI=OFF`). This harness does not need Gui. Vendored sources compiled into FreeCADBase are instrumented: zipios++, PyCXX, and fastsignals.

TSan interposes `malloc`/`free` and pthread for the whole process, so allocation and mutex calls from OCC, Qt, and Python are visible. Plain loads and stores inside those libraries are not. A report requires both conflicting accesses in instrumented code, or one intercepted call. A race inside OCC, or a FreeCAD access racing an uninstrumented OCC store, is not reported. `BRepPrimAPI_MakeBox` and the Boolean work inside OCC (`TShape`, `BOPAlgo`) are outside this coverage. Section 3.C item 2 asks for instrumented OCC. That is unavailable, so a clean FreeCAD log is not a pass, and thread adapters stay disabled.

#### Startup

`TsanStartupProbe.deliberateTwoThreadRace` was compiled into `Part_tests_run`, run once, and removed (source and the `CMakeLists.txt` line). TSan reported a data race at `tests/src/Mod/Part/App/TsanStartupProbe.cpp:16` and the process exited 66. Log: `~/Documents/code/agents/PRC/logs/m0-3a/startup-probe.log`. The qualification binary does not list that test.

#### Runs

`TSAN_OPTIONS=halt_on_error=0:exitcode=66:die_after_fork=0:report_thread_leaks=0`, log path under `logs/m0-3a/`. `FREECAD_USER_HOME` for the test process is `logs/m0-3a/user-home`. The harness overwrites the child `FreeCADCmd` home to a directory it creates under `XDG_CACHE_HOME` (`logs/m0-3a/cache`).

Filter, `--gtest_repeat=20` after one smoke iteration:

- `detachedBoxHarnessMatchesOwnerThreadAndIsolatedProcess`
- `booleanHarnessMatchesOwnerThreadAndIsolatedProcess`
- `concurrentFrozenKernelsWithOwnerRecompute`

Gtest on the 20 repeats: 60/60 passed, no skips. Process exit 66. Logs: `repeat20-gtest.log` and `repeat20.1921222` (21 warnings). The smoke log `smoke.1919835` is the same race, three warnings.

The stress test uses 4 workers. Each gets a private `BRepBuilderAPI_Copy` of a frozen Box and of Cut operands, then runs `executeBoxKernel` and `executeBooleanKernel` (Cut, Fuse, or Common) plus `completeBooleanComputation`. The owner recomputes unrelated boxes and an unrelated Cut on the document lane. The test requires at least two workers active together and at least one owner recompute during that window. It passed. No sleep is used to force overlap.

`boxVenueCostsWideAndNarrow` and `booleanVenueCostsWideAndNarrow` were not in the 20 repeats. They call the same kernels on one joined thread and spend the wall on the isolated process. They are not a second race sample.

#### Findings

All 21 warnings are one race. `nm -C -S Part.so` shows `currentSignalMode` as a 1-byte BSS symbol. The TSan address matches it.

| Class | Report | Shared state | Proposal |
| --- | --- | --- | --- |
| (a) real | `SignalException.cpp:211` static, ctor lines 217–218, dtor line 229. Reached from `FCBRepAlgoAPI_BooleanOperation.cpp:107` and `:139`. Worker side is `executeBooleanKernel` (Cut, Fuse, Common). Owner side is `Part::Boolean::execute` (`FeaturePartBoolean.cpp:150`) through `Part::Cut::makeOperation` (`FeaturePartCut.cpp:46`) on the document execution lane | Process-global `currentSignalMode`. Concurrent `Build` calls read and write it with no lock | Do not construct `Part::SignalException` on a worker. Keep signal-policy changes on the owner. The same dtor then calls `Base::SystemHandler::installSegfaultHandler()` at line 232 and does not put the previous `SIGSEGV` handler back. That restore bug is the one fixed on a separate branch; it is still in this tree. A mutex around the whole Boolean would hide the race and remove the overlap. No product edit in this slice |

No report in class (b), (c), or (d). No suppressions file. `Base::SignalException` (`src/Base/Exception.cpp:733`) did not appear. The Box kernel did not appear. MakeBox still ran in uninstrumented OCC.

#### Gate

| Kernel | Status | Evidence |
| --- | --- | --- |
| Box | blocked | No FreeCAD-side race in these runs. The kernel is OCC and is not instrumented. Thread adapters stay off |
| Boolean | fail | Unresolved race on `currentSignalMode` when concurrent `Build` calls overlap. OCC `BOPAlgo` / `TShape` coverage is also missing, so fixing this static would still not be an OCC pass |

Startup was a reported race and exit 66, then the probe was removed. This run is not a startup failure.

### M0.3b contracts and callback order

Date: 2026-10-31, round 2 the same day. Tree: `feat/parallel-recompute-m0` at `7a27574045`, plus this uncommitted test and documentation slice. No production change. `git diff` of `src/App`, `src/Mod/Part`, and `src/Mod/PartDesign` is empty. Generic serialization in `GenericIsolatedRecompute.cpp` is intact. `build/tsan` was not touched.

This slice pins today's serial recompute path. M1 may relax generic serialization only after these tests still describe the behaviour that must survive, or after they are updated in the same change that intentionally changes that behaviour.

#### What is pinned

Publication, in `tests/src/App/RecomputePublicationContracts.cpp` (`RecomputePublicationContract`):

- `exactPublicationInvalidatesPreparedClientsCoveringThatKey`. An exact integer publication invalidates a prepared client whose read set covers that property key. The assertion is a commit `Conflict` on the journal, after `pollPublications`.
- `siblingCompatibleExactPublicationLeavesDisjointPreparedClientValid`. A disjoint exact publication leaves the other prepared client valid and able to commit.
- `broadRecomputePublicationInformsExistingClientsAndInvalidatesCoveredContracts`. After a settled document, a forced recompute of one touched feature publishes `unknownModelMutation` (and that feature's object model). An existing exact client and an existing prepared recompute client both conflict on the wildcard. A client prepared after that publication still commits. Reading the key's current revision is not the assertion.
- `realAndBroadMutationsRejectPreparedRecomputeClient`. A prepared recompute client conflicts on an exact property write, on `objectStructure` from `addDynamicProperty`, on `documentStructure` from `addObject`, and on `unknownModelMutation` from a `PropertyLength` write.
- `pr114SameValueExtentWritesPublishWildcardAndInvalidatePreparedClients`. Writing the same `PropertyLength` value for `Length` and then `Width` still emits document, object, and property signals, publishes the wildcard, and invalidates prepared clients. That is the setter behaviour PR #114 had to stop calling from an unchanged datum refresh.

The same recorder checks publication from inside the changed callbacks, before the recompute call returns. For an integer or string it requires that property key, or that object's model key, to have advanced and to be present in `pollPublications` from the cursor taken before the recompute. A `PropertyLength` same-value write may instead show the wildcard. The live property value is read in that same callback. `expectPublications` then requires those readbacks and an empty failure list. A later journal poll is not a substitute: moving publication to after observer delivery fails this check while the new value is already readable.

Callback order, same file. Each test records document, object, and property signals and compares the full sequence:

- `independentSiblingsCommitAndNotifyInNameOrder`. Touched siblings `A` and `B`: commit and `signalRecomputedObject` both follow name order, then `signalRecomputed` and `signalBecameStable`.
- `chainCommitsSourceBeforeConsumerAndNotifiesByName`. `A_consumer` depends on `Z_source`. Commit order is the source, then the consumer. Final `signalRecomputedObject` follows snapshot name order (`A_consumer`, then `Z_source`).
- `abzReadinessCommitsDependentBeforeIndependentSibling`. Names `A`, `B`, `Z`, with `B` depending on `A`. Serial readiness commits `A`, then `B` (newly ready, and before `Z` in the ready-name scan), then `Z`. `signalRecomputedObject` is `A`, `B`, `Z`. The distinguisher is `B`'s `ExecCount` change before `Z`'s.

PR #114 GUI half: `PartPresentationCaptureTest.UnchangedDatumExtentsPreserveRevisionFence` in `tests/src/Gui/DocumentPresentationCache.cpp` records `signalBeforeChangeObject` and `signalChangedObject` for `Length` and `Width` only during `view->updateData(placement)`. An unchanged refresh, including a difference of half `Precision::Confusion()`, emits neither property. A real size change emits `before:Length`, `changed:Length`, `before:Width`, `changed:Width`, and still rejects the old capture.

Cold, error, and cancel, same App file. Each pins the serial result, the publication journal, undo, and the signal trace:

- `coldRestoredDocumentRecomputePinsResultsPublicationsUndoAndSignals` opens `data/tests/parallel-recompute/featuretest-abz-dirty.FCStd`. Open does not recompute. `A` is touched at `ExecCount` 1; `B` and `Z` are up to date at 1. Recompute runs `A` and `B` to 2, leaves `Z` at 1, adds no user undo, publishes the wildcard, and ends stable.
- `failedFeaturePinsPartialSuccessPublicationsUndoAndSignals`. `Down` depends on `Bad`; `Bad` throws before `ExecCount` changes. The job is `PartialFailure`. `Bad` is `Failed`, still touched, with a diagnostic containing `Test Exception`. `Down` is `Blocked` and keeps its pre-failure `ExecCount`. `Keep` and `Other` commit. `signalRecomputed` names every snapshot feature. The document does not become stable. With no caller transaction there is no user undo. The caller-transaction case sets `ExceptionType` before `openTransaction`, so that failure cause is not an undo record. Undo and redo restore `Keep` and `Other` `ExecCount` and emit that property's before/changed signals, then `undo` or `redo`, then `becameStable`. After both, `Bad` stays in error, stays touched, and the diagnostic still contains `Test Exception`. The `executed` flag on a `Blocked` node is not the assertion; it defaults true.
- `healthyFailureInsideCallerTransactionUndoesAndRedoes`. `Bad`, `Keep`, and `Down` (`Down` depends on `Bad`) are settled first. Aborting a transaction that only sets `ExceptionType` restores that value to 0 and leaves `Bad` touched. Setting `ExceptionType` inside a caller transaction and recomputing is `PartialFailure`: `Keep` advances, `Bad` and `Down` do not. Undo restores `ExceptionType` to 0 and `Keep`'s `ExecCount` and `ExecResult`. It does not clear `Bad`'s error bit, `Bad` or `Down` touch, or the recompute-log diagnostic. Redo puts `ExceptionType` and `Keep`'s two properties back. Those undo and redo signal sequences are pinned.
- `cancelMidRecomputeKeepsCommittedPrefixAndPinsSignals`. Cancel is requested from another thread once `A`'s `ExecCount` is publishing, and the slot waits until `cancel()` returns. A rendezvous or gate timeout fails the test with that message; the cancel thread is joined on every exit, including an exception from `cancel()`. The job is `Cancelled`. `A` is committed; `B` is `Cancelled`, still touched, and not executed. The document does not become stable. `A`'s object-model key is in the journal. With no caller transaction there is no user undo. Inside an open caller transaction, undo and redo move only `A.ExecCount`, then `undo` or `redo`, then `becameStable`. `B` stays touched.

Those three are the `RecomputeHandle` path used by serial `Document::recompute`. Two paired tests, `handlePartialFailureFinalizesCommittedSiblingsAndPresentation` and `handleCancelFinalizesCommittedPrefixAndPresentation`, require one presentation-boundary callback, `signalRecomputedObject` for each committed sibling, and `signalRecomputed` naming the snapshot. The lane twins, `lanePartialFailureSkipsTerminalFinalization` and `laneCancelSkipsTerminalFinalization`, perform the same setup and then `GTEST_SKIP` with `known defect: lane skips terminal finalization for PartialFailure/Cancelled; tracked as FIX-LANE-TERMINAL-FINALIZE`. Their bodies assert the handle behaviour and are not a contract for the lane. Run once without the skip, the lane still updates the successful siblings' values and property signals, and a failed feature's diagnostic is present, but the presentation callback stays at 0 and both `signalRecomputedObject` and `signalRecomputed` are absent.

Compatibility fixtures, for M1 to reopen:

| File | Serial result asserted by `ParallelRecomputeBaseline` |
| --- | --- |
| `featuretest-abz-clean.FCStd` | `A`→`B` plus independent `Z`, each `ExecCount` 1; recompute leaves them there |
| `featuretest-abz-dirty.FCStd` | only `A` touched; recompute returns 2 and advances `A` and `B` to 2; `Z` stays 1 |
| `part-siblings.FCStd` | box volumes 6000 and 120; recompute returns 0 |
| `part-chain.FCStd` | `Base` 8000, `Tool` 4000, unrefined `Cut` 4000, one solid; recompute returns 0 |
| `part-abz.FCStd` | box `A` 1000, cut `B` 500, independent box `Z` 24; recompute returns 0 |
| `part-invalid-box.FCStd` | `Length` 0, state `Invalid`, null shape, still so after recompute |
| `part-dirty-box.FCStd` | `Length` 20 mm, touched, volume 2000; recompute clears touched and keeps 2000 |

Volumes use a delta of `1e-4`. After the restore and no-op checks, each test force-recomputes (`recompute(None, True)`). Clean FeatureTest counts become 2. The dirty document becomes `A`/`B` 3 and `Z` 2. Box and cut volumes are checked again, and each box bound box matches `Length`, `Width`, and `Height`. Element names and topology equivalence (which face is which) are not compared. The invalid box stays invalid with a null shape after the forced recompute; that case does not require a positive recompute count. `fixture_dir()` resolves only `FreeCAD.getHomePath()` plus `Mod/Test/TestData/parallel-recompute`. CMake copies `data/tests/parallel-recompute/*.FCStd` there for the build tree and installs the same relative path. The Python unit is `src/Mod/Test/ParallelRecomputeBaseline.py`, registered in `src/Mod/Test/Init.py` and `src/Mod/Test/CMakeLists.txt`. The App file is listed in `tests/src/App/CMakeLists.txt`.

#### What M1 must not change

Until generic serialization is relaxed on purpose, and in that same change the affected tests are updated:

- Dropping `unknownModelMutation` from the legacy recompute publication, or leaving a prepared client that covers it valid across that publication.
- Treating a disjoint exact publication as a conflict, or treating a real or broad overlap as compatible.
- Informing existing clients only by a later read of a revision key. The publication journal is the contract, and a changed callback must already see that journal entry together with the new value.
- Treating the lane's skipped PartialFailure/Cancelled finalization as the serial presentation contract. The handle path is the pin. `FIX-LANE-TERMINAL-FINALIZE` stays open.
- Public commit order for `A` → `B` plus independent `Z`. Serial readiness commits `A`, then `B`, then `Z`. A frontier that commits `A`, then `Z`, then `B` changes property callbacks.
- Final `signalRecomputedObject` order. It follows snapshot name order and is separate from execute/commit order.
- The cold, failed-feature, healthy-edit, and cancel results above: published keys, which touch and error bits undo restores and which it leaves, and the pinned signal traces, including absence of `signalBecameStable` when the document stays touched.
- The seven fixture results in the table, including the forced rebuild of volumes and box bounds. Naming and topology equivalence are not yet part of that pin.

#### Break-to-prove

Two scratches were applied, the named test failed, and the production file was restored.

- Ready-name scan in `DocumentRecomputeCoordinator::scheduleReady` selected the last ready node. `abzReadinessCommitsDependentBeforeIndependentSibling` failed: `Z`'s `ExecCount` change preceded `B`'s, while `signalRecomputedObject` stayed `A`, `B`, `Z`. Log: `~/Documents/code/agents/PRC/logs/m0-3b/break-abz.log`. Restored to `std::ranges::find_if`.
- Owner-thread legacy recompute in `GenericIsolatedRecompute.cpp` skipped the `preserveLegacyRevisionSemantics` wildcard. `broadRecomputePublicationInformsExistingClientsAndInvalidatesCoveredContracts` failed `eventsContain` and `conflictsOn` for `unknownModelMutation`. Log: `~/Documents/code/agents/PRC/logs/m0-3b/break-broad.log`. The condition was restored.
- The success tail of `DocumentCommitCoordinator::commitOnDocumentThreadWithOptions` cancelled the reservation, delivered `finishCollaborationCommitNotificationBarrier`, then published the effects. `independentSiblingsCommitAndNotifyInNameOrder` failed inside the changed callbacks: `ExecCount` already read 1 and `ExecResult` already read `Exec` while the property revision stayed 0 and the object-model revision stayed at its baseline. Log: `~/Documents/code/agents/PRC/logs/m0-3b/break-publication-after-observers.log`. Publish-then-barrier was restored.

#### Still open

Cold/error/cancel under TSan (M0.3a). Pinned upstream `74168a8ae2d0c1a6e3006297673a871e8c1c8399` and the add-on corpus. `FIX-LANE-TERMINAL-FINALIZE` (section 5 item 5). Element names and topology equivalence of the force-recomputed fixtures. The failed feature's error bit, touch, and recompute-log diagnostic surviving undo of the caller transaction are pinned as observed serial behaviour; this slice does not change them. A touch bit set by a value write also survives `abortTransaction` after that value rolls back.

### Compatibility and failure matrix to run with builds

Use exactly pinned upstream `74168a8ae2d0c1a6e3006297673a871e8c1c8399`
(plan line 526), supplied by CI as a separate artifact under the allowed logs
workspace. Record its build/archive hashes and matching OCC/Python/compiler
versions. Run the same unchanged macros and documents fork→save→upstream
open/recompute/save→fork open/recompute and the reverse direction. Save copies
only under run output, never overwrite corpus originals. Compare identity/order,
placement, property values, expressions, Body Tip, named subelements/history and
downstream face/edge references, geometry/topology/validity, diagnostics/touched
state, callback execution context/order and undo/redo. Byte-identical FCStd is
not required. The probe's area/volume summary cannot pass this comparison.

Pin and require genuine Assembly3 hooks, Draft/Arch proxies, FeaturePython and
the MCP add-on. This clone's MCP submodule is **uninitialized**, with gitlink
`8899541b41443e6c9d24faf5be620898420bf3f3`; its scripts were not read or executed
here. Have CI supply that exact dependency before using the plan's `--native`
qualification lane; missing/skipped native work is a failure. Exact add-on
versions and user-model corpus remain an owner decision.

Failure/cancel/undo matrix covers ordinary partial-success and open caller
transaction semantics separately: invalid dimensions, null/invalid Boolean,
prerequisite failure and sibling success, preference/source/schema change after
capture, cancel before/during/after compute, worker crash/late result, close and
reopen using the same name, apply/postcondition failure, broad observer mutation,
and delayed presentation admission. Assert no stale/double apply, dependent
execution on old failed outputs, resource leak or duplicate callback. Preserve
previous commits according to serial policy, with rollback tested at both model
and revision/notification boundaries.

## 5. Corrections, unresolved assumptions and owner decisions

1. **Additional hard thread blocker:** the plan mentions OCC defaults and
   callbacks but does not explicitly identify per-Boolean Base/Part
   SignalException handler installation/static modes or the mutable fuzzy
   factor. These must be removed from the detached path by a separately audited
   policy or serialized; a lock enclosing the whole Boolean would eliminate the
   desired overlap. TSan alone may not diagnose signal-handler semantic races,
   so the recorder must assert no worker signal-policy mutation as well.
2. **The worked Box effect set needs tighter admission:** inherited
   GeoFeature::onChanged can set `_ElementMapVersion`, update a global reverse
   reference registry and rewrite other objects' subelement-link properties.
   This is not covered by “Shape plus bookkeeping” or excluding Box's own
   attachment/expressions. Initially require a settled version/no registered
   subelement references, or explicitly contract and publish the owner updates.
   Arbitrary touch propagation is not a substitute for these property effects.
3. **Boolean output scope:** native execution conditionally writes
   ShapeMaterial and defaultMaterial can mutate shared appearance state. A
   Shape-only Boolean adapter/publication is incomplete. Source cache creation
   can change other objects' schema; owner capture is observable work and must
   preserve ordering/revision semantics. History is not assigned by the binary
   execute body despite its Prop_Output declaration.
4. **M0 exit wording:** “credible/feasible end-to-end gain” can be a costed
   hypothesis before M1; measured full parallel document speedup cannot exist
   without M1's integration. Decide whether an optimistic measured-stage model
   plus a strict M1 go/no-go suffices, or fund a test-only integrated prototype
   in M0. Do not mark the real performance gate green from P3/raw kernels.
5. **`FIX-LANE-TERMINAL-FINALIZE` is unresolved and is not the contract.**
   `RecomputeHandle::finalizeIfTerminal` (`src/App/RecomputeHandle.cpp:92–99`)
   finalizes any terminal snapshot.
   `DocumentExecutionLane::executeActiveRecompute`
   (`src/App/DocumentExecutionLane.cpp:1024–1046`) calls
   `finalizeDetachedRecompute` only when the snapshot state is `Completed`.
   Observed on the lane for PartialFailure and Cancelled: successful siblings'
   values and property-change signals are already updated, and a failed
   feature's diagnostic is set, but the presentation-boundary callback is not
   invoked and `signalRecomputedObject` / `signalRecomputed` are absent. The
   handle path emits those. Paired tests assert the handle behaviour. The lane
   variants use the same assertions and `GTEST_SKIP` after setup. Fix the lane
   in an implementation slice; do not pin the skip as intended behaviour.
6. **Owner/CI decisions:** choose instrumented OCC/FreeCAD runner and supported
   OCC/TBB builds; retain thread adapters disabled until coverage works. Choose
   exact add-on versions/large user corpus and artifact retention location.
   Confirm CPU/memory/speedup/regression limits after release-build noise data.
   A process-only pilot is a separate explicit go/no-go if thread qualification
   remains unavailable. None of these decisions is permission to weaken the
   publication, native-run or naming gates.

## 6. Validation performed for the initial static research change

- Verified branch/worktree and source SHA, inspected the plan and source anchors;
  no AGENTS.md was present in this clone or its traversed parent directories.
- Read FCStd archives as ZIP/XML only to inventory saved object/type counts and
  SHA-256 hashes; raw inventory is in `../logs/m0-static/fixtures.json`.
- Used `uv run --offline --no-project --with pytest python ...` with a copied
  pre-existing uv cache under `../logs/m0-static/uv-cache` (default cache is
  read-only). No packages were installed. Checked probe syntax/import without
  FreeCAD, 71 source citations comprising 100 line spans, and all five shell
  blocks with `bash -n`. Native probe/race/performance/compatibility runs were
  pending at that revision; later local serial runs are in section 7. Validation details and source excerpts live under
  `../logs/m0-static/`.
- Changed only this report and the research probe; no src edits, compiler/CMake
  invocation, forbidden checkout access, push or PR action.

## 7. Serial baseline, measured

Measured locally on 2026-10-30. **Local build, not CI:** the supplied read-only
RelWithDebInfo artifact is from `FreeCAD-start` source
`ec84cacee3440461e142c29ba9cfa6a7aac21453`. Its build directory is
`/home/rch/Documents/code/FreeCAD-integration/build/relWithDebInfo`.
The local checkout HEAD read, CMake cache, FreeCADCmd/App/Part SHA-256 hashes and
their `ldd` output are retained in `../logs/m0-runtime/build-manifest.json`
and `CMakeCache.txt`. The FreeCADCmd SHA-256 is
`bb83c5b27271938ba7535bbb744b15ebd5e5e8decf196b763355ccc950d1663f`.
Runtime versions are FreeCAD 27.1.0 development, OCC 8.0.1, Python 3.13.15
(conda-forge, GCC 15.3.0), Qt 6.11.2. These are local provenance records,
not a CI attestation that proves the binary/source relationship.

**Fixture source differs from the build:** the input FCStd files come from this
clone at `06af2770baab3463915f328e4ad7cef1acf8161b`, not ec84cacee3.
Their hashes are in the local build manifest and per-run manifests. Synthetic
archives are generated from the committed probe recipe and saved only under
the run output. Each synthetic process then closes and reopens its archive,
asserts identical restored object names/types, and rebinds its edit target.
No source FCStd input is saved over.

All FreeCAD and native test processes use a user systemd scope with
`MemoryMax=5G`, `taskset -c 0-3`, and per-run FREECAD_USER_HOME, HOME,
XDG config/data/cache paths. Probe invocations also pass explicit user/system
configuration paths. The host reports an AMD Ryzen 7 4700G, 8 cores/16 logical
CPUs; only logical CPUs 0–3 are admitted. The default
session bus failed inside the sandbox; the working launch environment sets
`DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus` and removes
XDG_RUNTIME_DIR. GUI tests additionally use `xvfb-run -a` and the xcb Qt platform.
The runner records exact commands/environment overrides, timestamps, load,
console output and `/usr/bin/time -v` output. It performs no builds or installs.
Existing native Boolean wrappers request inner parallelism; **this is serial
document recompute with the default inner policy, not an inner-one baseline**.

Two probe API assumptions needed correction. `obj.isTouched()` does not exist
on this build's Python wrappers; both touch-state checks now use
`"Touched" in obj.State`. FEM mesh objects have an `App::PropertyLink` named
`Shape`, whose value is another document object, not a BRep. The final summary
now checks only `Part::PropertyPartShape` properties; the linked Part object's
actual BRep is still checked. The first FEM all/noop attempts completed their
timing samples but failed this summary API call. They are retained in
`failed-fem-summary-api/` and replaced by successful full FEM reruns. This fix
changes only the outside-timer summary; AST comparison confirms that `main()`
and its timed loop are identical between the two captured probe versions.
Hashes and the reason are recorded in `probe-versions.json`. No synthetic
geometry recipe needed a change. Runtime Qt version and a separate
`restore.json` diagnostic were also added outside the recompute timer. FreeCADCmd
returned zero even on the initial Python exception, so success requires the
expected sample count, final completed record, geometry validity and settled
objects; missing add-on imports/unrestored proxies fail the fixture.

The interrupted first driver continued running and overlapped the next attempt.
Both `smoke/` and `smoke-v2/` are retained but **excluded** from reported timings;
see `discarded-attempts.txt`. After stopping their remaining scopes, the complete
smoke matrix was repeated in `smoke-serial/`. The runner now holds an exclusive
file lock, checks for pre-existing M0 scopes, and stops its named scope on an
interrupt or timeout. Timings below come from separate subsequent serial runs.

Each reported recompute excludes mutation, dirty-set enumeration, restore and
geometry summaries. CPU/wall is the sum of process CPU time inside measured
recompute calls divided by their summed wall time. p95 uses linear interpolation
at `(n-1)*0.95`; warmups are excluded and no measured outliers are dropped.
“Peak RSS MB” means `/usr/bin/time -v` maximum resident kbytes / 1024 (MiB),
over the entire process, including fixture creation/restore and validation.
It is neither aggregate descendant memory nor recompute-only memory.

All 19 clean synthetic smoke cases passed with one warmup and three measured
all-dirty repetitions. `estimate.json` prices **each** of all/edit/noop at the
smoke all-dirty mean action cost, plus measured fixed process/setup/restore/
validation cost. This is deliberately conservative because edit/noop were not
timed in the requested smoke stage. At 08:44 UTC it estimated 23,522 seconds
(6.53 hours) for 30 repetitions against 15,979 seconds left in the five-hour
task budget. Reserving 2,700 seconds for files/cold/suites/reporting selected
**14 measured repetitions uniformly**, with five warmups, for every synthetic
scenario and file scenario. Its selected synthetic estimate is 13,168 seconds
(3.66 hours). Cold-after-restore remains 30 fresh processes with zero warmups
and one measured recompute each. This is one local serial session, not the
future three-session randomized detached-harness comparison.

Edit samples alternate the first recipe dimension between its original value
and original + 0.01 mm (seven measured samples of each). They need not have the
same topology as the original all-dirty fixture: the final `chain:200` tip has
eight faces and volume 18010.1 mm³ after edit, versus six faces and 18010 mm³
in all/noop. A slower edit here is not a same-geometry execution comparison.
Initially touched names and the recompute API's returned object count are
retained separately; neither is an exact native execute/kernel trace.

The live Pair 512 scope snapshot confirmed MemoryMax=5368709120 bytes and
24 tasks, including application/runtime threads. Task count is not kernel
overlap or an OCC inner-worker count. The snapshot is retained in
`live-scope-resource-snapshot.txt`; actual native overlap remains unavailable.
`campaign.json` records the task start, launch working directories and helper
hardening during execution. The timed probe remains identical across clean
smoke/full runs. Later file/cold/native launches also pin cwd and TMPDIR to
their output directory and have a campaign deadline leaving ten minutes for
reporting. A fresh `M0_RUN_ROOT` selects a new campaign without overwriting
earlier provenance or case folders.

### Results and restored counts

All 19 clean smoke cases, 57 synthetic case/scenario runs and 18 file runs
passed the probe's completion, settled-state and BRep checks after the two API
fixes. Each final synthetic/file run has all 14 selected measured samples.
No missing add-on imports or unrestored data proxies were reported in this build; no dependency
failure was skipped or replaced with a stub. These are headless data checks,
not full naming/history, add-on semantic or GUI equivalence qualification.

Synthetic restored counts agree with the recipes: Box=N; binary Boolean=3N;
Pair=5N; Chain=N+9; Bodies=11N. Each Body creates an Origin, an origin point,
three axes and three planes in addition to the Body and modeled features.
File counts are restored counts, not the saved XML inventory: expression,
cross-reference, migration and carbon-copy controls restore 11/13/14/26 objects
versus 10/12/13/24 saved; the other listed fixture counts are unchanged.

BIM's genuine proxies restore and its headless data checks pass, but console
output reports that axis/dimension SVG and text export require GUI mode.
The 7197.823 ms all-dirty median includes those warning paths, not rendered
annotations or a complete visible GUI update. No rendering output is substituted.
FEM's mesh-link property is excluded only from BRep inspection; mesh quality and
solver correctness are not claimed by this probe.

| Case | Scenario | Objects | Recompute median ms | p95 ms | CPU/wall | Peak RSS MB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `box:2` | all | 2 | 3.085 | 5.395 | 0.16 | 112.8 |
| `box:2` | edit | 2 | 0.228 | 0.256 | 1.00 | 112.7 |
| `box:2` | noop | 2 | 0.030 | 0.061 | 1.02 | 112.9 |
| `box:512` | all | 512 | 1996.408 | 2029.922 | 0.44 | 161.9 |
| `box:512` | edit | 512 | 1.941 | 1.983 | 0.99 | 157.3 |
| `box:512` | noop | 512 | 1.012 | 1.102 | 0.99 | 157.4 |
| `cut:24` | all | 72 | 342.250 | 349.704 | 0.73 | 125.6 |
| `cut:24` | edit | 72 | 8.700 | 10.899 | 1.04 | 124.9 |
| `cut:24` | noop | 72 | 0.151 | 0.157 | 0.96 | 124.7 |
| `cut:128` | all | 384 | 2196.752 | 2223.433 | 0.78 | 163.7 |
| `cut:128` | edit | 384 | 10.312 | 10.888 | 1.04 | 158.6 |
| `cut:128` | noop | 384 | 0.725 | 0.833 | 0.98 | 158.8 |
| `cut:512` | all | 1536 | 14834.059 | 14890.907 | 0.87 | 300.2 |
| `cut:512` | edit | 1536 | 17.370 | 17.720 | 1.02 | 282.8 |
| `cut:512` | noop | 1536 | 3.828 | 3.970 | 0.99 | 283.1 |
| `fuse:24` | all | 72 | 371.820 | 378.861 | 0.76 | 126.9 |
| `fuse:24` | edit | 72 | 9.928 | 10.560 | 1.05 | 126.1 |
| `fuse:24` | noop | 72 | 0.153 | 0.160 | 0.97 | 126.1 |
| `fuse:128` | all | 384 | 2372.545 | 2390.048 | 0.80 | 167.7 |
| `fuse:128` | edit | 384 | 11.447 | 14.113 | 0.99 | 163.5 |
| `fuse:128` | noop | 384 | 0.726 | 0.909 | 0.99 | 163.5 |
| `fuse:512` | all | 1536 | 15556.301 | 15660.804 | 0.88 | 319.4 |
| `fuse:512` | edit | 1536 | 18.577 | 18.952 | 1.02 | 301.4 |
| `fuse:512` | noop | 1536 | 3.826 | 3.862 | 1.00 | 301.4 |
| `common:24` | all | 72 | 328.103 | 333.685 | 0.71 | 124.5 |
| `common:24` | edit | 72 | 8.269 | 9.067 | 1.03 | 123.7 |
| `common:24` | noop | 72 | 0.151 | 0.184 | 0.97 | 123.6 |
| `common:128` | all | 384 | 2092.452 | 2105.450 | 0.76 | 158.5 |
| `common:128` | edit | 384 | 10.082 | 10.790 | 1.00 | 154.2 |
| `common:128` | noop | 384 | 0.668 | 0.823 | 0.99 | 154.1 |
| `common:512` | all | 1536 | 13858.939 | 13906.928 | 0.86 | 280.7 |
| `common:512` | edit | 1536 | 16.706 | 17.459 | 1.01 | 265.5 |
| `common:512` | noop | 1536 | 3.826 | 3.866 | 1.00 | 265.6 |
| `pair:24` | all | 120 | 775.912 | 782.077 | 0.86 | 133.6 |
| `pair:24` | edit | 120 | 22.565 | 23.719 | 1.07 | 132.7 |
| `pair:24` | noop | 120 | 0.226 | 0.235 | 0.98 | 132.9 |
| `pair:128` | all | 640 | 5303.857 | 5347.639 | 0.89 | 207.6 |
| `pair:128` | edit | 640 | 26.315 | 27.088 | 1.06 | 197.6 |
| `pair:128` | noop | 640 | 1.376 | 1.442 | 0.99 | 197.7 |
| `pair:512` | all | 2560 | 39123.912 | 39452.515 | 0.94 | 472.1 |
| `pair:512` | edit | 2560 | 41.299 | 41.713 | 1.04 | 434.5 |
| `pair:512` | noop | 2560 | 6.780 | 6.862 | 1.00 | 434.7 |
| `chain:50` | all | 59 | 941.339 | 947.262 | 1.16 | 146.3 |
| `chain:50` | edit | 59 | 1081.813 | 1259.125 | 1.18 | 150.2 |
| `chain:50` | noop | 59 | 0.170 | 0.179 | 0.97 | 142.9 |
| `chain:200` | all | 209 | 9043.631 | 9169.521 | 1.25 | 317.4 |
| `chain:200` | edit | 209 | 11509.773 | 14234.449 | 1.23 | 371.0 |
| `chain:200` | noop | 209 | 0.615 | 0.868 | 0.91 | 279.1 |
| `bodies:20` | all | 220 | 734.671 | 771.338 | 0.38 | 135.4 |
| `bodies:20` | edit | 220 | 10.031 | 10.365 | 0.69 | 131.3 |
| `bodies:20` | noop | 220 | 0.376 | 0.497 | 0.95 | 131.5 |
| `bodies:100` | all | 1100 | 6357.873 | 6439.848 | 0.64 | 193.2 |
| `bodies:100` | edit | 1100 | 15.128 | 15.736 | 0.79 | 172.3 |
| `bodies:100` | noop | 1100 | 2.542 | 2.581 | 0.99 | 172.4 |
| `bodies:500` | all | 5500 | 114321.180 | 116409.991 | 0.90 | 487.4 |
| `bodies:500` | edit | 5500 | 45.262 | 46.053 | 0.93 | 375.3 |
| `bodies:500` | noop | 5500 | 13.356 | 13.448 | 1.00 | 375.4 |

File fixtures (same columns):

| Case | Scenario | Objects | Recompute median ms | p95 ms | CPU/wall | Peak RSS MB |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| AssemblyExample.FCStd | all | 54 | 186.360 | 188.617 | 0.37 | 170.8 |
| AssemblyExample.FCStd | noop | 54 | 0.151 | 0.178 | 0.96 | 163.5 |
| BIMExample.FCStd | all | 361 | 7197.823 | 7227.042 | 1.01 | 404.2 |
| BIMExample.FCStd | noop | 361 | 1.913 | 2.090 | 0.97 | 364.2 |
| EngineBlock.FCStd | all | 36 | 353.186 | 357.897 | 1.97 | 170.2 |
| EngineBlock.FCStd | noop | 36 | 0.099 | 0.112 | 0.98 | 141.0 |
| FEMExample.FCStd | all | 43 | 116.818 | 118.138 | 0.20 | 169.8 |
| FEMExample.FCStd | noop | 43 | 0.093 | 0.146 | 1.00 | 166.6 |
| ModelFromV021.FCStd | all | 14 | 58.367 | 59.325 | 0.56 | 126.1 |
| ModelFromV021.FCStd | noop | 14 | 0.042 | 0.045 | 1.04 | 125.0 |
| PartDesignExample.FCStd | all | 28 | 369.783 | 373.949 | 1.53 | 145.9 |
| PartDesignExample.FCStd | noop | 28 | 0.112 | 0.138 | 0.96 | 123.5 |
| TestSketchCarbonCopyReverseMapping.FCStd | all | 26 | 64.853 | 65.709 | 0.17 | 118.9 |
| TestSketchCarbonCopyReverseMapping.FCStd | noop | 26 | 0.067 | 0.132 | 0.97 | 118.0 |
| TwoLengthsPadCrossObjectRef.FCStd | all | 13 | 35.506 | 36.887 | 0.30 | 123.4 |
| TwoLengthsPadCrossObjectRef.FCStd | noop | 13 | 0.040 | 0.050 | 1.05 | 122.2 |
| TwoLengthsPadWithExpression.FCStd | all | 11 | 25.958 | 27.054 | 0.18 | 119.9 |
| TwoLengthsPadWithExpression.FCStd | noop | 11 | 0.043 | 0.045 | 1.05 | 119.5 |

### Edit and action cost

Mutation is outside the recompute interval, including the native work triggered
by Box dimension setters. Action total also includes Python dirty-set enumeration;
its median need not equal the sum of separately computed component medians.
The lists below are the observed count ranges. A returned count is a framework
object count, not a kernel-call trace (setters can execute native work earlier).

| Case | Mutation median ms | Recompute median ms | Action total median ms | Initially touched | Returned count |
| --- | ---: | ---: | ---: | ---: | ---: |
| `box:2` | 0.054 | 0.228 | 0.294 | [1] | [1] |
| `box:512` | 0.120 | 1.941 | 2.799 | [1] | [1] |
| `cut:24` | 0.118 | 8.700 | 8.930 | [1] | [2] |
| `cut:128` | 0.134 | 10.312 | 10.898 | [1] | [2] |
| `cut:512` | 0.167 | 17.370 | 19.575 | [1] | [2] |
| `fuse:24` | 0.120 | 9.928 | 10.144 | [1] | [2] |
| `fuse:128` | 0.137 | 11.447 | 12.049 | [1] | [2] |
| `fuse:512` | 0.168 | 18.577 | 20.802 | [1] | [2] |
| `common:24` | 0.121 | 8.269 | 8.505 | [1] | [2] |
| `common:128` | 0.146 | 10.082 | 10.691 | [1] | [2] |
| `common:512` | 0.179 | 16.706 | 18.951 | [1] | [2] |
| `pair:24` | 0.132 | 22.565 | 22.860 | [1] | [3] |
| `pair:128` | 0.152 | 26.315 | 27.249 | [1] | [3] |
| `pair:512` | 0.182 | 41.299 | 45.196 | [1] | [3] |
| `chain:50` | 0.034 | 1081.813 | 1082.011 | [1] | [51] |
| `chain:200` | 0.047 | 11509.773 | 11510.361 | [1] | [201] |
| `bodies:20` | 0.026 | 10.031 | 10.343 | [1] | [3] |
| `bodies:100` | 0.040 | 15.128 | 16.559 | [1] | [3] |
| `bodies:500` | 0.056 | 45.262 | 54.545 | [1] | [3] |

### Cold-after-restore and native suites

Thirty fresh processes opened the already generated `full/pair-24-all/fixture.FCStd`,
restored 120 objects, touched all and recomputed once, with zero warmups.
All 30 completed. The source archive hash is identical in every process.

| Cold metric | Median | p95 |
| --- | ---: | ---: |
| First dirty recompute after restore, ms | 739.503 | 745.265 |
| Restore/load span, ms | 150.821 | 152.312 |
| Scope launch to process exit, s | 1.618 | 1.669 |

Maximum per-process `time -v` RSS across the cold series is 129.594 MiB.
Launch-to-exit includes imports/startup, restore, mutation/enumeration, recompute,
validation, summary and teardown; it is not a spawn-to-ready measurement.
Restore and imports may warm OCC, so this does not measure a truly first raw
kernel invocation or the existing geometry-worker transport protocol.

| Existing suite / runbook filter | Passed | Failed | Errors | Skipped / not run | Exit |
| --- | ---: | ---: | ---: | ---: | ---: |
| App: coordinator, generic isolated, native request capture, commit coordinator | 42 | 0 | 0 | 0 | 0 |
| Part: ShapeCacheIsolatedRecomputeTest.* | 8 | 0 | 0 | 0 | 0 |
| Gui under Xvfb: unchanged datum extents revision fence | 1 | 0 | 0 | 0 | 0 |

All expected names match `native-expected-tests.json`; there are no unexpected
cases. Full XML/console/time output is under `native/{App,Part,Gui}/` and test
binary hashes are in `native-binary-hashes.txt`. These are the pre-existing
baseline tests, not the unimplemented M0 recorder/detached/race/publication gate.

### Interpretation and owner decisions

- **The phase serial fraction is still unmeasured.** Kernel, capture, TNP,
  publication, GIL waits and presentation spans are unavailable. CPU/wall is
  process utilization over recompute, not a serial-fraction estimate. Ratios
  above one (EngineBlock 1.97, chain 200 all 1.25) also show why this default
  inner-policy baseline must not be called inner-one.
- The largest case, Bodies 500 (114.321 s median), contains **zero** exact
  Part::Box/Cut/Fuse/Common objects. It remains entirely outside the initial
  four-type pilot and cannot justify that pilot's gain. Keep both narrow
  controls and unsupported-type workloads as regression/fallback controls.
- Pair 24→512 increases branch count 21.3× but all-dirty wall time 50.4×.
  Identical analytic operands do not make the full document cost linear.
  Owner/framework/naming contributions need measured spans before assigning
  this time to parallelizable geometry. Section 8 now profiles the growing
  native owner overhead; this baseline alone did not establish its cause.
- Single-property edits expose one dependent branch: binary Boolean runs
  return two objects, Pair three, Bodies three; Chain edits return N+1.
  Wide all-dirty speed potential is not wide incremental speed potential.
  Select real multi-branch edits and heavy imported/spline/shared-topology
  Boolean models in the owner corpus before choosing a venue.
- Timings are descriptive for one local session. First→last measured times
  rise 1.2% for Cut 512, 2.9% for Pair 512, 4.8% for Chain 200 and 3.6% for
  Bodies 500 (112.459→116.507 s). Nothing is removed as an outlier; cause and
  long-run stationarity are unqualified. Do not apply performance gates or
  extrapolate a parallel speedup from these medians alone.
- **Owner decisions:** fund the native phase recorder and instrumented
  FreeCAD/OCC race harness; choose the heavy/user corpus and pinned upstream/
  add-on versions; obtain a genuine GUI BIM annotation/presentation baseline.
  Defer the thread/process venue decision until capture/transport/naming/commit
  costs and qualification exist. Keep thread adapters disabled. A process-only
  pilot remains a separate owner decision after its isolation/transport gate.

The completed artifact audit (`validation.json`) passes 246 checks: expected
run/sample counts, synthetic restored counts and analytic one-solid volumes,
unchanged source archive and build binary/library hashes, identical runtime
versions, one common cold input hash, resource-wrapper arguments, exact native
case names and no overlapping measured process intervals. Raw data and generated
statistics are in `../logs/m0-runtime/{summary.json,tables.md,edit-timings.md,trends.json}`.
The two contaminated smoke attempts and failed FEM summaries remain separately
retained with their reasons. No production source or scheduling changes, build,
package installation, upstream run or race detector is part of this measurement.

## 8. Superlinear serial cost, profiled

Measured locally on 2026-10-30, starting from `0905b8b83c`. The growing cost is
in the serial document coordinator: repeated scheduling scans, document-wide
preparation admission scans, and full progress snapshots. Independent geometry
branches still pay these shared loops once per feature. This is evidence for
serial algorithm improvements before using these timings to size a parallel pilot.
No production code, build, preference policy or scheduling behavior was changed.

### Method, window and limitations

All artifacts are under `../logs/m0-profile/`. The FreeCADCmd/App/Part SHA-256
checks in `artifact-check.json` match the section 7 build exactly. The profiler
reads the same existing `m0-runtime/full/{case}-all/fixture.FCStd` archives;
`M0_CASE=file`, `M0_SCENARIO=all`, `M0_WARMUPS=0`, `M0_REPETITIONS=1` select one
first all-dirty recompute after restore. Per-run command manifests retain the
original family/count and archive hash. Inputs were never saved over.

First tried the requested parent-spawned command:

```text
uv run --no-project --with py-spy py-spy record --native --rate 200 -f raw -- <FreeCADCmd and probe arguments>
```

Py-spy 0.4.2 reads the embedded Python 3.13 interpreter, but misses this build's
native document owner thread. In Bodies 100 it recorded 1,020 samples/49 errors;
only 48 included the named recompute wrapper, all waiting in the caller's
`std::future`. The document and shape summary completed; py-spy returned 1 with
`No child process`. Its Bodies 500 attempt was stopped when this attribution
limitation was established. Those Python-thread stacks cannot explain the native
work, so **the reported profiler is parent-launched GDB 17.1**, the permitted
fallback. No perf command or sysctl change was attempted.

A copy of the existing probe under each run folder wraps only its timed
`doc.recompute(None, False, True)` with SIGUSR1/SIGUSR2 gates (stopped and suppressed
by gdb). GDB/MI resumes for 50 ms, interrupts all threads, records
`thread apply all bt 100`, and repeats until the end gate. Restore, touching,
verification and shape summaries are outside the gates. Each launch is under
`systemd-run --user --scope -q -p MemoryMax=5G`, with `taskset -c 0-3` inherited
by debugger and inferior; HOME, FREECAD_USER_HOME, XDG homes/cache and TMPDIR
are in that run folder. The corrected four GDB cases ran sequentially and exited
normally; every object was dirty and every sample/shape validation completed.
An initial gdb gate configuration attempt is explicitly retained as discarded.

`gdb_profile.py`, `gdb_sample.py`, the exact `probe.FCMacro` copies, command/env
manifests, `gdb-mi.log`, `gdb-stacks.jsonl`, `samples.jsonl`, `sampler.json` and
process/time logs reproduce the measurements. Run helpers with `python3` from
the logs directory; each case requires a fresh output folder. `fold.py` writes
root-to-leaf `folded.raw`, `inclusive.tsv`, and `folded-summary.json`; `report.py`
writes the complete share-growth rankings and exact unfiltered top-15 lists
in `growth-rankings.json`, `*-growth.tsv` and `top15-all.md`.

For each stop, fold the unique native owner stack containing
`App::DocumentExecutionLane::threadMain`; count an inclusive frame at most once
per stop. All stops have exactly one such stack. Other threads, including the
blocked Python caller, are excluded from the denominator. Shares are **owner
elapsed-time sample shares**, including its sleeps, not CPU percentages. Most
thread-entry/dispatch/wait frames are present in 100% of samples and tell us
nothing about growth. A 50 ms interval is inferior run time between stops;
backtrace/symbol-loading time adds substantial pauses. Thus the instrumented
wall times below are not new performance baselines. Single profiles, especially
the roughly 100-stop small cases, support attribution, not precise speedup claims.

| Case | Objects / recomputed | Owner stops | GDB recompute wall s | Inferior process CPU s |
| --- | ---: | ---: | ---: | ---: |
| `bodies:100` | 1100 / 1100 | 115 | 13.639 | 3.715 |
| `bodies:500` | 5500 / 5500 | 1951 | 162.332 | 89.612 |
| `pair:128` | 640 / 640 | 93 | 12.009 | 4.225 |
| `pair:512` | 2560 / 2560 | 662 | 56.568 | 31.812 |

### Inclusive frame shares

The following are each case's top 15 inclusive `App::` frames, omitting only
generated lambda/invocation forwarding and owner dispatch/wait infrastructure.
Ties use frame-name order. These frames include parent/child overlap and must
not be summed. Exact unfiltered top-15 native frames (including standard-library
and 100% transport frames) are also listed for every case in
`../logs/m0-profile/top15-all.md`; the full rankings are in each `inclusive.tsv`.

**bodies:100**

| Inclusive application frame | Share |
| --- | ---: |
| `App::DocumentRecomputeCoordinator::poll` | 67.8% |
| `App::DocumentCollaborationService::commitRecomputeEdit` | 47.0% |
| `App::DocumentCollaborationService::commitRecomputeEditOnDocumentThread` | 47.0% |
| `App::DocumentCommitCoordinator::commitRecompute` | 47.0% |
| `App::DocumentCommitCoordinator::commitOnDocumentThreadWithOptions` | 46.1% |
| `App::DocumentCommitCoordinator::commitWithPreparationPolicyAndOptions` | 46.1% |
| `App::DocumentRecomputeCoordinator::scheduleReady` | 20.0% |
| `App::Document::beginCollaborationCommitNotificationBarrier` | 18.3% |
| `App::DocumentCollaborationService::prepareEditAsync` | 8.7% |
| `App::DocumentCollaborationService::prepareEditAsyncOnDocumentThread` | 8.7% |
| `App::Document::collaborationPreparationSupported` | 7.8% |
| `App::Document::_recomputeFeature` | 6.1% |
| `App::DocumentObject::recompute` | 6.1% |
| `App::Document::finishCollaborationCommitNotificationBarrier` | 3.5% |
| `App::ExtensionContainer::getPropertyList` | 3.5% |

**bodies:500**

| Inclusive application frame | Share |
| --- | ---: |
| `App::DocumentRecomputeCoordinator::poll` | 70.0% |
| `App::DocumentRecomputeCoordinator::scheduleReady` | 36.7% |
| `App::DocumentCollaborationService::commitRecomputeEdit` | 29.9% |
| `App::DocumentCollaborationService::commitRecomputeEditOnDocumentThread` | 29.9% |
| `App::DocumentCommitCoordinator::commitOnDocumentThreadWithOptions` | 29.8% |
| `App::DocumentCommitCoordinator::commitRecompute` | 29.8% |
| `App::DocumentCommitCoordinator::commitWithPreparationPolicyAndOptions` | 29.8% |
| `App::DocumentCollaborationService::prepareEditAsync` | 17.1% |
| `App::DocumentCollaborationService::prepareEditAsyncOnDocumentThread` | 17.1% |
| `App::Document::collaborationPreparationSupported` | 16.7% |
| `App::Document::beginCollaborationCommitNotificationBarrier` | 12.5% |
| `App::ExtensionContainer::getPropertyList` | 8.5% |
| `App::DocumentRecomputeCoordinator::status` | 7.8% |
| `App::DocumentRecomputeCoordinator::statusLocked` | 7.8% |
| `App::Document::finishCollaborationCommitNotificationBarrier` | 5.0% |

**pair:128**

| Inclusive application frame | Share |
| --- | ---: |
| `App::DocumentRecomputeCoordinator::poll` | 72.0% |
| `App::DocumentCollaborationService::commitRecomputeEdit` | 60.2% |
| `App::DocumentCollaborationService::commitRecomputeEditOnDocumentThread` | 60.2% |
| `App::DocumentCommitCoordinator::commitOnDocumentThreadWithOptions` | 60.2% |
| `App::DocumentCommitCoordinator::commitRecompute` | 60.2% |
| `App::DocumentCommitCoordinator::commitWithPreparationPolicyAndOptions` | 60.2% |
| `App::Document::_recomputeFeature` | 50.5% |
| `App::DocumentObject::recompute` | 50.5% |
| `App::DocumentRecomputeCoordinator::scheduleReady` | 9.7% |
| `App::Document::collaborationPreparationSupported` | 6.5% |
| `App::DocumentCollaborationService::prepareEditAsync` | 6.5% |
| `App::DocumentCollaborationService::prepareEditAsyncOnDocumentThread` | 6.5% |
| `App::Document::beginCollaborationCommitNotificationBarrier` | 4.3% |
| `App::DynamicProperty::getPropertyList` | 4.3% |
| `App::ExtensionContainer::getPropertyList` | 4.3% |

**pair:512**

| Inclusive application frame | Share |
| --- | ---: |
| `App::DocumentRecomputeCoordinator::poll` | 74.2% |
| `App::DocumentCollaborationService::commitRecomputeEdit` | 46.2% |
| `App::DocumentCollaborationService::commitRecomputeEditOnDocumentThread` | 46.2% |
| `App::DocumentCommitCoordinator::commitOnDocumentThreadWithOptions` | 46.2% |
| `App::DocumentCommitCoordinator::commitRecompute` | 46.2% |
| `App::DocumentCommitCoordinator::commitWithPreparationPolicyAndOptions` | 46.2% |
| `App::Document::_recomputeFeature` | 31.3% |
| `App::DocumentObject::recompute` | 31.1% |
| `App::DocumentRecomputeCoordinator::scheduleReady` | 27.3% |
| `App::DocumentCollaborationService::prepareEditAsync` | 19.0% |
| `App::DocumentCollaborationService::prepareEditAsyncOnDocumentThread` | 19.0% |
| `App::Document::collaborationPreparationSupported` | 18.6% |
| `App::ExtensionContainer::getPropertyList` | 8.0% |
| `App::Document::beginCollaborationCommitNotificationBarrier` | 5.4% |
| `App::DynamicProperty::getPropertyList` | 4.8% |

### Frames whose shares grow

Rank by large-minus-small percentage points, collapsing forwarding aliases and
choosing distinct causes rather than repeating a hot parent and its child.
The all-frame rankings, including every STL frame and negative change, remain
in `growth-rankings.json`. The three leading causal frames are the same in both
families. Paths/lines below refer to this clone at `0905b8b83c`.

| Growing frame | Bodies 100 → 500 | Growth pp | Pair 128 → 512 | Growth pp | Source |
| --- | ---: | ---: | ---: | ---: | --- |
| `DocumentRecomputeCoordinator::scheduleReady` | 20.0% → 36.7% | +16.7 | 9.7% → 27.3% | +17.7 | `src/App/DocumentRecomputeCoordinator.cpp:389` |
| `Document::collaborationPreparationSupported` | 7.8% → 16.7% | +8.8 | 6.5% → 18.6% | +12.1 | `src/App/Document.cpp:2483` |
| `DocumentRecomputeCoordinator::statusLocked` | 1.7% → 7.8% | +6.1 | 1.1% → 3.6% | +2.6 | `src/App/DocumentRecomputeCoordinator.cpp:965` |

`DocumentRecomputeCoordinator.cpp`, `DocumentCollaborationService.cpp`,
`RecomputeHandle.cpp` and `DocumentCommitCoordinator.cpp` match the supplied
build source byte-for-byte as text. `Document.cpp` has unrelated save/relabel
differences: the preparation loop is identical, at build line 2497 rather than
clone line 2483. `source-comparison.json` and `*.source-diff` retain this check.


### The three loops and proposed fixes (design only)

Let F be planned features, N document objects, and P average properties per
object. Here F=N: 1,100/5,500 for Bodies, 640/2,560 for Pair. Each Body adds
eleven objects, including origins, planes, axes and an origin point; all of
those are touched and traverse the coordinator, despite only two modelling
features per Body. The broad legacy revision fence admits one preparation at
a time (`GenericIsolatedRecompute.cpp:2483–2488`); independence does not avoid
the repeated coordinator work. Preserve that fence in all proposed fixes.

1. **Scheduling rescans: `DocumentRecomputeCoordinator.cpp:375–456`.** Each
   `scheduleReady()` iteration walks `job.nodes` at line 389, rechecks failed
   dependencies with map lookups (399–404), scans for pending payloads (416),
   scans active states (422), then searches again for the first ready feature
   (431). It repeats after submission and on every `poll()` (823). With one
   outstanding feature, O(F) completions repeatedly inspect O(F+E) state and
   perform O(log F) dependency lookups: at least quadratic, and O(F² log F)
   for sparse bounded-degree graphs with those map lookups. The waiting-node
   suffix and already-completed prefix alone give triangular repeated scans.

   **Fix:** maintain reverse dependency lists, remaining-dependency counts,
   a ready set ordered by the current feature-ID map comparator, active/pending
   counts and terminal/failure counts. Update them at each state transition;
   propagate failures/cancellation through the same dependency edges. Keep the
   single-active broad-fence rule, payload backpressure, retry behavior and
   existing ready-selection order. Intended behavior is unchanged; a plain FIFO
   ready queue would change the order and is not the proposed fix. Internal
   readiness work becomes O((F+E) log F), with cancellation propagation charged
   to affected edges. Incremental bookkeeping must cover every state transition.

2. **Repeated preparation eligibility: `Document.cpp:2470–2485`, called at
   `DocumentCollaborationService.cpp:954`.** Every preparation enumerates every
   object's property list and tests each property for `PropertyPythonObject`.
   These fixtures contain no such payload, so no early rejection shortens the
   scan. Repeating O(NP) per feature costs O(FNP), quadratic for fixed P and
   F proportional to N. `ExtensionContainer::getPropertyList` and its vector
   allocations/type checks are child frames, not separate causes.

   **Fix:** check `intent.operationType != GenericIsolatedRecomputeOperationType`
   before calling `collaborationPreparationSupported()`. The current `&&`
   evaluates the full document scan first, although this operation is already
   exempt from rejection. This preserves the existing acceptance/refusal rule
   and the rejection check for other operation types; it removes the whole
   redundant scan on this path. For other operations, a later schema-generation
   cache could help, but would need invalidation on object/extension/property
   creation/removal, restore and rollback. No cache is needed for the narrow fix.

3. **Full status materialization: `DocumentRecomputeCoordinator.cpp:950–988`.**
   `statusLocked()` reserves F entries, iterates the entire node map at 965,
   copies feature IDs/diagnostics and recounts progress. `RecomputeHandle.cpp:156–172`
   calls coordinator status at 160 and handle status again at 165 inside the
   per-progress wait loop; the latter can fall through `DocumentExecutionLane.cpp:587`
   to coordinator status again. With O(F) iterations this is O(F²) string/vector
   construction, followed by matching destruction. String allocation and
   `DocumentRecomputeSnapshot`/feature-snapshot destructors grow with this path.

   **Fix:** keep aggregate state/counters incrementally and use a private light
   terminal/progress query inside `wait()`. Materialize a full snapshot on
   terminal return, timeout, and explicit public `status()` requests. Public
   snapshots must retain all current fields, ordering and consistency; keep
   finalization, failure/cancel results and timeout behavior. This changes
   internal work, with no intended observable change. A change to public
   progress cadence or omission of feature records would be a separate behavior
   decision and is unnecessary for this fix. Snapshot destruction/copies outside
   `statusLocked()` provide extra potential saving, omitted from the estimate.

### Saving budgets, overlap and remaining quadratic work

`scheduleReady()` includes `prepareEditAsync()` and hence the eligibility scan.
Subtract that entire child before estimating scheduling savings: exclusive of
preparation, scheduling is **11.3% → 19.6%** for Bodies (rather than summing
36.7% and 16.7%). The three disjoint regions below account for about 44.1% of
the large-case samples. These are conditional engineering budgets, not measured
speedups: apply the zero-warmup profile shares to the section 7 uninstrumented
median of 114.321 s, assuming a similar mixture. Never multiply by the 162.332 s
GDB wall time, which includes debugger pauses.

| Proposed fix | Disjoint large share | Entire sampled-region budget | Rough saving at 500 Bodies |
| --- | ---: | ---: | ---: |
| Incremental ordered scheduling, excluding all preparation | 19.6% | 22.4 s | ~19 s |
| Skip already-exempt Python-payload scan | 16.7% | 19.0 s | ~19 s |
| Lightweight internal status, full public/terminal snapshots | 7.8% | 9.0 s | ~8 s |

For scheduler/status estimates, retain five times their Bodies-100 region time
as residual linear work: `114.321*large_share - 5*6.358*small_share`. For the
exempt scan, removal uses the whole 19.0 s budget. The combined scenario is
about **46 s saved**, inside a **50 s sampled-region ceiling**; neither figure
is a guarantee. The short small profiles, cold-versus-warm difference, sampling
noise, allocator effects and changed execution mixture after optimization all
limit precision. Validate changes on the existing all/edit/noop and error/
cancellation/rollback/observer controls before treating this as a performance
prediction. No fix or performance A/B was implemented here.

There is further quadratic work even when its *relative share decreases*.
`Document::beginCollaborationCommitNotificationBarrier` copies document-wide
identity/order/touched collections and every object's scheduler state
(`Document.cpp:1687–1704`); finish clears/destroys them (`1825–1846`). This is
O(FN) per-feature rollback bookkeeping. Begin+finish occupies **21.7% → 17.5%**
for Bodies, still about 20 s of the large uninstrumented median if the shares
transfer. A future journal/copy-on-write boundary could avoid full copies, but
must preserve exact rollback order, identities, active object, touched state,
undo/redo, arbitrary callback mutations and allocation-free recovery. Simply
dropping the snapshot for apparently independent features would change failure
semantics. This is an additional design candidate, not one of the top three
share-growth causes and not included in the 46 s scenario.

The owner sleep frame shrinks from 28.7% to 15.3% for Bodies: the fixed 2 ms
poll sleep (`RecomputeHandle.cpp:172`) is a separate linear/polling overhead,
not evidence of superlinear geometry. `_recomputeFeature` shrinks from 6.1%
to 2.1% for Bodies and 50.5% to 31.3% for Pair. Parallelizing just feature
execution cannot remove the growing coordinator/property/status loops.

**Owner decisions:** prioritize the already-exempt eligibility-check reorder,
then ordered incremental scheduling and internal status counters. Keep the
current broad invalidation fence, callback order and public observations.
Choose whether to fund the more involved rollback-journal design after those
measurements. These findings do not qualify detached kernels, change the M0
race/behavior gate, or decide thread versus process execution. Bodies remains
outside the four exact-type pilot. Raw evidence and validation are local only;
this change commits documentation and does not push.

The local artifact audit (`../logs/m0-profile/validation.json`) passes 42 checks:
zero-warmup/all-dirty sample counts, normal gated completion, resource-wrapper
arguments and isolated homes, unchanged input/build hashes, restored type and
shape-summary equality with the serial baseline, exactly one owner per stop,
folded denominators, helper syntax, and unchanged probe `main()` AST except for
the recompute wrapper. `git diff --check` passes. No native behavior tests were
rerun for this documentation-only change.
