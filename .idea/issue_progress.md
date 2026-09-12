# Chair MCP issue progress

Target PR branch: `integrate/change-aware-save-mcp-autonomy`

Starting FreeCAD commit: `c48a6c9eac`

Starting MCP submodule commit: `23bedfd`

Started: 2026-09-12

## Workflow

Codex orchestrates **Sol (high reasoning review) → Terra (implementation) → Luna (Docker tests) → Sol (review)**. Failures and new findings return to Terra. After the final review and passing tests, create a new branch, commit, push, and open a PR into the target branch. Publish any MCP submodule changes first so the parent commit resolves from a fresh checkout.

## Issue ledger

| ID | Report | Status | Resolution / evidence |
| --- | --- | --- | --- |
| 1 | GUI mutations time out while still committing | Fixed; regressions passed | Restore authenticated late-result journaling after native cutover, correct status/cancellation precedence, and finalize public typed responses consistently. The integrated RPC → GUI → replay → client → tool regression proves one execution. Original chair latency is not reproduced or optimized. |
| 2 | Assembly joint creation dereferences a missing ViewObject | Fixed; GUI regressions passed | Guard migration, register identity-bound deferred provider attachment, and clean up failed joint creation. All 22 TestCore tests and native deferred publication smoke pass under GUI/Xvfb. |
| 3 | Invalid assembly blocks repair with pending_recompute | Fixed; native regression passed | Force-delete uses narrow native deferred recovery and returns an explicit recompute instruction, including late replay. Native invalid-link → delete → recompute → normal modeling regression passes. |
| 4 | Lifecycle tools require authenticated RPC v2 | Fixed; launcher tests passed | Add root `--authenticated-isolated` option and print the verified manifest-bound MCP command. Legacy mode is clearly labeled. Retired lease tools remain retired. |
| 5 | Typed App::Link cannot bind LinkedObject | Fixed; unit and native regressions passed | Resolve typed link properties to document objects before assignment; missing targets fail atomically. |
| 6 | Successful mutation reported as failed if presentation raises | Fixed; regressions passed | Preserve committed success with presentation warnings for create/edit, generated code, sketch deletion screenshots, and server object presentation. |

## Review and validation log

