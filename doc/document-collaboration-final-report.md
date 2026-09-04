# Document Collaboration Architecture Final Report

## Status

**GO.** CC-WP00 through CC-WP15 are complete. The collaboration, isolated geometry, coordinated recompute, GUI/MCP ingress, revision validation, and change-aware save paths passed their package gates and final Windows/Linux staged acceptance.

Completed at 2026-09-04T11:29:29Z on `fix/change-aware-save-mcp-autonomy`.

## Delivered architecture

- `DocumentCollaborationService` remains the typed model-intent facade.
- `DocumentCommitCoordinator` is the sole supported live model-transaction owner; the exhaustive ingress inventory and architecture scans have no unclassified bypass.
- `PreparedEditExecutor` is restricted to trusted lightweight detached value work.
- App-owned `GeometryJobManager` dispatches bounded isolated geometry jobs through `FreeCADCmd` using the FCG/1 archive and trusted parent-side adapters.
- Boolean, Sweep, Fillet, and generic opted-in recompute work execute without live `App::Document` or `DocumentObject` pointers in workers.
- Per-document recompute uses dependency-ordered immutable capture, cancellation/coalescing, stale-result rejection, and coordinator-owned commits.
- GUI personal state remains conflict-neutral while GUI model intent and remote MCP intent use the collaboration route.
- Save reports `RECOMPUTE_PENDING` while recompute is unresolved, then `Written`, followed by `Unchanged` for an unchanged document.

## Source and runtime identities

| Item | Identity |
|---|---|
| WP15 baseline | `62768c9744d44e4e25540c6a238cac420bde41cb` |
| Stage C enablement | `6059dc68d7503d23eea35b105d74164768c35a09` |
| Final tested parent / upstream | `7880bb81279458b3df19b675f19fe4eac432dd96` / same |
| Nested HEAD / upstream / gitlink | `4472c30e01e2926ed11fa321f5e723af3342d41c` / same / same |
| Reviewed/tested WP15 diff | SHA-256 `fc4ba704202cf009e187dc40a75ed33fbaaf74acb8c92af7594d2ab958c4b988`, 30,994 bytes, 8 paths |
| Stage runner | SHA-256 `57e635bd66326b40262f29e38da59c7f6a339fd5c7f6ba25f752fec4ec6e6c97` |
| Linux FreeCAD | SHA-256 `b5fa33b0cebe282ce92442aa4828cfd769fd094bdaf6caae91bcdb90dc99e6c1` |
| Windows FreeCAD | SHA-256 `5389013cbd7773c5443a79a111a56179fa1a5a1591449727d3db4d75ef2a8766` |
| Windows FreeCADApp / Base / Gui | `5f62ad63f0b38a3c81c083a8b73aed2746ab207b3c88da95657b40804844d615` / `05b8a78273f50f835da268215149aed855fe017df3eeb09aa8c92a08cbf9016d` / `420ae9b8f1be319af3389003d7c9b5683a964cc19f490c8ba37d191ef413664a` |
| Executed Linux image | `sha256:b7f4360dd6c8cfd69499310f5e225afb89623e2943eaa6fc72cae68fdc515b5c` |

The mutable Docker tag later resolved to `sha256:b34e0e1ecabafa22c760850548b7e8239c4a3428c7d4084927ed5d1109f5142f`; the accepted Linux packets record and used the immutable `b7f4360d…` ID.

## Qualification results

All reported return codes below are real terminal process results. Packet validation was rerun as a data-integrity check; no stress suite was repeated during closeout.

| Platform | Stage | Source | Cycles / saves | Test result | Packet SHA-256 | Evidence SHA-256 |
|---|---:|---|---:|---|---|---|
| Linux Docker | A | `121d633d84…` | 10 / 5 | RC 0, `PASSED`, 585.48 s | `2afe310bc1454eb4022719c68bb45db4038f0e9ae9ff23815cb9f21f41af2ab8` | Recorded in CC-WP14 iteration 019 |
| Windows | A | `121d633d84…` | 10 / 5 | RC 0, `PASSED`, 1,106.34 s | `eafd0cfee525c5f775f8c616ec3b3247a059813786d8ce3fe49809b200717078` | Recorded in CC-WP14 iteration 019 |
| Linux Docker | B | `7880bb8127…` | 50 / 20 | RC 0, `PASSED`, 726.327 s | `51d49a71b4dea80e1a829b7fe7e4ad460daf1ac2ef0d0e39f9d75b4772d28a70` | `b6e576eee307c0abad7102db7f1f036f707c7738220a65b7555158156cbb26a8` |
| Windows | B | `7880bb8127…` | 50 / 20 | RC 0, `PASSED`, 4,515.404 s | `1c22977ba5f963cf920f91e2a2c7619dcbe0993fd8f5bea0b40f423c1d046abb` | `52a8f7d568cf6be4fa686a829db588732f732aa6ff45460af28f8ace567e4902` |
| Linux Docker | C | `7880bb8127…` | 500 / 100 | RC 0, `PASSED`, 2,431.303 s | `0a4c7a286db920d8b8909ef7286a49e5aeb2de519bc08bfad1b683e10b66c4b9` | `3b8ad5586a40250aa2efed68a0f8701b3eb03b8bd1e7d2878590e0ab1477c1a0` |
| Windows | C | `7880bb8127…` | 500 / 100 | RC 0, `PASSED`, 72,246.637 s | `4f1ed6be37e740e7d64b3c67a5e916705277c2ec3601eae03413663a6cf536ab` | `ea8cd52da00b3487a202047a7bdbf1d142fb0256c84ef701e3265233ab0e7abb` |

