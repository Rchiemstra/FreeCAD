# Evidence-System Hardening Progress

## Status

**IN PROGRESS — scope corrected to require tracked FreeCAD/MCP integration.**

This record is Orchestrator-owned. Diagnostic-001 through Diagnostic-034 and
all existing evidence are immutable predecessors. Diagnostic-044 work is
offline-only: no Docker daemon, FreeCAD process, live stage, production
authorization, or production signing is permitted.

## User scope correction — 2026-08-24

The earlier results-only delivery interpretation is superseded. Diagnostic-044
is a reference implementation and mutation fixture; it is not the shipped
implementation. Completion now requires the hardened behavior and its tests to
live in tracked FreeCAD/MCP locations and to run without the ignored
Diagnostic-044 directory.

The controlling steering prompt is
`doc/evidence-system-hardening-tracked-integration-steer.md`.

Expected ownership, subject to Sol's architecture review:

- tracked MCP implementation:
  `tools/mcp/freecad-mcp/src/freecad_mcp/evidence_system/`;
- focused tracked MCP tests:
  `tools/mcp/freecad-mcp/tests/evidence_system/`, split into startup,
  preflight, child reconciliation, terminal authorization, lifecycle, and
  Docker-contract modules;
- one tracked parent integration boundary:
  `tests/gui/part3/test_evidence_system_integration.py`.

No tracked source or test may import from ignored `results/`, inject the
Diagnostic-044 path, or require its files at runtime. No evidence writer,
completion verdict, commit, or push is authorized from an ignored-only
candidate.

## Locked policy

| Policy | Value |
|---|---|
| Run ID | `P3-WP27` |
| Attempt ID | `linux-startup-diagnostic-044` |
| Sequence | `44` |
| Replay identity | One signed 256-bit nonce plus exact single-use output root |
| Authorization lifetime | 15 minutes |
| Preflight maximum age | 60 seconds |
| Future-clock tolerance | 5 seconds |
| Work package | `results/part3-orchestration/2026-08-22/P3-WP27-prelive/linux-startup-diagnostic-044` |
| Runtime output | `results/part3-orchestration/2026-08-22/P3-WP27-prelive/linux-startup-diagnostic-044-runtime` |
| Offline evidence | `results/part3-orchestration/2026-08-22/P3-WP27-prelive/linux-startup-diagnostic-044-offline-evidence` |
| Commit policy | Tracked MCP production/tests, one scoped FreeCAD integration test, and documentation after Luna green plus Sol `NO_ISSUES`; Diagnostic-044 remains an ignored reference |
| Docker policy | Existing `/tmp` semantic validator frozen; offline regression only |

## Revised tracked-integration plan

1. Freeze the current Diagnostic-044 bytes as a reference candidate; do not
   treat further ignored-only green results as delivery.
2. Sol maps each invariant to its actual tracked MCP or FreeCAD owner and
   rejects duplicated/test-only production validators.
3. Terra adds failing tracked tests in the owner repository before porting the
   corresponding production behavior.
4. Keep focused tests in six scoped MCP modules and one parent FreeCAD
   integration module; do not create one monolithic diagnostic test file.
5. Luna runs focused tracked tests, the parent integration boundary, frozen
   offline Docker regressions, and the relevant full suites.
6. Prove independence by running the tracked tests with Diagnostic-044 made
   unavailable and by searching for forbidden ignored-results imports/path
   injection.
7. Sol reviews the exact Luna-tested tracked hashes. Findings return to Terra.
8. Preserve unrelated parent and nested worktree changes. Stage narrowly in
   each repository, inspect cached diffs, then commit/push only after
   `NO_ISSUES`.

Environment policy frozen for Diagnostic-044:

- Host outer/executor variables: `SystemRoot`, `WINDIR`, `ComSpec`, `TEMP`,
  `TMP` only.
- Docker `--env` variables: `HOME`, `APPDATA`, `XDG_CONFIG_HOME`,
  `XDG_CACHE_HOME`, `XDG_DATA_HOME`, `FREECAD_USER_HOME`,
  `FREECAD_USER_DATA`, `FREECAD_USER_TEMP`, `LD_LIBRARY_PATH`, `DISPLAY`, and
  `QT_QPA_PLATFORM` only.
- Image-provided environment additions are forbidden; inspected `Config.Env`
  must equal the explicitly authorized rows.
- Python startup variables and every undeclared variable are forbidden.
- Host Git, Docker, and PowerShell plus container Python, Xvfb, and GDB use
  exact absolute paths in the command contract; no `PATH` lookup is accepted.

## Starting repository state

| Field | Value |
|---|---|
| Captured UTC | `2026-08-24T14:14:50.7549345Z` |
| Branch | `fix/change-aware-save-mcp-autonomy` |
| Parent HEAD/upstream | `249ba1d652c0c4218b0715065b9fbf666ddcad03` / same |
| Parent ahead/behind | `0/0` |
| Nested HEAD/upstream | `e734f525e2dee47fb39a3b1bc552ca37766e48d4` / same |
| Existing state | Dirty user work present in parent and nested repositories; it is outside the Diagnostic-035 staging allowlist and must be preserved. |

Pre-existing modified paths include `.gitignore`, the Part 3 tracker/design and
GUI harness files, plus a dirty nested `tools/mcp/freecad-mcp` worktree.
Pre-existing untracked documentation, logs, tests, and lock-anchor artifacts
are user-owned and excluded from this work.

## Preservation baseline

The Orchestrator created a deterministic recursive predecessor manifest in
the fresh Diagnostic-044 work package before Terra edited production code. It
covers every `linux-startup-diagnostic-001` through `-043` directory and
every matching offline-evidence directory by relative path, entry type, byte
size, and SHA-256. The final gate must reproduce the manifest exactly.

Baseline manifest: **873 entries**, canonical entries SHA-256
`63a82b3c2d5862939648f6a2518fbc7f70a3b135cdcbc6ed9c113b3bad4ac353`.

Freshness audit after Sol's handoff discovered that Diagnostic-035 through
Diagnostic-043 and their offline-evidence roots pre-existed this execution.
Diagnostic-035 was created at `2026-08-24T11:40:12Z`; the Orchestrator added
only its `baseline/` directory at `2026-08-24T14:15:32Z` before discovering
the collision. Nothing is deleted or repaired. Diagnostic-035 is now a
preserved, zero-credit collision attempt and must not receive further writes.

## Handoff log

