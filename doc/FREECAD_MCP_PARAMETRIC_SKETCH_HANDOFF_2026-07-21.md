# Handoff — Parametric sketch automation gaps (FreeCAD vs freecad-mcp)

**Date:** 2026-07-21  
**Audience:** agent or human improving freecad-mcp / FreeCAD for live parametric PartDesign via MCP  
**Context chat:** AutoCurtains V6 — why case/cover stayed as baked `Part::Feature` booleans instead of Spreadsheet→Sketch→Pad  
**Related (different topic):** `doc/FREECAD_MCP_COVERAGE_PRIORITY_HANDOFF_2026-07-21.md` (test coverage from MCP workload)  
**Related (CAD cleanup):** `docs/V61_FACE_CLEANUP_HANDOFF_2026-07-21.md`

---

## 1. One-line verdict

Sketches are **already automatable** via MCP (`sketch_*`, `pad_feature`, `pocket_feature`, `get_sketch_diagnostics`). The bottleneck is **parametric control + robust PartDesign automation** — not “MCP cannot sketch.”

Without spreadsheet + expression APIs, a sketch pad remains a **one-shot solid**, not a live parametric model.

---

## 2. What already works in freecad-mcp

Installed addon (user machine):  
`%APPDATA%\FreeCAD\v26-3\Mod\FreeCADMCP\`

Upstream / editable tree (typical):  
`C:\Users\Rchie\Music\FreeCADModeling\FreeCAD\tools\mcp\freecad-mcp\`

### Sketch / PartDesign surface (present)

| Capability | Tools / entry points |
| --- | --- |
| Create sketch (optionally in Body) | `sketch_create` |
| Geometry | `sketch_add_geometry`, `sketch_add_line/circle/arc/rectangle/…`, polyline, fillet, trim, extend, split, symmetry, construction toggle |
| Constraints | `sketch_add_constraint`, `sketch_constrain_*` (coincident, distance, radius, equal, H/V, parallel, perpendicular, tangent) |
| External geometry | `sketch_add_external_projection` |
| Diagnostics | `get_sketch_diagnostics`, `get_sketch_geometry` |
| Features | `pad_feature`, `pocket_feature` (+ patterns/mirror/revolve/loft/sweep exist separately) |
| Recompute feedback | `recompute_document`, `get_recompute_log` |

Agents can build a closed, constrained sketch and pad it. That is **geometry automation**, not **parameter-driven design**.

### What is missing (gap list)

| Priority | MCP addition | Why |
| ---: | --- | --- |
| P0 | **Spreadsheet tools** — create sheet; set/get cell; set/get **alias**; list aliases | Parametric source of truth |
| P0 | **Expression tools** — `set_expression` / `clear_expression` / `list_expressions` on object properties and `sketch.Constraints[i]` | Bind dims to aliases (`<<Sheet>>.Wall`) |
| P1 | **Body workflow helpers** — create Body; set Tip / BaseFeature; move feature in Group; attach sketch to face with stable API | Agents currently invent fragile `execute_code` |
| P1 | **Named / aliased constraint edit** — edit by constraint name or alias, not shifting geo index after trim/fillet | Geo indices are the #1 agent footgun |
| P2 | **Stronger recompute feedback** — extend `get_recompute_log` with “which expression / which object broke” and constraint DOF summary after bind | Faster debug loops |
| P2 | **Stable attach helpers** — attach by geometric predicate (plane normal + COM) or binder, not raw `FaceN` | Survives Face ID churn better |

**Without P0:** agents hardcode mm in constraints → every design change is rewrite geometry, same cost as boolean scripts.

---

## 3. What FreeCAD itself still makes hard

These remain even after perfect MCP wrappers:

| Issue | Effect on agents |
| --- | --- |
| **Topological naming (TNP)** | Face/edge IDs move after edits → sketch attachments and external geometry break |
| **Sketcher solver** | Under/over-constrained sketches fail mid-script; DOF hard to recover automatically |
| **PartDesign tip / history** | Long feature trees fragile; tip wrong → pad/pocket attaches wrong |
| **Performance** | Full Body recompute heavier than one `Part::Feature` boolean — bad for path-sweep experiments |

MCP cannot fully paper over TNP or solver fragility; it can only expose clearer diagnostics and safer attach patterns.

---

## 4. Layered change map

| Layer | Change |
| --- | --- |
| **freecad-mcp** | Add spreadsheet + expression APIs; Body/attach helpers; stable constraint addressing; clearer expression/recompute errors |
| **FreeCAD** | Better TNP + more reliable sketch solve/recompute for scripted PartDesign |
| **AutoCurtains V6** | `V6_Case_Print` / `V6_Cover_Print` are already baked solids — new MCP sketch tools do **not** make them spreadsheet-driven; that requires a rebuild as Body/Sketch history |

**Practical rule for this project today:** keep boolean scripts for case/cover path experiments; use sketch+pad for new small parametric parts (axles, spacers, fasteners) once P0 tools exist.

---

## 5. Suggested implementation plan (freecad-mcp)

Work in upstream: `FreeCAD/tools/mcp/freecad-mcp/` then sync addon to Mod.

### Phase A — Spreadsheet (P0)

Proposed tools (names illustrative):

- `spreadsheet_create(doc, name)`
- `spreadsheet_set_cells(doc, sheet, cells: [{address|alias, value, …}])`
- `spreadsheet_get_cells` / `spreadsheet_list_aliases`
- Optional: `spreadsheet_set_alias(doc, sheet, address, alias)`

Implement via FreeCAD `Spreadsheet::Sheet` APIs (`set()`, `setAlias()`, `getContents()`, etc.) on GUI thread like other RPC methods.

### Phase B — Expressions (P0)

Proposed tools:

- `set_expression(doc, object, prop_path, expr)` → `obj.setExpression(prop, expr)`
- `clear_expression(doc, object, prop_path)`
- `list_expressions(doc, object)` → dump `ExpressionEngine` / known bound props
- Support paths like `Constraints[3]` on sketches and `Length` / `Length2` on Pad/Pocket

Acceptance: change `<<Dims>>.PadH` → pad height updates after `recompute_document` without rewriting sketch geometry.

### Phase C — Body + attach (P1)

Proposed tools:

- `body_create(doc, name)`
- `body_set_tip(doc, body, feature)`
- `sketch_attach(doc, sketch, support: {object, subname} | plane_predicate)`
- Document recommended pattern: Body → Sketch on XY_Plane → Pad → Pocket…

### Phase D — Constraint identity (P1)

- Allow `sketch_constrain_distance(..., name="WallThick")` and later `sketch_edit_constraint(name=…, value=…)`
- Or map Spreadsheet aliases ↔ constraint names in one helper
- After trim/fillet, prefer **name/alias** over geo index in agent-facing docs

### Phase E — Diagnostics (P2)

- Enrich `get_recompute_log` / add `diagnose_parametric(doc, object?)`:
  - invalid objects
  - expression parse errors
  - sketch DOF / conflicting constraints (reuse `get_sketch_diagnostics`)

### Tests

Add e2e under `freecad-mcp/tests/`:

1. Sheet alias → circle radius constraint → pad → change alias → volume changes.
2. Attach sketch to Body XY_Plane → pad → pocket.
3. Intentional bad expression → structured error (not silent Invalid).

---

## 6. Acceptance criteria (when “sketch automation is practical”)

An agent can, **without ad-hoc `execute_code` for params**:

1. Create Spreadsheet with aliases (`Wall`, `Bore`, `Depth`).
2. Create Body + Sketch; constrain with expressions bound to aliases.
3. Pad/Pocket with lengths bound to aliases.
4. Change two aliases; recompute; geometry updates; diagnostics clear on success.
5. Edit a named constraint after a trim without guessing new geo indices.

Until then: boolean/`Part::Feature` workflows remain the faster path for large experimental solids (as in V6 case/cover).

---

## 7. Out of scope for this handoff

- Rewriting V6 case/cover into PartDesign history (separate CAD project).
- FreeCAD TNP core fixes (upstream FreeCAD; track separately).
- Coverage ranking of FreeCAD C++ from MCP logs (see coverage handoff).

---

## 8. Quick reference — problem statement to paste

> MCP can drive sketches today. What’s missing for parametric CAD via MCP is Spreadsheet + Expression tools, Body/attach helpers, stable (named) constraint addressing, and clearer recompute/expression failure feedback. FreeCAD still adds TNP, solver, and PartDesign-history fragility. V6 print bodies stay baked until rebuilt as Body/Sketch trees.
