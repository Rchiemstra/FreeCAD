# Active-Assembly Interference and Clearance Check

## Summary

Add an on-demand `Assembly_CheckInterference` command for the active assembly. It will recursively inspect visible physical leaf occurrences, detect penetration, contact, and insufficient clearance, and present transient warnings and 3D previews.

The clearance threshold will be saved per assembly; zero means contacts and penetrations are still violations. Exclusions will apply to source-definition pairs, so they affect every occurrence of those two sources. No automatic recompute-time checking, generic Part command, persistent result objects, or Python detection API will be added in v1.

The original plan was based solely on static repository inspection; the
historical review section below records that pre-completion context.

## Current implementation status — 2026-07-28

**Verdict: PASS for the planned on-demand feature and its mandatory focused
validation.**

The implementation now includes:

- selection-aware all-component and exact selected-pair scope with top-level
  occurrence identity, world placement, visibility, nested containers, rigid
  and flexible links, Bodies, and collapsed/expanded link arrays;
- fail-closed Part classification plus face-specific spreadsheet clearances,
  canonical face-path validation, deterministic governing-hit selection, and
  row/comment provenance;
- persistent unordered source exclusions with atomic insertion, undo/redo,
  same-document deletion and cross-document FCStd identity, opt-in detached
  link behavior, and shared affected-component-pair counting;
- generation-aware asynchronous scans with cooperative cancellation, accepted
  zero-row results, stale monitoring, worker and synchronous preparation
  exception conversion, obsolete-result rejection, and document-close cleanup;
- quantity-aware GUI controls, exception-safe GUI transactions, filtered result
  rows, and immutable worker-produced preview geometry in world coordinates.

Final isolated validation stamp `20260728T145157Z-12989` passed:
`InterferenceDetectionTest.*` **23/23**; `InterferenceScanTest.*` **107/107**
plus `AssemblyObjectTest.*` **1/1**; offscreen
`TaskInterferenceCheckTest.*` **45/45**; mandatory Xvfb/xcb lifecycle/preview
lane **8/8**. Markers were `BUILD_EXIT:0`,
`ASSEMBLYGUI_PLATFORM_GUARD_OK platform=xcb`, and
`ALL_MANDATORY_PASSED`. The offscreen negative xcb control was also rejected
as expected.

Deferred scope remains continuous/live checking, multi-assembly batch
workflows, broader indexing for very dense assemblies, and cancellation inside
an OCCT call already in progress. Invalid or tolerance-sensitive geometry
continues to fail closed as Invalid or Inconclusive rather than being reported
Clear.

## Interfaces and Detection

- Add an exported Part App pair-classification service alongside, without changing, the existing `Part::checkIntersection()` in [PartFeature.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Part/App/PartFeature.cpp:2236>).

  - `InterferenceKind`: `Clear`, `ClearanceViolation`, `Contact`, `Penetration`, `InvalidInput`, `Inconclusive`, `Cancelled`.
  - `InterferenceOptions`: nonnegative clearance and an internal linear tolerance defaulting to `Precision::Confusion()`.
  - `InterferenceResult`: kind, minimum distance, closest-point pair, common/contact shape, total overlap volume, and diagnostic text.
  - Preserve the existing Boolean helper’s ABI and behavior for external consumers.

- Use `BRepExtrema_DistShapeShape`, following the existing distance implementation in [TopoShapePyImp.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Part/App/TopoShapePyImp.cpp:2428>), followed by an exact Common operation when distance is within kernel tolerance:

  - Distance greater than clearance plus tolerance is clear.
  - Positive distance within the threshold is a clearance violation.
  - Near-zero distance triggers `FCBRepAlgoAPI_Common`.
  - A common result containing any `TopAbs_SOLID` is penetration; collect those solids and sum their volumes without imposing a minimum-volume cutoff.
  - A successful zero-distance Common without solids is contact.
  - Invalid geometry or failed OCCT operations produce explicit invalid/inconclusive results, never “clear.”
  - Use the non-destructive, internally parallel FreeCAD Boolean wrapper from [FCBRepAlgoAPI_BooleanOperation.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Part/App/FCBRepAlgoAPI_BooleanOperation.cpp:41>), but do not add its size-scaled auto-fuzzy value for classification; rely on stored shape tolerances so a small gap is not turned into penetration.

