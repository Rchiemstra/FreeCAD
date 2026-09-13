# Chair MCP current-branch fixes

This change keeps inspection data serializable, reports the Sketcher solver's
actual fields, and places GUI document focus outside the native mutation
boundary.

```mermaid
flowchart LR
  Client -->|spreadsheet_get_cells| Reader
  Reader -->|Quantity object| JSON[JSON encoder]
  Client -->|execute_code activate_document| Prepared[prepared compatibility commit]
  Client -->|get_sketch_diagnostics| Sketch[SketchObject]
  JSON --> Failure[serialization failure]
  Prepared --> Lifecycle[document lifecycle rejection]
  Sketch --> Gap[DOF state omitted]
```

```mermaid
flowchart LR
  Client --> Reader[Spreadsheet reader]
  Reader --> Value[primitive or unit-preserving string]
  Value --> JSON[JSON response]
  Client --> Activate[GUI task activates target]
  Activate --> Commit[prepared compatibility commit]
  Commit --> Restore[GUI task restores original document]
  Client --> Diagnostics[Sketch diagnostics]
  Diagnostics --> Solver[DoF and FullyConstrained when exposed]
```

```mermaid
flowchart LR
  Declared[affected_documents with distinct names] -. old path .-> Native[one native compatibility commit]
  Native -. late refusal or partial scope .-> ScopeGap[declared scope cannot be committed]
  Declared --> Validate[pre-dispatch distinct-document validation]
  Validate -->|one document, duplicates allowed| Commit[one native compatibility commit]
  Validate -->|multiple documents| Ordered[dependency-ordered per-document calls]
  Ordered --> Commit
```

```mermaid
flowchart LR
  Source[Saved source document] --> Cached[Dependent expression cache]
  Cached --> Restore[Dependent afterRestore]
  Restore --> OldGate{old restore gate}
  OldGate -->|external target lacks restore flag| Skip[skip evaluation]
  Skip --> Stale[stale cached dimensions]
  Restore --> Gate{corrected shared predicate}
  Gate -->|ordinary local expression| Local[retain existing behavior]
  Gate -->|cross-document dependency| Evaluate[evaluate expression]
  Evaluate --> Touch[touch expression owner]
  Touch --> Recompute[normal recompute updates geometry]
```

```mermaid
flowchart LR
  SourceProp[Source Parameters.A1] -->|fine-grained backlink| Consumer[Dependent ExpressionEngine]
  Consumer -->|external expression dependency| SourceProp
  Consumer -. old teardown leaves raw edge .-> Freed[freed dependent pointer]
  Consumer --> Teardown[document clear/restore expression teardown]
  Teardown --> Unregister[remove source property backlink]
  Unregister --> Destroy[destroy dependent safely]
```

```mermaid
flowchart LR
  Restore[Dependent document restore] --> Parse[restore expression paths]
  Parse --> Guard{owner restoring?}
  Guard -. old hasSetValue exits .-> Missing[no fine backlink registration]
  Missing --> Later[future source edit cannot touch dependent]
  Parse --> Elements[restore-safe element-reference pass]
  Elements --> Rebuild[rebuild object/property, label, and hidden dependencies]
  Rebuild --> Backlink[register source property backlink]
  Backlink --> LaterUpdate[later source edit reaches dependent]
```

```mermaid
flowchart LR
  Source[Source recompute] --> Settle[settle source feature]
  Settle -. old cross-document enforce inside prepared target .-> Guard[mutation-target guard rejects dependent touch]
  Guard --> Stale[dependent remains at restored value]
  Settle --> Queue[coalesce cross-document dependent property touches]
  Queue --> Commit[commit source and publish revisions]
  Commit --> Release[release prepared mutation target and barrier]
  Release --> Touch[validate live dependent then touch once before observer replay]
  Touch --> Pending[dependent pending recompute]
  Pending --> Recompute[dependent recompute updates geometry]
```

```mermaid
flowchart LR
  PNG[PNG IDAT scanlines] --> Filter[filter byte + residual bytes]
  Filter -. residual variance .-> FalseBlank[false near-blank result]
  Filter --> Decode[unfilter None/Sub/Up/Average/Paeth]
  Decode --> Variance[pixel variance]
  Variance -->|near uniform| Fallback[active-view saveImage fallback]
  Variance -->|content| Return[return captured image]
```