| UTC | Cycle | Agent | Action | Result | Next |
|---|---:|---|---|---|---|
| 2026-08-24T14:14:50.7549345Z | 1 | Orchestrator | Captured repository baseline, locked policy, fresh paths, and predecessor-preservation requirement. | In progress; no live process or evidence mutation. | Sol high-reasoning review of Diagnostic-034. |
| 2026-08-24T14:16:37Z | 1 | Orchestrator | Created the deterministic predecessor manifest before production edits. | 647 entries across Diagnostic-001..034 and matching evidence; root `4c2b24a5...ce1`. | Await Sol findings, then hand exact scope to Terra. |
| 2026-08-24T14:19:02Z | 1 | Sol / Orchestrator | Freshness audit found pre-existing Diagnostic-035..043. The Orchestrator's new `baseline/` files collided with the preserved 035 root. | `POLICY_DECISION_REQUIRED`; 035 is frozen from this point with zero new credit. No Terra, Docker, FreeCAD, signing, or live work started. | Obtain authority for a genuinely unused sequence/path, recommended Diagnostic-044. |
| 2026-08-24T14:24:00.7517204Z | 2 | User / Orchestrator | User explicitly authorized Diagnostic-044. Verified work-package, runtime, and evidence roots are all absent. | Fresh authority accepted: attempt `linux-startup-diagnostic-044`, sequence `44`; 035..043 remain immutable predecessors. | Rebuild predecessor manifest through 043, then restart Sol review. |
| 2026-08-24T14:25:22Z | 2 | Orchestrator | Built the fresh 044 predecessor manifest before production edits. | 873 entries across Diagnostic-001..043 and matching evidence; canonical root `63a82b3c2d5862939648f6a2518fbc7f70a3b135cdcbc6ed9c113b3bad4ac353`. | Sol review of latest production baseline and locked 044 changes. |
| 2026-08-24T14:30:15Z | 2 | Sol | High-reasoning review of Diagnostic-043 and the 044 contract. | `PROBLEMS`: ESH-044-001..006 cover trusted startup, semantic preflight, authorization v2, terminal snapshot ordering, child reconciliation, and create-only lifecycle. Diagnostic-043's 60 cases are retained; 65 new rows make 125 total. | Terra test-first implementation in fresh 044. |
| 2026-08-24T14:31:20Z | 2 | User / Orchestrator | User confirmed the plan remains valid and authorized start; Orchestrator froze the exact host/container environment and absolute executable-resolution policy. | Environment `POLICY_DECISION_REQUIRED` resolved for the offline candidate. | Terra implementation. |
| 2026-08-24T14:45:40Z | 2 | Terra | First test-first implementation pass on fresh Diagnostic-044. | Added schema 44, governed `hardening044.py`, structured validation, authorization/binding, child reducer, package verification, create-only lifecycle, ledger v2, and 65 named hardening rows. Compile and RFC self-test passed; complete suite remains red because retained 043 assertions still encode old relative-command/PYTHONPATH contracts. No runtime/evidence root created. | Luna authoritative offline failure packet, then Terra correction. |
| 2026-08-24T14:58:10Z | 2 | Luna | Cycle-1 offline tests on Terra hashes. | Compile rc0; startup 9/9, preflight 18/18, child 11/14, terminal 9/9, lifecycle 15/15; Docker regression 6/7; full suite 108/125 with 17 failures; RED incorrectly rc0 with `semantics_valid:false` and unrelated image acceptance. Package/cache aftermath clean; no runtime/evidence root. | Terra cycle-2 correction, including mutation-isolation and production wiring. |
| 2026-08-24T15:06:30Z | 2 | Terra | Cycle-2 primitive hardening. | Derived-result interface, expanded binding/auth, exact child names, hard-link publish, wider fresh gate, and binding-rich ledger added; 65 direct h44 rows pass. Terra explicitly reports actual preflight/run_outer/finalizer migration and exact `/diagnostic` separation unfinished; no Luna authorization. | Fresh Terra integration pass on the two remaining production blockers. |
| 2026-08-24T15:13:18Z | 2 | Terra / Orchestrator | Recorded the production-topology integration handoff. | Created exact 14-file `/diagnostic` sibling and independently frozen `/trusted/bootstrap.py`; rendered distinct read-only package/bootstrap mounts and writable 044 runtime output; added pre-execution stale rejection and create-only verdict publication. Exact source compilation passed. Full 125 and RED remain red because retained fixtures and RED baseline have not been migrated. Terra also left generated `__pycache__` entries inside the new 044 tree; no runtime/evidence root or live tool was used. Tested root/diagnostic core hash `aaeb88e4...39d7`, bootstrap `07055387...0886`. | Luna cycle-2 exact failure packet; all failures and cache contamination return to Terra. |
| 2026-08-24T15:18:00Z | 2 | Luna | Cycle-2 read-only offline regression on the integrated topology. | Source-byte compilation 11/11; focused startup 9/9, preflight 18/18, child 14/14, terminal authorization 9/9, lifecycle 15/15; frozen Docker semantic group 5/5 without a daemon. Full suite 108/125, rc1. RED incorrectly rc0: 17 base mutations and 34 rows instead of 50/99, baseline stops at `live`, and corresponding-guard isolation is false. Luna found root, diagnostic, and trusted `__pycache__`/`.pyc` contamination before testing. Runtime/evidence roots remain absent; no live tool used. Tested hashes include core `aaeb88e4...39d7`, bootstrap `07055387...0886`, hardening `549e7d40...f95e8`, tests `c7772b4f...853ef`, RED `bee9044a...56a76`. | Remove only verified generated caches, then Terra fixes all 17 retained-path failures and rebuilds RED semantics. |
| 2026-08-24T15:26:07Z | 2 | Terra | Cycle-2 security-boundary correction checkpoint. | Renamed the structured issue member to required `field`; made package verification recursive and reparse/symlink/bytecode/`.pth`/unknown/missing aware; enforced `-I -S -B` on container and mounted-child commands; synced governed bytes to `/diagnostic`. Source-byte compile 10/10 and focused schema-44 cases 65/65 pass; cache scan clean. Actual outer success still fails because no `diagnostic-execution.json` is produced, retained all-fields preflight stalls, and RED remains invalid at 17/50 and 34/99. Runtime/evidence roots absent. | Continue Terra on the actual executor publication, bounded preflight fixture, and RED rebuild; no Luna re-test yet. |
| 2026-08-24T15:35:40Z | 2 | Terra | Actual coordinator lifecycle correction. | Found that the child execution existed but lacked the post-cleanup `final_identity` binding required by runtime-chain validation; restored the pre-ledger bind update. Split final inventory lookup between immutable `/diagnostic` sources and governed `/out` artifacts. Actual child/outer success, timeout, stale-preserve, config, preflight, and Docker-version focused cases pass. Root and diagnostic copies synchronized; cache scan clean. Current core `4ecc96a6...020d`, run_outer `e9e1bf1c...6aee`, tests `0cc81be3...8049`. | Production auth-v2/ExecutionBinding/child-reducer wiring and RED 50/99 remain open; continue Terra before Luna. |
| 2026-08-24T15:53:55Z | 2 | Terra | Production schema-44 preflight slice. | The real preflight now constructs the exact schema-44 check packet, validates raw JSON with duplicate-key rejection, and binds configured/raw candidates, repository, image, package, bootstrap, commands, and execution identity. `run_outer` requires that structured result before executor launch; the retained core accepts only the explicit nested extension. Eighteen independently named production-gate mutations pass exact issue-field assertions and prohibit final PASS. Compile 5/5 and cache scan clean. Hashes: preflight `327c646f...3cf2`, core `9d9b0641...b380`, run_outer `e7f8b809...36c2`, hardening `2ccd22e0...fc56`, tests `f3e05eba...7a4d`. The retained all-field combined test exceeded three minutes without a result and was terminated; no timeout was enlarged. | Migrate the actual child producer/reducer and its 14 cases. |
| 2026-08-24T16:07:11Z | 2 | Terra | Production child reconciliation slice. | Config now owns the exact four-artifact registry. `run_diagnostic`, runtime-chain validation, and bundle validation all use `reconcile_child_results` as the sole whole-child reducer. Success writes the exact success terminal envelope; the legacy `NO_FAILURE` authorization path is removed; failure remains the exact failure terminal. Unknown result-shaped artifacts, cardinality, bindings, and parent exit are enforced. Focused reducer 14/14 plus actual child, outer success, and timeout are 17/17; source compile 8/8; cache scan clean. Hashes: hardening `91375cac...852`, diagnostic_phase `110572cd...0042`, run_diagnostic `7087cb64...a61e`, core `f521dfe3...36e3`, run_outer `42347eaf...9e10`, config `923006bd...0d86`. | Migrate terminal authorization and finalization lifecycle through the actual production commit path. |
| 2026-08-24T16:16:56Z | 2 | Terra | Production terminal-authorization and lifecycle slice. | Actual `run_outer` now performs initial v2 binding, ordered outer/ledger-v2 validation, immutable final-candidate construction, terminal exact-byte authorization/signature recheck, and atomic create-only verdict publication. The 9 terminal and 15 lifecycle rows invoke production finalizer/reducers and pass 24/24; retained child success, outer success, and timeout pass 3/3. Compile 6/6, governed inventory passes, root/package copies match, and cache scan is clean. Hashes: hardening `7e2d0036...363f`, run_outer `2cc4f31e...d095`, tests `44655905...af05`. | Convert startup bite cases to real isolated bootstrap launches, then rebuild RED and run Luna. |
| 2026-08-24T16:20:45Z | 2 | Terra / Orchestrator | Real-bootstrap startup test conversion reviewed. | Nine named cases now launch the absolute trusted bootstrap under `-I -S -B` with sanitized environment and marker checks; reported 9/9, compile 4/4, cache clean. Orchestrator rejected the result for mutation credit because the package lacks `package-manifest.json`/signature and every subprocess stops at the same package-load rejection rather than the intended signature, bytecode, authorization, or mount guard. Trusted bootstrap hash `31c5b909...1d5b`, tests `097f8408...d104`. | Terra must add an explicitly non-authoritative signed static fixture with a verified positive baseline, then prove one intended guard per startup mutation. |
| 2026-08-24T16:34:42Z | 2 | Terra / Orchestrator | Locked startup catalog completed after rejection and rework. | A deterministic, explicitly non-authoritative signed temporary package supplies a valid real-bootstrap baseline. Counted rows now exactly cover hostile `PYTHONPATH`, current-directory shadowing, invalid signature before import, undeclared bytecode, old authorization/new argv, missing `-I`, and writable/missing/changed `/diagnostic` mounts. Startup 9/9 passes with intended guards; the auth mutation uses the actual initial gate and rejects at `authorization/AUTHORIZATION_BINDING/review-authorization.json//binding`. Mount cases use production inspect-shaped validation without a daemon. Compile passes and cache is clean. Tests hash `71524dbe...3c87`. | Rebuild RED to 50 base mutations and 99 isolated catalog rows, then Luna full regression. |
| 2026-08-24T16:46:06Z | 2 | Terra | RED mutation matrix rebuilt. | Independently authored offline baseline reaches production bundle PASS semantics. RED now has 50 distinct base mutations and 99 unique catalog rows; `semantics_valid:true`, `only_corresponding_guard_flips:true`, unrelated image rejects at `manifest_image`, and standalone rc1. Focused RED catalog case passes. Governed copies match, inventory/compile pass, cache clean. Hashes: RED `c3e0d3da...19a6`, tests `e3f250ca...9e53`. | Luna cycle-3 authoritative compile/focused/Docker/full/RED run. |
| 2026-08-24T16:52:12Z | 2 | Luna | Cycle-3 authoritative read-only offline regression. | Pre/post inventory clean; exact source-byte compile 11/11; focused startup 9/9, preflight 18/18, child 14/14, terminal 9/9, lifecycle 15/15; frozen Docker plus `/diagnostic` mount semantics 7/7; RED rc1 with valid 50/99 isolated semantics. Full suite 123/125, rc1. Failures are retained fixture-path drift: missing new-registry terminal artifact in `child_no_failure_is_separately_bound_and_every_field_rejects`, and reviewer public key still looked up under the runtime root in `strict_record_schema_rejects_added_field`. Runtime/evidence roots absent; no live tool. `/tmp` validator functions are byte-identical to 043; the sole composition difference locates `core.py` under the separated diagnostic package. | Terra corrects only the two retained fixtures; Luna repeats full gates. |
| 2026-08-24T16:57:37Z | 2 | Terra | Cycle-3 narrow retained-fixture correction. | The child case now mutates the registry-owned success terminal envelope rather than a removed legacy file; strict-record fixtures resolve the immutable reviewer key and governed targets through `diagnostic_package(out)`. Repaired cases 2/2 and child group 14/14 pass; compile, inventory, sync, and cache scans pass. Only test source changed, hash `3136611f...d1b1`. Terra's full run outlived the desktop bridge's 30-second yield and produced no authoritative packet. | Luna repeats repaired group, Docker, full suite, and RED. |
| 2026-08-24T17:03:37Z | 2 | Luna | Cycle-4 authoritative read-only regression on final Terra candidate. | Pre/post package scans clean; 11/11 source-byte compile; repaired group 16/16; focused startup 9/9, preflight 18/18, child 14/14, terminal 9/9, lifecycle 15/15; frozen Docker plus mount group 7/7; full suite **125/125**, schema44, rc0 within 360 seconds; RED rc1 with accepted baseline, 50/99 rows, valid isolation, broad-injection rejection, and unrelated-image rejection. Runtime/evidence roots absent; no Docker, FreeCAD, live stage, or signing. Exact tested hashes include core `f521dfe3...36e3`, trusted bootstrap `5f0f516b...96e9`, hardening `82f6384a...f556`, tests `3136611f...d1b1`, RED `c3e0d3da...19a6`, preflight `327c646f...3cf2`, run_diagnostic `7087cb64...a61e`, run_outer `2cc4f31e...d095`, config `923006bd...0d86`. | Sol high-reasoning review of these exact hashes, preservation, coverage, and Docker non-regression. |
| 2026-08-24T17:11:24Z | 2 | Sol | Final high-reasoning review of the exact Luna-green hashes. | `PROBLEMS`: ESH-044-007..016. Hashes match Luna; predecessor manifest independently replays 873/873 with zero differences and canonical root `63a82b3c...c353`; runtime/evidence roots absent; no staged files; frozen `/tmp` functions unchanged aside from the package-source hash path. Blocking source findings: bootstrap executes governed hardening before verification; package-root reparse not rejected; subprocess environments inherit undeclared inputs; auth-v2 is optional with legacy PASS fallback and incomplete scope/reviewer/time semantics; preflight omits/approximates locked command/freshness checks; production binding/reducer remains split; freshness/write-once lifecycle incomplete; v2 finalization omits outer validation, uses `bytes` instead of `size`, and calls a callback after terminal auth; hardening tests still contain helper/alias coverage and non-distinct interruption boundaries. | Return ESH-044-007..016 to Terra; repeat Luna and Sol. No evidence writer, commit, or push. |
| 2026-08-24T19:08:21Z | 3 | User / Orchestrator | Scope correction: ignored Diagnostic-044 is reference and mutation-fixture material only, not the delivered implementation. | Completion now requires tracked ownership in nested `tools/mcp/freecad-mcp/src/freecad_mcp/evidence_system/`, six tracked MCP test modules, and one narrow parent Part-3 integration test. Tests must pass with Diagnostic-044 unavailable and contain no result-path or runtime-`sys.path` dependency. Nested baseline is branch `fix/change-aware-save-mcp-autonomy`, HEAD/upstream `e734f525e2dee47fb39a3b1bc552ca37766e48d4`, ahead/behind 0/0, with pre-existing user edits in `tests/test_operation_scope.py` and `tests/test_phase18_registered_tool_runtime.py` plus user-owned untracked Luna logs. | Sol maps invariants to tracked owners; Terra adds failing tracked tests before porting. |
| 2026-08-24T19:14:41Z | 3 | Sol | Tracked FreeCAD/MCP architecture review. | `PROBLEMS`: tracked delivery is absent. Sol assigned one cohesive nested `freecad_mcp.evidence_system` package with policy, structured validation, non-circular bindings, trusted bootstrap, sanitized environment/launcher, Docker contract, preflight, authorization, child reducer, create-only publication, ledger-v2, finalization, and runner modules. Exact six nested test owners cover 9/18/14/9/15/7; parent owns only one Part-3 smoke integration. Hatchling discovery requires no pyproject edit; no console script or package-root import may precede the trusted bootstrap. Nested Python is >=3.12, while the directly executed bootstrap remains Python-3.11 compatible. | Terra implements tracked RED-first slices; Luna runs 72 focused cases, architecture/full suites, parent smoke, RED, and independence gate. |
| 2026-08-24T19:42:00Z | 3 | Terra / Orchestrator | Rejected two shallow tracked implementations, then completed the cohesive tracked port under `src/freecad_mcp/evidence_system/` and the six locked test owners. | Production now has structured validation, full direct/post-snapshot bindings, authorization-v2, isolated approved launcher, captured-byte bootstrap, exact preflight, sole child reducer, frozen Docker semantics plus `/diagnostic`, fresh/create-only publication, ledger-v2, immutable final candidate, and one ordered runner. The earlier HMAC/test-helper version and aliased T2 mutations received no credit. Focused groups pass 9/18/14/9/15/7 = **72/72**. | Run parent boundary, independence, complete-suite characterization, Luna, then Sol. |
| 2026-08-24T19:59:46Z | 3 | Orchestrator | Ran tracked integration, independence, source, and repository gates on the final Terra candidate. | Parent Part-3 boundary **1/1**. With Diagnostic-044 temporarily moved aside and restored in `finally`: nested **72/72** and parent **1/1**. Dependency search for the two ignored result identifiers and runtime `sys.path` injection is empty. Exact Python 3.11 `-I -S -B` source-byte compile passes 22/22. Complete nested suite finished within 360 seconds: 2623 passed, 41 skipped, 1 xfailed; 79 failures and 5 errors are pre-existing FreeCAD/Assembly runtime lanes and the architecture check has only two pre-existing unrelated findings. No Docker, FreeCAD, live stage, or production signing was invoked. | Luna read-only hashes and focused/relevant-suite gate, then Sol exact-hash review. |
| 2026-08-24T20:36:34Z | 4 | Orchestrator | Consolidated the tracked review/test loop completed after the first tracked candidate. | Sol first returned seven startup/capture/auth/preflight/Docker blockers; Terra corrected them. Luna then compiled 11 governed/trusted sources, passed the locked **72/72**, passed the parent boundary **1/1**, and validated RED 50/99 isolation without Docker, FreeCAD, live stages, or production signing. Exact Luna hashes included authorization `0455ebbe...c505`, Docker contract `d08c5c47...7f86`, launcher `20953289...febb`, policy `0d0de7ac...471`, preflight `e45e2cca...c38b`, runner `26088df4...0777`, trusted bootstrap `2a453877...eb86`, and parent integration `ad9fc224...13c8`. | Sol cycle-4 exact-source review. |
| 2026-08-24T20:36:34Z | 4 | Sol | Re-reviewed the exact Luna-green tracked candidate and searched workspace-wide production consumers. | `PROBLEMS`: TRACK-ESH-C4-001..007. The bootstrap bridge only checks argv shape; Docker validators are orphaned and omit pinned image/entrypoint/Cmd/network/read-only-root; foreign container bindings can pass; child reconciliation is not the sole outcome reducer; terminal expiry reuses start time; outer validation precedes write-once publication; and the 18/14/7 negative groups call helpers rather than the production entrypoint. Generated cache paths were reported but were already absent on the Orchestrator's immediate exact-path check. No file, test, Docker, FreeCAD, or signing action by Sol. | Terra cycle-5 production wiring and production-entrypoint mutation coverage; repeat Luna and Sol. |
| 2026-08-24T21:01:00Z | 5 | Terra | Implemented all seven cycle-4 production-integration findings in tracked MCP/parent code. | The bootstrap entrypoint now runs the real ordered runner; the runner consumes the exact offline Docker launch/inspect contract and rejects foreign container bindings; the child reducer owns PASS/FAIL and ledger membership; terminal authorization uses a fresh preloaded terminal time; outer is create-only published before relationship validation; and locked negative rows traverse production. Parent integration proves trusted bootstrap through final ledger/verdict. Exact compile rc0; focused **23/23 + 49/49 = 72/72**; parent **1/1**. Final package/cache/dependency scans clean. Hashes: runner `06dac38a...3bdf`, trusted bootstrap `e62de05a...a7ef`, child reducer `e6c3c266...8ff6`, Docker contract `19612462...6c13`, policy `f0b18e09...3bc8`. No Docker, FreeCAD, live stage, production signing, staging, commit, or evidence write. | Luna cycle-5 authoritative read-only gate on exact hashes. |
| 2026-08-24T21:09:00Z | 5 | Luna | Authoritative read-only gate on Terra cycle-5 hashes. | Package/test inventory clean; exact byte compile rc0; locked startup 9, preflight 18, child 14, terminal 9, lifecycle 15, Docker 7 = **72/72**; parent integration **1/1** without GUI fixture; dependency/post scans clean; Docker semantic checks offline only. Tested hashes include authorization `0455ebbe...c505`, child reducer `e6c3c266...8ff6`, Docker contract `19612462...6c13`, policy `f0b18e09...3bc8`, runner `06dac38a...3bdf`, trusted bootstrap `e62de05a...a7ef`, and parent integration `fe011c1a...bf00e`. | Sol exact-hash review. |
| 2026-08-24T21:12:00Z | 5 | Orchestrator | Repeated the independence and preservation gates on the exact cycle-5 tree. | First nested replay used the wrong isolated interpreter and failed collection because the src-layout editable package was absent; no semantic test ran, parent still passed, and Diagnostic-044 was restored in `finally`. Correct nested editable interpreter replay with Diagnostic-044 unavailable passed **72/72** and parent **1/1**, then restored the exact directory. Predecessor manifest replay is **873/873**, byte-equal, root `63a82b3c...c353`. | Sol review remains authoritative for source semantics. |
| 2026-08-24T21:15:00Z | 5 | Sol | Re-reviewed exact Luna hashes and production data flow. | `PROBLEMS`: TRACK-ESH-C5-001..007. Bootstrap discards captured authorization bytes; post-execute validation can skip cleanup/outer publication; actual Docker argv and inspect/container identities are unbound; production preflight is synthetic; child records are fabricated and overwrite-capable; structured downstream issues collapse to `RUNNER_FAILURE`; and parent integration replays signed observations rather than proving an executor produced them. Preflight/child and three mount rows still bypass the trusted production entrypoint. No files/tests/prohibited actions by Sol. | Terra cycle-6 captured-byte and real offline-executor correction; repeat Luna and Sol. |
| 2026-08-24T21:33:00Z | 6 | Terra | Corrected the cycle-5 captured-byte and execution-observation boundary. | Trusted bootstrap now passes immutable captured authorization/signature/key bytes into the runner; cleanup and forensic outer publication precede every post-execution relationship rejection; Docker validation binds exact signed argv, raw inspect hash, and container ID; new tracked `executor.py` invokes a controlled offline command to produce preflight and then Docker/child observations; child writes are create-only; structured issues survive bootstrap; signed runtime replay was removed. Exact compile rc0; locked **31/31 + 41/41 = 72/72**; parent **1/1**. Hashes: authorization `c00ebf0c...0152`, executor `d9ae1fb3...37e8`, runner `5dca68e1...4fbc`, Docker contract `81a71067...56d`, bootstrap `2514bff3...7b0a`. Dependency/cache scan clean; no prohibited action. | Luna cycle-6 authoritative read-only test and source gate. |
| 2026-08-24T21:39:00Z | 6 | Luna | Authoritative read-only compile/tests and deep data-flow review on cycle-6 hashes. | Compile rc0; locked **72/72**; parent **1/1**; pre/post scans clean; no prohibited action. `PROBLEMS`: the 18 preflight, 14 child, and three mount negatives still bypass trusted bootstrap; bootstrap still collapses governed failures to generic `RUNNER_FAILURE`; failpoints and uncaught execute/cleanup exceptions can avoid forensic outer publication; and no locked row proves the controlled external executor freshly produced preflight/Docker/child observations through trusted bootstrap. Hashes match Terra, including runner `5dca68e1...4fbc`, executor `d9ae1fb3...37e8`, bootstrap `2514bff3...7b0a`. | Terra cycle-7 end-to-end test conversion and failure-path hardening. |
| 2026-08-24T22:02:00Z | 7 | Terra | Completed the end-to-end failure-path and locked test conversion. | Bootstrap preserves structured outcomes; cleanup and forensic create-only outer publication cover caught post-execution failures; parent proves separate preflight and execution invocations plus create-only child writes. Added tracked `packet_harness.py` using the explicitly non-authoritative key; all preflight 18, child 14, and mount 3 cases now launch absolute trusted bootstrap through captured `bootstrap_entrypoint`, controlled executor, and runner, assert the intended structured issue, and prohibit final PASS; valid child failure produces final FAIL. Locked **72/72**, parent **1/1**, isolated compile rc0, dependency/cache scan clean. Hashes: runner `bf1ddf76...a264`, bootstrap `5025c505...f5b8`, executor `d9ae1fb3...37e8`, harness `6d8aa6e5...e3ae`, preflight tests `c28cf48b...043e`, child tests `99f16a60...95d4`, startup tests `367f1c3a...8e9a`. No prohibited action. | Luna cycle-7 authoritative exact-hash gate. |
| 2026-08-24T22:08:00Z | 7 | Luna | Deep gate on the converted end-to-end rows. | Compile and **72/72** plus parent **1/1** were green, and prior captured-byte/executor/forensic fixes were confirmed. `PROBLEMS`: two duplicate mount test names remained, and the three Docker-file `/diagnostic` mutations still called `EvidenceRunner` directly rather than the trusted-bootstrap packet path. | Terra cycle-8 changes only the affected test identities and mount paths. |
| 2026-08-24T22:12:00Z | 8 | Terra / Luna | Terra made the narrow test-only correction; Luna independently repeated the full gate. | AST reports **72 tests / 72 unique**. Startup 9, preflight 18, child 14, terminal 9, lifecycle 15, Docker 7 = **72/72**; parent **1/1**; exact compile rc0; clean pre/post/dependency scans. The three Docker mount rows now use `run_packet()` through trusted bootstrap, captured entrypoint, controlled executor, and runner, asserting exact `MOUNT_SET_CONTRACT` and no verdict. Luna: `NO_ISSUES`. Final production hashes include runner `bf1ddf76...a264`, bootstrap `5025c505...f5b8`, executor `d9ae1fb3...37e8`, Docker `81a71067...56d`; final tests include harness `6d8aa6e5...e3ae`, Docker `b643d6f1...e931`, startup `0005b96a...3f`. | Sol final exact-hash review. |
| 2026-08-24T22:18:00Z | 8 | Sol | Final exact-hash review after Luna `NO_ISSUES`. | `PROBLEMS`: TRACK-ESH-C8-001..005. Actual executor argv/worker bytes are outside the signed/approved command contract; cleanup is a hardcoded PASS claim; mount sources are not bound to the exact package/output/bootstrap identities; many negatives omit the required exact stage/code/artifact/field assertion and four Docker rows still bypass trusted bootstrap; `after_execution` and `after_cleanup` have identical side effects. All hashes match Luna and prior captured-auth/Docker/binding/reducer/terminal fixes are present. No file or state change by Sol. | Terra cycle-9 exact command/cleanup/mount/test/boundary correction. |
| 2026-08-24T22:25:00Z | 9 | Terra | Bound the real offline executor and aftermath contract. | Actual executor argv now equals signed `policy.executor_argv`/command contract; the absolute worker is no-follow/stat/hash checked before preflight, evidence, and cleanup invocations. Cleanup is an observed third executor phase; parent marker proves `preflight -> evidence -> cleanup`. Exact `/diagnostic`, `/trusted/bootstrap.py`, and `/out` mount sources bind to captured package/bootstrap/authorization identities. All Docker 7 use the trusted packet path. Interruption boundaries are distinct: `after_execution` observes only execute; `after_cleanup` observes execute+cleanup; semantic failures still cleanup and publish forensic outer. Production hashes: runner `0f17bd21...63f4`, executor `94141b68...2d5a`, bootstrap `64845e56...7ba1`. | Complete the full-tuple negative assertion audit. |
| 2026-08-24T22:31:00Z | 10 | Terra | Mechanical locked-negative audit across all six test owners. | Exactly **72 unique** tests; 64 negative and 8 positive rows. Every negative asserts exact `(stage, code, artifact, field)` and valid-final-PASS prohibition. Bounded groups pass startup 9, preflight 18, child 14, terminal 9, lifecycle 15, Docker 7; parent **1/1**; exact compile 16 production sources rc0; cache scan clean. Test hashes include startup `d2e49efc...0275`, preflight `373d19e1...2bb4`, child `09547715...6dea`, terminal `ace84e3e...4e0d`, lifecycle `716fcb2e...7b46`, Docker `0ea80bc4...1e38`, harness `d0e2020a...3d9a`, parent `faed2000...897d`. | Luna cycle-10 authoritative full gate. |
| 2026-08-24T22:36:00Z | 10 | Luna | Authoritative combined replay and deep source audit. | `NO_ISSUES`: combined locked **72/72** in 94.29s; parent **1/1**; exact Python 3.11 `-I -S -B` byte compile 25 tracked files rc0; 72/72 unique names; clean pre/post, dependency, and independence scans. Confirmed exact signed executor/worker checks, real cleanup order, inspect/container/mount binding, distinct interruption boundaries, full issue tuples/no PASS for all 64 negatives, valid child final FAIL, and frozen Docker semantics without a daemon. No Docker, FreeCAD, live stages, production signing, evidence/result write, edit, stage, or commit. | Sol final exact-hash review. |
| 2026-08-24T22:40:00Z | 10 | Sol | Exact-hash source review of the Luna-green candidate. | `PROBLEMS`: TRACK-ESH-C10-001..002. Executor worker and root trusted-bootstrap approval remain pathname hash-then-spawn sequences; neither preserves a no-follow stable object through process acquisition, so post-hash replacement can execute unapproved bytes. All other lifecycle, authorization, child, Docker, ledger, independence, and dirty-separation invariants were coherent. | Terra cycle-11 OS-handle launch-source lock and deterministic race tests. |
| 2026-08-24T22:45:00Z | 11 | Terra | Implemented one reusable stable launch-source gate for the root bootstrap and every worker phase. | Windows rejects ancestor/leaf reparse points, opens with `CreateFileW(FILE_FLAG_OPEN_REPARSE_POINT)` and `FILE_SHARE_READ` only, hashes and checks identity from the same handle, and holds it through child acquisition; POSIX uses `O_NOFOLLOW`, fstat, held `/proc/self/fd`, and live-name identity rejection. Structured launch issues survive runner ordering. Existing-count tests exercise ancestor reparse and post-hash/pre-spawn replacement for both bootstrap and worker with malicious side effects absent. Bounded locked **72/72**, parent **1/1**, 17-source compile rc0, 72 unique names, cache scan clean. Hashes: launch source `344e54c8...7da1`, launcher `c615ca43...f52a`, executor `ef08bf08...c9da`, runner `5b2df4d3...2fbb`, startup test `9cea8893...b2c1`, Docker test `50f73921...4957`. No prohibited action. | Luna cycle-11 authoritative handle/race gate. |
| 2026-08-24T22:47:00Z | 11 | Luna / Sol | Luna passed the current-tree compile, combined **72/72**, parent **1/1**, race review, and scans; Sol then inspected the exact hashes. | Sol `PROBLEMS`: TRACK-ESH-C11-001..002. Windows used `0x02000000` (backup semantics) instead of `0x00200000` (`OPEN_REPARSE_POINT`), leaving a pre-open leaf race. The reparse test could silently omit its assertion, and the worker swap subcase reused an unrelated valid verdict rather than an isolated full-chain negative. Other reviewed invariants were coherent. | Terra cycle-12 corrects the constant and makes both races mandatory, isolated production-chain packets. |
| 2026-08-24T22:51:00Z | 12 | Terra | Corrected the Windows flag and rebuilt the race regressions. | Named `FILE_FLAG_OPEN_REPARSE_POINT=0x00200000`; stable gate now has a deterministic pre-open interval, post-open ancestor recheck, canonical `GetFinalPathNameByHandleW` comparison, share-read-only lock, and held lifetime. Startup 9 mandates a real junction race with exact structured issue and no side effect/verdict. Docker 7 adds fresh full-chain `worker_race` and `worker_swap` roots; replacement is denied on Windows while approved bytes run, with no malicious effect or verdict. Final bounded groups total **72/72**, parent **1/1**, 17-source compile rc0, 72 unique, caches clean. Hashes: launch source `007271ef...13de`, launcher `d36198d0...c1ac`, executor `e3e075b5...8e53`, runner `7d2077ae...bb98`, harness `ab36fbe2...e9a`, startup `0a449732...61ae`, Docker `29e60065...c3d3`. No prohibited action. | Luna cycle-12 authoritative exact-hash gate. |
| 2026-08-24T22:53:54Z | 12 | Luna / Sol / Orchestrator | Final exact-hash acceptance and delivery gate. | Luna: full locked **72/72** in 125.14s, parent **1/1**, 26-file byte compile rc0, 72 unique, clean scans, mandatory races and exact Windows handle semantics verified. Sol returned exact `NO_ISSUES`. Orchestrator then moved Diagnostic-044 aside and restored it in `finally`; current tracked tests passed **72/72** plus parent **1/1** without it. Predecessor replay is **873/873**, byte-equal, root `63a82b3c...c353`; caches remain absent. | Narrow nested commit/push, then parent progress/integration/conftest-hunk/gitlink commit/push. |

