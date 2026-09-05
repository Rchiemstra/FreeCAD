# FreeCAD MCP Unified Improvement Plan

**Repository:** `https://github.com/Rchiemstra/freecad-mcp`  
**Branch:** `main`  
**Baseline inspected:** commit `a474f3889b62e826028b3bd20ece390f438f1393`  
**Scope:** implement all items below in one continuous engineering pass.

> Do not stop after each phase to ask for approval. Work through the full plan, run the complete test suite, repair regressions, and finish with one consolidated implementation report. Use reasonable assumptions where details are missing. Preserve backward compatibility unless this plan explicitly authorizes a breaking change.

---

## 1. Mission

Improve the FreeCAD MCP so that every operation is:

- attributable to an exact MCP, addon, FreeCAD, document, request, and worker runtime;
- represented by a consistent structured result;
- logged through correlated, redacted, machine-readable lifecycle events;
- classified correctly as success, negative observation, protected rejection, timeout, cancellation, degradation, or failure;
- validated against FreeCAD document health;
- transactionally safe where FreeCAD supports rollback;
- recoverable after worker or GUI timeouts;
- measurable through reproducible task-level benchmarks;
- biased toward typed tools rather than arbitrary `execute_code`.

Do **not** replace the existing stdio MCP transport, XML-RPC backend, GUI dispatcher, lease system, worker architecture, or typed tool surface. Extend and normalize what already exists.

---

## 2. Existing capabilities that must be preserved

The current `main` branch already contains important functionality. Do not rebuild or remove it:

- authenticated RPC protocol v2;
- exact isolated-instance manifests;
- per-document renewable write leases;
- lease heartbeat and recovery states;
- request idempotency and replay protection;
- GUI-thread authorization revalidation;
- typed mutation descriptors;
- `GuiMutationTransaction`;
- isolated FreeCADCmd workers;
- worker status and cancellation;
- late GUI completion journaling;
- snapshots and restore;
- typed PartDesign, sketch, spreadsheet, assembly, geometry, IO, diagnostic, and view tools;
- Docker-based unit, e2e, and FreeCAD-core tests.

The work below should integrate with those systems.

---

## 3. Main problems to solve

### 3.1 Runtime identity is strong internally but unclear to users

Problems:

- the package version is still `0.1.20`;
- the MCP client build ID is hard-coded as `freecad-mcp-0.1.20`;
- installed wheels may not reveal their Git revision;
- the README in the fork still contains upstream clone commands;
- an operator cannot get one concise report showing the exact MCP, addon, FreeCAD, profile, protocol, and Git identities currently connected.

### 3.2 Tool results are inconsistent

Problems:

- `json_response()` serializes JSON into text but does not always populate `structuredContent`;
- different tools return different success keys such as `success`, `ok`, or MCP `isError`;
- error codes are not preserved consistently at the MCP boundary;
- condition checks such as `check_rpc_sync` use `isError=true` when they correctly observe `synchronized=false`;
- transport success, tool success, backend success, transaction outcome, and document-health outcome are not separated.

### 3.3 Logging is not a complete lifecycle event stream

Problems:

- `debug_log.py` is useful but does not emit a complete correlation hierarchy;
- event fields are not standardized across MCP, RPC, GUI, worker, transaction, health, cancellation, and recovery;
- task, call, execution, worker, and document IDs are not consistently attached;
- timestamps lack monotonic timing and high-resolution UTC;
- multiple MCP instances can target the same file;
- KPI analysis still depends on pairing requests, parsing text, and inferring incidents.

### 3.4 Document-health validation is incomplete

Problems:

- postflight validation is strongest for selected PartDesign tools but not all mutation classes;
- the system does not consistently compare health before and after an operation;
- pre-existing errors and newly introduced errors are not always separated;
- modified, created, deleted, and unexpectedly changed objects are not always reported;
- successful tool execution can still leave a degraded document.

### 3.5 Rollback outcome can disappear

Problems:

- `GuiMutationTransaction.abort()` suppresses rollback exceptions;
- transaction opened, committed, aborted, and rollback-failed states are not returned or logged consistently;
- arbitrary `execute_code` has intentionally limited transaction coverage, but that limitation is not expressed through a common result model.

### 3.6 Timeout and recovery classification still uses text

Problems:

