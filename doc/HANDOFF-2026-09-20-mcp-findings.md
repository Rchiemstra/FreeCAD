# Handoff — FreeCAD MCP findings campaign (2026-09-20)

Self-contained state for the next agent. It supersedes
`HANDOFF-2026-09-19-mcp-findings.md`, which stays for the history of parts 1–3 and for the
D-09 / D-23 evidence details. Read `CLAUDE.md` first; it overrides `AGENT.md`.

## 1. TL;DR

- freecad-mcp `fix/mcp-limits` = **`6d816dd`**, contains `main` (merge `cca3873`).
  FreeCAD `fix/mcp-limits` = **`b8d088c`**, pins MCP `6d816dd`. Everything is pushed and pulled
  in WSL; both checkouts are clean apart from long-standing untracked scratch files.
- FreeCAD `FreeCAD-start` is still `31e3b69`. **No PR is open yet** — `gh` is not logged in.
- Unit suite (Docker, `-m unit`): **8 failed, 12 087 passed**. Those 8 are exactly the tests that
  fail on freecad-mcp `main` itself, so the branch adds no failures. The old "17 baseline" from
  the 2026-09-19 doc is obsolete.
- FreeCAD was rebuilt with the D-29 fix: 0 warnings, `App_tests_run
  --gtest_filter='GenericIsolatedRecompute*'` 21/21. No C++ change since, so the running build
  matches the source.
- **Nothing is closed.** No finding has had an independent review.

## 2. Branch state

| Repo / branch | Commit | Contents |
|---|---|---|
| freecad-mcp `main` | `818caca` | upstream of the branch; merged in |
| freecad-mcp `fix/mcp-limits` | `6d816dd` | `cf34f8b` (D-23/D-25/D-27/D-15 caller path and earlier) → `73b4f02` D-14 → `3b3a89d` D-30 → `cca3873` merge `main` → `99b8d9f` D-22 → `826b5a9` D-26 → `6d816dd` fixes for the 6 branch-only failures |
| FreeCAD `FreeCAD-start` | `31e3b69` | untouched; changes only through a PR |
| FreeCAD `fix/mcp-limits` | `b8d088c` | 4 older commits (pins + CI gates `96d0e3a`) → `9847041` `doc/pr` → `9250403` **D-29 C++ fix** → `d5e74d5` / `9e91227` pin bumps → `b8d088c` PR text note |

Left behind on purpose: a Windows stash `stash@{0}: D-22 D-26 before merge` (its content is
committed — drop it whenever), and the pre-existing worktree
`~/freecad-test/worktrees/mcp-limits-runtime` at `18b1436`.

## 3. Findings

`Native` = live GUI through MCP tools, evidence = raw response fields + a state check.
Details of the older evidence are in §2 and §8.3 of the 2026-09-19 doc.