## Findings and disposition

| Finding | Owner | Status | Resolution/evidence |
|---|---|---|---|
| ESH-044-001..006 | Sol | `ACCEPTED` | Terra must resolve all six findings and preserve frozen Docker `/tmp` behavior. |
| Retained 043 assertion drift | Terra | `OPEN` | Update retained behavioral cases for the newly authorized absolute commands, `-I -S -B`, sanitized environment, and separate mounts without weakening Docker semantics. |
| Luna cycle-1 failures | Luna | `OPEN` | 17 full-suite failures, three child binding failures, one frozen Docker command-resolution failure, and invalid RED semantics require Terra correction. |
| Production v2 wiring | Terra | `OPEN` | New primitives are not yet consumed by the real preflight, executor, outer finalizer, and verdict writer. |
| Exact mounted package | Terra | `OPEN` | Current flat package skips `baseline/`; must use a dedicated exact `/diagnostic` source and separately frozen trusted bootstrap with no skipped entry. |
| Luna cycle-2 failures | Luna | `OPEN` | Focused 65 and Docker 5 pass, but retained production-path suite is 108/125 and RED is structurally invalid. Exact failures cover actual coordinator/finalization, child binding, absolute Docker version argv, stale-output expectation drift, frozen identity, timeout/cleanup, ledger rows, terminal authorization, temp reviewer key fixture, and tracker snapshot guards. |
| ESH-044-007..016 | Sol | `OPEN` | Source review found fail-open bootstrap, unchecked root reparse, unsanitized subprocess environments, optional/incomplete auth-v2, incomplete preflight contract, split production binding/reduction, non-write-once lifecycle, v2 finalization gaps, and helper/alias hardening tests. Luna green does not override these findings. |
| Tracked ownership | User | `RESOLVED` | Durable implementation and locked tests live in tracked MCP/parent paths. Final independence passes 72/72 + 1/1 with Diagnostic-044 unavailable; Luna is green and Sol returned `NO_ISSUES`. |
| TRACK-ESH-001..005 | Sol | `ACCEPTED` | Exact tracked package/test ownership, parent boundary, independence gate, and dirty-tree separation handed to Terra. No tracked implementation exists yet. |
| TRACK-ESH-C4-001..007 | Sol | `RESOLVED` | The bootstrap runs the ordered production lifecycle; Docker/container binding, reducer ownership, terminal clock, outer ordering, and production-entrypoint rows are enforced. |
| TRACK-ESH-C5-001..007 | Sol | `RESOLVED` | Captured authorization is handed through immutably; cleanup/outer ordering, exact Docker identities, observed preflight/children, structured issues, and controlled external execution are enforced. |
| TRACK-ESH-C8-001..005 | Sol | `RESOLVED` | Signed executor equality, observed cleanup, exact mount sources, full four-field negative assertions, and distinct interruption boundaries are covered by final tests. |
| TRACK-ESH-C10-001..002 | Sol | `RESOLVED` | Reusable launch-source gate holds no-follow approved bootstrap/worker objects through process acquisition and rejects deterministic reparse/swap races. |
| TRACK-ESH-C11-001..002 | Sol | `RESOLVED` | Correct Windows `OPEN_REPARSE_POINT` flag, mandatory real junction setup, canonical handle path verification, and isolated full-chain worker race packets passed Luna; Sol returned `NO_ISSUES`. |
| Diagnostic-035 path collision | Orchestrator | `BLOCKED` | Existing 035..043 make the locked 035 identity non-fresh; recommend fresh 044 while preserving all bytes now present. |
| Diagnostic-044 authority | User | `RESOLVED` | Explicitly authorized on 2026-08-24; all three 044 roots verified absent before creation. |