- Initial working trees are clean. The MCP project is a detached Git submodule.
- No applicable `AGENTS.md` files were found in the workspace or ancestor directories.
- Sol is reviewing all five reports and nearby branch changes.
- Luna is preparing Docker validation using the available FreeCAD CI images.
- Sol confirmed issue 2: `JointObject.Joint` runs before presentation replay can create `ViewObject`; `_attach_joint_view_provider` then silently skips absent providers. Both creation APIs also need cleanup after exceptions. These findings were handed to Terra.
- Sol confirmed issue 5: object-reference decoding handles a short property-name allowlist, omitting `App::Link.LinkedObject`. Terra will resolve typed link properties before the creation transaction recomputes.
- Sol confirmed an additional retry hazard in create/edit tool responses: a screenshot exception can hide an already committed mutation. The delete tool already preserves this distinction; Terra is extending that behavior to create/edit.
- Issue 3 is partially addressed by existing recompute changes on this branch. Sol confirmed the remaining gap: force-delete still uses eager admission, which requires an already settled document and therefore blocks deletion of the failed object.
- Docker baseline MCP unit run: **2,671 passed, 141 deselected, 1 expected failure** in 283 seconds. A sidecar integration module was excluded from this initial submodule-only mount because the parent sidecar package was unavailable; final validation will include the parent package. Log: `/tmp/chair-mcp-tests/baseline-unit-ignored-sidecar.log`.
- The cached branch FreeCAD release build loads successfully inside Docker with its Pixi runtime. Native collaboration and PartDesign checks are running.
- Sol reviewed issue 1's current authenticated path: request replay is keyed by UUID; running timeouts preserve uncertain-completion evidence; late completion replaces the cached result; request status exposes the final response. Terra will add an integrated typed regression with an injected short timeout.
- Sol reviewed issue 4: authenticated lifecycle support already exists in the isolated setup/launcher. The normal root launcher still points users to legacy mode. Terra will add `--authenticated-isolated` by delegating to existing scripts, preserving strict manifest identity checks.
- Sol completed the initial review of all five reports. Additional presentation-result failures were confirmed in sketch geometry/constraint deletion and server-side create/edit presentation callbacks. These are part of issue 6's fix scope.
- The deeper timeout integration review found a remaining issue 1 defect: `control_status_state.py` prioritizes handler `failed` over active uncertain GUI work and a later successful result. Terra is correcting that precedence; the new integration regression will verify `running_after_timeout` followed by completed late-result recovery.
- Sol also found that some late mutation callbacks journal raw booleans/lists before the public typed response adapter runs. Those late results can be rejected by the client. Terra is applying the same typed adapters to late completion.
- Luna's native Docker baselines passed: **1,018 App tests** (2 skips), **27 PartDesign tests**, and **110 Assembly tests**. Initial focused MCP changes passed **53 tests**. Native Python/GUI harness setup continues so new live regressions are executed rather than counted as skipped.
- The composed timeout review uncovered the missing connection after native cutover: authenticated requests no longer populate the legacy mutation context, so GUI completion never registered a replay callback. Terra now derives replay context directly from authenticated inflight identity, without restoring retired lease authority.
- Native live MCP regression suite now passes **6/6** under the real `FreeCADCmd`, including a dirty invalid link → force-delete → recompute → normal edit and typed link creation. The binary reports exact base commit `c48a6c9eac`. Evidence: `/tmp/chair-mcp-tests/native-i5-cmd.log`, `native-i5.xml`.
- Real GUI/Xvfb smoke confirms `ViewObject` is absent during native joint creation and attached after committed publication. The first 22-test Assembly GUI run found one setup defect (dirty fixtures before eager commit), now fixed and awaiting rerun.
- Focused timeout, typed late-result, object, property, and recovery tests passed **100 tests** before the final generated-code/delete refinements. Evidence: `/tmp/chair-mcp-tests/final-focused.log`.
- Sol's follow-up review identified generated rectangle late-result finalization and force-delete late replay losing its recompute instruction. Terra applied shared finalizers and is finishing focused regression coverage.
- Root launcher tests pass **30 tests**. MCP CI byte-compilation, changed parent Python compilation, and version synchronization checks pass. Evidence: `/tmp/chair-mcp-tests/terra-launcher.log`, `final-syntax.log`.
- Architecture overview completed in [`doc/chair-mcp-issue-architecture.md`](../doc/chair-mcp-issue-architecture.md), including all reported problem areas and the additional presentation-result finding.
- Final GUI rerun: **22 TestCore tests passed**, and deferred native joint creation again confirmed no ViewObject inside the callback followed by attached presentation after publication. Evidence: `/tmp/chair-mcp-tests/assembly-gui-final.log`, `assembly-gui-smoke.json`.
- Follow-up response contracts pass **71 tests**, including generated execute-code late success/error adaptation, force-delete late recompute metadata, and create/edit/deferred-factory presentation exceptions. Evidence: `/tmp/chair-mcp-tests/final-contracts.log`.
- Final launcher and lint checks: **30 parent launcher tests + 44 isolated-profile tests passed**; changed-file codespell and all MCP Python syntax checks passed. The printed MCP command now quotes paths safely. Evidence: `/tmp/chair-mcp-tests/final-launcher-lint.log`.
- Broad parent-mounted tests exposed stale dispatch doubles after adding the late-result transform; Terra updated them and their focused files passed. The next full run reached **2,699 passed, 1 failed, 144 deselected, 1 expected failure**. The remaining test fixture import fails only with full collection; Terra is investigating namespace pollution. Evidence: `/tmp/chair-mcp-tests/final-unit-clean.log`.
- Terra isolated the final collection failure with an A/B run: adding the sidecar project root to `PYTHONPATH` shadows the MCP `tests` namespace; its `src` directory alone is sufficient. Full collection with the affected test selected passes when that extra root is removed (**1 passed, 2,844 deselected**). No source workaround was added. Luna is running the complete suite with the corrected environment.
- **Final full Docker unit suite passed: 2,700 passed, 144 deselected, 1 pre-existing expected failure; exit 0 in 250.01 seconds.** The expected failure is the existing structured screenshot-diff feature. Evidence: `/tmp/chair-mcp-tests/final-unit-pass.log`. The native/GUI and launcher verdicts above also pass.
- Sol issued unconditional scoped approval after the passing full run: all five reports and the confirmed presentation/retry hazards are resolved, with no remaining production findings in scope. Original chair performance remains unmeasured.
- Delivery branch: `fix/chair-mcp-modeling-recovery` in both repositories. MCP local commit: `51e083a3bdde0043d3242072623756d583ecf359`.
- **Publication blocked by authentication:** the connected GitHub account can read both repositories, but creating the staged MCP tree returned HTTP 403, `Resource not accessible by integration`. Command-line HTTPS Git has no credentials, and this workspace has neither SSH keys nor an authentication agent. No remote branch or PR has been created. Local commits and prepared PR descriptions are ready; publishing must resume after repository write access is available. This is a GitHub credential restriction, not a rejected safety approval.

## Reproduce the MCP unit run

Run from this checkout, using the local CI image:

```sh
docker run --rm --network none \
  -v "$PWD:$PWD:ro" -w "$PWD/tools/mcp/freecad-mcp" \
  -e PYTHONDONTWRITEBYTECODE=1 \
  -e "PYTHONPATH=$PWD/tools/mcp/freecad-mcp:$PWD/tools/mcp/freecad-mcp/src:$PWD/tools/freecad_git/src" \
  127.0.0.1:5001/freecad-ci-mcp:24.04 \
  python -m pytest -q -m unit -o cache_dir=/tmp/pytest-cache
```

Native checks used the branch release binaries and matching Pixi libraries in
Docker, with current source Assembly/MCP modules. The GUI harness persists its
unittest verdict before exit; a FreeCAD process exit code alone is insufficient
to establish that embedded Python tests passed.

## Delivery

- [x] Confirm findings and implement fixes with regression coverage.
- [x] Docker tests pass for the changed behavior.
- [x] Sol final review has no unresolved findings in scope.
- [x] Architecture overview completed.
- [x] New local branches and commits prepared.
- [ ] Both branches published (blocked by GitHub write authentication).
- [ ] PR opened into `integrate/change-aware-save-mcp-autonomy`.
