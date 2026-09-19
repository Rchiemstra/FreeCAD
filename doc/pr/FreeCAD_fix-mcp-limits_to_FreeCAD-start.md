## Summary

- **CI gate `mcp-pin-on-main`** (`ci/woodpecker/mcp-pin-on-main-check.sh`, step in `.woodpecker/ci.yml`): for a PR into `FreeCAD-start`, the `tools/mcp/freecad-mcp` pin must be an ancestor of `freecad-mcp` `main`, fetched fresh and unshallowed if needed. MCP work must be merged into MCP `main` through its own PR first. Other targets (`fix/*`, `chore/*`, `integrate/*`), tags and manual runs skip inside the script, so no dependent step gets filtered out.
- **`.woodpecker/block-main.yml`**: `ci.yml` does not trigger for `main`, so a PR into `main` got no status at all. This workflow now fails it.
- **MCP pin bumps** to `fix/mcp-limits` (a8cd66d → 8e23c29 → cf34f8b). These are the 2 commits that had landed on `FreeCAD-start` without a PR; `FreeCAD-start` was reset to `31e3b69`, and they come back through this PR instead.

## Expected CI state

**`mcp-pin-on-main` is red on purpose** until the freecad-mcp PR (`fix/mcp-limits` → `main`) is merged and this PR re-pins to a commit on freecad-mcp `main`. This PR is the first to exercise the gate.

## Test plan

- [x] Gate script, run in throwaway clones:
  - fails for pins `8e23c29` and `18b1436`, which are not on main
  - passes for freecad-mcp main's head, including from a shallow submodule clone
  - skips for `fix/*` targets and for runs without a target
- [x] Both workflow files parse as YAML; the script passes `sh -n`
- [ ] Woodpecker run on this PR: `mcp-pin-on-main` red now, green after the re-pin
- [ ] A throwaway PR into `main` shows `reject-pr-to-main` red

🤖 Generated with [Claude Code](https://claude.com/claude-code)
