# Nonblocking Document Execution and GUI Presentation

**Implementation plan for FreeCAD**

*This plan replaces FreeCAD's GUI-driven recompute path with one stable execution thread per document and an immutable, versioned presentation cache owned by the GUI. The change prevents document work from becoming a dependency of repainting, navigation, or non-model controls.*

***While the document-owner thread is busy or deliberately stalled, the GUI must continue repainting, navigating the last committed presentation, and handling non-model controls. A GUI-thread synchronous document call is not admitted onto a busy or stalled lane: it waits at most GuiSyncAdmissionTimeoutMs (default 500 ms) and then raises DocumentWouldBlock. A call that has already been admitted still waits until that work finishes, servicing only marshalled tasks.***

*The contract applies to every production model-to-GUI path. A provider or Python extension that cannot meet it must fail explicitly as unsupported and must never fall back to synchronous GUI execution.*

## Interfaces and Contracts

- **DocumentHandle** is the only thread-safe GUI-facing document reference. Commands contain stable document and object identities, expected revisions, and copied values. They never carry live Document, DocumentObject, or Property pointers.
- **DocumentHandle trySubmit** accepts a DocumentCommand without waiting and returns Accepted, Busy, Closed, Conflict, or Unsupported.
  - Only one model command is admitted per document.
  - Edits, undo, redo, save, and close return Busy immediately while work is active.
  - An identical recompute request may share the current handle.
- **DocumentCommandHandle and RecomputeHandle** expose immutable operation state, progress, results, errors, and cooperative cancellation. Status checks cannot advance work or acquire a model lock.
- Synchronous document APIs on the GUI thread (`recompute`, `undo`/`redo`, `save`/`saveAs`, `touch`, `closeDocument`) stay as a compatibility layer for macros and third-party add-ons.
  **Admission is bounded**: if the lane is busy with other work, wait at most `GuiSyncAdmissionTimeoutMs` (default **500 ms**, `User parameter:BaseApp/Preferences/Document`), then raise `DocumentWouldBlock` naming the active command kind and its diagnostic. If `isWatchdogStalled()` is true, raise immediately without waiting.
  **Once admitted**, wait until completion: service only `MainThreadSignalConfig::serviceMarshalledTasks()` while waiting—do not pump the Qt event loop. Do not time out work that has been admitted.
  FreeCAD's own GUI (commands, task panels, property editor, AutoSaver, and the pinned MCP addon) must not call those sync APIs on the GUI thread; they use `trySubmit`, `DocumentCommandHandle`, or the `*Async` methods.
  `commitCompatibilityMutation()` stays fail-fast on the GUI thread. Callers use the real `commitCompatibilityMutationAsync()`.
- **PresentationRevision** identifies the document instance, lifecycle epoch, sequence, and source model revision.
- **PresentationDelta** contains pointer-free tree descriptors, display property values, status and error data, selection mappings, and immutable render buffers.
- **DocumentPresentationState** reports committed, pending, preparing, applying, stalled, or error.
- **Presentation providers** capture copied inputs on the document thread, prepare render payloads on bounded workers, and apply those payloads incrementally on the GUI thread. A provider without this contract returns Unsupported. Synchronous updateData calls with live Property pointers are not a fallback.
- Python features and view providers declare supportsDocumentThreadExecution and supportsAsyncPresentation capabilities. Undeclared or unsafe implementations fail before execution.

## Implementation Changes

### Document Execution and Command Admission

- Create one serial DocumentExecutionLane for every open document. The lane constructs, owns, executes, and destroys the live App Document.
- Move recompute progression, transactions, edits, undo, redo, imports, save, and close onto the lane. The GUI timer may refresh passive status but cannot execute or commit model work.
- Separate document dispatch from main-thread dispatch. Remove blocking queued connections, thread waits, model-lock acquisition, and live-reference callbacks from production GUI paths.
- Publish status as atomically replaceable immutable snapshots. Cancellation uses an independent atomic stop request and remains callable even if the document thread is stalled.
- Mark an operation Stalled after five seconds without progress. The watchdog exposes diagnostics but does not terminate the thread or claim recovery.
- Normal close completes only when the lane is idle or returns Busy. Shutdown requests cooperative cancellation while keeping the GUI active. If a lane remains unresponsive, offer keep waiting or whole-process termination; never terminate the thread inside the process.
- Use revision-bound immutable snapshots for cross-document reads. Reserve documents for a multi-document command in stable document-ID order without making one document thread wait while holding another. Undeclared live cross-document dependencies return Unsupported.

### Committed GUI Presentation

- Capture one immutable presentation revision at a stable operation boundary. Do not expose intermediate feature states during recompute.
- Add a GUI-owned DocumentPresentationCache for the last committed tree and property values, stable selection mappings, current Coin scene roots, and pending, progress, stalled, and error indicators.
- While model work is active, tree inspection, property inspection, selection, picking, and navigation read only the cache. Model-dependent actions return a visible Busy reason.
- Deliver presentation packets with queued, pointer-free notifications. Reject stale packets by document instance, epoch, sequence, and source revision, including after close and reopen or document-name reuse.
- After a partial model failure, publish only a stable terminal revision with failure annotations. If presentation preparation or application fails, keep the previous complete presentation and show the error.

### Bounded Coin3D Updates

