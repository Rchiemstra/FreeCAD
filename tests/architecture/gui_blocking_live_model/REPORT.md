# AB-21 report — GUI blocking and live-model ingress

This report summarizes the reproducible inventory (1053 findings across seven
categories, snapshot in `inventory.json`) and proposes bounded follow-up task
candidates. It proposes work only; no production behavior is changed here.

## Inventory at a glance

| Category | Findings | Disposition | Summary |
| --- | ---: | --- | --- |
| `live-app-dereference` | 476 | investigate | `App::GetApplication().getActiveDocument()/getDocument()/getDocuments()` and `FreeCAD.ActiveDocument` reach. |
| `live-reference-callback` | 194 | migrate | State-change signal/slot callbacks and observer subscriptions carrying live references. |
| `update-data-provider` | 187 | migrate | `ViewProvider::updateData` overrides (qualified and inline). |
| `direct-recompute` | 133 | migrate | Synchronous `recompute()` from GUI commands/tasks/dialogs. |
| `process-events-polling` | 38 | investigate | Manual event-loop pumping. |
| `thread-waits` | 24 | investigate | Blocking `waitFor*()`/`wait()` on processes, futures, conditions, and threads. |
| `blocking-invokes` | 1 | migrate | Single `Qt::BlockingQueuedConnection` dispatch hook. |

The snapshot spans 751 C++ and 302 Python findings. Owning subsystems: `Gui`
(324), `CAM` (311), `Part` (87), `Mesh` (62), `Fem` (61), `PartDesign` (46),
`TechDraw` (41), `Surface` (21), `Measure` (18), `Sketcher` (15), `Assembly`
(12), `Material` (12), and smaller workbenches (20 subsystems in total).

## Bounded follow-up task candidates

Each candidate is a bounded unit of work on top of this inventory; none is
started here.

### 1. Replace the Mesh evaluation busy-poll loop (`Mesh`)
`src/Mod/Mesh/Gui/DlgEvaluateMeshImp.cpp` contains 15 `processEvents` polls and
9 `recompute()` calls inside a single long-running evaluation loop — the
clearest GUI-thread blocking cluster in the inventory. Replace the poll loop
with a queued/asynchronous progress mechanism driven by the event loop.

### 2. Convert the remaining blocking invoke hook (`Gui`)
`src/Gui/Application.cpp:468` installs the one `Qt::BlockingQueuedConnection`
dispatch (`blocking ? Qt::BlockingQueuedConnection : Qt::QueuedConnection`).
Migrate its synchronous worker-to-GUI callers to queued, pointer-free dispatch
and then remove the blocking branch. Pairs with the existing
`Guard GUI waits joins and live-reference event payloads` work item.

### 3. Route GUI recompute onto the execution lane (framework + workbenches)
The 133 `direct-recompute` sites (Command, Task, Dlg, and property-editor code,
C++ and Python alike) must stop recomputing on the GUI thread. This is the
caller side of the existing `Migrate transactions and property edits to the
execution lane` work item; the inventory provides the exact call-site list to
drive it.

### 4. Migrate `updateData` providers in bounded batches (`Part`, `Fem`, …)
The 187 `update-data-provider` findings (qualified definitions plus the inline
`void updateData(const App::Property*) override` declarations) are the concrete
provider list for the existing `Extract immutable Part render-buffer
preparation` / `Migrate Part view providers to bounded presentation adapters`
work items. Batch by workbench: `Part` (largest), then `Fem`, `Mesh`,
`TechDraw`, `Surface`.

### 5. Convert live-reference callbacks to queued value events
The 194 `live-reference-callback` findings (native-C++ `signal*Object`/`slot*Object`
handlers plus Python `addObserver`/`removeObserver` subscriptions) are the
complement of the existing `Convert Python observers to queued value events`
work item. Prioritize the `Gui` framework observers (DocumentObserver, Tree,
PropertyView, DocumentModel) before workbench dialogs.

### 6. Triage `live-app-dereference` into management vs presentation reads
The 476 `live-app-dereference` sites are pervasive and mostly legitimate
document-management reach (`getActiveDocument()` to run a command, or
`FreeCAD.ActiveDocument` transaction plumbing). Split them: (a) document/command
management — mark `accepted`; (b) presentation reads that should read committed
state — migrate per the `Migrate tree and property inspection to committed
presentation cache` and `Migrate selection picking and viewer roots to
committed presentation cache` work items.

### 7. Replace blocking process/future waits with async completion (`Gui`, `Mesh`, `MeshPart`, `Part`, `CAM`)
The 24 `thread-waits` findings are `QProcess`/`QFuture`/`QThread`/`QWaitCondition`
waits on the GUI thread (GraphvizView, Assistant, NetworkRetriever, CrossSections,
RemeshGmsh, SensorManager, SplashScreen, plus the CAM `self.SIM.wait()` subprocess
waits). Move each to a `finished`-signal-driven continuation.

### 8. Triage `processEvents` flushes
The 38 `process-events-polling` findings fall into two classes: one-shot
deferred-delete/activation flushes (likely `accepted`) and long-duration
progress loops (migrate). Triage each and record the disposition.

### 9. Broaden scanner coverage (tooling follow-up)
Extend this inventory tooling to: (a) the Python workbenches whose GUI code is
not under a `Gui` directory (`Draft`, `BIM`, `OpenSCAD`, `AddonManager`, `Web`,
`Help`, `Plot`, `Show`, `Tux`), which need a module-aware scanner to separate
App from GUI Python; and (b) the `src/App/TranslationQtBridge.cpp`
`BlockingQueuedConnection` use (App-side blocking reach into the GUI thread).

## Non-goals and limitations

* The scanner is directory-scoped to `src/Gui` and `src/Mod/*/Gui` (see the
  README). Python workbenches without a `Gui` directory and the `src/App`
  model core are out of scope for this pass and tracked by candidate 9.
* Dispositions are category-level defaults; per-site reclassification belongs
  to the triage candidates above.