## Test commands and results

Terra development checks only:

- Exact Python 3.11 `py_compile` for `core.py`, `hardening044.py`, and
  `offline_contract_tests.py`: pass.
- Embedded RFC 8032 verifier self-test: pass.
- Package inventory: clean after removal of Terra's disposable
  `__pycache__`; runtime/evidence roots remain absent.

Tracked candidate hashes at `2026-08-24T19:59:46Z`: production `__init__`
`3b99a681...bd89`, authorization `7a3a9156...9962`, bindings
`17cc09ae...69f2`, child reducer `487251e1...d397`, Docker contract
`a882701b...883a`, environment `910a812b...5b82`, finalization
`bdd12535...b8cb`, launcher `e85ba51f...c4c8`, ledger
`bd51e68c...bb8f`, policy `0d438078...109e`, preflight
`3ad2d579...8055`, publication `181d07aa...3b4`, runner
`4b46a7e7...9181`, trusted bootstrap `0d7b1842...a4e4`, validation
`9708a0ec...20be`. Tests: startup `cf10302f...b51`, preflight
`8d8bf951...13e2`, child `aa8c9424...6b1d`, terminal
`3d87642f...b28`, lifecycle `916899f7...fac7`, Docker
`ff110756...ab1`; parent integration `8967230b...4444`.