- some GUI timeout staging is inferred by searching exception messages for phrases such as `before execution` or `while executing`;
- generic `GUI_TIMEOUT` hides the actual stage;
- clients must use `check_rpc_sync` without a first-class request lifecycle query;
- late completion, cancellation, mutation-started state, and uncertainty exist internally but are not exposed uniformly;
- worker jobs and GUI requests have different public lifecycle models.

### 3.7 The MCP has many typed tools, but agents still overuse `execute_code`

Problems:

- public arbitrary-code calls and internal generated-code calls are not separated in KPI reporting;
- tool descriptions and results may make the universal tool easier for an agent than the safer typed tool;
- there is no benchmarked evidence showing where typed tools fail to cover real workflows;
- adding more tools without measuring adoption may only enlarge the schema.

### 3.8 Tests validate components but not complete user tasks

Problems:

- unit, e2e, and core suites exist;
- there is no dedicated benchmark suite that groups tool calls into tasks;
- task success, first-attempt success, selection accuracy, safe failure, calls per task, and tokens per task cannot be measured reliably;
- CI does not produce a consolidated MCP KPI report.

---

# 4. Implementation rules

1. Work from `main` and keep the repository buildable after every logical change.
2. Do not weaken lease enforcement, authentication, redaction, or worker isolation.
3. Do not log raw credentials, lease tokens, session tokens, HMAC proofs, full Python code, or base64 image bodies in normal mode.
4. Keep human-readable MCP text responses for compatibility, but make `structuredContent` authoritative.
5. Prefer stable enums and codes over parsing human-readable text.
6. Negative observations are not tool failures.
7. A transport-level success is not automatically a domain-level success.
8. Validate health deltas, not only final state.
9. Do not run expensive full-document shape validation after every small edit.
10. Do not claim rollback coverage for filesystem changes, process-global state, GUI side effects, or arbitrary Python unless it is actually provided.
11. Add tests with every behavioral change.
12. Keep old log parsing available for migration, but make the new JSONL event format the default for KPI work.
13. Do not remove `execute_code`; reduce reliance on it safely.
14. Do not stop after implementing observability. Complete the benchmark and adoption work too.

---

# 5. Target architecture

```text
MCP client
    │
    │ task_id / call_id / attempt metadata where available
    ▼
FastMCP tool middleware
    │
    ├── normalized result envelope
    ├── lifecycle telemetry
    └── typed tool operations
            │
            ▼
FreeCADConnection / authenticated RPC v2
            │
            ├── request_id / execution_id
            ├── document-session credentials
            └── replay protection
                    │
                    ▼
FreeCAD addon RPC server
    ├── policy and lease validation
    ├── mutation transaction
    ├── GUI dispatcher
    ├── isolated worker manager
    ├── document-health postflight
    └── cancellation/recovery registry
```

Correlation hierarchy:

```text
session_id
└── task_id
    └── call_id
        └── request_id / execution_id
            ├── document_session_uuid
            ├── worker_job_id
            └── recovery_incident_id
```

---

# 6. Phase A: Build and runtime identity

## Goal

Make the exact running source identity visible and machine-verifiable.

## Changes

### A1. Add a central build-info module

Create:

```text
src/freecad_mcp/build_info.py
```

It should expose:

```python
package_version
build_id
git_commit
git_dirty
build_timestamp
protocol_version
event_schema_version
```

Rules:

- obtain package version using `importlib.metadata`;
- allow CI/release builds to inject Git SHA and dirty state through generated metadata or environment variables;
- do not depend on `.git` existing at runtime;
- use a deterministic fallback such as `freecad-mcp-<version>+unknown`;
- never hard-code the version in `server.py`.

### A2. Replace the hard-coded client build ID

Replace:

```python
client_build_id="freecad-mcp-0.1.20"
```

with the canonical build ID from `build_info.py`.

### A3. Add a typed runtime-info tool

Add:

```text
get_runtime_info
```

It must return structured data containing:

```json
{
  "mcp": {
    "version": "...",
    "build_id": "...",
    "git_commit": "...",
    "git_dirty": false,
    "pid": 123,
    "runtime_id": "..."
  },
  "addon": {
    "version": "...",
    "build_id": "...",
    "runtime_id": "..."
  },
  "freecad": {
    "version": "...",
    "revision": "...",
    "pid": 456
  },
  "rpc": {
    "protocol_version": 2,
    "features": []
  },
  "profile": {
    "instance_id": "...",
    "path_fingerprint": "..."
  },
  "compatibility": {
    "compatible": true,
    "warnings": []
  }
}
```

Reuse the authenticated manifest and handshake information already stored in `ServerState`.

