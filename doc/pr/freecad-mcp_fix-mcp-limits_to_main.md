## Summary

Fixes from the MCP findings campaign (`doc/HANDOFF-2026-09-19-mcp-findings.md` in the control folder). Each fix has a regression test that fails before and passes after it, and each was confirmed on a live FreeCAD.

- **D-23**: a native commit whose apply/postcondition callback refused, and which FreeCAD rolled back, is reported as `outcome: rejected` with the real code and message (e.g. `SKETCH_NOT_FOUND`). Before, it came back as `*_NATIVE_EXCEPTION` / `uncertain`. Only the exact exception our callback raised counts as proof of the rollback.
- **D-25**: the 12 typed feature tools (revolve, booleans, chamfer, fillet, helical sweep, linear/polar pattern, loft, mirror, sweep) failed on the v1 fallback with "unexpected keyword argument 'doc_name'" before sending anything. They now send positional params, checked against the real addon handler signatures.
- **D-15 caller path**: a GUI call blocked by a modal dialog returns `GUI_TIMEOUT_BLOCKED_BY_MODAL_DIALOG` with the dialog title, instead of "invalid contract response; document state requires reconciliation".
- **D-27**: pad/pocket postconditions require a solid with positive volume. A pocket that removed the whole body (0 solids) and a pad of overlapping wires (volume -0.0) were both committed before.
- **D-14**: `create_object` / `edit_object` accept a `PlacementList` of Placement dicts for App::Link arrays. An edit that FreeCAD would silently ignore (element objects exist, `ShowElement` true) is refused with a message naming the three ways that work.
- **D-30**: `bounding_box`, `measure_*`, `center_of_mass` and `inspect_geometry`'s global bbox applied a placed object's own Placement twice (a box at x=5 rotated 90° about X reported x 10..20). They now apply only the enclosing containers.
- **D-22**: a tool call with an argument the tool does not declare is rejected before the tool runs, naming the unknown and the accepted arguments. Before, a misspelled optional argument was dropped and the default used.
- **D-26**: `pocket_feature` refuses a Body without a base solid (`POCKET_NO_BASE_SOLID`). FreeCAD would otherwise add the pocket's shape as material.
- Earlier on this branch: D-02b, D-03, D-04, D-07, D-08, D09-E4, D-16 to D-21, F-05.

## Merge with `main`

`main` is merged in with a merge commit. 10 files conflicted, and each resolution keeps both sides' intent:

- `collaboration_api.py`: main's `_settle_pending_recompute` and `recompute=` pass-through, plus the branch's exact-exception rollback proof (D-23). It replaces main's looser mapping, which also reported unproven exceptions as rolled back. Main's own test for the mapping passes unchanged.
- `pad_feature.py` / `pocket_feature.py`: main's profile diagnostics and `SKETCH_PROFILE_NOT_CLOSED` code, plus the branch's self-intersection refusal (D-07). Pocket keeps both postconditions: D-27 (a real solid) and main's material-delta direction check. Fixed a merge-introduced `NameError` (`shape`).
- `sketch_create.py`: main's positional `attachment_offset`, plus the branch's refusal of an offset without `attach_to` before any object is created.
- `find_edges.py` / `find_faces.py`: main (adds `direction_approx`).
- `diagnostics_shape_actions.py`: the branch's filters (a radius filter excludes subshapes without a radius), plus main's `geom_type` field and aliases. A new main call site now uses the shared `read_global_placement`.
- `create_object.py`, `policy_runtime.py`: main's helpers (link pre-resolution; string or Document for recompute) with the branch's postcondition.
- RPC contract snapshot: 3-way merged per key. Only `find_edges` / `find_faces` parameters collided and take main's; the result matches the handler signatures exactly.

The branch also carried 6 unit failures that `main` does not have. They are fixed in `6d816dd`: a public-surface lint issue in `gui_core` and `measure_io_actions`, stale frozen examples for the typed `get_object` and the F-05 build identity, and outdated test counts and fakes.

## Test plan

- [x] Docker unit suite: 8 failures, exactly the 8 that fail on `main` itself (5 × sweep_pipe, 2 × typed_platform_discovery, 1 × architecture CLI); 12 087 passed
- [x] Native (live FreeCAD, 3 independent runs each, raw response + state check): D-23, D-25, D-03 (symmetric revolve), D-15, D-14, D-27; D-30 and D-26 old behavior reproduced and fixed
- [ ] Native re-check after the merge (pad/pocket/sketch_create/revolve on the merged addon)
- [ ] Independent review

🤖 Generated with [Claude Code](https://claude.com/claude-code)