Luna's authoritative tracked-hash results are pending.

Luna cycle 1 tested exact hashes `core.py 67875dea...13b1`,
`hardening044.py 3ab87f20...61ff0`, `offline_contract_tests.py
f8f2aaef...66ad`, and `offline_red_044.py c42f740c...1ebe`. Complete
suite: **108/125**, rc1. RED: **unexpected rc0**, invalid semantics. These
results authorize no Sol re-review or evidence writer.

Luna cycle 2 tested exact hashes: `bootstrap.py` and
`trusted/bootstrap.py 07055387...0886`, root/diagnostic `core.py
aaeb88e4...39d7`, `diagnostic_phase.py 0ef94824...4600`,
`hardening044.py 549e7d40...f95e8`, `offline_contract_tests.py
c7772b4f...853ef`, `offline_red_044.py bee9044a...56a76`, `preflight.py
45d920bb...9bd7`, `run_diagnostic.py 7f95ab3b...20cf`, `run_outer.py
ac933d3b...d04e`, and `write_implementer_packet.py 9090aecb...bc08`.
Complete suite: **108/125**, rc1. RED: **unexpected rc0**, with 17/50
base mutations and 34/99 catalog rows. These results authorize no Sol
re-review or evidence writer.

## Final delivery

Commit, push, and final verdict remain blocked until the behavior is present in
tracked FreeCAD/MCP code and scoped tracked tests, those tests pass without the
Diagnostic-044 directory, Luna passes the complete tracked gates, and Sol
returns `NO_ISSUES` for the exact tested tracked hashes. An ignored-only
Diagnostic-044 candidate cannot receive `IMPLEMENTED` status.