- Add an Assembly App scan service with snapshot types for physical instances, pair results, per-component problems, aggregate counts, cancellation, and progress.

  - Recursively descend `AssemblyObject`, `AssemblyLink`, `App::Link`, and container groups with cycle protection.
  - Treat a PartDesign Body as one final leaf using its Tip; treat a solid-bearing GeoFeature or linked equivalent as one leaf even if it contains multiple solids.
  - Do not also test a descended container, PartDesign history features, joints, origins, view groups, helpers, null shapes, or surface-only objects.
  - Preserve every full occurrence path so repeated links to the same source remain distinct.
  - Resolve world geometry through `Part::Feature` shape extraction with `ResolveLink | Transform`; do not combine raw child shapes and placements manually.
  - Evaluate visibility through the complete ancestor/element path. “Include hidden” bypasses this filter.
  - Do not reuse `AssemblyUtils::getAssemblyComponents()` unchanged: its rigid/flexible solver semantics differ from the selected recursive-leaf inspection scope in [AssemblyUtils.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Assembly/App/AssemblyUtils.cpp:747>).

- Validate each leaf once with `BRepCheck_Analyzer`. Report invalid leaves once rather than generating an invalid row for every possible pair.

- Precompute conservative world-space bounding boxes and use deterministic sweep-and-prune on the widest global axis. Enlarge each box by half of clearance plus tolerance, filter on the other axes, and run exact checks only for unique candidate pairs. Pair checks remain sequential because OCCT already parallelizes internally.

## Assembly Data and GUI

- Add two non-geometric, no-recompute properties to `AssemblyObject`:

  - `InterferenceClearance`: visible length-constrained property, minimum and default `0 mm`.
  - A hidden `PropertyXLinkSubList` containing alternating source-definition endpoints for excluded unordered pairs. Helper methods must enforce even pairing, canonical lookup, uniqueness, undo/redo, cross-document links, and same-source pairs.

- Source exclusions are intentionally definition-wide: excluding sources A and B suppresses every occurrence combination derived from A and B; excluding A with itself suppresses all distinct A-instance pairings. Unresolved or deleted source references remain reviewable as invalid exclusion rules rather than being silently discarded.

- Results remain transient. Exclusions and threshold changes use explicit document transactions, but running or browsing a scan must not dirty the document or trigger the assembly solver.

- Register `Assembly_CheckInterference` through [Commands.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Assembly/Gui/Commands.cpp:370>) and add it to the Assembly menu and contextual Tools group, not the primary toolbar. Enable it only for a valid active assembly; detailed eligibility is checked when the task opens.

- Add an Assembly task panel modeled on the result-model/lifecycle patterns in [TaskCheckGeometry.cpp](</C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Part/Gui/TaskCheckGeometry.cpp:399>), with:

  - Clearance input, Include hidden, Show excluded, Run, Cancel scan, Select pair, Manage exclusions, and Close controls.
  - Columns for status, both occurrence paths, minimum clearance, and overlap volume.
  - Text/icon summaries for penetrations, contacts, clearance violations, excluded violations, invalid inputs, and inconclusive pairs. Clear, warning, and incomplete states must not rely on color alone.
  - Exclusions applied after classification: current excluded violations remain available to the Show excluded filter. Manage exclusions also lists dormant rules with no current violation.
  - Explicitly named “Exclude source pair” and “Restore source pair” actions so their definition-wide effect is clear.
  - Row changes update only the preview; they do not alter FreeCAD selection. “Select pair” is the sole action that selects the two occurrences.

- Run the scan as a background `QtConcurrent` job:

  - Collect world-space shapes, paths, display strings, visibility, and source IDs before starting the worker.
  - The worker accesses only the immutable snapshot, performs pair checks sequentially, and reports progress through queued GUI updates.
  - Cancellation is cooperative between OCCT operations; cancelling immediately removes the preview and discards any eventual late result.
  - Any involved document recompute, placement/group/link change, document closure, or view closure marks the snapshot stale, cancels/discards its result, and requires a rerun.
  - Coin scene changes remain exclusively on the GUI thread.

- Use a task-owned, unpickable scene-graph separator and `PartGui::SoPreviewShape`:

  - Penetration: translucent red exact common solids.
  - Contact: orange common face/edge/vertex geometry or coincident closest-point markers.
  - Clearance violation: amber closest-point markers and connecting line.
  - Add low-opacity copies of the two selected component shapes for context without recoloring their source view providers.
  - Keep one row preview active and remove all nodes on row change, rerun, cancellation, task destruction, document closure, and view closure.