```mermaid
flowchart LR
  Server[HTTP peer] --> Chunk[partial chunked response]
  Chunk --> Read[bounded response read]
  Timer[shared response deadline] --> Abort[mark expired and abort socket]
  Abort --> Read
  Abort -. old incomplete-read mapping .-> Protocol
  Read -->|incomplete before deadline| Protocol[protocol mismatch]
  Read -->|incomplete after deadline| Timeout[TimeoutError]
```

```mermaid
flowchart LR
  Client[deadline-aborted screenshot client] --> Listener[JSON-RPC or identity listener handler]
  Listener -. old stdlib wfile send .-> Pipe[SIGPIPE terminates embedded Python process]
  Listener --> Writer[shared handler-local response writer]
  Writer -->|Linux MSG_NOSIGNAL| Socket[accepted socket]
  Writer -->|macOS SO_NOSIGPIPE when available| Socket
  Socket --> Error[broken peer reported to handler]
  Error --> Alive[listener remains available for control request]
```

The reader retains FreeCAD quantity units by returning their string form. The
diagnostic response omits unavailable solver fields instead of manufacturing a
zero value. PNG blank detection reconstructs 8-bit non-interlaced grayscale,
truecolor, grayscale-alpha, and RGBA scanlines before measuring variance.

On restore, ordinary local expressions retain existing restore behavior. An
expression whose resolved dependency belongs to another document is evaluated,
so a changed source cannot leave dependent geometry at its serialized value.
Closing that dependent also unregisters its fine-grained property backlink from
the still-open source before destruction, so a later source edit cannot visit a
freed expression owner. Restoring an expression now rebuilds the same property
backlinks after its restore-safe element-reference pass, so later source edits
continue to propagate after a clear/restore cycle.

A malformed chunked response remains a protocol error when the peer closes it
immediately. When the transport's own deadline aborts a stalled partial read,
the response instead reports a timeout, preserving the caller's recovery
classification.

The listeners keep the host process's SIGPIPE disposition intact. Their shared
response writer applies the platform socket suppression only to listener
headers, error responses, and bodies, so an abandoned screenshot response
becomes a normal request failure and the listener can continue serving control
traffic.

Each live `execute_code` mutation has one native compatibility commit, so its
scope is limited to one distinct document. Callers must update cross-document
dependencies in dependency order. Existing-object dynamic-property restrictions
remain intentional; create a compatible object with the required schema rather
than modifying an arbitrary existing object's property schema.

## CHAIR-07 assembly link replay touch

GDB v9 proved the defect window is `Gui::LinkView::onLinkedUpdateData` at
`ViewProviderLink.cpp:1543`, not `LinkBaseExtension::extensionExecute`. During
deferred collaboration notification replay, the GUI link observer must not call
`_LinkTouched.touch()` after eager recompute has already settled the links.

```mermaid
sequenceDiagram
 participant Commit as Compatibility commit
 participant Rec as Eager recompute
 participant Barrier as finishCollaborationCommitNotificationBarrier
 participant Replay as Deferred ObjectChanged
 participant Slot as Gui::Document::slotChangedObject
 participant LV as LinkView::onLinkedUpdateData
 participant Link as App::Link _LinkTouched
 Commit->>Rec: execute graph; links settle
 Rec->>Barrier: committed=true; barrier already down
 Barrier->>Replay: collaborationReplayingNotifications=true
 Replay->>Slot: signalChangedObject(linked body, non-Output)
 Slot->>LV: ViewProvider updateData
 Note over LV,Link: Old: Property::touch after execute leaves Touch/Enforce
 Note over LV,Link: New: skip App-dirtying touch while replaying; signal-only tree notify
```

Composer patch applied in worktree
`C:\Users\Rchie\Music\FreeCADModeling\FreeCAD-wt-chair07`; GUI gtests added in
`tests/src/Gui/CollaborationLinkReplayGuard.cpp`. Luna v12 gtests passed
(replay 3/3, compatibility 18/18, domain 34/35 with the pre-existing FBO skip).
Luna run17 passed the producer gate (`live_ready=true`, links Up-to-date, no
extra recompute). Sol verified that run; fixture `verdict: "reproduced"` is
the ~25.5s latency classifier, not the producer defect.