## Pause checkpoint — 2026-08-24T22:55:33.9111471Z

- Latest completed cycle: **12**. Luna returned `NO_ISSUES`; full locked suite
  **72/72** in 125.14s, parent integration **1/1**, exact 26-file Python 3.11
  `-I -S -B` byte compilation rc0, 72 unique names, and clean dependency/cache
  scans. Sol reviewed the exact Luna hashes and returned `NO_ISSUES`.
- Orchestrator's final independence replay temporarily made Diagnostic-044
  unavailable, passed **72/72 + 1/1**, and restored it in `finally`.
  `Diagnostic-044 restored: yes`; temporary hold absent; tracked-tree cache,
  `.pyc`, `.pyo`, and `.pth` scan: zero. Predecessor manifest: **873/873**
  byte-equal, root `63a82b3c2d5862939648f6a2518fbc7f70a3b135cdcbc6ed9c113b3bad4ac353`.
- Unresolved Sol/Luna findings: **none**. Delivery is paused only by the user's
  explicit stop instruction; no staging, commit, or push has started.
- Nested repository: branch `fix/change-aware-save-mcp-autonomy`; HEAD and
  upstream `e734f525e2dee47fb39a3b1bc552ca37766e48d4`; staged state empty. Dirty
  state is the two preserved user modifications
  `tests/test_operation_scope.py`, `tests/test_phase18_registered_tool_runtime.py`
  plus the untracked candidate directories `src/freecad_mcp/evidence_system/`
  and `tests/evidence_system/`.
- Parent repository: branch `fix/change-aware-save-mcp-autonomy`; HEAD and
  upstream `249ba1d652c0c4218b0715065b9fbf666ddcad03`; staged state empty. Existing
  user dirty files remain untouched. Candidate-owned parent paths are this
  progress document, `tests/gui/part3/test_evidence_system_integration.py`, and
  only the `test_evidence_system_integration.py` exemption line inside the
  mixed user-owned `tests/gui/part3/conftest.py` hunk. The nested gitlink is
  dirty but uncommitted.
- Current production SHA-256 values: `__init__ 3b99a681fb689e3d8217ac7e5c7e406db009409a50efb1f0dec3320ded23bd89`;
  `authorization c00ebf0c1122fadce5c00f9350ca7a83afa4410b31c273b8d9a18a93fc7b0152`;
  `bindings 17cc09ae6b0a1c3896a71d06b6c8eada9d04cd808313a7394806a2d1c61f69f2`;
  `child_results e6c3c26633a105964a7e093893af1772a1d2f4c546833b6361e2020969a78ff6`;
  `docker_contract 81a710677ba97ce0aea22886008c57715e52c068e13bab6374f4592e5173556d`;
  `environment cfe4a9d6b6e0476f9357c0013dd8352f0af2eab69d9cd2de463317d405fef4e0`;
  `executor e3e075b5b493cc9398c7479344f635ca99bc434cd4d9dd6109712f3a95538e53`;
  `finalization bdd12535e88f112562646553c1db49f4ac44d407eb37aab9447b1c368ab8b8cb`;
  `launch_source 007271ef9d303397ac9c2dcdcd6258b042e4fc794f93da358b4187d6bcb113de`;
  `launcher d36198d062387f41d925a04d44191fa0450df8cc522669bfea44de9c1a81c1ac`;
  `ledger bd51e68c59d6c0ebf93e9cedd622fa56250513520bd570f31461caa39c69bb8f`;
  `policy f0b18e0929988b8072502fdd16f826fb481f2a2a707f40942cf45adf289c3bc8`;
  `preflight e45e2cca6ed47896395a901a720c5d91f3789bb9c0e5245a77a0c0d3bcf9c38b`;
  `publication 7f5962ce0292422643cd4aa4a684b08a6a253fe5ad80d913098a4de67952ce58`;
  `runner 7d2077aeaee5f2908c46f37576bce0c537eb200e9c6eda011516ce06eb51bb98`;
  `trusted_bootstrap 64845e56b038d806145c67764d4d3955658081d0626e206b9b25c719255f7ba1`;
  `validation 53a97dc3650dde0a00af141fc03518d8aa48935d8ae754abe53e0a669ad9f70c`.
