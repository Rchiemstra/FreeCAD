# Chair MCP follow-up review and verification

This follow-up starts from `integrate/change-aware-save-mcp-autonomy` at
`0cc288732e7ee12df5523b33df681de117290a52`, including the previously merged
`fix/chair-mcp-modeling-recovery`. Before editing, the original target and
baseline were recorded and `fix/idea-chair-mcp-review-20260913` was created.
The MCP submodule baseline is `51e083a3bdde0043d3242072623756d583ecf359`.

The user subsequently selected **FreeCAD-start** as the PR destination and
authorized merging after verification. Existing integration history between
these branches is distinct from this fix batch. Existing user changes and
historical source/build volumes are preserved; user files are excluded from
staging.

## Scope and review process

Scope comprises the current `.idea/chair-mcp-issue-*.md` reports 6–22 and related
defects demonstrated during review. Codex orchestrates Sol (`gpt-5.6-sol`, high
reasoning) for review, Terra (`gpt-5.6-terra`) for fixes and regression tests,
Luna (`gpt-5.6-luna`) for Docker validation, then Sol for verification. This
cycle repeats for unresolved findings. All three agents actually participated.

The detailed local ledger is `.idea/issue_progress.md`, ignored by the existing
repository configuration. Raw evidence is under
`results/idea-chair-mcp-20260913/`, also excluded from commits. This document
provides a portable record; unavailable or skipped tests are never passes.

## Current findings

| ID | Evidence and correction | Verification status |
| --- | --- | --- |
| CHAIR-06 | Generated and addon sketch diagnostics omit solver degrees of freedom and fully constrained state. Both readers now report available native fields without fabricating unavailable values. | Resolved; Sol verified implementation and regression evidence. |
| CHAIR-12 | Raw evaluated `FreeCAD.Quantity` cannot be encoded as JSON. Generated and addon spreadsheet readers now retain units in serializable values. | Resolved; Sol verified both readers and unit-bearing regressions. |
| CHAIR-13 | Blank-image detection previously read PNG filter residuals as pixels. It now reverses supported filters and conservatively handles unsupported layouts. | Parser resolved; Sol also verified four successful synthetic live captures across save/close/reopen. Original Assembly fixture symptom remains unverified. |
| CHAIR-15 | Public `affected_documents` contract allowed scopes beyond the native single-document commit. Distinct multiple-document scopes now fail before callback side effects; callers use dependency-ordered single-document commits. | Resolved; Sol verified scope, dispatch and generated contract regressions. |
| CHAIR-16 | Restore-time expression filters omit ordinary external dependencies. A shared predicate includes these consumers. | Source-open close/reopen included in Sol-verified CHAIR-24 v9 targeted 3/3 and Python 90/90. No separate CHAIR-16 Sol packet. |
| CHAIR-17 | Requested document activation ran inside the prepared commit. Activation and restoration now surround that boundary; final session metadata reports actual restored focus, including initially absent focus. | Resolved; Sol verified ordering, errors and session regressions. |
| CHAIR-23 | Closing the dependent leaves a property backlink to a destroyed expression consumer; the next source edit SIGSEGVs. Whole-document teardown now clears expression dependencies before deletion, with source-side backlink cleanup. | Original crash regression plus clear/restore included in Sol-verified CHAIR-24 v9 targeted 3/3. No separate CHAIR-23 Sol packet. V6 zero-selected gtest is not a pass. |
| CHAIR-07 | Deferred collaboration `ObjectChanged` replay after an eager Assembly commit re-dirtied `App::Link` `_LinkTouched`, leaving `pending_recompute` set. `Gui::LinkView::onLinkedUpdateData` now emits a signal-only tree notify while `Document::collaborationNotificationsReplaying()`; interactive non-Output changes still call `Property::touch()`. | Resolved: Sol verified Luna run17. `live_ready=true`, `ready_for_next_mutation` absent (not `false`), FrontLeft/FrontRight only `recomputed`/`Up-to-date`, no recovery recompute. Gtests: replay 3/3, compatibility 18/18, domain 34 passed + 1 pre-existing FBO skip (skip is not a pass). Fixture `verdict: "reproduced"` is the ~25.5s latency classifier against a 10s threshold, not the producer defect. `smoke-run17-exit.txt` is a lone newline, not a numeric sidecar; success is from `run17/result.json` (`completed=true`, `shutdown.success=true`, `shutdown.forced=false`). Residual: synthetic fixture; original 37-object chair unavailable. |
| CHAIR-24 | Populated-dependent restore followed by source 20→30 still leaves geometry at 20. Runtime tracing shows restored edges exist, but cross-document recompute propagation trips the source-only mutation boundary and invalidates the source. Propagation now waits until successful boundary completion; observer-close and rollback regressions were added. | Resolved: Sol verified native gates. Hashes match `CollaborationCompatibility.cpp` `2528691ae49a5bda9e71e3661ecb25efc51b3a181a7911760ef7082923530184` and `DocumentObject.cpp` `536c037a333e9c46812c06e677219f3f442ec88ba501dd3df01bc9fb6cd0d853`. Luna `luna-chair24-deferred-build-v9` exit 0; both new gtests listed and run 2/2; compiled Spreadsheet 11/11; Python 90/90; targeted 3/3; App 1018 passed, 2 skipped, 7 disabled (skips/disabled are not passes). `Spreadsheet_tests_run` sha256 `fae6387682480144c82239b450cd3f98d9f70d67b7b0fd4905d20ca597eab564`. V6 zero-selected gtest is not a pass. Residual: no wrapper `tests-v9-exit.txt` (six step exits used); `App_tests_run` preamble timestamp vs later `.so` relink. |
| CHAIR-25 | Linux deadline cancellation while reading a chunked response becomes `IncompleteRead`, incorrectly reported as protocol mismatch. Only deadline-expired reads now become timeouts; immediate truncation remains a protocol error. | Resolved; Sol verified the two-line correction, real HTTP regressions and independent baseline/patched reproductions. Original CHAIR-11 linkage remains unproven. |
| CHAIR-26 | A client screenshot deadline closes its socket; native FreeCAD receives SIGPIPE while writing the delayed response. GDB confirms `libc send(flags=0)` through Python. Production uses a separate identity handler, so socket setup lives in setup-only `_NoSigPipeRequestHandler`; `JsonRpcRequestHandler` and `McpIdentityRequestHandler` inherit that base independently. Per-write `MSG_NOSIGNAL` / per-socket `SO_NOSIGPIPE` when available; no process-wide SIGPIPE policy. | Resolved: Sol verified. MCP `git diff HEAD` sha256 `ab411f972868d732642fae895d384283cd0b93d3886701ac1d6f7d92aca5611c`. Luna `luna-mcp-focus-unit-chair26-v9` exit 0; focus 278 passed including 8 required files and both `[default]` and `[identity]` SIGPIPE params; unit 2724 passed, 144 deselected, 1 xfailed I10/P10 (xfail is not a pass). Live `luna-gui-smoke-chair26-v9` exit 0; timeout observed; terminal status passed; six PNGs including `get_view`/`refresh_view` after timeout; primary lane usable; unforced ordered shutdown; did not overwrite `gui-smoke-chair26-final/`. Empty `mcp-focused-chair26-production.log` and predating 2722/2723 counts are not this gate. Residual: synthetic 0.01s timeout, Linux-only skipif, `isolation_verified` false, I10/P10 xfail. |
| DOC-01 | A historical architecture “issue 6” identifier collides with current CHAIR-06. | Resolved; Sol verified explicit historical attribution. |

