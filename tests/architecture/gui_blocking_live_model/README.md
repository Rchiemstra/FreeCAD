# GUI blocking and live-model ingress inventory (AB-21)

This directory holds a **reproducible, machine-readable inventory** of
production GUI C++ sites that may block the GUI thread or access live model
state. It is investigation tooling for the `Nonblocking document execution and
GUI presentation` epic (AB-4) / `Publish immutable committed presentation
state` feature (AB-8). It documents and classifies sites; it never changes
runtime behavior.

## Files

| File | Purpose |
| --- | --- |
| `rules.py` | The seven inventory categories, their search rules (regex), scope, and default migration dispositions. |
| `scanner.py` | The reproducible scanner. Walks the GUI C++ source, masks comments/literals, applies the rules, and emits sorted findings. Runnable standalone. |
| `inventory.json` | The committed snapshot: `scanner --write` output (scanner results minus exclusions). |
| `exclusions.json` | Narrow, justified false-positive exclusions. |
| `../test_gui_blocking_live_model_inventory.py` | The validation test (registered as a CTest). |

## Scope

The scanner covers production GUI C++ source only:

* `src/Gui` — the GUI framework, and
* `src/Mod/<Workbench>/Gui` — workbench GUI code.

Excluded from scope:

* `src/App` and other model-layer code (this inventory is about *GUI* blocking
  and *live-model ingress from GUI* code);
* Python workbench GUI code (needs a Python-aware scanner; tracked by the
  `Isolate Python and the GIL` and `Convert Python observers to queued value
  events` work items);
* `src/Mod/Test/Gui` (unit-test workbench, not production GUI);
* `src/Tools/_TEMPLATE_` (template scaffolding).

## Categories and search rules

Every rule is a single regular expression applied to C++ source *after*
comments and string/char/raw literals are masked (so a commented-out
`processEvents()` or a string containing `getDocument(` is never a finding).
Each finding records: repository-relative `path`, 1-based `line`, `category`,
owning `subsystem` (the `Gui` framework or the owning workbench), migration
`disposition`, and the matched source line as `evidence`.

| Category | Search rule (regex) | Default disposition |
| --- | --- | --- |
| `thread-waits` | `\bwaitForFinished[^\S\n]*\( \| QThread::wait[^\S\n]*\( \| pthread_join[^\S\n]*\(` | `investigate` |
| `blocking-invokes` | `Qt[^\S\n]*::[^\S\n]*BlockingQueuedConnection` | `migrate` |
| `process-events-polling` | `\bprocessEvents[^\S\n]*\(` | `investigate` |
| `direct-recompute` | `(?:\.\|->)recompute[^\S\n]*\(` | `migrate` |
| `live-app-dereference` | `App::GetApplication[^\S\n]*\([^\S\n]*\)[^\S\n]*\.[^\S\n]*(?:getActiveDocument\|getDocument\|getDocumentObject\|getDocumentGroup\|getDocumentByTag)[^\S\n]*\(` | `investigate` |
| `live-reference-callback` | `\b(?:signal\|slot)(?:Changed\|Change\|Touched\|Deleted\|Delete\|BeforeChange\|Recomputed)(?:View)?Object\b` | `migrate` |
| `update-data-provider` | `::updateData[^\S\n]*\([^\S\n]*const[^\S\n]+App::Property` | `migrate` |

Notes on deliberately narrow rules (these are what keep the false-positive
surface small):

* **thread-waits** excludes `QStringList::join`/`QString::join` (string joins)
  and `std::thread::detach`. It captures real blocking waits on helper
  processes and futures (`waitForFinished`), `QThread::wait`, and `pthread_join`.
* **blocking-invokes** keys on the `Qt::BlockingQueuedConnection` connection
  type, the only portable way FreeCAD requests a blocking cross-thread dispatch.
  Synchronous *widget* event forwarding (`QApplication::sendEvent` to a widget)
  is intentionally out of scope: it is ordinary Qt widget plumbing, not a
  cross-thread blocking invoke.
* **direct-recompute** keys on the `.recompute(` / `->recompute(` member call
  from GUI code; the model-layer `Document::recompute` definition lives in
  `src/App` and is out of scope.
* **live-app-dereference** keys on the `App::GetApplication()` document/object
  accessors (global live-model reach). Broader live-handle patterns such as
  `Gui::Document::getDocument()` are tracked separately (see the report).
  The scanner is line-oriented; a multi-line receiver chain that splits
  `App::GetApplication()` from its accessor across a newline is a documented
  limitation (see the report).
* **live-reference-callback** keys on the document-object state-change
  signals/slots (`Changed/Change/Touched/Deleted/Delete/BeforeChange/Recomputed`,
  plus the `ChangedView/DeletedView` view variants). Creation/rename/activate
  lifecycle signals (`New`, `Relabel`, `Activated`, `Created`) and tree
  navigation signals (`Highlight`, `Expand`, `Scroll`) are out of scope.
  Pure `fastsignals::signal<...>` member declarations are excluded in
  `exclusions.json`.
* **update-data-provider** anchors on the concrete override signature
  `::updateData(const App::Property* ...)`, so base-class delegation calls
  (`X::updateData(prop)`) and the unrelated
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
output.

## Validation

```bash
FREECAD_SOURCE_ROOT="$PWD" \
  python3 tests/architecture/test_gui_blocking_live_model_inventory.py
```

The validation test (also registered as CTest `GuiBlockingLiveModelInventory`)
rejects:

* **duplicate** findings (same `path:line:category`);
* **malformed** findings (missing/ill-typed fields, unknown category or
  disposition);
* **nonexistent** findings (path missing, line out of range, or evidence that
  no longer matches the source line);
* **malformed or stale** exclusions (an exclusion the scanner no longer
  produces);
* **drift** — the committed `inventory.json` must exactly equal the current
  scanner output minus exclusions, so any new blocking/live-model site in the
  GUI source fails the test until it is inventoried.

## Dispositions

`migrate`, `investigate`, `accepted`. Defaults are assigned per category (see
the table). `investigate` marks mechanisms whose per-site use needs triage
(for example a shutdown-time process wait versus a busy progress loop). The
report refines these into bounded follow-up tasks.
