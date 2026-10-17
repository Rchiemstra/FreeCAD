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
`evaluation_error`, `dependency_cycle`, `stale_value`, and `dependency`.
Multiple roots may yield multiple findings for a downstream property. One shortest
chain per root is retained; cycles have an actual closed witness chain.

Choose **Tools → Expression diagnostics…** in any workbench. The modeless table is
read-only; selecting a row selects its object. Close and invoke the command again
for a fresh report. Rows continue to show the captured report if the document
changes; selecting a row for a deleted object/document does nothing.

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

`DocumentObject.getExpressionReferences(text)` is the only added C++ API. Python
cannot otherwise access the expression parser's identifiers, resolved property
subpaths, hidden-reference flags, or resolution errors. The binding only parses
and resolves a temporary expression; it neither evaluates nor installs it. All
classification, graph traversal, and presentation live in new Python files.
There are no changes to recompute/execution code.

Merge exposure (upstream commits since 2026-04-01, measured with
`git log --since=2026-04-01 --oneline origin/main -- <path> | wc -l`):

| Existing file | Commits | Change |
| --- | ---: | --- |
| `src/App/CMakeLists.txt` | 12 | Register one new binding source |
| `src/App/DocumentObject.pyi` | 7 | Declare read-only parser binding |
| `src/Gui/FreeCADGuiInit.py` | 4 | Import global command before initial workbench activation |
| `src/Ext/freecad/CMakeLists.txt` | 1 | Install two new Python files |
| `src/Mod/Test/CMakeLists.txt` | 19 | Register two new test files |
| `src/Mod/Test/Init.py` | 8 | Register application tests |
| `src/Mod/Test/InitGui.py` | 8 | Register GUI tests |

Follow-up: expose the same plain-data API in `tools/mcp/freecad-mcp` separately.
Spreadsheet cell-level presentation is outside this change.