- Register the new App/GUI sources, task UI, translation strings, icon, QtConcurrent dependency, and Assembly resources in the corresponding CMake files and `Assembly.qrc`/`InitGui.py`.

## Test Plan

- Part App unit tests, extending the existing box fixtures in `tests/src/Mod/Part/App`:

  - Known-volume penetration, containment, coincident solids, multiple disconnected common solids, and compound/multisolid inputs.
  - Disjoint objects and bounding-box false positives.
  - Face-, edge-, and vertex-only contact.
  - Gaps just inside and outside the clearance threshold, almost-touching geometry, and barely positive penetration.
  - Null, surface-only, invalid, failed/inconclusive, and cancelled inputs.
  - Correct closest points, volume, and absence of a fixed minimum-volume cutoff.

- Assembly App tests:

  - Nested Parts, rigid and flexible AssemblyLinks, transformed links, Body Tip handling, and repeated instances sharing one source.
  - Correct world transforms, visibility inheritance, Include hidden behavior, deterministic unique pairs, and no parent/child or self-pair duplication.
  - Broad-phase candidate counts on many separated objects without brittle timing assertions.
  - Source-wide exclusions, same-source exclusions, FCStd save/restore, undo/redo, deleted/unresolved endpoints, and malformed stored-pair handling.
  - Partial results when one component or pair is invalid.

- Assembly GUI regression tests:

  - Command activation and background completion/cancellation.
  - Correct summary, rows, units, filters, and exclusion/restore actions.
  - Preview geometry for all violation categories and replacement rather than accumulation.
  - Preservation of user selection and source appearance.
  - Threshold persistence and intentional dirty state only for metadata changes.
  - Stale-result handling and complete Coin cleanup after rerun, cancellation, document/view closure, or task destruction.

## Risks and Deferred Work

- OCCT distance and Common operations remain scale- and tolerance-sensitive and can fail on complex B-reps; diagnostics and inconclusive states are required.
- Sweep-and-prune reduces typical work but dense assemblies remain quadratic in candidate pairs and may retain substantial common geometry.
- Cancellation cannot be guaranteed inside every individual OCCT distance call, although background execution keeps the UI responsive.
- Full occurrence paths, inherited visibility, and cross-document source identity are the highest-risk integration areas and require the dedicated tests above.
- Definition-wide exclusions can suppress many occurrences; the UI must show their scope and affected count before committing the transaction.
- Continuous checking during drag/recompute, multi-assembly or arbitrary-selection scope, General Fuse optimization, persistent result objects, and a public Python scan API are deferred beyond v1.

## Face-specific spreadsheet clearances (design clearances) — phase 2

Spreadsheet **Tolerance** values are **design clearances** (required gaps). OCCT `linearTolerance` / `Precision::Confusion()` remain internal numerical tolerances only and must not be conflated with spreadsheet rules.

Phase 1 whole-object selection, hidden selected-pair scanning, GearMesh exclusion, and Include-hidden semantics remain unchanged.

### Spreadsheet contract

- Optional host property `InterferenceClearanceSheet` (`App::PropertyLink` to `Spreadsheet::Sheet`, no-recompute).
- Columns (header names, order flexible): `Enabled`, `Face`, optional `FaceB`, `Tolerance`, `Comment`.
  - `Face` = occurrence-relative face path (TNP-normalized) or `*` for default.
  - `FaceB` empty → single-face rule; set → exact unordered face-pair override.
- Lookup for participating faces A,B:
  1. Enabled exact pair rule matching `{A,B}`.
  2. Else max of enabled individual rules for A and B (larger = stricter); preserve **all** contributing rows in `sourceRows`.
  3. Else enabled `*` default.
  4. Else assembly `InterferenceClearance`.
- Duplicate equal-precedence rules: apply the strictest clearance; keep every contributing row (deterministic, order-independent).
- Disabled rows ignored. Enabled but unresolved/malformed paths produce visible diagnostics (`invalidRules`).
- Rule table is snapshotted on the GUI/caller thread before the worker (immutable). Tolerance cells use FreeCAD length quantities / formula evaluation where available.

### Conservative broad phase

```
maxDesignClearance = max(
  enabled face-rule Tolerances,
  enabled exact face-pair Tolerances,
  * default when present,
  host InterferenceClearance when reachable as unmatched-face fallback
)
```

