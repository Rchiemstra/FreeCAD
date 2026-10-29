# Parallel recompute M0: static contracts and qualification runbook

Research date: 2026-10-30. Inspected source/plan commit:
`e1446662caadf9ca2e2335fd81ee4eed460f01a4`, branch
`research/parallel-recompute-m0`. This is the source in this clone, not a build
identity. The plan cites an earlier product audit at `d7b0d2ffa7fce1ac21a5a9fdabdfd901fea434d8`;
its measurements have not been repeated here.

**The static part of M0 is recorded; the M0 exit gate is not satisfied.** No
FreeCAD/OCC executable, compiler, CMake, package installation, upstream run or
race detector was used. Nothing under `src/` changes. No production scheduling
or parallel feature execution is introduced. All four live target methods need
serialization. Their proposed detached kernels remain unqualified.

The only additional script, `tools/parallel_recompute_m0_probe.py`, is an
unexecuted FreeCAD serial measurement probe. It changes temporary in-memory
documents and writes generated fixtures/results to an explicit output directory;
it never saves over an existing input FCStd. Its Python syntax was checked, but
its FreeCAD API behavior still needs a build. Do not treat it as a race detector,
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
| Opt-in phase timing, reproducible fixture capture, build/DAG/resource identity | Static runbook/probe; native instrumentation needed | **Done (design/probe); needs build** for validated probe, phase spans, production property graph, concurrency and memory traces. |
| Detached Box/Boolean geometry harness and negative contract tests, with unchanged production scheduling | Static harness/test specification | **Done (design); needs build** and native test-only kernels; no detached harness exists in this change. |
| Compare serial/thread/process costs, wide/narrow and incremental workloads, two narrow controls | Static recipes/commands | **Done (runbook); needs build** for all timings and venue/crossover decision. No new runtime numbers. |
| Working instrumented race checks, including cold/error/cancel cases | Static gate design | **Done (design); needs build** plus instrumented OCC and a compatible runner. No race pass claimed. |
| Pinned upstream and ordinary/macro/add-on compatibility baseline fixtures | Static fixture inventory/pin | **Needs build** for bidirectional runs; **blocked** for final large user corpus/add-on version selection pending owner input. |
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

## 4. Measurement runbook (no measured performance values)

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

## 6. Validation performed for this research change

- Verified branch/worktree and source SHA, inspected the plan and source anchors;
  no AGENTS.md was present in this clone or its traversed parent directories.
- Read FCStd archives as ZIP/XML only to inventory saved object/type counts and
  SHA-256 hashes; raw inventory is in `../logs/m0-static/fixtures.json`.
- Used `uv run --offline --no-project --with pytest python ...` with a copied
  pre-existing uv cache under `../logs/m0-static/uv-cache` (default cache is
  read-only). No packages were installed. Checked probe syntax/import without
  FreeCAD, 71 source citations comprising 100 line spans, and all five shell
  blocks with `bash -n`. Native probe/race/performance/compatibility runs remain
  pending. Validation details and source excerpts live under
  `../logs/m0-static/`.
- Changed only this report and the research probe; no src edits, compiler/CMake
  invocation, forbidden checkout access, push or PR action.