- Add a PresentationApplyScheduler with an approximate four-millisecond CPU budget per GUI event-loop turn.
- Coalesce superseded presentation packets and retain at most the committed revision and the newest staging revision.
- Move Part tessellation and equivalent Mesh and Points preparation into immutable buffers containing vertices, normals, topology, materials, and subelement mappings.
- Add an immutable Coin mesh node or equivalent buffer-backed representation. Incrementally upload GPU data and install scene content. Use coarse geometry or bounding-box level of detail when a full update cannot fit the latency budget.
- Build a new scene below a detached staging root. Keep the old root navigable until tree, property, selection, and scene data are ready, then activate the complete revision with one constant-time root and cache swap.
- Inventory every production updateData provider. Before removing the legacy path, give each provider a pointer-free adapter or classify it as explicitly unsupported.

### Python and GIL Isolation

- Centralize GUI-side Python entry in GuiPythonGate. The gate checks document and GIL availability before acquisition, then queues an allowed non-model callback or rejects it immediately.
- The native GUI event loop never waits to acquire the GIL while document Python is running.
- Accepted native extensions release the GIL around long operations that do not access Python objects.
- Convert Python observers to queued value events without live model references.
- Provide async Python APIs for recompute, edits, undo, redo, save, and close. Legacy synchronous calls on the GUI thread fail fast.
- Remove behavior that moves an undeclared Python feature or view provider back to the GUI thread.

### Cutover

1. Land telemetry, responsiveness tests, and architectural checks before changing execution behavior.
2. Move document execution and every GUI model ingress to DocumentExecutionLane.
3. Introduce the committed presentation packet and migrate the tree, property editor, selection, and viewer to DocumentPresentationCache.
4. Migrate Part, Mesh, Points, and all remaining production view providers to bounded pointer-free presentation adapters.
5. Enforce Python and GIL isolation, then migrate cross-document and lifecycle operations.
6. Use the legacy path only for development comparison. Complete cutover when every ingress and provider is classified and automated checks find no direct GUI access to live model state.

## Verification and Acceptance

| **Test area** | **Required result** |
| --- | --- |
| **Document stall** | **A deterministic operation stalls the actual owner thread for 30 seconds while the GUI remains interactive.** |
| **GUI latency** | **Event-loop and paint latency remain at or below 50 ms at p99, with no measured delay above 100 ms.** |
| **Presentation update** | **GUI application is divided into approximately 4 ms slices and the previous complete scene remains usable until activation.** |
| **Revision integrity** | **Tree, property, selection, and scene data switch together; mixed revisions are never visible.** |
| **Busy commands** | **Edit, undo, redo, save, and close return Busy immediately while model work is active.** |
| **Python and GIL** | **Unsafe Python is rejected before execution and native GUI activity never waits for the GIL.** |

- Add a deterministic operation that stalls the actual document-owner thread for 30 seconds.
- During the stall, automate rotate, pan, zoom, repaint, window resize, tree expansion and scrolling, cached property inspection, and non-model controls.
- Confirm that edit, undo, redo, save, and close attempts return Busy without waiting, while passive status and cooperative cancellation remain responsive.
- Measure native event-loop and paint latency for the whole stall. Require p99 latency no greater than 50 ms and no measured delay greater than 100 ms.
- Repeat latency measurement during a large presentation build and incremental application. Record each apply slice and verify the approximate four-millisecond budget.
- Verify that the committed revision stays visible and consistent until one atomic activation of the new revision. Tree, properties, selection, and scene must never show mixed revisions.
- Cover stale, duplicate, and out-of-order packets; deletion; close and reopen; name reuse; partial recompute failure; presentation failure; cancellation; and lifetime or use-after-free cases.
- Add GIL tests proving that unsafe Python is rejected before execution, native GUI activity remains within the latency limit while document Python holds the GIL, and GUI Python callbacks are deferred or rejected without blocking.
- Add provider-contract tests for Part, Mesh, Points, and every inventoried production view provider.
- Add static checks that reject GUI paths containing blocking queued connections, document-thread waits or joins, active recompute polling, owner-held model locks, live-reference event payloads, or direct App object dereferences.
- Run the responsiveness suite with native windows on Windows and Linux, then run the existing App, Gui, Part, architecture, and full regression suites.
- Expose production telemetry for GUI event-loop latency, apply-slice duration, presentation revision lag, scene preparation time, Busy rejections, document progress, and watchdog state.

## Assumptions and Defaults

- Coin3D remains the renderer. The plan changes scene preparation and installation, not the viewer technology.
- Busy model edits are rejected rather than queued. An identical recompute may join the active operation.
- The latency thresholds apply to defined reference scenes and supported hardware. Driver-level stalls and pathological rendering are reported separately, but model execution and presentation publication may not make the GUI wait.
- Cooperative cancellation is best effort. A watchdog is diagnostic and never forcibly terminates a document thread.
- Every production model-to-GUI path must use the async execution and committed-presentation boundary. Unsupported extensions fail explicitly; there is no emergency synchronous fallback.

## References

[Qt Threads and QObjects](https://doc.qt.io/qt-6/threads-qobject.html) - Queued delivery, thread affinity, and blocking queued connections.

[Qt QThread](https://doc.qt.io/qt-6/qthread.html) - Worker-object guidance, waits, interruption, and termination risks.

[Python Thread State and the Global Interpreter Lock](https://docs.python.org/3/c-api/threads.html) - GIL release, acquisition, and native thread-state requirements.