Host fallback is reachable when no enabled `*` default is present. AABB margin uses `0.5 * maxDesignClearance + linearTolerance`.

### Result model

One `InterferencePairResult` per component/leaf pair:

- `detection` — component-level solid classification (Penetration is never multiplied by face count).
- `faceHits` — vector of `InterferenceFaceHit` (paths, distance, applied clearance, classification, ruleKind, `sourceRows` / `sourceComments`, diagnostic), **including Clear** for App tests.

### Summary counts

| Field | Meaning |
|-------|---------|
| `penetrations` | Penetrating component/leaf pairs |
| `contacts` | Face interactions classified Contact |
| `clearanceViolations` | Face-pair clearance violations |
| `excludedViolations` | Excluded **component** pairs that would otherwise be reportable |
| `invalidRules` | Malformed / unresolved spreadsheet rules |
| `invalidInputs` | Invalid leaf/geometry inputs |
| `inconclusivePairs` | Unclassifiable component or face evaluations |
| `clearFaceHits` | Face evaluations classified Clear |

### Classification

- Per leaf pair: solid Common penetration → one Penetration pair result.
- Otherwise: enumerate proximate face×face pairs (AABB-pruned, cancellable, face geometry cached per leaf); classify each under its looked-up design clearance; store all hits under that pair.
- Default GUI shows non-Clear face hits (and penetrations); optional “Show clear face checks”.
- Spreadsheet link or cell changes mark existing scan results stale.

### Performance

- Conservative AABB prune; cancel during face enumeration; progress includes face-level work.
- Immutable per-leaf face cache in the worker.
- Oversized face-pair candidate sets: deterministic cap + visible diagnostic (never silent omit).

## Historical implementation review — 2026-07-24 (fresh follow-up)

**Historical verdict at that snapshot: FAIL.** The current status and final
validation are recorded above; this section is retained as an audit trail of
the defects that drove the subsequent fixes.

The implementation was reviewed read-only against this plan and `doc/PROGRESS.md`. No implementation file was changed, no commit was created, and no running MCP server, FreeCAD instance, or existing container was inspected, reused, stopped, restarted, or reconfigured. All executable validation ran only in new `--rm --network=none` Docker containers with no ports and a read-only host source mount.

### Confirmed fixes since the prior review

- Per-generation cancellation flags, the main assembly-deletion null guard, general row-action refresh, queued progress, Manage Exclusions, preview mesh/marker creation, result-unit formatting, path-only world-shape resolution, once-per-collected-leaf validation, nonnegative/finite clearance checks, broad-phase clear-pair counting, and preservation of fully null exclusion placeholders were added.
- The exported Part classifier and Assembly scan services, no-auto-fuzzy Common flow, persistent no-recompute metadata, command/menu/context/resource registration, source-wide and same-source exclusions, immutable worker snapshot, and intended deferred scope are present.
- Existing `Part::checkIntersection()` and MCP sources were not modified.

These fixes do not complete the plan. Core false-negative paths, a late-generation GUI corruption path, incorrect preview placement, incomplete lifecycle handling, and most of the planned integration matrix remain.

### Confirmed defects

#### High

- **A superseded scan can erase a newer completed result.** `InterferenceScanSession::finishScan()` rejects an old generation without mutating session ownership (`src/Mod/Assembly/App/InterferenceScanSession.cpp:36-50`), but after scan B has completed, `TaskInterferenceCheck::onScanFinished(A)` sees `isBusy()==false`, treats A as a current cancellation, and calls `discardResults()` (`src/Mod/Assembly/Gui/TaskInterferenceCheck.cpp:623-637`). The existing test finishes A only while B is still busy (`tests/src/Mod/Assembly/App/InterferenceScan.cpp:330-352`), so it misses the failing B-then-A order.
- **Invalid/default world bounds cause a verified penetration false negative.** Broad phase drops non-null, `shapeValid` leaves whose bounds are invalid (`InterferenceScan.cpp:493-503`), while clear-pair accounting still counts those leaves as valid and treats the absent candidate as proven clear (`InterferenceScan.cpp:619-630`). A Docker probe with two overlapping boxes and default bounds returned `candidates=0 complete=1 clearPairs=1 penetrations=0`.
- **Collapsed `App::Link` arrays are omitted.** Every link group is treated as a descended container, but children are read only from `ElementList` (`InterferenceScan.cpp:97-136,379-394`). A normal collapsed array keeps virtual instances in `PlacementList` with `ShowElement=false`; a Docker probe with `ElementCount=2`, two placements, and an empty `ElementList` returned `leaves=0`.
- **Placed preview geometry uses the wrong coordinate system.** The task populates `SoPreviewShape` without setting its `transform` (`TaskInterferenceCheck.cpp:761-775,814-815`). `setupCoinGeometry()` deliberately strips the shape’s root location (`src/Mod/Part/Gui/ViewProviderExt.cpp:1098-1101`), while FreeCAD’s canonical preview helper restores that transform (`src/Mod/Part/Gui/ViewProviderPreviewExtension.cpp:221-233`). Placed/nested context shapes therefore render at local/origin coordinates while markers are in world coordinates.