- Current test SHA-256 values: `non_authoritative_signing 8805cfa30766f0ae6e3c434b08a2dc500c236c7415125d8d915607a9d7525828`;
  `packet_harness ab36fbe2c5fde8f082ba679ebc473cd40c694cffc1a580d879024da7aa591e9a`;
  `startup 0a44973265b6a2eb05337ff38ba563266d21dd1a16d672294f471632183961ae`;
  `preflight 373d19e1cbcb941d9c4b6aeb02a371850f5c280620390b2bc5c218784ade2bb4`;
  `child 0954771513c210473a89a510d4ee8b2fe5c69a31e1377a8b0b8b1db279726dea`;
  `terminal ace84e3e79754994c74aaeaf58b2a922897dfca81b80ce7ef5b644c2f0af4e0d`;
  `lifecycle 716fcb2eaa4b627300555b8ba39ed7abf22a997372bea6430d5c4c87028a7b46`;
  `Docker 29e60065e13e0386f8af02f58d99db8adac73de3c57728f0c57f198bae86c3d3`;
  parent integration `faed20004b79bfeccee962a43cda053d9ada83d170e59d47c20bcdb8125c897d`.
- Exact resumption action: wait for the literal user instruction
  `RESUME EVIDENCE HARDENING`; then recheck hashes/status/restoration, stage and
  inspect only the nested evidence-system source/tests, commit and push nested,
  append its SHA here, narrowly stage the parent integration/progress/gitlink
  and only the one conftest exemption line, verify both cached diffs, commit and
  push parent without force, and verify local/upstream/remote SHA equality.

## Portability resumption — 2026-08-25T06:18:41.6950990Z

- User issued the exact resume instruction and added a portability gate. The
  Orchestrator re-read this record and verified both repositories remain on
  `fix/change-aware-save-mcp-autonomy`, parent HEAD/upstream
  `249ba1d652c0c4218b0715065b9fbf666ddcad03`, nested HEAD/upstream
  `e734f525e2dee47fb39a3b1bc552ca37766e48d4`, with both indexes empty and all
  previously recorded unrelated dirty work preserved.
- Initial tracked scan found the developer-specific interpreter literal in
  production `policy.py`/`launcher.py`, nested `packet_harness.py`, startup,
  preflight, and terminal tests, and the parent integration. Additional host
  fixture literals include `C:/pkg`, `C:/repo`, `C:/Docker`, and `C:/Windows`.
  Contract-only container paths remain allowed.
- Sol cycle 13 returned `PROBLEMS`: `PORT-C13-001` removes the production
  `HOST_INTERPRETER` machine lock; `PORT-C13-002` extends the cycle-12 stable
  no-follow launch-object approval to the interpreter executable itself; and
  `PORT-C13-003` removes tracked machine/checkout literals and proves portable
  selection. One execution must bind exactly one selected absolute interpreter
  across policy, outer argv, executor argv, and signed `binaries.host_interpreter`.
- Portability decision: `FREECAD_MCP_EVIDENCE_PYTHON` is an explicit selector;
  otherwise tests use `Path(sys.executable).resolve()`. The selector is not
  forwarded to child/container environments. Relative, missing, reparse,
  changed, unapproved, and post-approval/pre-spawn replacements must reject at
  exact structured guards with no final PASS. Alternate-checkout proof and the
  forbidden-literal scan are folded into the existing 9 startup nodes; locked
  total remains 72. The hostile-`PYTHONPATH` row remains only an input-stripping
  security case and may not provide import resolution.
- Next action: Terra adds failing in-count portability/interpreter-race tests,
  then makes the smallest coherent selector/policy/stable-executable/fixture
  correction. Luna repeats compile, 72 locked groups, parent integration,
  portability, Diagnostic-044 independence, and offline Docker semantics; Sol
  reviews the exact hashes. No staging/commit/push before both final gates.

### Portability handoff log

