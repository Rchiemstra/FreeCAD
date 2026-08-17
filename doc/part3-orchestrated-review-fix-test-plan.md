# Part 3 Execution Plan: High-Review, Composer Fix, Docker Gate

- **Status:** Proposed
- **Date:** 2026-08-16
- **Target branch:** `fix/change-aware-save-mcp-autonomy`
- **Reported starting checkpoints:** parent `2c4c16bb`, nested `0bd67ad5`
- **Architecture source:** `doc/part3-gui-collaboration-stress-design.md`
- **Scope:** Part 3 collaboration correctness through Stage B. Stage C remains a separate fresh-session gate.

This execution plan augments the Part 3 architecture decision record. It does not replace the actor, trust, save, recovery, or collaboration contracts already defined there.

---

## 1. Fixed agent and model policy

The orchestration roles are fixed for the entire run:

| Role | Required agent/model | Permissions |
| --- | --- | --- |
| Orchestration | Main Agent | Controls scope, evidence, routing, tests, commits, and pushes. Does not self-approve. |
| Review | **Grok 4.5 High** | Read-only review. Produces findings and verdicts. Does not edit files. |
| Implementation | **Composer 2.5** | Implements approved work and fixes findings. Does not approve its own work. |
| Validation | Docker and required native platform gates | Executes exact commands and records machine-readable evidence. |

### 1.1 Prohibited substitutions

- The model tier named **Fast** is forbidden.
- Automatic model selection is forbidden.
- Automatic fallback to another model is forbidden.
- Low-effort, mini, lightweight, or reduced-reasoning substitutes are forbidden.
- Grok 4.5 High may not be replaced by Composer for review.
- Composer 2.5 may not be replaced by Grok for implementation.
- If a required model is unavailable, the work item becomes `BLOCKED_MODEL_UNAVAILABLE`. The orchestrator stops rather than silently changing the process.

The selected model and mode are recorded in every review and fix packet.

---

## 2. Authoritative loop

The loop runs once per **commit-sized work package**.

```text
[Main Agent: define work package and capture baseline]
                         |
                         v
              [Grok 4.5 High: review]
                         |
              +----------+-----------+
              |                      |
         PROBLEMS                  NO ISSUES
              |                      |
              v                      v
 [Main Agent accepts/routes]   [Required tests already
              |                pass for exact diff?]
              v                      |
      [Composer 2.5: fix]       +----+----+
              |                 |         |
              v                NO        YES
 [Docker focused validation]    |         |
              |                 v         v
       +------+------+      [Run tests] [Final gate]
       |             |           |         |
     FAIL           PASS         +----+----+
       |             |                |
       v             v                v
 [Composer 2.5] [Grok 4.5 High] [Commit]
       ^             |                |
       |       +-----+-----+          v
       |       |           |        [Push]
       |   PROBLEMS    NO ISSUES       |
       |       |           |           v
       +-------+           +-------- [Done]
```

The actual state machine is:

```text
BASELINED
  -> REVIEW_REQUIRED
  -> FIX_REQUIRED | TEST_REQUIRED | FINAL_GATE_REQUIRED
  -> REVIEW_REQUIRED
  -> FINAL_GATE_REQUIRED
  -> COMMIT_READY
  -> PUSH_READY
  -> DONE
```

No state may be skipped. In particular, `Composer -> Commit` and `Tests -> Commit` are invalid transitions. A final Grok review is mandatory after the last code change and after the required validation evidence exists.

---

## 3. Baseline before every work package

The Main Agent records:

- parent branch, HEAD, upstream, ahead/behind, and status;
- nested branch, HEAD, upstream, ahead/behind, and status;
- parent gitlink and nested HEAD consistency;
- exact untracked files and the exclusion list;
- source diff fingerprint;
- relevant generated-contract fingerprints;
- active FreeCAD, FreeCADCmd, Python, test, Ninja, CMake, and Docker processes;
- port 9875 ownership;
- Docker image tag and immutable image ID/digest;
- build configuration and target binaries;
- line-ending classification for every file the work package may edit.