#### Medium

- Negative finite `linearTolerance` is accepted and normalized instead of rejected (`src/Mod/Part/App/InterferenceDetection.cpp:194-196,229-230`). A Docker probe with `-1.0` returned `Clear` with no diagnostic. `runInterferenceScan()` also normalizes a `NaN` tolerance for broad phase before the classifier can reject it (`InterferenceScan.cpp:606-617`), so pruned pairs can produce a completed result for invalid options.
- Invalid leaves and invalid/inconclusive pairs are counted but the result is still unconditionally marked complete (`InterferenceScan.cpp:590-599,691-710`); the GUI then says “Scan complete” (`TaskInterferenceCheck.cpp:646-655`) despite unclassified geometry.
- Clearance editing does not stale existing results because no spin-box value-change handler is connected (`TaskInterferenceCheck.cpp:172-190`). The input itself remains a fixed-mm `QDoubleSpinBox` with an arbitrary maximum, while only output cells use the active unit schema (`TaskInterferenceCheck.cpp:128-133,415-423`).
- Exclude and Restore share one indiscriminate enable predicate (`TaskInterferenceCheck.cpp:364-377`). Both are enabled for the wrong exclusion state and for invalid/inconclusive pairs; exclusion confirmation also counts non-violations (`TaskInterferenceCheck.cpp:851-869`).
- Pair diagnostics are never displayed, unavailable `-1` distance/zero-volume values are formatted as real measurements, and point/line preview nodes are created at the origin for results without closest points (`TaskInterferenceCheck.cpp:659-703,749-815`).
- Detailed task eligibility remains absent (`src/Mod/Assembly/Gui/Commands.cpp:383-395`). Planned summary/status icons and updated Assembly translation catalogs are also missing.
- Rewriting detached/unresolved XLinks reduces them to raw null pointers and can lose stored source identity (`AssemblyObject.cpp:2228-2262,2286-2319`; `src/App/PropertyLinks.cpp:3914-3983`). The current test inserts an anonymous null placeholder itself; it does not validate deletion, detached cross-document identity, or FCStd restoration.

### Confirmed risks and unverified items

- Stale monitoring connects only to the assembly’s owning document (`TaskInterferenceCheck.cpp:425-475`); changes, recomputes, visibility/placement changes, or closure in linked/source documents can leave results presented as current. Link-array `PlacementList`/`ScaleList` changes are not named by the property filter.
- The modal Manage Exclusions callbacks retain unchecked access to `assembly` after the owning-document callback can null it (`TaskInterferenceCheck.cpp:449-471,909-1055`). Document teardown during the nested dialog is an untested null-dereference/lifetime risk.
- `shapeHasSolid()` and `shapeBoundBox()` remain outside the collection exception boundary (`InterferenceScan.cpp:323-335`). An exception occurs after the task marks the session busy but before it creates a watcher (`TaskInterferenceCheck.cpp:530-546`), which can strand the panel in a busy state.
- Queued progress checks generation but not stale/cancel/completed state (`TaskInterferenceCheck.cpp:577-590`); worker `QPointer` lifetime, multiple cancelled OCCT jobs, and complete cleanup on destruction remain unverified.
- Preview tessellation cleans/remeshes shallow TopoDS copies with hard-coded deflection; mutation of triangulation shared with source features and preservation of source appearance are unverified.
- Rigid/flexible `AssemblyLink`, linked containers, Body Tip, placed/rotated root assemblies, inherited and element visibility, Include hidden, cycles, no-duplicate pairing, partial invalid results, FCStd round-trip, undo/redo, cross-document rules, deleted endpoints, dirty-state boundaries, and solver nonactivation remain unverified.
- No Assembly GUI/Xvfb runtime test target exists. MCP sources were unchanged, but MCP runtime behavior, broader FreeCAD runtime behavior, ABI/platform behavior, and modules outside the built Part/Assembly scope were not verified.