Architecture diagrams showing components, failure paths and corrected designs
are in [chair-mcp-current-branch-fixes.md](chair-mcp-current-branch-fixes.md).
Historical context is in
[chair-mcp-issue-architecture.md](chair-mcp-issue-architecture.md).

## Remaining investigations and report dispositions

- **CHAIR-07:** Sol-verified on Luna run17. Historical v2–v4 probes remain
  defect reproductions (`verdict reproduced`, 22 anomalies): their exit 0 is
  reproduction, not an acceptance pass. Run17 still reports fixture
  `verdict: "reproduced"` because seat-mutation latency is about 25.5 seconds
  against a 10 second classifier; that is not the `_LinkTouched` producer
  returning. The original 37-object chair remains unavailable.
- **CHAIR-11:** The original transient request-status mismatch lacks its raw
  response/exception. CHAIR-25 fixes a demonstrated related transport failure,
  without assuming the original cause. The earlier synthetic live timeout probe
  that lost its endpoint is superseded by Sol-verified CHAIR-26 `luna-gui-smoke-chair26-v9`
  recovery; that does not prove original CHAIR-11 causation.
- **CHAIR-13 original workflow:** Live synthetic captures are correct, but the
  original Assembly/view-state fixture is unavailable. Sol classifies this as
  a validation limitation, not an independent artifact gate. The original
  dark/cyan symptom is not reproduced; synthetic evidence is identified separately.

The referenced `/home/msi/freecad-test/` models and logs were not found in the
workspace or local WSL distribution. Installed FreeCAD connector calls also
failed to connect; they supplied no original runtime evidence.

The remaining reports have these source-reviewed dispositions. They are not
product test passes and do not justify unrelated changes.

