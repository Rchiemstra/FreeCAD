# PR 52 review and finish — evidence log

Date: 2026-09-14
Integrator branch: `fix/wp348-core-late-result-transform`
PR: https://github.com/Rchiemstra/FreeCAD/pull/52
Base: `FreeCAD-start`

## Identities (start of this review)

| Item | Value |
| --- | --- |
| Parent HEAD | `562639de15818fa89cbdae2c4a62879ac96b9b32` |
| MCP submodule pin | `7c9e4aac49f3954a54dd2375b1cb2442353c5420` |
| MCP message | Accept late_result_transform in the core collaboration dispatch stub. |
| Woodpecker at review start | pipeline **360** running on `562639de15` |
| Isolated worktree A | `C:/Users/Rchie/Music/FreeCADModeling/pr52-gui-runner` branch `pr52-gui-runner` |
| Isolated worktree B | `C:/Users/Rchie/Music/FreeCADModeling/pr52-camera-grid` branch `pr52-camera-grid` |
| Docker at review start | `docker ps` empty; existing volumes/containers **not** mounted or mutated |
| Isolation rule | new containers only (`--rm`, unique names); no chair/MCP sockets; no user cfg |

## Review vs current diff (`origin/FreeCAD-start...HEAD`)

### Introduced bugs (this PR)

1. **GUI runner still false-greens discovery.** Pipelines **341–343** exited 0 in 7–8s with an empty module list. Current `run_gui_tests.py` still returns 0 when listing exits 0 and parse yields no units, and when no name contains `Gui`. Failed listing is only preserved if parse is also empty.
2. **`TestSketcherGui` aggregate dropped.** Expansion to 7 class units replaced the registered suite. Pipeline 336’s passing e2e ran the aggregate. A class-only run can miss cross-class state and can false-green if `FreeCAD -t ClassName` completes 0 tests.
3. **No completed-test gate.** A module that exits 0 without `Ran N tests` (N>0) is treated as success. That is how an unknown split unit or a listing-parse miss stays green.
4. **Inconsistent finite-volume guards (this PR).** `isUsablePickVolume` rejects non-finite width/height/depth. `ViewProviderSketch::getPickedPointsOnRay` still uses `<= 0` only, so Inf extents still reach `SoRayPickAction`.
5. **Coin field edit on early return (this PR).** `createGridPart` null-checks `vertex.startEditing()` then `coinRemoveAllChildren` without `finishEditing()`. `numVertices.startEditing()` is still unchecked. gdb pipeline 358: write `-inf` through a null `SbVec3f*`.
6. **Grid/camera recovery missing.** Inf extent skips drawing (no crash) but does not restore a finite camera/grid. Pipeline 359 then failed real tests, not SIGSEGV — crash-avoidance ≠ valid view.
7. **MCP sync fake applies replay transform.** `test_native_collaboration_api._dispatch_gui` runs `late_result_transform` inline. Production `dispatch_gui` returns the raw `dispatcher.submit` result; transform is for late/replay (`build_replay_on_complete`). Core-test fakes of the form `lambda callback: callback()` still reject kwargs (`benchmarks/runner.py`, several unit tests).

### Existing hazards (not introduced here; still in the crash path)

- Coin `SoRayPickAction` on a degenerate frustum (uninitialized ray unless `COIN_DEBUG`).
- `short(std::roundf(normalized * pixels))` is UB if `normalized` is Inf/NaN (now mostly gated by pick-volume, not at the conversion).
- `setCameraType` historically no-op’d a null camera (fixed in this PR; still needs a finite height after create).

### Unproven hypotheses (do not treat as fact)

- Coin 4 / pivy #48 is the only reason 336 passed and 350+ crashed. 336 was a different binary (`0cc28873`); not re-run on this revision.
- Hiding `BaseSketch` in `TestExternalFacePreselection` is a product fix for #28639. Evidence only shows Vertex2 beat Face under vertex-over-face pick priority on pipeline 359.
- `convertToNURBS` no-op for internal BSplines is required beyond the GUI assertion; no App-level test existed before 562639de15.

## Three demonstrated GUI-runner false-green cases

| # | Evidence | Required behavior |
| --- | --- | --- |
| 1 | WP 341–343, ~7–8s, empty list, exit 0 | Preserve failed discovery (non-zero) |
| 2 | `if not gui_tests: return 0` | Require expected suites (the 13 modules WP 336/359 actually run) |
| 3 | Split `TestSketcherGui` + no `Ran N` check | Require completed tests; keep aggregate `TestSketcherGui` |

## Camera/grid numeric / Coin / recovery

- First invalid state to capture under UBSan: `short(roundf(INFINITY * 1024.f))` and null `SbVec3f::setValue`.
- Then failing regression on helpers, then production wiring.
- Recovery: Inf/non-positive camera extent falls back to a finite grid extent so a grid can be planned (not only skipped).

## Isolated tests this turn

| Gate | Image / command | Result |
| --- | --- | --- |
| GUI runner | `python:3.12-slim` `python -m unittest test_run_gui_tests -v` | **19 tests OK** |
| Inf→short first invalid state | `freecad-ci-deps:24.04-gdb` g++ `-fsanitize=undefined` on old conversion | scaled=Inf, `short` **32767** (no UBSan trap; wrong pixel) |
| Camera/grid recovery | same image, `tests/standalone/test_camera_grid_guards.cpp` | **ok** `recovered_vlines=30 recovered_nlines=60` |
| MCP sync fake | `python:3.12-slim` import helper | **ok** (transform not applied) |

Containers: `pr52-a-gui-runner-tests`, `pr52-b-camera-grid-guards`, `pr52-b-ubsan-old-file`, `pr52-mcp-sync-python`. No chair volumes, no live MCP sockets.

## Woodpecker

- Do **not** merge until `ci/woodpecker/pr/ci` is success for the **pushed** revision that contains these files.
- Pipeline 360 is `562639de15` (pre-runner/recovery). Treat it as evidence of that revision only.
