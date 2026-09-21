# GUI blocking and live-model ingress inventory (AB-21)

This directory holds a **reproducible, machine-readable inventory** of
production GUI sites that may block the GUI thread or access live model state.
It is investigation tooling for the `Nonblocking document execution and GUI
presentation` epic (AB-4) / `Publish immutable committed presentation state`
feature (AB-8). It documents and classifies sites; it never changes runtime
behavior.

## Files

| File | Purpose |
| --- | --- |
| `rules.py` | The seven inventory categories, their search rules (regex, per language), scope, and default migration dispositions. |
| `scanner.py` | The reproducible scanner. Walks the GUI source (C++ and Python), masks comments/literals per language, applies the rules, and emits sorted findings. Runnable standalone. |
| `inventory.json` | The committed snapshot: `scanner --write` output (scanner results minus exclusions). |
| `exclusions.json` | Narrow, justified false-positive exclusions. |
| `../test_gui_blocking_live_model_inventory.py` | The validation test (registered as a CTest). |

## Scope

The scanner covers production GUI source only:

* `src/Gui` — the GUI framework, and
* every `Gui` directory under `src/Mod` — workbench GUI code, including the
  nested Python GUI directories such as `src/Mod/CAM/Path/*/Gui`.

Both C++ (`.cpp`/`.h`/`.hpp`) and Python (`.py`) GUI source are scanned.

Excluded from scope:

* `src/App` and other model-layer code (this inventory is about *GUI* blocking
  and *live-model ingress from GUI* code);
* Python workbenches whose GUI code is *not* organised under a `Gui` directory
  (`AddonManager`, `BIM`, `Draft`, `Help`, `OpenSCAD`, `Plot`, `Show`, `Tux`,
  `Web`, and the import/export helpers), where App and GUI Python share the
  top-level package: the directory-based scanner cannot separate their GUI
  Python from their App Python, so they need a module-aware scanner and are
  tracked as a bounded follow-up in the report;
* `src/Mod/Test/Gui` (unit-test workbench, not production GUI);
* `src/Tools/_TEMPLATE_` (template scaffolding);
* the GUI test harness `src/Gui/CommandTest.cpp` and test support module
  `src/Gui/FreeCADGuiTest.py` (test infrastructure, not a shipped GUI surface).

## Categories and search rules

Every rule is a pair of deliberately narrow regular expressions — one applied
to C++ source and one to Python source — run *after* comments and string/char/
raw literals are masked (so a commented-out `processEvents()` or a string
containing `getDocument(` is never a finding). Each finding records:
repository-relative `path`, 1-based `line`, `category`, owning `subsystem` (the
`Gui` framework or the owning workbench), migration `disposition`, and the
matched source line(s) as `evidence`.

| Category | Search rule (C++ regex) | Default disposition |
| --- | --- | --- |
| `thread-waits` | `\b(?:waitForFinished\|waitForDone\|waitForStarted\|waitForBytesWritten)[^\S\n]*\( \| QThread::wait[^\S\n]*\( \| pthread_join[^\S\n]*\( \| (?:->\|\.)wait[^\S\n]*\(` | `investigate` |
| `blocking-invokes` | `Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection` | `migrate` |
| `process-events-polling` | `\bprocessEvents[^\S\n]*\(` | `investigate` |
| `direct-recompute` | `(?:\.\|->)recompute[^\S\n]*\(` | `migrate` |
| `live-app-dereference` | `App::GetApplication\s*\(\s*\)\s*\.\s*(?:getActiveDocument\|getDocuments\|getDocumentOrActive\|getDocumentByPath\|getDocument)[^\S\n]*\(` | `investigate` |
| `live-reference-callback` | `\b(?:signal\|slot)(?:Changed\|Change\|Touched\|Deleted\|Delete\|BeforeChange\|Recomputed)(?:View)?Object\b` | `migrate` |
| `update-data-provider` | `\bupdateData[^\S\n]*\([^\S\n]*const[^\S\n]+App::Property` | `migrate` |

The Python patterns mirror the C++ ones where a Python equivalent exists:
`thread-waits` (`.waitFor*` and `.wait(`), `blocking-invokes`
(`BlockingQueuedConnection`), `process-events-polling` (`processEvents(` and
the `FreeCADGui`/`Gui.updateGui()` wrapper), `direct-recompute` (`.recompute(`),
`live-app-dereference` (`App`/`FreeCAD.ActiveDocument` and `getDocument(`), and
`live-reference-callback` (`addObserver`/`removeObserver`). Two categories are
C++-only by nature — `blocking-invokes` (the Qt `BlockingQueuedConnection`
connection type) and `update-data-provider` (the
`ViewProvider::updateData(const App::Property*)` interface) — and carry a `None`
Python pattern to record that explicitly.