| UTC | Cycle | Agent | Action | Result | Next |
|---|---:|---|---|---|---|
| 2026-08-25T06:36:15.9241952Z | 13 | Terra | Added in-count portability assertions, then generalized interpreter selection, signed identity, and stable launch approval. | Added `host.py`: explicit `FREECAD_MCP_EVIDENCE_PYTHON` mapping override or `Path(sys.executable).resolve()` default; selector is removed from child env. Policy now binds one absolute interpreter across policy, outer/executor argv, and signed host digest. Launcher/executor hold both interpreter and script approved objects through spawn. Host fixtures derive from `__file__`, `tmp_path`, and the active interpreter; parent uses hardened `run_isolated`. Startup 9 includes selector rejection, alternate-checkout, and static scan coverage. Bounded locked groups **16/16 + 18/18 + 14/14 + 24/24 = 72/72**; parent **1/1**; exact byte compile pass; 72 unique; forbidden scan zero; cache scan zero. No prohibited action. Hashes: host `0f82b0b6...f004`, policy `0e70e41e...42bd`, launch source `85513870...e58e`, launcher `d42db49a...2448`, executor `9ec3a447...870b`, runner `50f29353...60d9`, parent `d9630e5d...8ad5`. | Luna authoritative portability/locked/independence/Docker-offline gate. |
| 2026-08-25T06:42:11.4350127Z | 13 | Luna | Ran the authoritative portability, locked, parent, Docker-offline, and Diagnostic-044 independence gates. | `NO_ISSUES`: exact 27-file Python 3.11 `-I -S -B` compile rc0; locked **72/72** in 74.93s; parent **1/1**; 72 unique; clean pre/post and static dependency scans. Diagnostic-044 was moved only to a validated sibling hold, **72/72 + 1/1** passed while unavailable, and it was restored in `finally` with hold absent. Verified dynamic selector/fallback, structured relative/missing/reparse/changed rejection, exact policy/argv/signed-digest binding, dual interpreter+script stable handles, selector sanitization, and real alternate-checkout coverage. No Docker, FreeCAD, live stage, production signing, source edit, staging, commit, deletion, or evidence-byte change. Hashes: host `0f82b0b6...f004`, launch source `85513870...e58e`, launcher `d42db49a...2448`, executor `9ec3a447...870b`, runner `50f29353...60d9`, harness `dff18d48...9c0b`, startup `6e7d615b...4843`, preflight `63f347cb...0c19`, terminal `084c71f9...ceb5`, parent `d9630e5d...8ad5`. | Sol final exact-hash portability review. |
| 2026-08-25T06:45:42.6135033Z | 13 | Sol | Reviewed the exact Luna-tested portability hashes and root-to-signed identity continuity. | `PROBLEMS`: `PORT-C13-FINAL-001` root launcher verifies a caller-supplied interpreter digest but the digest is absent from the signed outer command/request, so bootstrap/runner cannot prove root H1 equals signed `binaries.host_interpreter` H2. `PORT-C13-FINAL-002` existing tests cover selector relative/missing and bootstrap/worker races, but not interpreter leaf/ancestor reparse, post-approval swap, wrong external digest, or signed-digest mismatch through the production entrypoint. All hashes match; forbidden-literal, ignored-results, Docker, prior lifecycle, and dirty-separation gates remain clean. No prohibited action. | Terra carries the root digest through the exact signed command and adds deterministic in-count interpreter negative packets; repeat Luna and Sol. |
| 2026-08-25T06:57:52.5905159Z | 14 | Terra | Bound the root-approved interpreter digest into the exact signed command and added in-count interpreter negatives. | Outer argv now contains exact `--interpreter-sha256 <64hex>` supplied from the held root executable. Bootstrap parses/captures it before package work and requires equality with signed command/policy binary/path/outer/executor identities; runner repeats the binding. Existing startup 9 includes fresh interpreter reparse/junction, wrong external hash, signed-H1/root-H2 mismatch, and Windows pre-spawn replacement attempts with hook evidence, exact structured non-PASS, and no verdict. One new setup first failed because its nested packet root was absent; harness now creates it. Locked bounded total **72/72**, parent **1/1**, exact compile pass, 72 unique, forbidden scan zero, caches zero. No prohibited action. Hashes: bootstrap `1376444f...4f9b`, launcher `c1e3f849...208c`, runner `7c3ce0da...6f6f`, policy `eecd57ab...2523`, harness `8acd8e64...6c44`, startup `5c724509...3a0b`, parent `4438ab31...0130`. | Luna authoritative cycle-14 portability/independence gate. |
| 2026-08-25T07:01:22.2758999Z | 14 | Luna | Ran the complete authoritative gate on the exact cycle-14 hashes with Diagnostic-044 unavailable. | `NO_ISSUES`: exact 27-file compile rc0; locked **72/72** in 77.34s and parent **1/1** while the exact Diagnostic-044 directory was in a validated sibling hold; restored in `finally`, hold absent. Counts 9/18/14/9/15/7, 72 unique, cache/reparse/dependency/forbidden scans clean. Verified single signed interpreter token, bootstrap/runner continuity, relative/missing/reparse/changed/wrong-digest/signed-mismatch/parser/replacement failures, alternate checkout, no valid verdict, and frozen Docker semantics without a daemon. No prohibited action. Hashes: bootstrap `1376444f...4f9b`, launcher `c1e3f849...208c`, runner `7c3ce0da...6f6f`, policy `eecd57ab...2523`, harness `8acd8e64...6c44`, startup `5c724509...3a0b`, Docker `29e60065...c3d3`, parent `4438ab31...0130`. | Sol final exact-hash review. |
| 2026-08-25T07:04:20.6522983Z | 14 | Sol | Reviewed the exact cycle-14 hashes for functional portable execution and complete interpreter mutation coverage. | `PROBLEMS`: `PORT-C14-001` POSIX replaces signed argv[0] with an fd path, so bootstrap raw `sys.executable` can disagree with the valid selected pathname; keep signed argv[0] and use the held fd only as `subprocess.executable`, while scripts keep fd argv paths. Windows copied-interpreter proof currently stops at unrelated authorization binding. `PORT-C14-002` lacks mandatory leaf-reparse, missing-token, duplicate-token, and changed/unapproved pre-spawn full-chain cases with side-effect/no-verdict proof. All supplied hashes match; other portability, prior lifecycle, Docker, and dirty gates remain clean. | Terra cycle-15 functional POSIX launch and complete in-count parser/race packets; repeat Luna and Sol. |
| 2026-08-25T07:14:00.3724619Z | 15 | Terra | Corrected POSIX dual-source execution and completed the in-count interpreter packet matrix. | POSIX preserves signed selected argv[0], rewrites only the governed script to its held fd, passes both descriptors, and uses the held interpreter fd as `subprocess.executable`; bootstrap captures the invoked identity and runner binds it rather than ambient selection. A real alternate approved interpreter now reaches intended downstream `MOUNT_SET_CONTRACT`. Startup 9 folds ancestor and mandatory unprivileged leaf junctions, wrong external digest, signed H1/H2 mismatch, missing/duplicate digest tokens, alternate-interpreter functional path, and changed-byte pre-spawn replacement with hook, absent malicious side effect, exact issue, and no verdict. Initial privileged leaf symlink setup failed with WinError 1314 and was replaced with mandatory junction setup. Locked bounded **72/72**, parent **1/1**, compile pass, 72 unique, forbidden/cache scans zero. No prohibited action. Hashes: launch source `d4b0ddca...ed92`, bootstrap `90f44fe5...80bc`, runner `57befdc0...5a0d`, harness `08c98a04...6aea`, startup `fda7e9cc...0ce8`. | Luna cycle-15 authoritative functional portability/independence gate. |
| 2026-08-25T07:17:47.7095060Z | 15 | Luna | Ran the exact cycle-15 portability, locked, parent, independence, and Docker-offline gate. | `NO_ISSUES`: 27-file compile rc0; **72/72** in 82.19s and parent **1/1** with Diagnostic-044 unavailable; restored in `finally`, hold absent. Counts 9/18/14/9/15/7, 72 unique, clean scans. Confirmed POSIX signed argv/executable/fd semantics, functional alternate interpreter reaching `MOUNT_SET_CONTRACT`, mandatory leaf/ancestor races, wrong digest, signed mismatch, byte replacement/side-effect suppression, and manual missing/duplicate token packets returning exact `startup/BOOTSTRAP_ARGUMENTS/bootstrap//`. No host literals or import-path hiding; prior invariants remain green. No prohibited action. Hashes: launch source `d4b0ddca...ed92`, bootstrap `90f44fe5...80bc`, launcher `c1e3f849...208c`, runner `57befdc0...5a0d`, policy `eecd57ab...2523`, harness `08c98a04...6aea`, startup `fda7e9cc...0ce8`, Docker `29e60065...c3d3`, parent `4438ab31...0130`. | Sol final exact-hash review. |
| 2026-08-25T07:20:04.3070332Z | 15 | Sol | Reviewed the exact cycle-15 hashes and portability test determinism. | `PROBLEMS`: only `PORT-C15-001` remains. Startup test reconstructs `LOCALAPPDATA/.../Python311/python.exe`, raises where the variable is absent, and silently omits the alternate-interpreter proof when that installation is missing. Replace with deterministic current selected interpreter or an explicit fixture and require downstream `MOUNT_SET_CONTRACT`; no conditional omission. POSIX argv/fd behavior, digest continuity, negative guards, scans, Docker, prior invariants, and dirty separation otherwise review clean. | Terra cycle-16 test-only deterministic selector correction; repeat Luna and Sol. |
| 2026-08-25T07:22:59.1366647Z | 16 | Terra | Made the remaining portability test deterministic without production changes. | Removed all `LOCALAPPDATA`/per-user/version discovery and the silent existence branch. Startup passes `Path(sys.executable).resolve()` through the packet harness's explicit selector mapping; the fresh signed packet reaches exact downstream `MOUNT_SET_CONTRACT`, while alternate-checkout bootstrap proof remains separate. No process environment or import-path mutation. Locked bounded **72/72**, parent **1/1**, compile pass, forbidden scan including `LOCALAPPDATA` zero, caches zero. No prohibited action. Startup hash `524953de12658d07a0ade574fdd6f0e40871ff074dc0629063228832b77f99f4`. | Luna cycle-16 authoritative test-only regression and independence gate. |
| 2026-08-25T07:25:48.5243339Z | 16 | Luna | Repeated the complete portability, locked, parent, Docker-offline, and Diagnostic-044 independence gates on the test-only correction. | `NO_ISSUES`: exact 27-file compile rc0; **72/72** in 80.15s and parent **1/1** with Diagnostic-044 unavailable; restored in `finally`, hold absent. Counts 9/18/14/9/15/7, 72 unique, scans clean. Confirmed exact startup hash, no ambient user/version discovery or conditional skip, explicit current-interpreter packet reaches `MOUNT_SET_CONTRACT`, and all C15 descriptor/digest/reparse/token/race invariants remain intact. No Docker, FreeCAD, live stage, production signing, edit, stage, commit, deletion, or evidence-byte change. | Sol final cycle-16 exact-hash review. |
| 2026-08-25T07:26:53.5826275Z | 16 | Sol | Final exact-hash review of the Luna-green portability tree. | `NO_ISSUES`. `PORT-C15-001` is removed; deterministic current-interpreter selection reaches the intended downstream guard. All portability, stable interpreter/script launch, authorization/lifecycle/child/ledger, frozen Docker `/tmp`, independence, and dirty-separation gates are approved. | Orchestrator performs narrow nested cached-diff verification, commit/push, then parent progress/integration/conftest-hunk/gitlink delivery. |
| 2026-08-25T07:28:24.0398231Z | 16 | Orchestrator | Verified and delivered the narrow nested MCP index after final approval. | Cached diff contained only 18 `src/freecad_mcp/evidence_system/` files and 8 `tests/evidence_system/` files; cached check clean. Preserved user files remained outside the index. Commit `b2d657333ecec0e4f2d1172d3b8c8be380cf39e0` (`Harden tracked evidence system`) pushed without force; nested local HEAD, upstream, and remote SHA are identical. | Stage only parent progress, integration, nested gitlink, and the one conftest exemption line; inspect cached diff, commit/push, verify SHA equality. |
