## Summary

Fixes from the MCP findings campaign (see `doc/HANDOFF-2026-09-19-mcp-findings.md` in the control folder). Each fix has a regression test that fails before and passes after it.

- **D-23**: a native commit whose apply/postcondition callback refused, and which FreeCAD rolled back, is now reported as `outcome: rejected` with the real code and message (e.g. `SKETCH_NOT_FOUND`, "Sketch 'NoSuchSketch' not found"). Before, it came back as `*_NATIVE_EXCEPTION` / `uncertain` with message `_AbortPadFeatureMutation`. Proof: `DocumentPy::commitCompatibilityMutation` re-raises the callback exception only after a restored rollback, and returns `RollbackFailed` otherwise.
- **D-25**: the 12 typed feature tools (revolve, boolean union/difference/intersection, chamfer, fillet, helical sweep, linear/polar pattern, loft, mirror, sweep) failed on the v1 fallback with "unexpected keyword argument 'doc_name'" before sending anything. They now send positional params, and the test binds them against the real addon handler signatures.
- **D-15 caller path**: the blocked-by-modal-dialog timeout is now `GUI_TIMEOUT_BLOCKED_BY_MODAL_DIALOG`, so the typed contracts and the client pass the dialog title through. Before, callers got "invalid contract response; document state requires reconciliation".
- **D-27**: pad/pocket postconditions require a solid with positive volume. Natively, a pocket that removed the whole body (0 solids) and a pad of overlapping wires (volume -0.0) were both committed.
- Earlier on this branch (8e23c29 and before): D-02b, D-03, D-04, D-07, D-08, D09-E4, D-16 to D-21, F-05.

## Merge note

This branch conflicts with `main` in 9 files: `create_object`, `diagnostics_shape_actions`, `find_edges`, `find_faces`, `pad_feature`, `pocket_feature`, `policy_runtime`, `sketch_create` and the RPC contract snapshot. They collide with the KEEP BOTH commits on `main`. Resolve by merging `main` into this branch and re-running the suite.

Use a merge commit (not squash) if you want the FreeCAD pin to be able to name this branch's commits. Otherwise, pin FreeCAD to `main`'s head after merging.

## Test plan

- [x] Docker unit suite: only the 17 known pre-existing baseline failures (12 063 passed)
- [x] Native: D-23 3/3 (rejected, `ApplyFailed`, `rollback_succeeded: true`, no orphans); D-15 1/3 (a real "Document Recovery" dialog named in the error)
- [ ] Native D-25 / D-03 revolve, D-27 and D-15 runs 2-3 (need a `/mcp` reconnect)
- [ ] Resolve the conflicts with `main`, then re-run the suite
- [ ] Independent review

🤖 Generated with [Claude Code](https://claude.com/claude-code)
