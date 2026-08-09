# Feedback — FreeCAD MCP server & FreeCAD API

Written after a long inspection/planning session (source `AutoCurtains.FCStd`, target
`autocurtains_spool_rail_case.FCStd`): reverse-engineering `SpoolCaseBody`, mapping its upstream
dependencies, and running two feasibility spikes. Everything below is something that actually
happened in that session, not a general wishlist.

---

## 1. MCP server — friction, ranked by cost

### 1.1 `execute_code` returns a full viewport screenshot on **every** call — biggest single problem
`capture_view` defaults to `false`, yet **every** `execute_code` call came back with a
~1600×620 PNG of the 3D view (~25 k tokens each). I made ~10 such calls; the images alone consumed
a large majority of the session's context and repeatedly triggered
`[OUTPUT TRUNCATED - exceeded 25000 token limit]`, which then hid the *text* I actually needed.

**Impact:** I had to restructure nearly every query into "compute → `json.dump` to a scratch file →
`Read` the file", i.e. two round-trips instead of one, purely to dodge the image. That is a lot of
wasted tokens and wall-clock for pure read-only inspection.

**Suggested fixes (in order):**
1. Honour `capture_view: false` — return **no image** unless explicitly asked.
2. If an image must be returned, downscale hard (e.g. ≤512 px) or return it only when the code
   mutates the document.
3. Cheap win: an `execute_code(..., return_text_only=true)` escape hatch.

### 1.2 No pagination / filtering on large read tools
`get_document_tree("AutoCurtains")` returned **63 766 characters** and was refused outright
("exceeds maximum allowed tokens"). There's no `depth`/`root_filter` combination that reliably
bounds the size, and no cursor. For `autocurtains_spool_rail_case` the tree was dominated by
~100 auto-generated `Origin*/X_Axis*/XY_Plane*` nodes that are pure noise.

**Suggested fixes:** a `limit`/`offset` or cursor; an `exclude_origins: true` flag (or exclude
`App::Origin`/`App::Line`/`App::Plane` by default); and let `include` actually *shrink* the payload.

### 1.3 25 k-token output cap truncates the useful part
When output is truncated, the truncation lands wherever it lands — several times the text result
was cut while the (useless) screenshot survived. Truncating **after** appending a large image is
the worst possible ordering.

**Suggested fix:** truncate images first, text last.

