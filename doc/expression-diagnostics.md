# Expression diagnostics

```python
from freecad.expression_diagnostics import diagnose
findings = diagnose(App.ActiveDocument)
```

The `freecad` Python namespace is the upstream home for reusable, workbench-independent application helpers.

Each finding is JSON-compatible plain data: `object` (internal `Document#Object` name),
`property` (including subpath), `expression`, `kind`, `message`, `role` (`root` or
`affected`) and `chain` (ordered `{object, property}` steps from cause to consequence).
Kinds are `unresolved_reference`, `external_document`, `unit_error`,
`evaluation_error`, `dependency_cycle`, `stale_value`, `not_evaluated`, and `dependency`.
Multiple roots may yield multiple findings for a downstream property. One shortest
chain per root is retained; cycles have an actual closed witness chain.

Choose **Tools → Expression diagnostics…** in any workbench. The modeless table is
read-only and refreshes after document changes. Selecting a row selects its object
and exact property in the Data tab. Capture and selection wait for busy documents,
including external sources. Closing the report’s document closes the report.

The property editor's **Data** tab uses the same diagnostics to show broken
expressions in orange, with the expression path, formula and reason in the
property/value tooltip. Compound properties also indicate failures in their
children, and merged rows indicate failures in any selected object. Good expressions retain the existing palette link colour; stale values
and expressions that could not safely be evaluated are not labelled broken.
Bindings that still evaluate successfully keep their normal colour even when
the Tools report identifies exposure to a failed input.
`User parameter:BaseApp/Preferences/PropertyView/ExpressionErrorColor` is an
unsigned packed RGBA colour (default `0xD35400FF`). Parameter changes apply live.
Checkbox value labels use the same error colour. Diagnostic refreshes preserve
uncommitted text in active value editors.

A GUI-thread timer coalesces document changes and recompute completion into a
cached report, including reference creation, label changes, property removal,
object deletion, undo/redo and document closure. Observer callbacks only queue
invalidation, including when recompute runs on a worker. On this fork, observation
waits for the existing live-presentation reservation; upstream's synchronous
execution needs no additional admission. Diagnostic colour/tooltip lookups only read cached plain data:
they neither invoke Python nor evaluate expressions. The Tools table refreshes
its captured rows through the same observation admission. Sketcher edit-mode
constraint colours, solver messages and rejected-datum explanations are described
below.

Diagnosis evaluates each expression against currently stored input values and
compares its result with the stored destination. It never calls recompute, sets
properties, clears touched flags, opens external documents, or creates transactions.
A stale value means those values differ; a touched object alone is not an error.
A dependent stale value is reported as affected instead of an independent root.
Independent evaluation errors remain roots even if they also depend on a failure.
References to outputs without expression bindings propagate through an explicit
object step, since a failing expression can prevent that object from executing.
Structural links report affected objects, with empty property and expression
strings, without claiming a particular generated output has already failed.
Roots in open external documents are included when they explain findings in the
requested document. Hidden expression references carry impact but are excluded
from cycle classification. Fine-grained input references do not require execution
of their owner; non-input and structural scheduling cycles are diagnosed in both
recompute modes. Scheduler delays without a failing binding on the consumer are
shown as object-only consequences. Invoke on a quiescent document, consistent with existing Python
read APIs; no event loop is pumped during observation.
Detectable Python attribute paths and subobject expressions are conservatively reported as `not_evaluated` with a reason (subobject references carry `unresolved_metadata`), but native getters, including uncached expression serialization, can still invoke callbacks that cannot be detected statically.

`DocumentObject.getExpressionReferences(text)` is the only added C++ API. Python
cannot otherwise access the expression parser's identifiers, resolved property
subpaths, hidden-reference flags, or resolution errors. The binding only parses
and resolves a temporary expression; it neither evaluates nor installs it. All
classification and graph traversal live in Python. The global report is Python;
Sketcher uses a cached C++ presentation with small hooks in its existing GUI files.
There are no changes to recompute/execution code.

