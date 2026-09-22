# AB-21 report — GUI blocking and live-model ingress

This report summarizes the reproducible inventory (2,439 findings across seven
categories, snapshot in `inventory.json`) and proposes bounded follow-up task
candidates. It proposes work only; no production behavior is changed here.

## Inventory at a glance

| Category | Findings | Disposition | Summary |
| --- | ---: | --- | --- |
| `live-app-dereference` | 1,458 | investigate | `App::GetApplication().getActiveDocument()/getDocument()/getDocuments()` and `FreeCAD.ActiveDocument` reach. |
| `live-reference-callback` | 234 | migrate | State-change signal/slot callbacks and observer subscriptions carrying live references. |
| `update-data-provider` | 236 | migrate | C++ `ViewProvider::updateData` overrides plus provider-aware Python `updateData` callbacks. |
| `direct-recompute` | 432 | migrate | Synchronous `recompute()` from GUI commands/tasks/dialogs. |
| `process-events-polling` | 51 | investigate | Manual event-loop pumping. |
| `thread-waits` | 27 | investigate | Blocking `waitFor*()`/`wait()` on processes, futures, conditions, and threads. |
| `blocking-invokes` | 1 | migrate | Single `Qt::BlockingQueuedConnection` dispatch hook. |

The snapshot spans 751 C++ and 1,688 Python findings. Owning subsystems:
`BIM` (839), `CAM` (390), `Gui` (324), `Fem` (185), `Draft` (139), `Part`
(110), `TechDraw` (99), `Assembly` (79), `PartDesign` (68), `Mesh` (62),
`OpenSCAD` (30), `Surface` (21), `Sketcher` (19), `Measure` (18), `Material`
(12), `Spreadsheet` (8), `MeshPart` (7), `Import` (6), `Inspection` (6),
`Points` (6), `ReverseEngineering` (5), `Robot` (5), and `Start` (1).

## Bounded follow-up task candidates

Each candidate is a bounded unit of work on top of this inventory; none is
started here.

### 1. Replace the Mesh evaluation busy-poll loop (`Mesh`)
`src/Mod/Mesh/Gui/DlgEvaluateMeshImp.cpp` contains a dense `processEvents` and
`recompute()` cluster inside a single long-running evaluation loop — the
clearest GUI-thread blocking cluster in the inventory. Replace the poll loop
with a queued/asynchronous progress mechanism driven by the event loop.

### 2. Convert the remaining blocking invoke hook (`Gui`)
`src/Gui/Application.cpp:468` installs the one `Qt::BlockingQueuedConnection`
dispatch (`blocking ? Qt::BlockingQueuedConnection : Qt::QueuedConnection`).
Migrate its synchronous worker-to-GUI callers to queued, pointer-free dispatch
and then remove the blocking branch. Pairs with the existing
`Guard GUI waits joins and live-reference event payloads` work item.

### 3. Route GUI recompute onto the execution lane (framework + workbenches)
All `direct-recompute` sites (Command, Task, Dlg, and property-editor code,
C++ and Python alike) must stop recomputing on the GUI thread. This is the
caller side of the existing `Migrate transactions and property edits to the
execution lane` work item; the inventory provides the exact call-site list to
drive it.

### 4. Migrate `updateData` providers in bounded batches (`Part`, `Fem`, …)
The `update-data-provider` findings (qualified definitions, inline
`void updateData(const App::Property*) override` declarations, and provider-aware
Python `ViewProvider.updateData` callbacks) are the concrete
provider list for the existing `Extract immutable Part render-buffer
preparation` / `Migrate Part view providers to bounded presentation adapters`
work items. Batch by workbench: `Part` (largest), then `Fem`, `Mesh`,
`TechDraw`, `Surface`.

### 5. Convert live-reference callbacks to queued value events
The `live-reference-callback` findings (native-C++ `signal*Object`/`slot*Object`
handlers plus Python `addObserver`/`removeObserver` subscriptions) are the
complement of the existing `Convert Python observers to queued value events`
work item. Prioritize the `Gui` framework observers (DocumentObserver, Tree,
PropertyView, DocumentModel) before workbench dialogs.

### 6. Triage `live-app-dereference` into management vs presentation reads
The `live-app-dereference` sites are pervasive and mostly legitimate
document-management reach (`getActiveDocument()` to run a command, or
`FreeCAD.ActiveDocument` transaction plumbing). Split them: (a) document/command
management — mark `accepted`; (b) presentation reads that should read committed
state — migrate per the `Migrate tree and property inspection to committed
presentation cache` and `Migrate selection picking and viewer roots to
committed presentation cache` work items.

### 7. Replace blocking process/future waits with async completion (`Gui`, `Mesh`, `MeshPart`, `Part`, `CAM`)
The `thread-waits` findings are `QProcess`/`QFuture`/`QThread`/`QWaitCondition`
waits on the GUI thread (GraphvizView, Assistant, NetworkRetriever, CrossSections,
RemeshGmsh, SensorManager, SplashScreen, plus the CAM `self.SIM.wait()` subprocess
waits). Move each to a `finished`-signal-driven continuation.

### 8. Triage `processEvents` flushes
The `process-events-polling` findings fall into two classes: one-shot
deferred-delete/activation flushes (likely `accepted`) and long-duration
progress loops (migrate). Triage each and record the disposition.

### 9. Broaden scanner coverage (tooling follow-up)
The module-aware Python GUI scope is now explicit and deterministic: all
production top-level `InitGui.py` entry points plus reviewed GUI packages/files
for Draft, BIM, CAM, FEM, and Robot. Remaining Python workbench helpers that
are not proven GUI surfaces stay out of scope until separately reviewed. The
`src/App/TranslationQtBridge.cpp` `BlockingQueuedConnection` use remains a
separate App-side follow-up.

## Non-goals and limitations

* The scanner covers `src/Gui`, `src/Mod/*/Gui`, production workbench
  `InitGui.py` files, and the reviewed module-aware Python GUI paths listed in
  the README and `rules.py`. Direct local InitGui imports are either included
  or named in the explicit reviewed-exclusion manifest. Unreviewed App/test
  helpers and the `src/App` model core remain out of scope.
* Dispositions are category-level defaults; per-site reclassification belongs
  to the triage candidates above.
