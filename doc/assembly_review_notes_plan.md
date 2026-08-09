# Assembly Review Notes

## Summary

Reuse `App::AnnotationLabel`: it already provides persisted multiline text, a leader, visibility, and undoable label dragging. Draft annotations do not follow their targets, and TechDraw annotations are page-only, so add a thin Assembly-specific data object and view provider.

## Implementation Changes

- Add native `Assembly::ReviewNote : App::AnnotationLabel` with persisted properties:
  - `Target` (`PropertyXLinkSub`), component-rooted for geometry or linked to the joint.
  - `LocalAnchor` (`PropertyVector`), storing the picked point in target-local coordinates.
  - `JointSide` (`None`, `Reference1`, `Reference2`).
  - `Resolved` (`PropertyBool`).
  - Continue using inherited `LabelText`, computed `BasePosition`, and relative `TextPosition`.
- Add a lazy, non-draggable `Assembly::ReviewNoteGroup` named **Review Notes**. Show each note using its first non-empty text line, with open, resolved, and broken-attachment icons. Protect the group from independent deletion and include it in Assembly deletion cleanup.
- Add `CommandReviewNote.py` with `Assembly_AddReviewNote`, edit, and resolve/reopen commands. Insert **Add Review Note** from [`InitGui.py`](C:/Users/Rchie/Music/FreeCADModeling/FreeCAD/src/Mod/Assembly/InitGui.py:140) only for the 3D `"View"` context menu when exactly one supported Assembly target is selected; do not add a toolbar button.
- Normalize components, faces, edges, and vertices through `UtilsAssembly.getComponentReference()` and use `PickedPoints`; fall back to the component bounding-box center when no pick exists. Give 3D joint selection a synthetic `"Main"` subelement so its picked point can select the nearest JCS; tree-selected joints default to `Reference1`, and the leader anchors at that JCS origin.
- Prompt for non-empty multiline text, then create the group and note in one transaction. Initialize the relative text offset toward the camera’s upper-right using 15% of the target bounding-box diagonal, clamped to 10–100 mm with a 20 mm fallback; the inherited dragger handles later placement.
- Compute `BasePosition` in Assembly-local coordinates. Observe document object changes once per Assembly and refresh affected notes synchronously on component placement or joint-reference changes, before Assembly’s `purgeTouched()` can suppress recompute propagation. Also refresh on recompute and document restore.

## Behavior and Persistence

- Double-click or **Edit Review Note** edits text transactionally; dragging changes only relative `TextPosition`.
- **Resolve/Reopen** changes status and tree icon only. Standard visibility remains independent and persists through `ViewObject.Visibility`.
- Standard Delete removes only the note and is undoable. If its target is deleted or invalidated, retain the text and last anchor with a broken-reference icon; undoing target deletion reattaches it.
- Creation, editing, resolving, visibility, dragging, and deletion participate in undo/redo. Native properties, group membership, view properties, and attachment data save directly in `.FCStd`; no Python proxy serialization is required.
- Public additions are the two native object types and the three Assembly GUI commands. No new high-level Python factory is required for v1.

## Test Plan

- Add `AssemblyTests/TestReviewNotes.py` covering command eligibility and normalization for component, face, edge, vertex, both joint sides, unsupported targets, and multiple selections.
- Verify attachment under translation and rotation, direct and linked/nested components, non-identity Assembly placement, joint movement, and the solver’s `Placement`-then-`purgeTouched()` path.
- Test lazy group creation, tree membership, broken targets, Assembly deletion, and undo/redo for every mutation; first-creation undo must remove both note and newly created group.
- Save, close, and reload a temporary `.FCStd`, then verify text, target/subelement, local anchor, relative label offset, resolved state, visibility, group membership, and continued tracking after another move.
- Add GUI smoke coverage for context-menu presence, double-click editing, hiding, leader display, status icons, and inherited annotation-label dragging; no pixel-golden rendering test is needed.

## Assumptions

- One target per note; no authorship, replies, retargeting UI, or automatic hiding of resolved notes in v1.
- Initial placement uses the offset-and-drag workflow, and resolution leaves manual visibility unchanged.