| ID | What | Code | Native | Next |
|---|---|---|---|---|
| D-01 | GUI timeout commits server-side | unchanged | – | needs v2 request status or an adaptive budget |
| D-02 | v1/v2 capability gap | config | – | single protocol (§7) |
| D-02b | 30 sketch tools had no v1 fallback | fixed | 3/3 | review |
| **D-03** | revolve `Symmetric` | fixed (`SideType`) | **3/3** 90/120/60°, bbox symmetric about the sketch plane (±14.142 / ±17.321 / ±10.000 = 20·sin(θ/2)), volume = analytic | review |
| D-04 | `Body:FaceN` attach hangs | fixed | 3/3 | review |
| D-05 | bbox/volume on App::Link | fixed | 3/3 | review; see the D-30 follow-up below |
| D-06 | `get_object` missing → succeeded | fixed | 3/3 | review |
| D-07 | bow-tie profile accepted | fixed | 3/3 | review |
| D-08 | negative length | guard added | 3/3 | review |
| D-09 | rollback coverage | – | pad/pocket/expressions/constraints 3/3 each | review |
| D09-E4 | pad/pocket `strict` forwarding | fixed | 3/3 | review |
| D-10 | no screenshots | unchanged (v2-only) | – | with §7 |
| D-11 | `get_objects` pagination | fixed | 3/3 | review |
| D-12 | alias echo | fixed | 3/3 | review |
| **D-14** | App::Link array layout | **fixed** `73b4f02` | **3/3** + refusal case | review |
| **D-15** | modal dialog → generic timeout | fixed | **3/3** (2 test modals + a real "Document Recovery"), raw `GUI_TIMEOUT_BLOCKED_BY_MODAL_DIALOG` | review |
| D-16 … D-19 | see 2026-09-19 doc | fixed | 3/3 each | review |
| D-20 | rejections lifted from TRANSPORT_UNCERTAIN | fixed | 3/3 | review |
| D-21 | Link/Placement props always rejected | fixed | 3/3 | review |
| **D-22** | unknown tool arguments ignored | **fixed** `99b8d9f` (user chose: reject) | **3/3** | review |
| D-23 | apply-refusal reported as uncertain | fixed | 3/3 | review. Nit: `native_message` still says `_AbortPadFeatureMutation` |
| D-24 | `open_document` on v1 → reconciliation message | not fixed | – | list as a v2-only refusal / fix the message |
| **D-25** | 12 typed feature tools failed on v1 | fixed | **3/3** | review |
| **D-26** | pocket without a base solid adds material | **fixed** `826b5a9` (user chose: refuse) | old behavior ✓ (+1000 mm³ committed); **fix 3/3** | review |
| D-27 | pad/pocket committed without a real solid | fixed | pocket 3/3, pad 3/3 | review |
| D-28 | `set_expression` divide-by-zero hangs FreeCAD | not investigated | seen ×1 | reproduce in a fresh session |
| **D-29** (new) | every PartDesign revolve/groove refused by the isolated recompute | **fixed** FreeCAD `9250403` | **3/3** | review |
| **D-30** (new) | placed object's Placement applied twice in world-frame shape resolution | **fixed** `3b3a89d` | **3/3** + old behavior ✓ | review |
| **D-30b** (new, open) | `find_faces`/`find_edges` transform subshape centres by the global placement although `obj.Shape` is already placed | not fixed | – | same class as D-30; its test fake (unplaced box + placement) hides it |
| F-05 | build identity | fixed | 3/3 | review |
| obs. | `set_expression(Pad.Length = "-5 mm")` commits | – | ×1 | should it be rejected like D-08? |
| obs. | pad of two partly overlapping rectangles gives a suspicious sum-like volume | – | ×2 | inspect validity |

### D-29 (FreeCAD C++)
`Revolved::execute()` calls `updateAxis()`, which rewrites `Base`/`Axis` from `ReferenceAxis`.
Both predate `Prop_Output`, so `GenericIsolatedRecompute` refused every Revolution and Groove:
"undeclared property side effect: Rev1.Axis" (clean rollback, nothing committed). Fixed by
declaring exactly those two members on `PartDesign::Revolved` ancestry, like the Helix coupled
parameters (`8e4aedf`). Test: `tests/architecture/test_generic_isolated_recompute.py::
test_revolved_axis_is_a_declared_recompute_output`. D-25 hid this: before D-25 the revolve never
reached the addon. That file has one **pre-existing** failure,
`test_private_feature_execution_has_only_full_recompute_and_detached_friend_callers`
(4 `_recomputeFeature` callers, the test expects 3) — not ours, not investigated.

### D-30 follow-up (not fixed)
The broken-Link branch of `resolve_global_shape` multiplies the Link placement onto the target's
already-placed shape. With `LinkTransform=false` FreeCAD *replaces* the target's placement, so a
broken proxy of a placed target is still wrong. D-05 passed only because `Leg` has an identity
placement.

## 4. Merge with `main` (`cca3873`)

10 files conflicted (42 hunks). Resolutions, all "keep both":

- `collaboration_api.py`: main's `_settle_pending_recompute` and `recompute=` pass-through, plus
  D-23's exact-exception rollback proof. Main's looser mapping (any exception after the callback
  started counted as a rollback) was dropped; main's own test for it still passes.
- `pad_feature.py` / `pocket_feature.py`: main's `_profile_diagnostics` and single
  `SKETCH_PROFILE_NOT_CLOSED` code (main's e2e tests assert the message), plus D-07's
  self-intersection refusal. Pocket keeps D-27 **and** main's material-delta checks. A
  merge-introduced `NameError: shape` was fixed.
- `sketch_create.py`: main's positional `attachment_offset`; the branch's refusal of an offset
  without `attach_to` moved into the request builder, so it still fails before any object exists.
- `find_edges.py` / `find_faces.py`: main's (adds `direction_approx`).
- `diagnostics_shape_actions.py`: the branch's filters (a radius filter excludes radius-less
  subshapes), plus main's `geom_type` field and aliases.
- `create_object.py`, `policy_runtime.py`: main's helpers with the branch's postcondition.
- RPC contract snapshot: structural 3-way merge per key (`scratchpad/json3way.py`); only
  `find_edges`/`find_faces` parameters collided and take main's. Regenerating from the handler
  signatures changes nothing.