The work stops immediately if:

- `HamaAdapter_v3` could be touched;
- the user's normal profile would be used;
- port 9875 is occupied;
- an unknown FreeCAD/MCP session is running;
- the parent gitlink is inconsistent;
- the repository contains unexplained test artifacts;
- a reported baseline cannot be reproduced.

The baseline is written to:

```text
results/part3-orchestration/<run-id>/<work-item>/baseline.json
```

`results/` remains untracked and is never committed.

---

## 4. Work-package contract

Each work package must be small enough to become one intentional commit. It contains:

```yaml
work_item_id: P3-WP##
title: one concern only
repository: parent | nested | both
objective: observable end state
in_scope:
  - exact files or components
out_of_scope:
  - frozen subsystems
  - unrelated cleanup
architecture_invariants:
  - rules that may not change
acceptance_tests:
  - exact commands or named gates
platforms:
  - docker-linux
  - windows-native, when required
commit_message: proposed message
```

### 4.1 Scope discipline

Composer may not:

- refactor unrelated code;
- rename unrelated APIs;
- change a frozen subsystem merely to simplify a test;
- weaken assertions, skip tests, add `xfail`, or increase timeouts to obtain green output;
- add arbitrary sleeps for synchronization;
- reintroduce `execute_code` into the acceptance path;
- reintroduce transaction ownership in generated payloads;
- use a document-wide generation as the only semantic conflict fence;
- create a second live-document transaction/commit authority;
- merge or cherry-pick `feature/non-blocking-geometry-scaffolding` wholesale.

If an out-of-scope defect blocks the work package, Composer reports it as `BLOCKED_OUT_OF_SCOPE`; the Main Agent creates a new work package rather than smuggling the change into the current one.

---

## 5. Grok 4.5 High review contract

Grok performs a read-only review of the complete current diff and the relevant surrounding code. It reviews the work package against the architecture, not merely against compiler success.

### 5.1 Required review areas

1. **Actor boundaries**
   - local GUI actions remain local;
   - remote actions use typed JSON-RPC;
   - the coordinator does not execute arbitrary Python in FreeCAD.

2. **Mutation ownership**
   - one native live-document commit owner;
   - no nested transaction control;
   - explicit recompute policy;
   - local and remote model edits publish compatible semantic revisions.

3. **Identity and lifecycle**
   - document UID/instance/lifecycle identity is used where stale sessions or pointer/name reuse matter;
   - document names are diagnostic, not sufficient authority;
   - close/reopen invalidates old sessions and results.

4. **Conflict precision**
   - expected revisions bind to semantic read/write sets;
   - personal view activity cannot conflict;
   - unrelated model changes remain eligible;
   - structural changes invalidate dependent operations.

5. **Exactly-once behavior**
   - mutating RPCs use server-enforced operation identity where retries are possible;
   - duplicate operation IDs with equal payload return the stored result;
   - duplicate operation IDs with different payload fail explicitly.

6. **Undo/redo safety**
   - remote history actions cannot silently undo a user's intervening transaction;
   - any allowed history action is bound to an expected history head or operation identity.

7. **Cross-platform behavior**
   - Windows thread/module boundaries;
   - Windows lock/export rules;
   - 9p and persistent lock-anchor contracts;
   - CRLF preservation.

8. **Acceptance honesty**
   - no fallback from the real GUI pause checkbox in acceptance;
   - no forced shutdown reported as success;
   - no test-only bypass pretending to be a real GUI path;
   - test evidence covers the changed behavior rather than neighboring plumbing.

9. **Future non-blocking geometry compatibility**
   - detached geometry may become an execution backend later;
   - `DocumentCommitCoordinator` remains the sole final live commit owner;
   - future geometry results must be validated with semantic read-set revisions, not only global `modelGeneration`;
   - the exported out-of-line `MainThreadSignal` hook storage must not regress.

### 5.2 Finding format

Grok returns exactly one verdict:

```text
PROBLEMS
NO_ISSUES
BLOCKED_INSUFFICIENT_EVIDENCE
```