| ID | Disposition |
| --- | --- |
| CHAIR-08 | Direct `ExpressionEngine` assignment is caller misuse; use `set_expression`. |
| CHAIR-09 | Existing-object dynamic-property schema mutation is an intentional native restriction. Schema creation on newly owned objects remains supported. |
| CHAIR-10 | The external validator assumed unavailable methods; correct its API usage. |
| CHAIR-14 | The report withdraws copy-crash attribution and identifies launcher lifetime. The previous merged recovery work is already in the baseline. |
| CHAIR-18 | Reversed taper signs were a modeling error. |
| CHAIR-19 | Grounding after a free solve preserves the displaced pose; establish the intended anchor before solving. |
| CHAIR-20 | Hidden source Body tips also hide linked geometry; retain required source presentation. |
| CHAIR-21 | Apply styling after committed creation when a new object has no `ViewObject` inside the callback. The merged baseline covers its Assembly-specific delayed-provider case. |
| CHAIR-22 | The supplier catalog contradiction is external; no independent supplier verification or repository correction is claimed. |

## Test evidence

Luna ran the following Docker checks. The Sol-verified CHAIR-26 MCP gate is
patch SHA-256
`ab411f972868d732642fae895d384283cd0b93d3886701ac1d6f7d92aca5611c`
from `luna-mcp-focus-unit-chair26-v9` (focus 278 / unit 2724). Predating
**2722** (`luna/mcp-unit-final-v5.*`, freeze
`056f977d57c077150b9fb5f817ef009186134c2933ab7320cb5714a4da6da513`) and
**2723** (`luna/mcp-unit-chair26-final.*`) counts, and empty
`luna/mcp-focused-chair26-production.log`, are **not** that gate. The
Sol-verified CHAIR-24 native gate is `luna-chair24-deferred-build-v9`; the V6
zero-selected gtest run is **not** a pass.

| Run | Actual result | Durable artifacts below the run directory |
| --- | --- | --- |
| CHAIR-26 MCP focus+unit v9 | Container `luna-mcp-focus-unit-chair26-v9` exit 0. Focus **278 passed**, including 8 required files and both `[default]` and `[identity]` SIGPIPE params. Unit **2724 passed, 144 deselected, 1 xfailed** I10/P10 (xfail is not a pass). | `luna/mcp-chair26-v9/` (`mcp-focused-chair26-v9.*`, `mcp-unit-chair26-v9.*`) |
| CHAIR-26 live GUI smoke v9 | Container `luna-gui-smoke-chair26-v9` exit 0. Timeout observed; terminal status passed; six PNGs including `get_view`/`refresh_view` after timeout; primary lane usable; unforced ordered shutdown. Did not overwrite `gui-smoke-chair26-final/`. | `luna/gui-smoke-chair26-v9/` |
| CHAIR-24 native v9 | Container `luna-chair24-deferred-build-v9` exit 0. Both new gtests listed and run 2/2; compiled Spreadsheet **11/11**; Python **90/90**; targeted **3/3**; App **1018 passed, 2 skipped, 7 disabled** (skips/disabled are not passes). `Spreadsheet_tests_run` sha256 `fae6387682480144c82239b450cd3f98d9f70d67b7b0fd4905d20ca597eab564`. No wrapper `tests-v9-exit.txt`; six step exits used. | `luna/chair24-v9/` |
| V6 deferred Spreadsheet gtest | Zero tests selected (`Running 0 tests from 0 test suites`); compiled suite still 9/9. **Not a pass.** | `luna/spreadsheet-deferred-v6.log` |
| Final MCP focused v5 | **263 passed**, no failures/errors/skips, 41.05 seconds. Includes solver fields, quantity serialization, activation/session, PNG filters/layouts, generator byte/digest contracts, and deadline/truncation transport. Predates the CHAIR-26 identity-handler gate. | `luna/mcp-focused-final-v5.log`, `.xml`, `-exit.txt` |
| Final MCP unit v5 | **2722 passed, 144 deselected, 1 xfailed**, exit 0, 427.56 seconds. Not the CHAIR-26 v9 gate. | `luna/mcp-unit-final-v5.log`, `.xml`, `-exit.txt` |
| Native V4 actual regressions | **2 passed, 1 failed**: prior cross-link and source-open close/reopen pass; clear/populated-restore 20→30 fails. | `luna/chair24-actual-v4.log` |
| Native V4 Python Spreadsheet suite | **89 passed, 1 failed** at the same 20-versus-30 assertion. | `luna/spreadsheet-python-v4-full.log` |
| Native V4 compiled Spreadsheet | **9 passed**. | `luna/spreadsheet-native-v4.log`, `.xml` |
| Native V4 compiled App | **1018 passed, 2 skipped, 7 disabled**; 1020 ran. | `luna/app-native-v4.log`, `.xml` |
| Native preliminary V5 | **3/3 targeted**, **90/90 Python Spreadsheet**, **9/9 compiled Spreadsheet**; App **1018 passed, 2 skipped, 7 disabled**. Predates callback-order correction and observer-close/rollback tests; not the Sol-verified v9 gate. V6 zero-selected gtest is not a pass. | `luna/chair24-actual-v5.log`, `spreadsheet-python-v5-full.log`, `spreadsheet-native-v5.*`, `app-native-v5.*` |
| GUI v4 save/reopen captures | Four correct PNG captures, independently viewed by root and Sol. Before/after images are byte-identical per method. Later timeout/status stage failed with connection refused; **overall smoke failed**. | `luna/gui-smoke-v4/result.json`, four PNGs, launcher evidence; `luna/gui-smoke-v4.log` |
| CHAIR-07 GUI gtests v12 | Replay guard **3/3**, compatibility **18/18**, domain **34 passed + 1 skipped** (pre-existing FBO `nativePersonalRenderReturnsPngAndPreservesHumanGuiAndRevisionState`; skip is not a pass). | `luna/chair07-fix-v12/` |
| CHAIR-07 smoke run17 | Producer gate passed: `live_ready=true`, `ready_for_next_mutation` absent not false, links Up-to-date, no extra recompute, unforced shutdown. Fixture `verdict: "reproduced"` is the ~25.5s latency classifier, not a producer fail. `smoke-run17-exit.txt` is a lone newline, not a numeric `0`. | `luna/chair07-smoke-v12/run17/`, `smoke-run17.log` |
| Assembly/Seat v2 reproduction | Full typed sequence completed with **verdict reproduced, 22 anomalies** and graceful shutdown. Exit 0 establishes reproduction, not a product pass. Historical baseline, not run17. | `luna/chair07-smoke-v2/result.json`, `.log`, `-exit.txt` |