`6d816dd` then fixed the 6 unit failures the branch carried against main: two public-surface lint
violations (`gui_core` private import aliases; `measure_io_actions` no longer re-exports
`resolve_global_shape`), the frozen listener examples for the typed `get_object` and the F-05
identity (now pinned in the test like the pid), the typed-handler count (135), the failure-example
count (176) and a fake token missing `begin_mutation`.

## 5. Test recipes

- **Docker unit suite (Windows)** — Docker cannot mount `\\wsl.localhost`, so mirror the tree
  first and mount the copy:
  ```powershell
  $r='C:\Users\Rchie\Music\FreeCAD\tools\mcp\freecad-mcp'   # or the WSL path
  $w='<scratch>\dkw'
  foreach ($d in 'src','addon','tests','ci','scripts') { robocopy "$r\$d" "$w\$d" /MIR /XD __pycache__ .pytest_cache /NFL /NDL /NJH /NJS /NP /MT:16 | Out-Null }
  $v=@('-v',"${w}\src:/workspace/src",'-v',"${w}\addon:/workspace/addon",'-v',"${w}\tests:/workspace/tests",'-v',"${w}\ci:/workspace/ci",'-v',"${w}\scripts:/workspace/scripts")
  docker run --rm @v freecad-mcp-tests -p no:cacheprovider -m unit -q --tb=no -rf -n auto
  ```
  Full run ≈ 10–15 min → background it. The image has real `FreeCAD`/`Part` (1.1.3), so tests may
  use OCCT. Never edit the mirror while a run is in flight; the result becomes meaningless.
- **FreeCAD build (WSL, background)**: `export PATH="$HOME/.pixi/bin:$PATH"; cd ~/FreeCAD &&
  python3 build_freecad.py`. `-Werror`: fix warnings, never disable them.
  C++ tests: `cd ~/FreeCAD/build/release && ./tests/App_tests_run --gtest_filter='...'`.
- **Architecture tests** run on Windows too: `python -m pytest tests/architecture/... ` in the
  FreeCAD checkout (≈2 min).

## 6. Environment gotchas (beyond CLAUDE.md)

1. `git fetch origin <branch>` inside the **submodule** does not update `origin/<branch>`; the
   tracking ref went stale by a day and looked like a force-push. Use explicit refspecs:
   `git fetch origin +refs/heads/main:refs/remotes/origin/main`.
2. A `git worktree` under the session scratchpad fails on Windows ("Filename too long"); create
   worktrees in WSL (`~/mcp_merge`) or a short path.
3. `fc_recovery_aside.sh` needs `find -maxdepth 7`; a clean `kill -TERM` can still leave
   `~/.cache/FreeCAD/v26-3/Cache/FreeCAD_Doc_*/fc_recovery_file.xml`, and the next start then
   blocks on a modal "Document Recovery".
4. The PowerShell tool refuses a command that contains both `Remove-Item` and a `robocopy /MIR`
   flag (it reads `/MIR` as a path). Split them into two calls.
5. `pytest` truncates long diffs even with `-vv`; dump the values to a file from a temporary line
   in the test copy instead.