### 1.4 Server disconnected mid-session
The `freecad` server dropped all 108 tools partway through Check 2, right before I could re-verify
one sub-step. It reconnected later, but the spike had to be closed out with one item unverified. No
data loss (I'd already written results to disk), but worth noting that long sessions are exposed to
this — another argument for cheap, resumable, file-backed inspection.

### 1.5 Newly-appeared tools would have saved most of the session
After the reconnect, these appeared and are *exactly* what this task needed:
`get_dependency_graph`, `inspect_geometry`, `match_subshape`, `audit_hardcoded_dimensions`,
`create_placement_binder`, `create_placement_datum`, `run_transaction`, `validate_movement_follow`.

I hand-rolled all of the following in `execute_code` because I didn't know these existed:
- recursive `OutList` walk to detect dependency cycles → **`get_dependency_graph`**
- enumerating planar faces and scoring them by normal/area/boundary-circles to remap
  `HornBody.Pocket003.Face3` onto the target horn → **`match_subshape`**
- face/edge property dumps (type, centre, normal, area, bbox, radii) → **`inspect_geometry`**

**Suggested fix:** surface these in the tool list up front, and cross-reference them in the
descriptions of the generic tools (`execute_code`'s description should say "prefer
`inspect_geometry`/`match_subshape`/`get_dependency_graph` for read-only queries").

### 1.6 What worked really well
- **`get_sketch_geometry`** — text-only, complete (geometry + construction flags + constraints +
  external refs, local *and* global coords). This one tool did more useful work per token than
  anything else. **More tools should look like this.**
- `measure_volume` / `bounding_box` / `center_of_mass` — clean, cheap, no image.
- `execute_code` itself is genuinely powerful; the problem is purely the image payload.

---

## 2. FreeCAD / API gotchas discovered (worth keeping)

These cost real time and are the kind of thing that silently produces wrong answers.

| # | Gotcha | Detail |
|---|---|---|
| 1 | **Body `Shape` can be a `Part.Compound`** | `body.Shape.CenterOfMass` raises `AttributeError: 'Part.Compound' object has no attribute 'CenterOfMass'`. Use `CenterOfGravity`, or iterate `.Solids`. Bit me on the first mass-properties call. |
| 2 | **`AllowCompound`** | A PartDesign Body defaults to single-solid. `SpoolCaseBody.AllowCompound = True` is what lets `Pad013` produce **4 disconnected posts** without erroring; later pads fuse them into 1 solid. Easy to miss, fatal to a rebuild. |
| 3 | **Sketch external geometry is NOT in `Sketch.Geometry`** | `addExternal()` succeeds (`len(ExternalGeometry)==1`) but the projected edge does **not** appear in `sk.Geometry`. Reading it needs `sk.getGeometry(i)` with **negative** indices (external starts at `-3`). This produced a **false "FAIL"** in my App::Link spike until I spotted it. |
| 4 | **`MapMode = "Deactivated"` + a populated `AttachmentSupport`** | Looks attached, is actually a baked placement. `RailDatumPlane` had support `SpoolBody.SpoolPad.Face2` and offset `(0,6,0)` but `MapMode=Deactivated` — i.e. the link is decorative. Silent trap; always check `MapMode`, never just `AttachmentSupport`. |
| 5 | **Attachment sub-element names are feature-scoped** | `AttachmentSupport = [(HornBody, ['Pocket003.Face3'])]` — the subname is `<Feature>.<FaceN>`, not a body-level `FaceN`. Resolve with `obj.getSubObject('Pocket003.Face3')` (returns global-coord shape). |
| 6 | **Expressions bind to cell addresses, not aliases** | `Pad013.Length = CaseVars.B9`, `SpoolSketch.Constraints[3] = SpoolVars.B5 / 2`. So expressions reference **B9/B5**, not `casePadLength`/`spoolDiameter`. Inserting a spreadsheet row silently re-points every expression. Prefer alias-based expressions when authoring. |
| 7 | **`App::Link` *can* be attached — but not out of the box** | `App::Link` has no `MapMode` by default; `link.addExtension("Part::AttachExtensionPython")` then `AttachmentSupport`/`MapMode="FlatFace"`/`AttachmentOffset` **works** and the link's placement is driven by the attachment. Undocumented-feeling but solid. |
| 8 | **`SubShapeBinder` through an `App::Link` is stable** | Binding `(link, 'Edge2')` resolved correctly, tracked a parameter change (OD 22→23 ⇒ bound edge r11.0→11.5), followed the host face when the support moved, and survived save/close/reopen with no dependency cycle. Good news — this is the load-bearing assumption of the rebuild. |
| 9 | **Feature-level deps look circular but aren't** | The 4 bearings depend on `SpoolCaseBody.Pad013` (feature #4) while `SpoolCaseBody.Sketch024` (feature #10) depends on the bearings. `dependsOn_SpoolCaseBody` is `False` for all of them — the dependency is on *individual features*, so it's a legal **staged DAG**, not a cycle. Checking only at body granularity would have produced a false alarm; checking only naively would have produced a false "cycle". |
| 10 | **Independently rebuilt reference geometry ≠ same global frame** | The target `MG996RHornRef` is the "same" horn as the source `HornBody`, but sits **97 mm / 90° away** with an **inverted face normal** and slightly different dims (Ø20.5 vs Ø20). Any cross-document geometric comparison therefore needs an explicit **frame registration** step — a raw same-coordinate Boolean would be meaningless. |

---

## 3. Practical recommendations

**For the MCP server**
1. Kill the unconditional screenshot in `execute_code` (single highest-value fix).
2. Truncate images before text.
3. Add pagination + origin-filtering to `get_document_tree`.
4. Promote `inspect_geometry` / `match_subshape` / `get_dependency_graph` — they're the right
   abstraction level and were invisible to me for most of the session.

**For working in this repo**
- Prefer the text-only tools; treat `execute_code` as a last resort, and when using it, write
  results to a scratch JSON and `Read` it back.
- Before trusting any subtractive/attachment/expression relationship: check `MapMode`, check
  `AllowCompound`, check whether the expression binds to a **cell** or an **alias**, and verify
  volumes with a Boolean rather than eyeballing the viewport.
