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
| Detached Box/Boolean geometry harness and negative contract tests, with unchanged production scheduling | Static harness/test specification | **Done (design); needs build** and native test-only kernels; no detached harness exists in this change. |
| Compare serial/thread/process costs, wide/narrow and incremental workloads, two narrow controls | Local serial measurements; detached harness needed | **Done (serial baseline):** section 7 measures wide/narrow, edit/noop and cold-after-restore controls. Thread/process comparison and venue/crossover decision still need the detached harness. |
| Working instrumented race checks, including cold/error/cancel cases | Static gate design | **Done (design); needs build** plus instrumented OCC and a compatible runner. No race pass claimed. |
| Pinned upstream and ordinary/macro/add-on compatibility baseline fixtures | Local headless baseline; pinned upstream still needed | **Done (local baseline):** all nine source fixtures restore/recompute in all/noop (section 7), with genuine non-null proxies and no missing add-on dependencies reported. BIM SVG/text output remains GUI-only. Bidirectional compatibility/naming/macro runs still need the pinned upstream build; final large user corpus/add-on version selection remains **blocked** on owner input. |
| Existing native App/Part/Gui baseline suites | Local supplied binaries | **Done:** 42 App + 8 Part + 1 Gui tests pass, zero failures/errors/skips, exact expected names present (section 7). These do not implement the new detached/race/publication qualification tests. |
| Exact sibling publication, broad conflict invalidation and existing client visibility | Static assertions/test placement | **Done (design); needs build** for demonstration; generic serialization stays intact. |
| Callback order A/B/Z, PR #114, terminal failure/cancel presentation, undo/rollback | Static source comparison/test matrix | **Done (design); needs build** for traces and equivalence. Existing finalization difference is unresolved. |
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
reports, including exact new test manifest once implemented. New recorder,
detached kernel and publication tests **do not yet exist**; no invented gtest
filter/worker-count CLI in this document is runnable today. Likewise the current
probe has no `parallel=true` switch.

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
5. **Terminal presentation difference is real in this source:**
   `src/App/RecomputeHandle.cpp:91–98` finalizes any terminal snapshot;
   `src/App/DocumentExecutionLane.cpp:1001–1006` only finalizes Completed.
   Determine intended Failed/Cancelled callbacks and visible revision behavior
   against the serial reference, then test/fix in an implementation slice;
   do not assume these entry points are already equivalent.
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