### Docker validation

- Clean current-source run: container `freecad-review-fresh-audit-20260724T183816Z-24fa05dd`; retained volume `freecad-review-fresh-audit-20260724T183816Z-24fa05dd-data`.
- Tested pre-review-document snapshot: Git `30bf4287dfdd676e7a2d38e6db0e10cdb9401fa8`, 24 dirty paths (`-uall`), 17,664 source files, aggregate SHA-256 `c0f65f7e40be3dda12c99f8ceabce8ab6ac2e332b47682ae6379e3760052dc45`, image ID `sha256:1e0c674e6b58b62e1e8e02d1fdedb4b484e540263959c7e54ed032771e2527a0`.
- Configure: **PASS**. Clean Debug build including `Part`, `Assembly`, their test executables, `TaskInterferenceCheck`, and `AssemblyGui`: **2,921/2,921 PASS**.
- Focused Part interference tests: **22/22 PASS**. Focused Assembly/AssemblyObject tests: **15/15 PASS**.
- The first full Part attempt produced 304/308 passes solely because `PartTestData` was not built. After adding that required target in a cloned disposable Docker volume, baseline full suites passed: **Part 308/308**, **Assembly 15/15**.
- Audit harness volume `freecad-review-audit-harness-20260724T191855Z-aac617e5-data` was newly created and seeded from the clean volume through a separate no-network clone container with the seed read-only. It reverified the same source hash before Docker-only ephemeral test edits, then reproduced the negative-tolerance, invalid-bounds false-clear, and collapsed-link-array defects above.

The implementation’s own validation remains insufficient and not fully reproducible. `scripts/ci/interference-validate.sh:117-153` runs only focused tests and omits `PartTestData`; its claimed relevant manifest covers only a subset of the dirty implementation (`:18-27`). The historical logs named by `PROGRESS.md` use a persistent incremental volume and contain no source/image/invocation metadata.

### `PROGRESS.md` reconciliation

- Accurate: the prior shared-cancel-flag, main teardown dereference, blank-preview, missing Manage dialog, manual-transform fallback, repeated leaf validation, clearance-validation, null-placeholder, and clear-count defects were substantially addressed. The retained historical logs do show 22 focused Part passes, 15 Assembly passes, and an `AssemblyGui` link.
- Inaccurate: `doc/PROGRESS.md:5,9-13` says P0 lifecycle/session ownership is fixed and Docker-validated, but the B-then-A completion order still destroys B and no task-panel lifecycle test exists.
- Inaccurate: `PROGRESS.md:23-27` overstates P1 completion. Preview placement is wrong, input units are fixed to millimetres, diagnostics/action state remain incomplete, and exclusion/modal lifecycle is unsafe.
- Incomplete: the changed-file list at `PROGRESS.md:32-44` omits `AssemblyObject.h`, Commands, InitGui, GUI/Part/test CMake files, qrc/icon, and the modified AssemblyObject test.
- Inaccurate/incomplete: the validator description overstates provenance, `PROGRESS.md:48` documents a persistent incremental volume rather than the script’s current default, and `PROGRESS.md:61` claims `dd` plus non-zero verification that the script does not implement.
- The admitted lack of GUI/Xvfb tests and most remaining matrix items (`PROGRESS.md:57-60`) is accurate, but the issue list omits the confirmed false negatives and GUI defects above.

## Historical next steps from the 2026-07-24 audit

1. Add Xvfb tests for placed/nested previews, linked-document changes or closure, dialog teardown, and unchanged source appearance.
2. Test exclusion identity through an FCStd cross-document round trip and same-document endpoint deletion.
3. Finish the task UI: quantity-aware clearance, eligibility details, status icons, and translations.
4. Cover cycles and real mid-scan cancellation.
5. Extend CI with `PartTestData`, full App/GUI Xvfb suites, disposable volumes, and exact provenance.
6. Update `doc/PROGRESS.md` only after these tests pass. Do not claim P0 GUI complete before then.