Every blocking finding has this schema:

```yaml
id: GRK-P3-###
severity: critical | high | medium | low
category: architecture | correctness | concurrency | lifecycle | security | test | portability | maintainability
file: path
lines: start-end
claim: one precise defect
evidence: concrete code path or failed invariant
impact: observable consequence
required_fix: bounded outcome, not a vague suggestion
required_test: exact regression proof
```

Rules:

- No finding may consist only of style preference.
- No finding may demand unrelated cleanup.
- “Consider” and “maybe” are non-blocking notes, not blocking findings.
- A `NO_ISSUES` verdict must list the exact diff fingerprint reviewed and the test packet fingerprint considered.
- Re-review examines the **entire current diff**, not only previously reported lines.

Review output is stored under:

```text
results/part3-orchestration/<run-id>/<work-item>/iteration-###/review.yaml
```

---

## 6. Main Agent finding triage

The orchestrator does not rewrite findings into softer language and does not silently discard them.

Each finding becomes one of:

```text
ACCEPTED
DUPLICATE_OF:<id>
DISPUTED_WITH_EVIDENCE
OUT_OF_SCOPE_BLOCKER
```

For `DISPUTED_WITH_EVIDENCE`, the Main Agent sends the exact source/test evidence back to Grok. Grok must resolve it as withdrawn, revised, or still blocking. The orchestrator cannot overrule an unresolved critical/high finding and proceed to commit.

Accepted findings are passed to Composer unchanged, together with the work-package contract and relevant source context.

---

## 7. Composer 2.5 fix contract

Composer receives:

- the work-package definition;
- baseline fingerprints;
- accepted Grok finding IDs;
- exact failure logs from prior validation;
- frozen-layer and safety constraints;
- the required test list.

Composer must:

1. reproduce or reason from the supplied evidence before editing;
2. implement the smallest coherent fix;
3. add or update a regression test for every correctness finding;
4. preserve file line endings;
5. update generated fixtures only through the authoritative generator;
6. avoid changing the test to match broken production behavior;
7. report every edited file and why it changed;
8. map each finding ID to its resolution;
9. report any unresolved finding as unresolved rather than claiming completion.

Composer returns:

```yaml
model: Composer 2.5
resolved_findings:
  GRK-P3-001: explanation
unresolved_findings: []
files_changed:
  - path: reason
tests_added_or_changed:
  - test: invariant
generated_outputs:
  - path: generator command
known_limitations: []
```

Composer does not commit, push, or declare the work package approved.

---

## 8. Validation after every Composer change

Every Composer edit is followed by Docker validation. Docker success is necessary but not sufficient for Windows-specific work.

### 8.1 Iteration gate

Run the narrowest authoritative set that covers the changed concern:

1. compile/configuration checks for affected targets;
2. focused unit/integration tests named by Grok findings;
3. architecture/static policy tests;
4. generated-contract equality tests where applicable;
5. Ruff/lint/compileall for changed Python surfaces;
6. `git diff --check` and line-ending validation.

Use a fresh container process and an isolated working directory. Compiler caches may be retained, but build outputs are keyed by:

```text
source HEADs + git diff fingerprint + build configuration + toolchain fingerprint
```

When App ABI, Gui ABI, or shared headers change, rebuild every dependent target. A rebuilt `App_tests_run` with a stale `FreeCADGui` is not evidence.

### 8.2 Native platform gate

Run native Windows validation in the same loop when the work package touches:

- GUI owner-thread behavior;
- DLL exports or cross-module storage;
- Win32 file sharing, rename, recovery, or lock behavior;
- launcher/session behavior on Windows;
- graceful GUI/process shutdown.

Docker may not certify those properties by proxy. Computers are literal enough without us asking Linux to vouch for Windows kernel semantics.

### 8.3 Failure packet

On failure, the Main Agent sends Composer:

```yaml
command: exact command
exit_code: value
source_fingerprint: value
diff_fingerprint: value
container_image_id: value
platform: value
failed_tests:
  - exact name
log_path: path
first_relevant_error: verbatim excerpt
full_log_available: true
```