### A4. Improve compatibility checks

- keep exact isolated-instance verification;
- add an explicit warning when package and addon builds differ but protocol compatibility permits connection;
- fail only when protocol or required feature compatibility is broken;
- emit the runtime identity in telemetry at session startup and after authentication refresh.

### A5. Correct repository documentation

Update the README so the fork is unambiguous:

- clone URL should use `Rchiemstra/freecad-mcp`;
- mention upstream separately;
- explain how to install the MCP package and addon from the same checkout;
- document `get_runtime_info`;
- document how build IDs are created.

## Primary files

```text
pyproject.toml
README.md
src/freecad_mcp/build_info.py
src/freecad_mcp/server.py
src/freecad_mcp/server_state.py
src/freecad_mcp/rpc_auth.py
scripts/setup_isolated_profile.py
scripts/start_freecad_isolated.py
```

## Tests

- build ID fallback without `.git`;
- build ID injected by environment or generated file;
- matching runtime identities;
- warning on build mismatch;
- failure on protocol mismatch;
- no credential exposure in runtime-info output.

---

# 7. Phase B: Unified structured result model

## Goal

Make every MCP tool return a consistent structured outcome.

## B1. Add result types

Create:

```text
src/freecad_mcp/outcomes.py
```

Define enums or validated string constants:

```text
succeeded
condition_false
warning
degraded
rejected
failed
timed_out
cancelled
unknown
```

Define layers:

```text
transport_status
tool_status
backend_status
transaction_status
document_health_status
```

Define stable common error codes. Existing subsystem-specific codes may remain, but must be passed through unchanged.

## B2. Define the common result envelope

All tools should use:

```json
{
  "schema_version": 1,
  "status": "succeeded",
  "operation": "pad_feature",
  "message": "Pad created",
  "error": null,
  "error_code": null,

  "correlation": {
    "session_id": "...",
    "task_id": "...",
    "call_id": "...",
    "request_id": "...",
    "execution_id": "...",
    "worker_job_id": null,
    "document_session_uuid": "..."
  },

  "execution": {
    "mode": "gui",
    "stage": "completed",
    "duration_ms": 842
  },

  "transaction": {
    "enabled": true,
    "started": true,
    "committed": true,
    "rollback_attempted": false,
    "rollback_succeeded": null
  },

  "document_health": {
    "verdict": "healthy",
    "new_recompute_errors": [],
    "new_invalid_shapes": []
  },

  "data": {}
}
```

Fields may be omitted when irrelevant, but the meaning of included fields must be stable.

## B3. Refactor response helpers

Update:

```text
src/freecad_mcp/responses.py
```

Required behavior:

- `json_response(data)` must populate both readable text and `structuredContent`;
- `tool_ok()` must accept a structured result envelope;
- `tool_fail()` must preserve `error_code`, correlation, execution, transaction, and health data;
- screenshots remain MCP image content and must not be copied into `structuredContent`;
- text remains concise;
- `structuredContent` becomes authoritative for machine consumers.

## B4. Correct condition tools

Change `check_rpc_sync`:

- `synchronized=true` → `status=succeeded`;
- `synchronized=false` with a valid backend response → `status=condition_false`, `isError=false`;
- malformed response, nonce mismatch, or transport failure → actual failure.

Apply the same rule to status/query tools:

- worker busy;
- lease not currently recoverable;
- no matching objects;
- validation found issues;
- no collisions found.

A correctly observed false condition is not a failed tool call.

## B5. Preserve error codes

Audit every operation adapter and stop replacing structured backend errors with generic strings.

Priority locations:

```text
src/freecad_mcp/server.py
src/freecad_mcp/responses.py
src/freecad_mcp/operations/core.py
src/freecad_mcp/operations/diagnostics.py
src/freecad_mcp/operations/interactive.py
src/freecad_mcp/operations/parametric.py
src/freecad_mcp/operations/p3_features.py
src/freecad_mcp/operations/p7_assembly.py
```

## Tests

- every helper returns valid `CallToolResult`;
- `json_response()` includes `structuredContent`;
- structured errors survive MCP conversion;
- `condition_false` uses `isError=false`;
- screenshots do not appear in structured JSON;
- old text clients still receive useful messages.

---

# 8. Phase C: Unified telemetry event stream

## Goal

Emit complete, redacted, correlated JSONL lifecycle events.

## C1. Replace ad hoc debug entries with a versioned event schema

Create:

```text
src/freecad_mcp/telemetry/
    __init__.py
    context.py
    events.py
    writer.py
    redaction.py
    schema.json
```

Reuse and move the existing redaction logic from `debug_log.py`.

Event envelope:

```json
{
  "schema_version": 1,
  "timestamp": "2026-07-27T12:34:56.789123Z",
  "monotonic_ns": 1234567890123,
  "sequence": 42,

  "source": "gui_dispatcher",
  "event": "execution_completed",
  "status": "succeeded",

  "session_id": "...",
  "task_id": "...",
  "call_id": "...",
  "request_id": "...",
  "execution_id": "...",
  "worker_job_id": null,
  "document_session_uuid": "...",
  "recovery_incident_id": null,

  "duration_ms": 842.4,
  "error_code": null,
  "payload": {}
}
```

## C2. Use context propagation

Use `contextvars` for MCP-side request context.

Context fields:

```text
session_id
task_id
call_id
attempt_number
parent_call_id
request_id
execution_id
worker_job_id
document_session_uuid
recovery_incident_id
```

Sources:

- `session_id`: generated once per MCP process;
- `task_id`: client metadata where available, otherwise empty;
- `call_id`: client tool-use ID where available, otherwise generated;
- `request_id`: existing authenticated RPC request UUID;
- `worker_job_id`: existing worker manager ID;
- `document_session_uuid`: existing lease credential;
- `attempt_number`: client metadata or internal retry counter;
- `recovery_incident_id`: generated when a timeout creates an uncertain execution.

## C3. Instrument lifecycle boundaries

Emit at least:

```text
session_started
session_stopped
authentication_started
authentication_completed
authentication_failed

tool_call_received
tool_call_completed

validation_started
validation_completed
policy_rejected

routing_completed

rpc_invocation_started
rpc_invocation_completed
rpc_invocation_failed

gui_execution_queued
gui_execution_started
gui_execution_completed
gui_execution_timeout
gui_execution_late_completed

worker_job_created
worker_job_started
worker_job_completed
worker_job_timeout
worker_job_cancel_requested
worker_job_cancelled

transaction_started
transaction_committed
transaction_aborted
transaction_rollback_failed

document_health_checked

cancellation_requested
cancellation_acknowledged
cancellation_completed

recovery_started
recovery_completed
recovery_failed
```

## C4. Logging ownership and file safety

Default behavior:

- one writer instance per MCP process;
- one file per MCP session, not one shared daily file;
- filename includes date, PID, and session ID;
- example: `mcp_debug_2026-07-27_1234_<session>.jsonl`;
- no cross-process interleaving;
- every line flushed;
- rotate by size with bounded backups;
- emit a final session event during normal shutdown;
- support `FREECAD_MCP_DEBUG_LOG_DIR`;
- support an optional explicit file path for controlled test use.

Provide a merge/analyse utility rather than making many processes append to one file.

Create:

```text
scripts/merge_mcp_telemetry.py
scripts/analyze_mcp_telemetry.py
```

## C5. Redaction requirements

Normal mode must:

- replace code with hash and byte count;
- replace images/base64 with hash and byte count;
- redact credential-shaped fields;
- redact exact known session and lease values if they appear elsewhere in payload text;
- cap event payload size;
- record truncation metadata.

## C6. Legacy compatibility

Keep a small parser for the old mixed transcript format:

```text
src/freecad_mcp/telemetry/legacy_parser.py
```

It should be migration-only. New KPI calculations should use the JSONL schema.

## Tests

- JSON Schema validation;
- context isolation across concurrent calls;
- monotonic sequence per session;
- UTC timestamps;
- no credentials or raw code in logs;
- payload truncation;
- abrupt write simulation;
- concurrent MCP processes use separate files;
- lifecycle completeness for success, failure, timeout, cancellation, and recovery.

---

# 9. Phase D: Document-health snapshots and deltas

## Goal

Determine whether a mutation improved, preserved, degraded, or invalidated its target document.

## D1. Add health data structures

Extend:

```text
addon/FreeCADMCP/rpc_server/mutation_guard.py
```

Add:

```python
DocumentHealthSnapshot
DocumentHealthDelta
DocumentHealthVerdict
```

Snapshot fields:

```text
document_name
document_session_uuid
document_dirty
object_count
object_names
recompute_error_objects
invalid_state_objects
null_shape_objects
invalid_shape_objects
body_tip_issues
```

Do not calculate expensive shape validity for every object by default.

## D2. Validation profiles

Define:

```text
none
minimal
default
full
```

