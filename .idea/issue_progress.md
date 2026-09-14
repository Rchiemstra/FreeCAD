# PR 52 review and finish — evidence log

Date: 2026-09-14
Integrator branch: `fix/wp348-core-late-result-transform`
PR: https://github.com/Rchiemstra/FreeCAD/pull/52
Base: `FreeCAD-start`

## Identities

### At review start (origin HEAD)

| Item | Value |
| --- | --- |
| Parent HEAD | `562639de15818fa89cbdae2c4a62879ac96b9b32` |
| MCP submodule pin | `7c9e4aac49f3954a54dd2375b1cb2442353c5420` |
| MCP message | Accept late_result_transform in the core collaboration dispatch stub. |
| Woodpecker | pipeline **360** **success** on `562639de15` (e2e 19 units / 239 tests; no aggregate TestSketcherGui) |

### After isolated review/fix (local integrator; push after 360)

| Item | Value |
| --- | --- |
| Local parent HEAD | `a8e82b3a6e3732616f3e640d0a681ae2bb04d39e` Restore a finite sketch camera after viewObjects and pin the replay-only MCP transform. |
| Local parent first commit | `0b16f7e75f9ce87c03900b1839dd1478bda14806` Fail closed on GUI-runner false greens and recover a finite sketch grid/camera. |
| MCP pin (pushed) | `cc32c4fe70ea04627438380e4b4f580a20afc97f` |
| MCP commits pushed | `690409e6` sync fake; `cc32c4fe` late transform helper |
| Isolated worktree A | `C:/Users/Rchie/Music/FreeCADModeling/pr52-gui-runner` branch `pr52-gui-runner` (not pushed) |
| Isolated worktree B | `C:/Users/Rchie/Music/FreeCADModeling/pr52-camera-grid` branch `pr52-camera-grid` (not pushed) |
| python:3.12-slim | `sha256:25c5b8011a3425a140bf5fa73be0feabd3c0d5b323eecb19dc02437a368ae075` |
| freecad-ci-deps:24.04-gdb | `sha256:8b1b8755ab2b37fa98f80fad2b340a1e48d549039991880d3d4fbbece4fc185f` |
| Isolation | new `--rm` containers only; existing FreeCAD/MCP processes, chair volumes, sockets, user cfg **untouched** |

## Review vs origin `FreeCAD-start...562639de15`

Classified against the **current origin diff**, then re-checked after local fixes.

### Introduced bugs (this PR) — status after local fixes

1. **GUI runner false-green on empty/failed discovery (WP 341–343).** Introduced/retained on this PR. **Fixed:** listing rc preserved; missing `Registered test units:` or empty list → exit 2.
2. **Missing expected suites still exit 0.** Introduced. **Fixed:** missing any of the 13 WP 336/359 suites → exit 3.
3. **`TestSketcherGui` aggregate dropped + no completed-test gate.** Introduced by the class split. **Fixed:** aggregate runs first; class units are extras; no `Ran N tests` or N=0 → exit 4.
4. **Inconsistent finite-volume guards.** Introduced. **Fixed:** `getPickedPointsOnRay` uses `hasUsablePickVolume()` (isfinite + >0), same helper as pick-volume.
5. **Coin `startEditing()` null / missing `finishEditing()`.** Introduced by Inf grid path. gdb WP 358: `SbVec3f::setValue(this=0x0, x=-inf)`. **Fixed:** plan grid first; skip writes if null; always `finishEditing()`.
6. **Grid/camera recovery missing.** Inf skip on `4ba2a89ec7` avoided SIGSEGV (WP 359) but did not restore a usable view. **Fixed:** recover ortho height 200 before and after `viewObjects`; recover grid extent from `GridSize*20` / last finite value; plan must yield `nlines>=2`.
7. **MCP sync fake applied replay transform.** Introduced on the collaboration stub tests. **Fixed:** `synchronous_dispatch_gui` returns `task()` only. Transform lives in `apply_late_result_transform` used by `build_replay_on_complete`.

### Existing hazards (not introduced here)