Do not paraphrase away the useful part of a compiler or test failure.

### 8.4 Pass packet

On success, record:

```yaml
verdict: PASSED
commands:
  - exact command and exit code
test_counts: run/passed/skipped/failed
source_fingerprint: value
diff_fingerprint: value
binary_fingerprints: relevant outputs
artifacts: paths
```

The pass packet is supplied to Grok for re-review.

---

## 9. Re-review loop

After validation passes, Grok 4.5 High reviews:

- the complete current diff;
- all previously accepted findings;
- Composer's resolution map;
- test changes;
- the exact validation packet;
- any generated outputs;
- the final architecture impact.

Possible outcomes:

```text
PROBLEMS
  -> route accepted findings to Composer

BLOCKED_INSUFFICIENT_EVIDENCE
  -> Main Agent obtains the missing test/evidence

NO_ISSUES
  -> proceed only if all work-package tests passed for the same diff fingerprint
```

A new code change after `NO_ISSUES` invalidates the verdict and restarts review.

---

## 10. Loop-breaker rules

The loop must make measurable progress rather than orbiting the same defect until everyone forgets why it mattered.

- Every iteration must reduce unresolved findings, add a reproducer, or produce new evidence.
- If the same finding reopens twice, enter **root-cause mode**:
  1. Grok writes a causal hypothesis and minimal reproduction contract.
  2. Composer first adds the failing regression test or probe.
  3. The failure is observed.
  4. Only then is production code changed.
- If three consecutive Composer attempts fail the same test, stop normal patching and create a dedicated blocker work package.
- Timeout increases, retries, sleeps, skips, assertion weakening, and forced process termination do not count as progress.
- A flaky test is a defect to diagnose, not a coin to keep tossing until it lands green.

---

## 11. Final gate for a work package

A work package is `COMMIT_READY` only when all of these are true:

- Grok verdict is `NO_ISSUES`;
- every accepted finding is resolved;
- iteration Docker gate passed;
- required native platform gate passed;
- required repository/static gates passed;
- source and diff fingerprints in review and test evidence are identical;
- no file changed after final review;
- no unexplained artifacts exist;
- parent/nested/gitlink state is still consistent;
- commit contents match the declared work-package scope.

The Main Agent computes and records:

```text
git diff --binary | SHA-256
```

That fingerprint binds the final review and tests to the exact commit candidate.

---

## 12. Commit and push protocol

The loop ends in one intentional commit per work package.

### 12.1 Nested repository work

1. Verify nested final gate.
2. Stage explicit paths only.
3. Commit with the declared message.
4. Verify the commit tree contains no results, logs, user files, `tests/lib`, or research artifacts.
5. Push the nested branch without force.
6. Verify the remote branch resolves to the pushed SHA.
7. Update the parent gitlink in a separate parent work package or the explicitly paired parent package.

### 12.2 Parent repository work

1. Verify parent final gate against the exact nested SHA.
2. Stage explicit paths only.
3. Commit.
4. Push without force.
5. Verify the remote parent SHA and the recorded gitlink.

No amend, rebase, force push, or pull request.

If push fails, do not rewrite a reviewed commit merely to make transport convenient. Resolve the repository state explicitly and repeat the necessary verification if the commit candidate changes.

---

## 13. Updated Part 3 work-package order

The following order minimizes expensive cross-platform repetition and prevents the harness from being built on unresolved product semantics.

### P3-WP00 — ADR corrections and execution contract

- Add this orchestration loop to the Part 3 documentation.
- Replace “three trust domains” with precise actor and credential boundaries.
- State that the RemoteAgentDriver runs in a child process that never receives the local control token.
- Bind actor identity to authenticated session state, not a caller-provided string.
- Add the non-blocking geometry compatibility section without merging that branch.
- Prohibit an acceptance fallback from the real Pause Agent Writes checkbox.
- Define stable document identity and lifecycle invalidation.
- Define exactly-once and undo/redo semantics before implementation.