Merge exposure (upstream commits since 2026-04-01, measured with
`git log --since=2026-04-01 --oneline origin/main -- <path> | wc -l`):

| Existing file | Commits | Change |
| --- | ---: | --- |
| `src/App/CMakeLists.txt` | 12 | Register one new binding source |
| `src/App/DocumentObject.pyi` | 7 | Declare read-only parser binding |
| `src/Gui/propertyeditor/PropertyItem.cpp` | 8 | Small cached colour/tooltip hook |
| `src/Gui/propertyeditor/PropertyItemDelegate.cpp` | 2 | Use cached error colour for checkbox labels |
| `src/Gui/CMakeLists.txt` | 38 | Register presentation helper header |
| `src/Gui/FreeCADGuiInit.py` | 4 | Import global command before initial workbench activation |
| `src/Ext/freecad/CMakeLists.txt` | 1 | Install diagnostics and presentation Python files |
| `src/Mod/Test/CMakeLists.txt` | 19 | Register two new test files |
| `src/Mod/Test/Init.py` | 8 | Register application tests |
| `src/Mod/Test/InitGui.py` | 8 | Register GUI tests |

Follow-up: expose the same plain-data API in `tools/mcp/freecad-mcp` separately.
Spreadsheet cell-level presentation is outside this change.


## Sketcher edit mode (#26496)

Broken constraint expressions use a separate colour in the 3D view and the
constraint list. The list tooltip shows the expression and its failure reason,
including upstream causes. Set the colour in Sketcher appearance preferences,
or with `BaseApp/Preferences/View/BrokenExpressionConstrDimColor`
(an RGBA unsigned parameter, default `0xD020D0FF`). Selection and preselection
still use the normal highlight colours.

The Sketch Edit solver panel shows a separate count of constraints with broken
expressions, alongside the normal solver status. Click the count to select
those constraints. Multiple upstream causes count a constraint once. Repairing
an expression or undoing a deleted source clears the indicators on solver update.
Failed datum edits include the names/numbers of the affected constraints and
their expression causes in the existing notification system.

`freecad.sketcher_expression_diagnostics.constraint_errors(sketch)` adapts
`diagnose()` to zero-based constraint indices. It does not recompute or change
the sketch. Only actual expression failures and their downstream consequences
are marked; pending stale values and expressions which cannot safely be evaluated
are not classified as broken. An independent stored VarSet input is not marked
solely because another binding on that parameter container failed. Generated
feature-output dependencies retain the diagnostics API's conservative impact. C++ caches the result with solver information;
painting, list roles, tooltips and selection do not evaluate expressions.

The clipboard regression copies only the sketch (excluding its VarSet) with
`Std_Copy` and pastes into a second document with `Std_Paste`. It checks missing
variables, constraint and solver indicators, and the rejected diameter's count
and notification cause. Datum edits commit only after expression validation, so
an unsuccessful new dimension rolls back. Repair through the Data-tab expression
editor clears the indicators after recompute and re-entering edit mode.

The Tools report reserves discovery and all open documents before following
references. Busy documents defer capture and selection; queued document changes
refresh the report, and closing its document closes the report. Selecting a row
selects its canonical property in the Data tab, including nested paths. Sketcher
list rows snapshot pairs, legacy display roles and expression icons during
admitted population; paint and tooltip requests use these snapshots.

Presentation text is shared by the report, Data tab and Sketcher in
`freecad.expression_diagnostics_text`. Translation extraction includes
`src/Ext/freecad` in the FreeCAD catalogue. The GUI regression loads the actual
French catalogue and checks translated headers, classifications and reasons.
Native exception details, names, paths and formulas remain technical data.

Regression coverage is registered in `TestExpressionDiagnosticsGui`,
`SketcherTests.TestSketchExpressionDiagnostics`, and
`SketcherTests.TestSketchExpressionDiagnosticsGui`, including the aggregate
Sketcher application/GUI suites.