- Coin `SoRayPickAction` on a degenerate frustum (uninitialized ray unless `COIN_DEBUG`).
- `short(std::roundf(normalized * pixels))` on Inf/NaN: **captured**, not a trap. Local UBSan: `scaled_finite=0 pixel=32767`. Production now refuses non-finite normalized values.
- `setCameraType` historically no-op’d a null camera (already created on this PR).

### Unproven hypotheses (do not treat as fact)

- Coin 4 / pivy #48 is the only reason 336 passed and 350+ crashed. 336 was binary `0cc28873`.
- Hiding `BaseSketch` in `TestExternalFacePreselection` is the product fix for #28639. WP 359 only showed Vertex2 beat Face.
- `convertToNURBS` no-op for internal BSplines is required beyond the GUI assertion.
- Perspective near/far Inf after ortho height restore (not in gdb 358).

## Three demonstrated GUI-runner false-green cases (gate)

Expected suites (13): `GuiDocument`, `TestSpreadsheetWindowGui`, `TestSketcherGui`, `TestPartDesignGui`, `TestPartGui`, `MeshTestsGui`, `TestDraftGui`, `TestArchGui`, `TestTechDrawGui`, `TestImportGui`, `TestOpenSCADGui`, `TestMaterialsGui`, `TestCAMGui`.

| # | Evidence | Required behavior | Isolated test |
| --- | --- | --- | --- |
| 1 | WP 341–343, ~7–8s, empty list, exit 0 | Preserve failed discovery (non-zero) | `test_empty_discovery_does_not_return_zero` → 2; `test_failed_discovery_exit_is_preserved` → 17 |
| 2 | `if not gui_tests: return 0` | Require the 13 suites | `test_missing_suite_does_not_return_zero` → 3 |
| 3 | Split `TestSketcherGui` + no `Ran N` | Aggregate first; require N≥1 | `test_aggregate_sketcher_runs_before_class_units`; `test_zero_test_module_fails_the_gate` → 4 |

`python -m unittest test_run_gui_tests -v`: **19 tests, OK**.

## Camera/grid numeric / Coin / recovery

1. **First invalid state (UBSan, this turn):** `roundf(Inf * 1024)` non-finite; `static_cast<short>` → **32767**; UBSan **did not trap**. Binary: `tests/standalone/ubsan_old_inf_to_short.cpp`.
2. **First invalid state (gdb WP 358):** null `SbVec3f*` write of `-inf` in `createGridPart` from Inf camera extent / negative `nlines`.
3. **Recovery regression (header-only, UBSan):** Inf extent → fallback 200 → `planSketchGrid` **valid** `recovered_vlines=30 recovered_nlines=60`. Inf height after a simulated `viewObjects` rewrite recovers again to the same plan.
4. Production: restore ortho height when pick volume unusable, including **after** `viewObjects`; Inf `getMaxDimension()` (-1) forces a grid rebuild from a finite fallback.

## Re-review (grid safety) — demonstrated only

Reviewer probes used `-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all`. Plain `-fsanitize=undefined` does **not** enable `float-cast-overflow` (GCC instrumentation docs). Reproduced here:

| Probe | Sanitizer | Result |
| --- | --- | --- |
| Old `static_cast<int>(1e10)` / `149 + 2147483520` / `INT_MIN - 150` | `undefined,float-cast-overflow`, no recover | **trapped** (exit ≠ 0) for cases 1–3 |
| Same old large-X cast | `undefined` only (prior log) | silent; **not** evidence |
| `planSketchGrid` / `tryGridOffsets` on those three inputs, plus Inf→200 recovery | `undefined,float-cast-overflow`, no recover | **ok** `origin_vlines=150 recovered_vlines=30` |
| 64 rejected + failed-after-alloc FakeNode builds | ASan | **live=0** |

Resolved in source + those probes:

1. **Offset overflow.** `planSketchGrid` now refuses a plan unless `tryCastToInt` and the later add/sub of `vlines`/`nlines` succeed. `createGridPart` uses `plan.offsetX` / `plan.offsetY` and `tryAddInt` in the loops.
2. **Coin ownership on early return.** Plan is computed before `SoLineSet` / `SoVertexProperty` allocation. Nodes are `Gui::CoinPtr` and are published to `GridRoot` only after vertex fill succeeds. Rejected plans allocate no grid/vts; ASan 64-iteration probe stayed at `live=0`.