**Gate:** documentation review plus architecture consistency checks.

### P3-WP01 — Generic identity-bound mutation preconditions

- Avoid a property-test-specific final product API.
- Use a stable document selector containing instance identity/lifecycle data; name remains diagnostic.
- Provide generic expected semantic revisions and typed mutation intent.
- Keep explicit edit sessions only where a long-lived preparation really requires them.

**Gate:** native conflict, independent edit, close/reopen stale-session tests.

### P3-WP02 — Operation idempotency and safe history semantics

- Add server-side operation deduplication for mutating typed RPCs that may be retried.
- Equal operation ID + equal canonical payload returns the stored terminal result.
- Equal operation ID + different payload returns a protocol conflict.
- Remote undo/redo requires an expected history head or operation identity.
- An intervening local transaction causes safe refusal, not undo of user work.

**Gate:** lost-response retry test and intervening-local-edit history test.

### P3-WP03 — Save Copy and explicit recompute registry

- Add typed Save Copy.
- Declare `none` or `target` for every typed mutation.
- Keep public `execute_code` default at `none`.
- Regenerate contracts through authoritative tooling.

**Gate:** nested focused tests, generator equality, full nested unit/lint selection.

### P3-WP04 — LocalUserDriver foundation

- Isolated-profile-only module.
- Authenticated loopback endpoint with operation acknowledgements.
- Qt owner-thread execution.
- Camera, pan, zoom, fit, selection, tree, active view.
- Real checkbox-only pause/resume in acceptance.
- Remote agent child process proves it lacks the control token.

**Gate:** focused GUI tests and remote-resume denial.

### P3-WP05 — Prove the real local model path

Before adding `local_property_edit`, prove whether ordinary Property Editor, Sketcher, and task-panel commands use the same native revision/commit accounting.

- If they already do, drive that path.
- If they do not, unify the product path before using it in acceptance.
- A special test-only local mutation route is not sufficient evidence.

Also add one real local GUI Save action on a disposable document.

**Gate:** local model edit publishes expected revisions; local Save orders correctly with remote mutation.

### P3-WP06 — Stress coordinator and static architecture gate

- Add `tests/gui/part3/` package.
- Typed remote driver only.
- Local driver client only.
- Evidence schema and deterministic scenarios.
- Delete the retired harness in the same commit.
- Forbid `execute_code`, Pivy, transaction strings, remote pause/resume, and synchronization sleeps.

**Gate:** AST architecture policy and launcher/isolation tests.

### P3-WP07 — Conflict, independence, pause, and exactly-once integration

- Same-property targeted conflict.
- Independent-property success.
- Pause-after-current behavior.
- Reads while paused.
- Local resume.
- Lost-response retry commits once.
- Healthy error does not poison the global lane.

**Gate:** deterministic integration suite before stress.

### P3-WP08 — Windows lock anchor

- Use only the test-scoped export mechanism.
- Run the acknowledged A -> B -> C cross-process sequence.
- Prove abrupt exit releases kernel ownership and the pathname remains.

**Gate:** Windows native lock-anchor gate plus Linux recheck if shared infrastructure changes.

### P3-WP09 — Graceful owned-session shutdown

- Close disposable documents.
- Close RPC admission and workers.
- Close the GUI from the owner thread.
- Exit within the documented deadline.
- Forced termination always fails the run.

**Gate:** repeated native Windows and Linux shutdown tests with timestamped evidence.

### P3-WP10 — Stage A and Stage B acceptance

Run all pre-stress gates, then:

```text
Stage A: 10 view/mutation cycles, 5 save cycles
Stage B: 50 view/mutation cycles, 20 save cycles
```

Both must use exact committed parent/nested SHAs and record binary fingerprints. A failure creates a new fix work package and restarts the review–fix–test loop. Do not patch an acceptance failure directly inside an untracked stress run.

### P3-WP11 — Stage C, separate fresh session

From the exact successful Stage B SHAs and binaries:

```text
Stage C: 500 view/mutation cycles, 100 save cycles
```

