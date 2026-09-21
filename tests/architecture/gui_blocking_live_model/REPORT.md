# AB-21 report — GUI blocking and live-model ingress

This report summarizes the reproducible inventory (617 findings across seven
categories, snapshot in `inventory.json`) and proposes bounded follow-up task
candidates. It proposes work only; no production behavior is changed here.

## Inventory at a glance

| Category | Findings | Disposition | Summary |
| --- | ---: | --- | --- |
| `blocking-invokes` | 1 | migrate | Single `Qt::BlockingQueuedConnection` dispatch hook. |
| `direct-recompute` | 64 | migrate | Synchronous `recompute()` from GUI commands/tasks/dialogs. |
| `live-reference-callback` | 182 | migrate | State-change signal/slot callbacks carrying live references. |
| `update-data-provider` | 91 | migrate | `ViewProvider::updateData` overrides. |
| `live-app-dereference` | 231 | investigate | `App::GetApplication().getActiveDocument()/getDocument()` reach. |
| `process-events-polling` | 35 | investigate | Manual event-loop pumping. |
| `thread-waits` | 13 | investigate | Blocking `waitForFinished()` on processes/futures. |

Owning subsystems: `Gui` (277), `Part` (72), `Mesh` (56), `Fem` (38),
`PartDesign` (36), `TechDraw` (24), `Surface` (21), `Measure` (17), `Sketcher`
(13), `Material` (12), and smaller workbenches.

## Bounded follow-up task candidates

Each candidate is a bounded unit of work on top of this inventory; none is
started here.

### 1. Replace the Mesh evaluation busy-poll loop (`Mesh`)
`src/Mod/Mesh/Gui/DlgEvaluateMeshImp.cpp` contains 14 `processEvents` polls and
9 `recompute()` calls inside a single long-running evaluation loop — the
clearest GUI-thread blocking cluster in the inventory. Replace the poll loop
with a queued/asynchronous progress mechanism driven by the event loop.

### 2. Convert the remaining blocking invoke hook (`Gui`)
`src/Gui/Application.cpp` installs the one `Qt::BlockingQueuedConnection`
dispatch (`blocking ? Qt::BlockingQueuedConnection : Qt::QueuedConnection`).
Migrate its synchronous worker-to-GUI callers to queued, pointer-free dispatch
and then remove the blocking branch. Pairs with the existing
`Guard GUI waits joins and live-reference event payloads` work item.

### 3. Route GUI recompute onto the execution lane (framework + workbenches)
The 64 `direct-recompute` sites (Command, Task, Dlg, and property-editor code)
must stop recomputing on the GUI thread. This is the caller side of the
existing `Migrate transactions and property edits to the execution lane`
work item; the inventory provides the exact call-site list to drive it.

### 4. Migrate `updateData` providers in bounded batches (`Part`, `Mesh`, …)
The 91 `update-data-provider` findings are the concrete provider list for the
existing `Extract immutable Part render-buffer preparation` /
`Migrate Part view providers to bounded presentation adapters` work items.
Batch by workbench: `Part` (largest), then `Fem`, `Mesh`, `TechDraw`, `Surface`.

### 5. Convert live-reference callbacks to queued value events
The 182 `live-reference-callback` findings (subscriptions + `slot*Object`
handlers) are the native-C++ complement of the existing
`Convert Python observers to queued value events` work item. Prioritize the
`Gui` framework observers (DocumentObserver, Tree, PropertyView, DocumentModel)
before workbench dialogs.

### 6. Triage `live-app-dereference` into management vs presentation reads
The 231 `live-app-dereference` sites are pervasive and mostly legitimate
document-management reach (`getActiveDocument()` to run a command). Split them:
(a) document/command management — mark `accepted`; (b) presentation reads that
should read committed state — migrate per the
`Migrate tree and property inspection to committed presentation cache` and
`Migrate selection picking and viewer roots to committed presentation cache`
work items.

### 7. Replace blocking process/future waits with async completion (`Gui`, `Part`, `MeshPart`)
The 13 `thread-waits` findings are `QProcess`/`QFuture::waitForFinished()` on
the GUI thread (GraphvizView, Assistant, NetworkRetriever, CrossSections,
RemeshGmsh). Move each to a `finished`-signal-driven continuation.

### 8. Triage `processEvents` flushes
The 35 `process-events-polling` findings fall into two classes: one-shot
deferred-delete/activation flushes (likely `accepted`) and long-duration
progress loops (migrate). Triage each and record the disposition.

### 9. Broaden scanner coverage (tooling follow-up)
Extend this inventory tooling to: multi-line `App::GetApplication().getXxx()`
receiver chains (one known site), Python workbench GUI code, and the
`src/App/TranslationQtBridge.cpp` `BlockingQueuedConnection` use (App-side
blocking reach into the GUI thread).

## Non-goals and limitations

* The scanner is line-oriented; a single multi-line `App::GetApplication()`
  chain is a documented gap (see README).
* Python GUI and `src/App` are out of scope for this pass (see README).
* Dispositions are category-level defaults; per-site reclassification belongs
  to the triage candidates above.