Not marked resolved here:

- **Full GUI invalid→valid recovery in a running FreeCAD.** `test_inf_camera_recovers_finite_pick_and_edge_preselection` was added to `TestConstraintPreselectionGui`. It has not been executed against a local debug binary in this step.
- **Pipeline 362** on `6e24093e21` **failed** `freecad-e2e` (exit 1). Same aggregate gate as 361: `TestSketcherGui` 51 tests / `FAILED (failures=1)` on `testDistanceDatumTextWinsOverOverlappingCurve`, `midpoint_coin=(0, 0)`, `Vertex2`; class-only rerun **ok**. The volume-gate removal and fitAll-only retry were not enough. The GUI test now also restores a finite ortho height and searches the fitted viewport center for an edge hit when Coin returns the origin sentinel.

Container: `pr52-b-grid-sanitizers-20260914f` (`freecad-ci-deps:24.04-gdb`). Runner/MCP files were not changed.

## Isolated tests this turn

| Gate | Container | Image | Result |
| --- | --- | --- | --- |
| GUI runner 19 tests | `pr52-a-gui-runner-20260914` | python:3.12-slim | **OK** |
| Old Inf→short + recovery | `pr52-b-camera-grid-20260914c` | freecad-ci-deps:24.04-gdb | **pixel=32767**; **recovered_nlines=60** |
| MCP sync + late transform | `pr52-mcp-sync-20260914` | python:3.12-slim + pip pytest `--noconftest` | **5 passed** |

No chair volumes, no live MCP sockets, no user cfg mounts.

Gui gtests in `tests/src/Gui/View3DInventorViewer.cpp` were not re-run against a full FreeCAD debug binary this turn (would require a new isolated debug build, not chair incremental). Header-only UBSan covers the numeric/recovery helpers.

## Woodpecker

- Pipeline **360** finished **success** on `562639de15818fa89cbdae2c4a62879ac96b9b32` (all `ci` steps, including `freecad-e2e`).
- WP 360 e2e (log has no newlines; parsed `Ran N tests` concatenated): **19 units, 239 tests, 0 FAILED**. No aggregate `TestSketcherGui` unit — the class split replaced it. Per-unit counts: GuiDocument 12, TestSpreadsheetWindowGui 5, SketcherGuiTestCases 5, TestDistanceLabelExtensionGui 6, TestConstraintCommandsGui 16, TestOnViewParameterGui 11, TestSketchPlacementUpdate 4, TestExternalFacePreselection 3, TestSketcherOffsetGui 6, TestPartDesignGui 21, MeshTestsGui 2, TestMaterialsGui 3, TestImportGui 1, TestArchGui 48, TestPartGui 15, TestOpenSCADGui 1, TestDraftGui 38, TestTechDrawGui 5, TestCAMGui 37.
- 360 does **not** contain the runner fail-closed / camera recovery / MCP pin `cc32c4fe` work. Do not merge 360's SHA as the finished review.
- Pipeline **361** failed `freecad-e2e` on `84f9d8ec28` (exit 1). Runner fail-closed correctly: aggregate `TestSketcherGui` ran 51 tests / 119.9s / `FAILED (failures=1)`. Class units then passed (same counts as 360). Failure: `testDistanceDatumTextWinsOverOverlappingCurve` `before_kind` vertex vs edge, `midpoint_coin=(0, 0)`, `text_coin=None`, pick `Vertex2` at (130,100). Same test **ok** in the following class-only process. Classification: introduced over-guard — `getPointOnViewport` returned `{0,0}` when `hasUsablePickVolume` was false; (0,0) is also the failure sentinel. Fix: project anyway and reject only non-finite pixels; retry `viewTop`/`fitAll` in the GUI test until the projection is not the origin sentinel.
- Do **not** merge until `ci/woodpecker/pr/ci` is **success** for the **exact pushed SHA** that contains these runner/recovery/MCP-pin files.
- MCP origin is already at `cc32c4fe` (`https://github.com/Rchiemstra/freecad-mcp` branch `fix/wp348-core-late-result-transform`).