Suggested behavior:

### Read-only

- confirm document identity;
- no recompute;
- no mutation validation.

### Simple property mutation

- capture before;
- execute;
- recompute when required by the method;
- compare object states and recompute errors.

### Geometry mutation

- capture before;
- execute;
- recompute;
- validate affected or created shapes;
- check PartDesign Body/Tip invariants.

### Delete/relink

- compare dependency graph or dependents;
- report orphaned references;
- validate repaired link properties.

### Save/finalize

- recompute;
- save;
- reopen or open a validation copy;
- recompute the reopened document;
- confirm expected document identity and health.

### Full benchmark/checkpoint

- validate all relevant shapes;
- save/reopen;
- report complete health summary.

## D3. Calculate deltas

Report:

```text
new_recompute_errors
resolved_recompute_errors
new_invalid_state_objects
new_null_shapes
new_invalid_shapes
created_objects
deleted_objects
modified_objects
unexpected_modified_objects
object_count_delta
```

Verdicts:

```text
healthy
warning
degraded
invalid
unknown
```

Rules:

- no new errors and expected changes only → `healthy`;
- pre-existing errors remain but no new errors → `warning`;
- new errors or invalid affected geometry → `degraded`;
- document cannot recompute, save, reopen, or restore → `invalid`;
- validation unavailable or explicitly skipped → `unknown`.

## D4. Modified-object detection

Use the strongest available signals:

- object lists before and after;
- object `State`;
- `Touched`;
- property/status changes already captured by mutation attribution;
- transaction scope;
- known affected objects from typed tool results;
- dependency changes;
- shape hash or bounded geometry summary only where necessary.

Do not serialize every shape to detect a minor property edit.

## D5. Add health to MCP results

Every successful typed mutation should return a `document_health` section.

An execution that returns normally but introduces errors must not have top-level `status=succeeded`. Use `degraded`.

## Tests

- healthy mutation;
- pre-existing error preserved;
- new recompute error;
- invalid shape created;
- Body Tip broken;
- unrelated document changed;
- save succeeds but reopen fails;
- health validation skipped;
- performance test showing default validation remains bounded.

---

# 10. Phase E: Transaction and rollback reporting

## Goal

Make mutation commit and rollback behavior explicit and trustworthy.

## E1. Refactor `GuiMutationTransaction`

Update:

```text
addon/FreeCADMCP/rpc_server/mutation_guard.py
```

Track:

```text
enabled
documents
started
committed
abort_attempted
abort_succeeded
abort_errors
```

Do not silently discard `abortTransaction()` exceptions.

Behavior:

- collect rollback errors;
- expose them after cleanup;
- emit telemetry;
- mark document health `invalid` or at least `degraded` when rollback fails;
- retain compatibility with context-manager use.

## E2. Ensure validation occurs before commit

For transactional typed mutations:

1. capture health before;
2. open transaction;
3. execute;
4. recompute if required;
5. run postflight validation;
6. abort on tool failure, exception, or degraded validation;
7. commit only when policy allows;
8. verify rollback outcome after abort;
9. calculate final health.

Do not commit and then discover the document is broken.

## E3. Classify rollback coverage

Return:

```text
complete
document_only
partial
unavailable
```

Examples:

- typed FreeCAD document mutation in a transaction → `document_only` or `complete` for declared document state;
- filesystem export plus document edit → `partial`;
- arbitrary live Python without outer transaction → `unavailable`.

## E4. `execute_code` handling

Keep existing safety behavior.

Add explicit result fields:

```json
{
  "mutation_scope": {
    "declared_documents": [],
    "transaction_coverage": "unavailable",
    "rollback_policy": "none"
  }
}
```

For generated internal operations that are already signed and routed through typed mutation context, report their actual transaction coverage.

## Tests

- successful commit;
- result returns failure and transaction aborts;
- validator raises and transaction aborts;
- abort itself fails;
- multi-document transaction;
- arbitrary `execute_code` accurately reports limited rollback coverage;
- no false claim that file exports were rolled back.

---

# 11. Phase F: Typed timeout, cancellation, and recovery lifecycle

## Goal

Remove string parsing from execution-state classification.

## F1. Add typed GUI dispatch exceptions

In the GUI dispatcher define structured exceptions or result types carrying:

```text
error_code
timeout_stage
request_id
execution_started
mutation_started
completion_uncertain
```

Codes:

```text
GUI_TIMEOUT_BEFORE_EXECUTION
GUI_TIMEOUT_DURING_EXECUTION
GUI_BUSY_AFTER_TIMEOUT
GUI_COMPLETION_UNCERTAIN
GUI_TASK_FAILED
GUI_DISPATCH_FAILED
```

Stop using message substring checks to determine stage.

## F2. Normalize worker outcomes

Codes:

```text
WORKER_TIMEOUT_BEFORE_EXECUTION
WORKER_TIMEOUT_DURING_EXECUTION
WORKER_CANCEL_REQUESTED
WORKER_CANCELLED
WORKER_TERMINATION_FAILED
WORKER_TASK_FAILED
```

Preserve existing `job_id`.

## F3. Add request-status tooling

Add:

```text
get_request_status(request_id)
cancel_request(request_id)
```

`get_request_status` should return:

```json
{
  "request_id": "...",
  "state": "running_after_timeout",
  "stage": "gui_execution",
  "execution_started": true,
  "mutation_started": true,
  "cancellation_requested": false,
  "completion_uncertain": true,
  "late_completion_available": false,
  "result_available": false
}
```

States:

```text
queued
running
running_after_timeout
completed
failed
cancel_requested
cancelled
completed_after_cancel_request
unknown
expired
```

Use the existing inflight and replay registries.

## F4. Improve recovery incidents

When a timeout leaves execution uncertain:

- create `recovery_incident_id`;
- attach it to request-status events;
- attach later `check_rpc_sync` calls where possible;
- emit `recovery_started`;
- mark `recovery_completed` when the request becomes terminal and GUI synchronization is restored;
- mark `recovery_failed` when state is lost or reconciliation fails.

## F5. Correct `check_rpc_sync`

As defined earlier:

- busy/unsynchronized is a valid observation;
- nonce mismatch or malformed data is a failure;
- include the active recovery incident where available.

## F6. MCP Tasks integration

Only after the request lifecycle above is stable:

- negotiate MCP task capability;
- wrap heavy operations in MCP Tasks when the client supports it;
- preserve synchronous fallback;
- map MCP task ID to existing request ID and worker job ID;
- expose progress, cancellation, final result, and expiry;
- do not create a second unrelated job system.

Candidates:

- full document validation;
- expensive worker geometry checks;
- exports;
- save/reopen validation;
- large view/video operations;
- FEM.

## Tests

- GUI timeout before start;
- GUI timeout during execution;
- late successful completion;
- late failed completion;
- cancellation before mutation;
- cancellation after mutation begins;
- worker queued cancellation;
- active worker termination;
- recovery incident completion;
- condition-false sync result;
- MCP task and synchronous fallback behavior.

---

# 12. Phase G: KPI benchmark suite

## Goal

Measure end-to-end MCP usefulness, not only individual JSON-RPC calls.

## G1. Add benchmark marker and Docker service

Update:

```text
pyproject.toml
docker-compose.yml
```

Add:

```text
benchmark
```

Command:

```bash
docker compose run --rm benchmark
```

## G2. Benchmark structure

Create:

```text
benchmarks/
    fixtures/
    tasks/
    validators/
    runner.py
    report.py

tests/benchmark/
```

Suggested fixtures:

```text
empty_document.FCStd
healthy_partdesign.FCStd
document_with_preexisting_errors.FCStd
broken_links.FCStd
assembly.FCStd
large_geometry.FCStd
```

## G3. Required benchmark tasks

1. create document;
2. acquire lease;
3. Body → Sketch → constrained rectangle → Pad;
4. attach sketch → Pocket;
5. spreadsheet aliases and expressions;
6. datum plane and SubShapeBinder;
7. assembly joint creation;
8. worker-based geometry analysis;
9. policy rejection for unsafe GUI geometry loop;
10. invalid link input;
11. broken-reference repair;
12. typed mutation rollback;
13. worker timeout and cancellation;
14. GUI timeout and late completion;
15. recovery and synchronization;
16. save, reopen, recompute, validate;
17. snapshot, corrupt, restore;
18. multi-document scope protection;
19. public `execute_code` where no typed tool exists;
20. equivalent workflow using typed tools.

## G4. Task metadata

Each benchmark task must have:

```text
task_id
task_type
attempt_number
expected_tool_or_tool_family
expected_outcome
expected_modified_documents
expected_modified_objects
forbidden_side_effects
time_budget
call_budget
```

## G5. Validators

Validate:

- required object names and types;
- feature history;
- spreadsheet values and expressions;
- recompute health;
- affected shape validity;
- expected geometry bounds or volume;
- no unrelated-document changes;
- rollback result;
- save/reopen success;
- final task outcome.

## G6. KPI calculations

Produce:

```text
task_success_rate
first_attempt_success_rate
tool_execution_success_rate
completed_response_rate
protected_rejection_rate
false_positive_rejection_rate
unexpected_runtime_failure_rate
argument_validity_rate
tool_selection_accuracy
recovery_rate
safe_failure_rate
rollback_success_rate
document_health_regression_rate
unrelated_document_mutation_rate
timeout_rate_by_stage
p50_latency_by_tool_class
p95_latency_by_tool_class
calls_per_successful_task
public_execute_code_share
generated_internal_execute_share
typed_tool_share
tokens_per_successful_task
```

Token metrics should be optional input from the MCP client. Do not estimate exact tokens from bytes.

## G7. Reports

Generate:

```text
benchmark-results.json
benchmark-report.md
```

Include baseline comparison and regression flags.

## Initial quality gates

```text
benchmark task success                  >= 90%
typed tool execution success           >= 98%
argument validity                      >= 97%
first-attempt task success             >= 85%
safe failure rate                      >= 99%
successful save/reopen validation      = 100%
unrelated-document mutation            = 0
committed new recompute errors          = 0 by default
unclassified failures                  = 0
timeout rate                           < 1% for non-task operations
```

Do not fake gates by excluding difficult scenarios. Classify expected protected rejections separately.

---

# 13. Phase H: Tool-adoption improvement

## Goal

Reduce public arbitrary-code usage because typed tools are safer and easier to validate.

## H1. Separate execution categories

Telemetry and KPI reports must distinguish:

```text
public_execute_code
generated_internal_execute
typed_direct_rpc
read_only_worker_analysis
deprecated_execute_code_async
```

The existing `generated_operation` marker should be used rather than counting internal generated templates as agent-selected arbitrary Python.

## H2. Analyse public `execute_code`

Add an analysis report that groups public code calls by:

- imported FreeCAD APIs;
- operations performed;
- document scope;
- read-only versus mutating;
- worker versus GUI;
- repeated AST pattern;
- equivalent typed tool available;
- success/failure/timeout;
- latency.

Do not store raw code in the report. Use AST classification and hashes.

## H3. Improve tool discovery

- shorten verbose descriptions where they obscure the decision;
- make tool names and first sentence clearly distinct;
- document the preferred PartDesign workflow;
- add concise prompt/resource guidance for selecting typed tools;
- expose tool families or workflow recipes without adding unnecessary new tools;
- ensure structured results are easier to consume than arbitrary stdout.

## H4. Add missing tools only from evidence

Only add a dedicated tool when logs or benchmarks show:

- repeated public code pattern;
- meaningful safety or correctness benefit;
- stable input and output schema;
- adequate tests.

## H5. Add development-mode guidance

When public `execute_code` matches a known typed workflow:

- allow execution for compatibility;
- add a structured warning such as `TYPED_TOOL_AVAILABLE`;
- name the preferred tool;
- do not emit the warning for generated internal code.

## H6. Deprecate unsafe async behavior carefully

`execute_code_async` is already deprecated in enforcement mode.

Complete the transition:

- point users to worker jobs or MCP Tasks;
- retain compatibility in `off`/`observe` until documented removal;
- add deprecation telemetry;
- do not remove it in this implementation unless tests and migration docs prove it is safe.

## Adoption targets

Initial:

```text
public execute_code < 50% of benchmark tool calls
```

Mature:

```text
public execute_code < 25% of benchmark tool calls
```

Do not count internal generated execution against these targets.

---

# 14. Phase I: Documentation and migration

Create or update:

```text
README.md
doc/runtime-identity.md
doc/structured-results.md
doc/telemetry.md
doc/document-health.md
doc/transactions-and-rollback.md
doc/request-lifecycle.md
doc/benchmarking.md
doc/execute-code-migration.md
CHANGELOG.md
```

Document:

- exact fork installation;
- server/addon identity verification;
- result envelope;
- status meanings;
- stable error codes;
- telemetry schema and redaction;
- health verdicts;
- rollback coverage;
- timeout stages;
- request status and cancellation;
- benchmark execution;
- typed-tool preference;
- compatibility behavior.

Include example results for:

- success;
- condition false;
- protected rejection;
- degraded document;
- timeout during GUI execution;
- cancellation after mutation;
- rollback failure.

---