No code changes between successful Stage B and Stage C unless Stage C exposes a real defect. Until Stage C passes with graceful shutdown, final Part 3 remains `NO-GO`.

---

## 14. Required final report

For every work package and the final Part 3 run, report:

- work-item ID and objective;
- starting and ending parent/nested SHAs;
- Grok model and verdict history;
- Composer model and resolved finding IDs;
- Docker image ID and commands;
- native Windows commands where required;
- test counts and evidence paths;
- final diff fingerprint;
- commit and remote push SHAs;
- files changed;
- generated outputs;
- remaining limitations;
- Stage A/B/C status;
- final `GO`, `NO-GO`, or `BLOCKED` reason.

A green statement without its exact diff, test commands, and evidence fingerprint is not a result.

---

## 15. Role prompts

### 15.1 Main Agent

```text
Act only as the Part 3 orchestrator. Use Grok 4.5 High for every review and
Composer 2.5 for every implementation or fix. Never select or fall back to the
Fast model tier or any automatic/reduced model. If a required model is
unavailable, stop with BLOCKED_MODEL_UNAVAILABLE.

Operate on one commit-sized work package at a time. Record parent/nested/gitlink
state, source and diff fingerprints, process/port state, Docker image identity,
and line endings before work begins. Route all accepted review findings to
Composer unchanged. After every Composer edit, run the required Docker gate and
any required native Windows gate. Send passing evidence back to Grok for a full
re-review of the complete diff.

Commit and push only when Grok reports NO_ISSUES, all required tests pass for
the exact same diff fingerprint, no files changed after review, and repository
state is clean except for explicitly excluded artifacts. Commit nested changes
first, push them, then update and commit the parent gitlink. Never amend,
rebase, force-push, open a PR, touch HamaAdapter_v3, use the normal profile, or
accept forced process termination as success.
```

### 15.2 Grok 4.5 High

```text
Perform a read-only, adversarial review of the complete current work-package
diff and relevant surrounding code. Review architecture, concurrency,
lifecycle identity, semantic conflict precision, exactly-once behavior,
transaction/recompute ownership, Windows/Linux behavior, acceptance honesty,
tests, generated contracts, and line endings.

Do not edit files. Return only PROBLEMS, NO_ISSUES, or
BLOCKED_INSUFFICIENT_EVIDENCE. Every blocking finding must have a stable ID,
severity, file/lines, concrete evidence, impact, bounded required fix, and exact
required regression test. Re-review the entire current diff after every fix,
not merely prior findings. A NO_ISSUES verdict must name the exact diff and test
packet fingerprints reviewed.
```

### 15.3 Composer 2.5

```text
Implement only the current work package and accepted Grok finding IDs. Preserve
all frozen save/recovery/launcher/threading contracts. Make the smallest
coherent production fix and add a regression test for every correctness issue.
Do not refactor unrelated code, weaken tests, add skips or sleeps, increase
timeouts, use execute_code in Part 3, introduce a second transaction owner, or
merge the non-blocking geometry branch wholesale.

Preserve line endings and regenerate contract fixtures only through their
authoritative generator. Return a finding-resolution map, files changed,
tests added or modified, generator commands, and unresolved limitations. Do not
commit, push, or approve your own work.
```

---

## 16. Progression

Orchestrator tracker only. Does not replace §§1–15.

Checked 2026-08-17: parent HEAD `2c4c16bb` and nested HEAD `0bd67ad5` match the reported starting checkpoints on `fix/change-aware-save-mcp-autonomy`. P3-WP00 baseline, review, fix-notes, and re-review artifacts exist under `results/part3-orchestration/2026-08-17/P3-WP00/`. Nested working tree still reports dirty tests plus untracked nested logs (classified in baseline). Parent also has untracked `results/`, `tests/lib/`, and `doc/deep-research-report (1).md`. Plan header whitespace strip **Done**. Post-whitespace Grok 4.5 High re-review verdict **NO_ISSUES**. Docs/`git diff --check`/commit+push gate **PASS**. Nested gitlink held at `0bd67ad5`.