Each final B/C packet has one JUnit test, zero failures, zero errors, zero failed checks, and graceful shutdown with `forced: false` and no stalled shutdown stage. The Windows C session ran from 2026-09-03T15:20:22Z to 2026-09-04T11:23:32Z and closed both documents, RPC admission, workers, listener, window, and process within its 60-second shutdown deadline.

Stage A was qualified on both platforms at CC-WP14 source `121d633d84…`. The only later changes were qualified test-harness paths classified as binary-independent; the FreeCAD binaries did not change. Stage B was repeated on both platforms at `7880bb8127…`, so both Stage C runs use the exact successful Stage B parent, nested, gitlink, and per-platform binary identities.

The affected offline packet at the final implementation fingerprint passed 1,180 tests with three expected live-test skips and exit code 0. Separate stream/publication edge tests passed 7/7. Python compilation/import checks passed without writing bytecode.

Evidence is retained under:

- `results/collaboration-completion/2026-09-02/CC-WP14/iteration-019/` for final Stage A qualification and the earlier CC-WP14 A/B tranche.
- `results/collaboration-completion/2026-09-03/CC-WP15/iteration-002/` for final-source Linux/Windows B/C qualification.

## Retained failure and resolution

The first Linux C attempt remains unchanged under `results/collaboration-completion/2026-09-03/CC-WP15/iteration-001/linux-c/` and receives zero credit. CPython 3.12.3 exited with signal 11 at 486/500 cycles and 98/100 saves; Docker reported `OOMKilled:false`. Its evidence SHA-256 is `f44c804028db9b309ffa821f5ea3c6a8a5491ba070f11ada4444b27530b4bdf0`.

The cause was repeated construction of a giant serialized JSON string during checkpoint publication. Commit `7880bb81279458b3df19b675f19fe4eac432dd96` streams compact JSON to a unique same-directory temporary file, flushes and fsyncs it, then atomically replaces the checkpoint. Failure cleanup leaves the prior published checkpoint intact. Both Linux and Windows C subsequently completed all 500 cycles and 100 saves.

## Seven completion criteria

| Criterion | Result |
|---|---|
| Compiles and imports | Proven by final no-bytecode compilation/import checks and actual runner execution |
| Does not crash or hang | Proven by terminal RC 0 on final B/C runs, explicit deadlines, and graceful shutdown evidence |
| Passes its tests | Proven by the 1,180-test offline packet, edge tests, live A/B/C tests, and packet validators |
| Handles supported edge cases | Proven for corrupt/incomplete evidence publication, conflicts, stale results, cancellation, lifecycle, save/copy/unchanged behavior, and cleanup |
| Breaks nothing existing | Proven by the affected regression packet plus Windows/Linux staged runs using the qualified binaries |
| Needs no undocumented manual steps | Exact runner commands, environment, immutable image, artifacts, and validation inputs are embedded in each execution packet |
| Performs the claimed operation | Proven by live Windows/Linux A/B/C evidence, including 500 view/mutation cycles and 100 save cycles per Stage C run |

## Final review and aftermath

An inline adversarial review of the complete tested eight-file diff and all final packets found no unsupported claim, weakened assertion, hidden synchronous fallback, or remaining CC-WP15 product defect. No independent subagent verdict is claimed because the user explicitly directed this phase to run without subagents.

After Windows C, no test-owned pytest, Pixi, FreeCAD, launcher, coordinator, or runner process remained; ports 9875 and 9876 had no listeners; and no Stage container was present. Unrelated parent and nested working-tree edits, untracked files, numeric root receipts, and external AutoCurtains services were preserved and excluded from the closeout commit.

## Final decision

The documented architecture and acceptance targets are complete. Overall state is **GO**.
