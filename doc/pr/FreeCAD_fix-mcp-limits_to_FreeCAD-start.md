## Summary

- **D-29: PartDesign revolve/groove in the isolated recompute** (`src/App/GenericIsolatedRecompute.cpp`). `Revolved::execute()` calls `updateAxis()`, which rewrites `Base` and `Axis` from `ReferenceAxis`. Both predate `Prop_Output`, so the generic isolated recompute refused every Revolution and Groove as "undeclared property side effect: Rev1.Axis". The fix declares exactly those two members as outputs on `PartDesign::Revolved` ancestry, the same way the Helix coupled parameters are declared. `ReferenceAxis` stays an immutable input. Test: `tests/architecture/test_generic_isolated_recompute.py::test_revolved_axis_is_a_declared_recompute_output`, which fails before the fix and passes after it.
- **CI gate `mcp-pin-on-main`** (`ci/woodpecker/mcp-pin-on-main-check.sh`, step in `.woodpecker/ci.yml`): for a PR into `FreeCAD-start`, the `tools/mcp/freecad-mcp` pin must be an ancestor of `freecad-mcp` `main`. The script fetches `main` fresh and unshallows if needed. Other targets, tags and manual runs skip inside the script.
- **`.woodpecker/block-main.yml`**: `ci.yml` does not trigger for `main`, so a PR into `main` got no status at all. This workflow now fails it.
- **MCP pin**: moved to freecad-mcp `main` after the freecad-mcp PR merged (D-14, D-22, D-23, D-25, D-26, D-27, D-30, D-15 caller path).
- `doc/pr/`: the texts of both PRs.

## Expected CI state

`mcp-pin-on-main` is green only once the pin names a commit on freecad-mcp `main`. This PR is the first to exercise that gate.

## Test plan

- [x] D-29: FreeCAD rebuilt with `-Werror`, 0 warnings. `App_tests_run --gtest_filter='GenericIsolatedRecompute*'` passes 21/21.
- [x] D-29 native, 3 runs: symmetric revolves of 90°/120°/60° commit. `Axis (0,0,1)` is published from `ReferenceAxis`, the bbox is symmetric about the sketch plane, and the volume matches the analytic value. Before the fix, each was rejected with a clean rollback.
- [x] Gate script, run in throwaway clones: it fails for pins not on main, passes for main's head (also from a shallow submodule clone), and skips for `fix/*` targets and for runs without a target.
- [ ] Woodpecker run on this PR
- [ ] Independent review

🤖 Generated with [Claude Code](https://claude.com/claude-code)