### Phase / step status

| Phase / step | Status | Notes |
|---|---|---|
| Orchestration tracker (this section) | Done | Added 2026-08-17. Plan header remains **Proposed**. |
| P3-WP00 loop: §3 baseline | Done | VALID 2026-08-17. Wrote `results/part3-orchestration/2026-08-17/P3-WP00/{work-package.yaml,baseline.json}`. Checkpoints reproduced. Gitlink SHA consistent. Artifacts classified. |
| P3-WP00 loop: Grok 4.5 High review | Done | Verdict **PROBLEMS** (2026-08-17). Findings GRK-P3-001..009 in `iteration-001/review.yaml`. |
| P3-WP00 loop: Composer 2.5 fix (docs) | Done | 2026-08-17. Composer 2.5 resolved GRK-P3-001..009 in ADR; wrote `iteration-001/fix-notes.md`. Docs-only; no product code. No commit. |
| P3-WP00 loop: Grok 4.5 High re-review | Done | First **NO_ISSUES** (2026-08-17) in `iteration-001/re-review.yaml` — superseded for commit by later plan edits. |
| P3-WP00 loop: whitespace unblock | Done | 2026-08-17. Plan header trailing whitespace stripped; `git diff --check` on ADR+plan exit 0. |
| P3-WP00 loop: Grok 4.5 High re-review (after whitespace) | Done | Verdict **NO_ISSUES** (2026-08-17). Complete current ADR+plan reviewed. GRK-P3-001..009 PASS. Header meaning unchanged. ADR contracts intact. Wrote `iteration-001/re-review-after-whitespace.yaml`. |
| P3-WP00 loop: Docker+native gates / commit+push | Done | 2026-08-17. Docs consistency PASS. `git diff --check` exit 0. Parent commit of ADR + this plan; nested gitlink not bumped. Composer idle. |
| P3-WP00 — ADR corrections and execution contract | Done | Gate PASS after Grok `NO_ISSUES`. ADR leftover “three trust domains”, caller `actor_id` API, and pause-checkbox fallback remain gone. |
| P3-WP01 — Generic identity-bound mutation preconditions | Not started | Follows WP00. |
| P3-WP02 — Operation idempotency and safe history semantics | Not started | Follows WP01. |
| P3-WP03 — Save Copy and explicit recompute registry | Not started | Follows WP02. |
| P3-WP04 — LocalUserDriver foundation | Not started | Follows WP03. |
| P3-WP05 — Prove the real local model path | Not started | Follows WP04. |
| P3-WP06 — Stress coordinator and static architecture gate | Not started | Follows WP05. `tests/gui/part3/` does not exist. |
| P3-WP07 — Conflict, independence, pause, and exactly-once integration | Not started | Follows WP06. |
| P3-WP08 — Windows lock anchor | Not started | Follows WP07. Requires native Windows gate. |
| P3-WP09 — Graceful owned-session shutdown | Not started | Follows WP08. Native Windows and Linux. |
| P3-WP10 — Stage A and Stage B acceptance | Not started | Follows WP09. Scope of this plan ends here. |
| P3-WP11 — Stage C, separate fresh session | Not started | Out of this run. Fresh session from successful Stage B SHAs. Part 3 stays `NO-GO` until it passes. |

### Next step

P3-WP01 baseline. Do **not** start product/MCP/GUI implementation in this tracker update. Do not relaunch Composer 2.5 for WP00. Do not merge `feature/non-blocking-geometry-scaffolding`.

### Change log