Notes on deliberately narrow rules (these are what keep the false-positive
surface small):

* **thread-waits** excludes `QStringList::join`/`QString::join` (string joins)
  and `std::thread::detach`. It captures blocking waits on helper processes and
  futures (`waitForFinished`), `QThreadPool::waitForDone`, `waitForStarted`,
  `waitForBytesWritten`, `QThread::wait`, `pthread_join`, and the instance
  member wait (`thread->wait()` and `QWaitCondition().wait(...)`). The one
  worker-side self-wait (the `SignalThread` background thread blocking on its
  own condition) is excluded individually in `exclusions.json` because it does
  not block the GUI thread.
* **blocking-invokes** keys on the `Qt::BlockingQueuedConnection` connection
  type, the only portable way FreeCAD requests a blocking cross-thread dispatch.
  Synchronous *widget* event forwarding (`QApplication::sendEvent` to a widget)
  is intentionally out of scope: it is ordinary Qt widget plumbing, not a
  cross-thread blocking invoke.
* **direct-recompute** keys on the `.recompute(` / `->recompute(` member call
  from GUI code; the model-layer `Document::recompute` definition lives in
  `src/App` and is out of scope.
* **live-app-dereference** keys on the `App::GetApplication()` document/object
  accessors (`getActiveDocument`, `getDocument`, `getDocuments`,
  `getDocumentOrActive`, `getDocumentByPath`), including the multi-line
  receiver-chain form (the regex spans newlines). Broader live-handle patterns
  such as `Gui::Document::getDocument()` are tracked separately (see the
  report).
* **live-reference-callback** keys on the document-object state-change
  signals/slots (`Changed/Change/Touched/Deleted/Delete/BeforeChange/Recomputed`,
  plus the `ChangedView/DeletedView` view variants). Creation/rename/activate
  lifecycle signals (`New`, `Relabel`, `Activated`, `Created`) and tree
  navigation signals (`Highlight`, `Expand`, `Scroll`) are out of scope. Pure
  `fastsignals::signal<...>` member declarations are excluded in
  `exclusions.json`.
* **update-data-provider** anchors on the concrete signature
  `updateData(const App::Property* ...)` in both its qualified definition form
  (`ViewProvider::updateData(const App::Property* prop)`) and its inline
  declaration/definition form (`void updateData(const App::Property*) override`),
  so base-class delegation calls (`X::updateData(prop)`) and the unrelated
  `QAbstractItemModel`-style `PropertyItem::updateData()` are not matched.

## Reproducing the inventory

From the repository root:

```bash
FREECAD_SOURCE_ROOT="$PWD" \
  python3 tests/architecture/gui_blocking_live_model/scanner.py --write
```

`--write` regenerates `inventory.json` in place (scanner results minus the
committed exclusions). Without `--write` it prints the JSON to stdout. The
command is deterministic: identical source and rules always produce identical
output, and two fresh scans are byte-identical.

## Validation

```bash
FREECAD_SOURCE_ROOT="$PWD" \
  python3 tests/architecture/test_gui_blocking_live_model_inventory.py
```

The validation test (also registered as CTest `GuiBlockingLiveModelInventory`)
rejects:

* **duplicate** findings (same `path:line:category`);
* **malformed** findings (missing/ill-typed fields, unknown category or
  disposition, `..` path components, unclean paths);
* **nonexistent** findings (path missing, line out of range, or evidence that
  no longer matches the source line);
* **wrong subsystem** (a finding's `subsystem` must equal
  `scanner.subsystem_for(path)`);
* **malformed or stale** exclusions (an exclusion the scanner no longer
  produces, or one with a `..` path component);
* **drift** — the committed `inventory.json` must exactly equal the complete
  ordered generated payload (every finding dict, the `scope`, the `categories`,
  and the `excluded_count`), so any new blocking/live-model site in the GUI
  source fails the test until it is inventoried.

The test also carries focused rule regressions (thread/process/condition waits,
multiline and `getDocuments` live-model dereferences, inline `updateData`
overrides, and Python GUI ingress) and mutation tests (subsystem, scope,
excluded count, ordering, and full-payload drift).

## Dispositions

`migrate`, `investigate`, `accepted`. Defaults are assigned per category (see
the table). `investigate` marks mechanisms whose per-site use needs triage
(for example a shutdown-time process wait versus a busy progress loop). The
report refines these into bounded follow-up tasks.