The MCP xfail is the existing I10/P10 structured no-screenshot fallback, not a
pass. App skips are unconditional BackupPolicy cases concerning filesystem
cache reliability; seven preexisting disabled tests remain disabled. These
unrelated exclusions were not removed or weakened.

The Sol-verified CHAIR-26 v9 focus selects these eight files, including
`tests/test_transport_layer.py` and both `[default]` and `[identity]` SIGPIPE
params. The earlier v5 command omitted `test_transport_layer.py` and is not
this gate:

```sh
pytest -q -p no:cacheprovider \
  tests/test_phase14_execute_worker_injection.py \
  tests/test_phase15_cad_injection.py \
  tests/test_screenshot_blank_detection.py \
  tests/test_capability_manifest_generator.py \
  tests/test_capability_mirror_shims.py \
  tests/test_generated_registration_cutover.py \
  tests/test_json_rpc_client_transport.py \
  tests/test_transport_layer.py
pytest -q -m unit -p no:cacheprovider
```

MCP uses Python 3.12.3 and image `freecad-ci-mcp:24.04-phase1`, with the declared
Ruff development dependency installed for architecture tests. Early incomplete
logs, incorrect selectors, missing dependencies and failed generated-fixture
runs were retained in the detailed ledger; they are not accepted as passes.
Fixture corrections preserved strict emitted-byte and digest assertions.

The parent and six initialized submodules were archived at the exact baseline.
Dedicated volumes `freecad-chair-20260913-src` and
`freecad-chair-20260913-build` preserve the user's older source/build volumes.
The baseline 6,952-task native build and V4 rebuild completed successfully;
V4 file hashes are in `luna/chair24-sync-v4b-manifest.txt`.

```sh
cmake --preset debug -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build/debug -j4
```

Sol's independent review reproductions are separately attributed:

- CHAIR-25: a real chunked server reproduces Linux baseline misclassification
  10/10 and corrected timeouts 10/10. Immediate truncation remains a protocol
  error. Windows baseline and patched deadline paths already return timeouts.
  Artifacts: `sol/chair25-*`.
- CHAIR-24: `sol/chair24-v4-dependency-trace.log` proved restored coarse/fine
  dependency edges and source invalidation during the live source edit. The
  previous registration-only diagnosis was incomplete. Luna v9 native gates
  later passed and Sol verified; V4 remains historical diagnosis, not the
  current acceptance failure.

## Publication gate

CHAIR-07, CHAIR-24, and CHAIR-26 are Sol-verified. Publication into
`FreeCAD-start` is no longer blocked by those three. Missing original
artifacts still limit attribution for CHAIR-11/13; they do not create an
additional publication gate beyond the reviewed representative regressions.
I10/P10 remains an xfail, not a pass. App skips/disabled and the domain FBO
skip remain exclusions, not passes. CHAIR-26 live timeout recovery is
Sol-verified on `luna-gui-smoke-chair26-v9`.