- **2026-08-17**: P3-WP00 final gate **PASS**. Docs consistency PASS; `git diff --check` exit 0 on ADR + this plan. Committed only those two files after Grok `NO_ISSUES`. Nested gitlink held at `0bd67ad5`. Composer idle. Next: P3-WP01 baseline.
- **2026-08-17**: P3-WP00 Grok 4.5 High re-review (after whitespace) Done. Verdict **NO_ISSUES**. Complete current ADR+plan reviewed; prior pre-whitespace `NO_ISSUES` not treated as covering these files. GRK-P3-001..009 still PASS; header Status/Date/branch/checkpoints/architecture-source/scope meaning unchanged; plan still augments ADR; ADR contracts intact (whitespace pass did not alter ADR architecture). `git diff --check` exit 0 observed. Wrote `results/part3-orchestration/2026-08-17/P3-WP00/iteration-001/re-review-after-whitespace.yaml`. Next: Main Agent re-run docs/`git diff --check`/commit+push gate. No Composer. No commit by reviewer. Parent still `2c4c16bb`.
- **2026-08-17**: Plan header whitespace strip **Done**. Converted L3–L7 markdown two-space hard breaks to a short list (no trailing spaces; LF kept). `git diff --check -- doc/part3-orchestrated-review-fix-test-plan.md doc/part3-gui-collaboration-stress-design.md` exit 0. No commit. No push. Parent still `2c4c16bb`. Next: Grok 4.5 High re-review of the complete current ADR+plan diff (plan changed after prior `NO_ISSUES`). Not P3-WP01.
- **2026-08-17**: P3-WP00 final gate **BLOCKED**. Docs consistency PASS (ADR states every §13 P3-WP00 bullet; plan remains an augmentation; “three trust domains” absent; caller `actor_id` API absent except the prohibition; pause-checkbox fallback prohibited). `git diff --check` FAIL: `doc/part3-orchestrated-review-fix-test-plan.md` lines 3, 4, 5, 6, 7 trailing whitespace. ADR `--check` clean. Commit candidate would have been only the two `doc/part3-*` files; exclusions (`results/`, `tests/lib/`, nested gitlink, `doc/deep-research-report (1).md`) held. No commit. No push. Parent still `2c4c16bb`. Next: Main Agent strips those five trailing spaces (no Composer); Grok re-review; re-run gate. Not P3-WP01.
- **2026-08-17**: P3-WP00 Grok 4.5 High re-review Done. Verdict **NO_ISSUES**. All GRK-P3-001..009 PASS against current ADR; no new blocking findings. Wrote `results/part3-orchestration/2026-08-17/P3-WP00/iteration-001/re-review.yaml`; updated `review.md` / `review.json`. Next: Main Agent docs consistency / `git diff --check` / commit gate. No Composer relaunch. No commit by reviewer.
- **2026-08-17**: P3-WP00 Composer 2.5 iteration-001 Done (docs-only). Resolved GRK-P3-001..009 in `doc/part3-gui-collaboration-stress-design.md`. Added §0.1 orchestration loop, §3.1 document identity, §3.2 undo/redo safety, §2.2 exactly-once, §10.1 non-blocking geometry compatibility; rewrote §1 actor/credential boundaries and RemoteAgentDriver child-process isolation; removed pause-checkbox fallback and caller `actor_id`; bound checked-edit methods to authenticated session. Wrote `results/part3-orchestration/2026-08-17/P3-WP00/iteration-001/fix-notes.md`. No product code. No commit. Next: Grok 4.5 High re-review.
- **2026-08-17**: P3-WP00 §3–4 baseline captured. Wrote `results/part3-orchestration/2026-08-17/P3-WP00/work-package.yaml` and `baseline.json`. Verdict **VALID** (ok to launch Grok 4.5 High). Parent `2c4c16bb` and nested `0bd67ad5` reproduced; gitlink SHA consistent (`0bd67ad5-dirty` is nested autocrlf stat dirt, blobs match HEAD). Classified `results/`, nested Luna logs, `tests/lib/`, and `doc/deep-research-report (1).md` on the plan §3/§12.1 exclusion list. Nested dirty tests are known autocrlf dirt, not an invented exclusion. ADR `doc/part3-gui-collaboration-stress-design.md` is missing (searched tree and git history; alternatives recorded). No commit. No review started.
- **2026-08-17**: Orchestration started. Added progression table, next-step, and changelog tracker at the bottom of this plan. No review/fix/test work executed. No commit.