# 15. Required test execution

Run all of these from a clean checkout:

```bash
docker compose build
docker compose run --rm unit
docker compose run --rm e2e
docker compose run --rm core
docker compose run --rm benchmark
```

Also run focused tests during development.

If the repository contains format, lint, type-check, or generation checks, run those as well.

Do not claim completion when Docker tests are skipped because host FreeCAD tests passed.

---

# 16. Suggested implementation order

Implement in this exact order because later work depends on earlier contracts:

1. build/runtime identity;
2. common result envelope;
3. response helper migration;
4. correct condition-false semantics;
5. telemetry context and schema;
6. MCP/RPC/GUI/worker lifecycle instrumentation;
7. document-health snapshots and deltas;
8. transaction outcome and rollback reporting;
9. typed timeout stages;
10. request status, cancellation, and recovery incidents;
11. benchmark framework;
12. task validators and KPI reports;
13. tool-adoption classification;
14. tool-discovery improvements;
15. evidence-based missing tools;
16. optional MCP Tasks integration;
17. documentation and migration;
18. full Docker validation;
19. final consolidated report.

Do not stop between these steps except to repair a failing dependency.

---

# 17. Suggested logical commits

The work may be implemented in one agent run but should remain reviewable.

Suggested commits:

```text
1. Add canonical build and runtime identity
2. Add unified structured MCP outcomes
3. Correct status and condition semantics
4. Add correlated JSONL telemetry
5. Instrument RPC, GUI, worker, and policy lifecycle
6. Add document-health snapshots and deltas
7. Expose transaction and rollback outcomes
8. Add typed timeout and recovery lifecycle
9. Add request status and cancellation tools
10. Add benchmark suite and KPI reporting
11. Classify execute_code usage and improve tool guidance
12. Add MCP Tasks integration and synchronous fallback
13. Update documentation and migration notes
14. Fix final integration regressions
```

Do not create a pull request unless explicitly requested, but leave the branch in a PR-ready state.

---

# 18. Definition of done

The complete task is done only when all statements below are true:

- [ ] MCP and addon build identities are derived rather than hard-coded.
- [ ] `get_runtime_info` reports the exact connected runtime.
- [ ] Fork documentation no longer silently directs development to a different checkout.
- [ ] Every JSON-capable tool response includes real `structuredContent`.
- [ ] Every tool uses a normalized status.
- [ ] Negative observations do not count as MCP errors.
- [ ] Backend error codes survive to the MCP result.
- [ ] Every call emits correlated JSONL lifecycle events.
- [ ] Logs include session, call, request, execution, worker, and document IDs where applicable.
- [ ] Logs contain high-resolution UTC and monotonic timing.
- [ ] Logs do not expose raw credentials, code, or image bodies.
- [ ] Multiple MCP processes cannot corrupt one shared log.
- [ ] Every typed mutation returns a document-health verdict.
- [ ] Pre-existing and newly introduced errors are separated.
- [ ] Modified, created, deleted, and unexpected objects are reported where measurable.
- [ ] Transaction commit, abort, and rollback failure are visible.
- [ ] Rollback coverage is stated honestly.
- [ ] GUI and worker timeout stages use typed codes.
- [ ] Request status is queryable after timeout.
- [ ] Cancellation before and after mutation is distinguishable.
- [ ] Late completion is correlated to the original request.
- [ ] Recovery incidents are explicit rather than inferred.
- [ ] Benchmark tasks validate end-to-end FreeCAD results.
- [ ] KPI reports are generated automatically.
- [ ] Public and generated-internal `execute_code` calls are separated.
- [ ] Typed-tool adoption is measured.
- [ ] Existing security, lease, worker, snapshot, and typed-tool tests still pass.
- [ ] Unit, e2e, core, and benchmark Docker suites pass.
- [ ] Documentation matches the implemented behavior.
- [ ] A final report lists changed files, tests, KPI baseline, unresolved limitations, and any follow-up work.

---

# 19. Final implementation report format

At the end, produce:

```markdown
# FreeCAD MCP Improvement Result

## Summary
## Baseline commit
## Changed architecture
## Changed files
## Structured result model
## Telemetry events
## Document-health behavior
## Transaction and rollback behavior
## Timeout, cancellation, and recovery behavior
## Benchmark results
## KPI comparison
## Execute-code adoption results
## Tests executed
## Known limitations
## Follow-up recommendations
```

Be explicit about anything not completed. Do not report a feature as complete when only interfaces or tests were added.