6. `execute_code` cannot open a document while another document is active ("document lifecycle
   changes are unavailable during the prepared commit"); close the others first.
7. `execute_code` reports `saved: true, saved_documents: [...]` when it only means "not dirty" —
   the file is not written. Verify with sha256 before trusting it.
8. `wsl.exe` `0x8007274c`: retry in a loop; it came back often today.

Helper scripts from this session live in the session scratchpad: `rpc.py` (raw JSON-RPC probe),
`state.py`, `fc_kill.sh`, `fc_wait.sh`, `fc_recovery_aside.sh`, `d15_raw.sh`, `show_conf.sh`
(print conflict hunks), `take_ours.py` / `take_theirs.py`, `json3way.py`, `regen_snapshot.py`.

## 7. Open work, in order

1. **PRs** (needs `! gh auth login` by the user). Order is fixed by CLAUDE.md: freecad-mcp
   `fix/mcp-limits` → `main` first; then FreeCAD `fix/mcp-limits` → `FreeCAD-start` with the pin
   on a commit that is on MCP `main`, or the `mcp-pin-on-main` gate fails by design. Texts are in
   the FreeCAD checkout: `doc/pr/freecad-mcp_fix-mcp-limits_to_main.md` and
   `doc/pr/FreeCAD_fix-mcp-limits_to_FreeCAD-start.md`.
2. **Independent review** of every finding — nothing is closed without it.
3. Open findings: D-30b, D-24, D-28, the two observations in §3, the new observations in §9,
   and the "agent must be able to clear a blocking dialog" requirement (§5b of the 2026-09-19
   doc).
4. Recommended: converge on one RPC protocol (v2). D-02b, D09-E4, D-20, D-24 and D-25 are all
   v1/v2 dual-path bugs. It changes the user's launch setup — confirm first.

## 8. Native test model

`/home/msi/freecad-test/campaign_2026-09-19/Chair.FCStd` (sha256 `d546f5ed…`) is the untouched
backup. The rebuilt model is saved beside it as **`Chair_rebuilt.FCStd`** (sha256 `8257ad71…`):
3 leg App::Links, `SeatClearance` (6 × Ø4 through the seat), 6 `SubtractiveCone` countersinks,
`LegPilot` and `BackPilot` (Ø2.5 × 22, pocketed), a revolved 4.0 × 40 mm countersunk screw and a
6-element `Screws` Link array. Every body is a valid single solid whose volume matches the
analytic value to 0.000 mm³; each screw ∩ seat = 0.0 and ∩ leg/backrest = 134.156 mm³.
Live documents were dropped on the last restart; test documents are disposable (the user said so).

## 9. Native continuation (2026-09-20, later session)

Runtime: authenticated isolated RPC v2 on `127.0.0.1:9876`; addon checkout `6d816dd`, clean.
FreeCAD restarted once during the run, from pid `552551` to `569504`; the MCP session refreshed
and verified the new addon runtime `f8c19467-8368-4d0e-9947-d70e0a61b713`. The compiled FreeCAD
identity remains `9847041`, as already explained in §1/§2. The unsaved disposable test documents
`D30Resume_20260920` and `MergedSmoke_20260920` were closed after recording the evidence; no
documents remain open.

### Completed native evidence

- **D-22 3/3:** `body_create`, `create_object`, and `sketch_create` each received a distinct
  unknown top-level argument. Each was rejected before dispatch with the unknown name and accepted
  arguments listed. Independent worker snapshots showed that none of `D22Body1`, `D22Box2`, or
  `D22Sketch3` existed (zero objects after every attempt).
- **D-26 fix 3/3:** three independent empty Bodies, each with a closed circle sketch. Raw addon
  responses: `outcome=rejected`, `committed=false`, `retry_safe=true`,
  `native_status=ApplyFailed`, `rollback_succeeded=true`, `rollback_failed=false`,
  `error_code=POCKET_NO_BASE_SOLID`. Worker state: no Pocket objects, all Body shapes null with
  volume 0.0, all source sketches retained.
- **D-30 runs 2–3, now 3/3 total:**
  - 10×20×30 box, base (5,7,11), 90° about X → world bbox
    `(5,-23,11)..(15,7,31)`, dimensions `(10,30,20)`, volume 6000.
  - 8×12×16 box, base (-4,20,3), 90° about Z → world bbox
    `(-16,20,3)..(-4,28,19)`, dimensions `(12,8,16)`, volume 1536.
  In both runs the raw `bounding_box` response exactly matched `obj.Shape.BoundBox` in an
  independent worker snapshot, with `frame=world` and `used_linked_object=false`.
- **Merged-addon smoke:** Pad committed as one valid solid (3000.0 mm³); Pocket committed after
  reversing toward the solid (one valid solid, 2858.6283305884594 mm³, correct Body Tip);
  `sketch_create` preserved `XY_Plane` support plus attachment offset base `(2,3,4)` and 30° Z
  rotation; 90° Revolve committed as one valid solid (2356.194490192345 mm³, correct Body Tip).
  The first Pocket direction was safely rejected as `ZERO_MATERIAL_DELTA` with rollback proof.

### New observations (not fixed)

1. A D-26 refusal sent through the authenticated v2 MCP `pocket_feature` path was flattened to
   `INVALID_POCKET_FEATURE_RESPONSE`, `outcome=uncertain`, `committed=null`, with nested generic
   `RPC_V2_ERROR`. Calling the same addon method directly exposed the correct structured
   `POCKET_NO_BASE_SOLID` rejection and rollback proof. This is a v2 response-lifting defect in
   the same family as D-23; investigate before calling D-23 reviewed/closed.
2. A typed `create_object` committed `D30Box2`, then its automatic screenshot failed with
   `RemoteDisconnected`; the FreeCAD process exited and the next call got `ConnectionRefusedError`.
   No new `.fcrash` appeared. The isolated launcher restarted cleanly after port 9876 left
   `TIME_WAIT`. The same object creation through raw addon RPC did not reproduce the exit.
3. `get_object` for a known-missing object in an otherwise empty document failed in presentation
   code with `personal view focus has no renderable bounds` instead of returning `OBJECT_NOT_FOUND`.
