# FreeCAD MCP GUI timeout and isolated-worker fallback failure

## Handoff summary

On 2026-07-21, a long-running `execute_code` collision check timed out while
running on FreeCAD's GUI thread. MCP correctly rejected additional GUI work
until the original call finished. The agent then attempted to move the checks
to the isolated `FreeCADCmd` worker, but no worker job was started because the
snapshot preflight incorrectly classified valid Sketcher axis references
(`H_Axis`) as nonexistent.

Two additional worker-status defects were observed:

1. The worker executable readiness probe has a hard five-second timeout. A
   normal version probe takes approximately three seconds in this environment,
   so load can produce a transient false `available: false` result.
2. `last_error` is not cleared after a later successful executable discovery,
   allowing `available: true` and a stale timeout error to be returned together.

The CAD dependency document is currently healthy. All 28 objects in
`M2_5x8PanHeadBolt` report `Up-to-date` and `valid: true`. The primary worker
blocker is an MCP validation bug, not broken bolt geometry.

## Environment

- OS: Windows
- FreeCAD: `26.3.0`, revision `47772 (Git)`
- GUI executable:
  `C:\Users\Rchie\Music\FreeCADModeling\FreeCAD\build\release\bin\FreeCAD.exe`
- Worker executable:
  `C:\Users\Rchie\Music\FreeCADModeling\FreeCAD\build\release\bin\FreeCADCmd.exe`
- FreeCAD is launched through Pixi:
  `pixi run build/release/bin/FreeCAD.exe`
- Primary document:
  `autocurtains_spool_rail_case_v5`
- Dependency involved in snapshot failure:
  `M2_5x8PanHeadBolt`
- MCP debug log:
  `C:\Users\Rchie\Music\FreeCADModeling\AutoCurtains\mcp_debug_2026-07-21.log`
- MCP source checkout:
  `D:\code\FreeCAD\tools\mcp\freecad-mcp`
- Installed FreeCAD MCP add-on:
  `C:\Users\Rchie\AppData\Roaming\FreeCAD\v26-3\Mod\FreeCADMCP`

## Impact

- Long read-only geometry checks can occupy the GUI thread for several minutes.
- After a GUI timeout, the intended isolated-worker recovery path cannot accept
  this otherwise valid document.
- An agent may incorrectly report the worker as unavailable.
- Status may continue displaying an old timeout after the worker has recovered.
- The fallback failure encourages agents to return to the GUI and risk another
  period of unresponsiveness.

## Incident timeline

Times are local timestamps from `mcp_debug_2026-07-21.log`.

| Time | Log ID | Event |
| --- | ---: | --- |
| `02:39:37.048` | 52 | Large collision-sweep `execute_code` call starts in `execution_mode="gui"`. |
| `02:40:37.061` | 52 | MCP client cancels after its 60-second request timeout. |
| `02:41:37.096` | 52 | Server returns: `Timed out after 120s waiting for FreeCAD GUI response`; execution continues in FreeCAD. |
| `02:41:37.719` | 53 | A second GUI call is rejected because the timed-out request is still running. |
| `02:42:32.321` | 54 | A GUI liveness probe is also rejected for the same reason. |
| `02:42:44.637` | 55 | First worker attempt is rejected because `execution_mode='worker'` requires `read_only=True`. This is caller misuse, not an MCP implementation defect. |
| `02:42:56.933` | 56 | Corrected worker request is rejected during snapshot preflight: `Nonexistent linked subelements: M2_5x8PanHeadBolt.BoltProfileSketch.H_Axis, M2_5x8PanHeadBolt.ExternalThreadProfile.H_Axis`. No worker job starts. |
| `02:44:07.581` | 58 | GUI probe succeeds, proving that the original GUI operation has finished. |
| `02:45:04.188` | 59 | Later GUI geometry check responds with an unrelated OCCT exception, `Convert_TorusToBSplineSurface`. |
| `02:46:37.058` | 60 | A revised GUI collision check completes successfully. |
| `02:47:00.317` | 61 | Another GUI diagnostic completes successfully. |

## Exact observed errors

### 1. GUI call timed out but continued executing

```text
Failed to execute code: Timed out after 120s waiting for FreeCAD GUI response
while executing; execution continues in FreeCAD and may keep the GUI
unresponsive. New GUI work is rejected until the request finishes
```

This behavior is consistent with the current safety design: arbitrary Python
already running on FreeCAD's GUI thread cannot be terminated safely. The
triggering workload repeatedly performed expensive OCCT shape transforms and
boolean intersections over complex assemblies.

### 2. First worker request omitted `read_only=True`

```text
Failed to execute code: execution_mode='worker' requires read_only=True
```

This is an agent/caller error. The next request included `read_only=True`.

### 3. Snapshot preflight rejected valid `H_Axis` references

```text
Failed to execute code: Nonexistent linked subelements:
M2_5x8PanHeadBolt.BoltProfileSketch.H_Axis,
M2_5x8PanHeadBolt.ExternalThreadProfile.H_Axis
```

The owner properties are:

- `BoltRevolution.ReferenceAxis = (BoltProfileSketch, ["H_Axis"])`
- `ExternalThread.ReferenceAxis = (ExternalThreadProfile, ["H_Axis"])`

Both owners and both sketches are `Up-to-date` and valid. Live read-only
inspection produced:

```text
BoltProfileSketch TypeId=Sketcher::SketchObject
Shape.getElement("H_Axis") -> ValueError: Invalid shape name H_Axis
getSubObject("H_Axis") -> valid Part::TopoShape Edge

ExternalThreadProfile TypeId=Sketcher::SketchObject
Shape.getElement("H_Axis") -> ValueError: Invalid shape name H_Axis
getSubObject("H_Axis") -> valid Part::TopoShape Edge
```

FreeCAD also resolves `V_Axis` and `RootPoint` through `getSubObject()` on these
sketches. These are semantic subobjects rather than ordinary shape-element names.

### 4. Transient worker readiness failure

Observed status at the time of a heavy GUI operation:

```json
{
  "available": false,
  "version": null,
  "executable": null,
  "busy": false,
  "active_job_id": null,
  "queue_depth": 0,
  "pending_job_ids": [],
  "queue_capacity": 3,
  "last_error": "...FreeCADCmd.exe: ... --version ... timed out after 5 seconds"
}
```

The readiness implementation invokes `FreeCADCmd.exe --version` with
`timeout=5`. Baseline probes in the required Pixi environment completed as
follows:

| Probe | Duration | Result |
| --- | ---: | --- |
| Normal | 3.28 s | Exit 0, correct version |
| `--safe-mode` | 2.98 s | Exit 0, correct version |
| Clean user home and config | 3.36 s | Exit 0, correct version |

The five-second limit therefore has little headroom. The earlier timeout
occurred while FreeCAD was under a heavy OCCT workload; load-related delay is
the most likely explanation. A later probe succeeded.

Important reproduction note: running this development-build executable
directly from an ordinary shell can exit with Windows status `0xC0000135`
because its Pixi-provided DLL directories are absent. Manual probes must run
through `pixi run` or inherit the environment of the Pixi-launched GUI, as the
MCP worker normally does.

### 5. Stale status error after recovery

Current worker status after successful discovery:

```json
{
  "available": true,
  "version": "26.3.0",
  "executable": "FreeCADCmd.exe",
  "busy": false,
  "active_job_id": null,
  "queue_depth": 0,
  "pending_job_ids": [],
  "queue_capacity": 3,
  "last_error": "...FreeCADCmd.exe: ... --version ... timed out after 5 seconds"
}
```

`available: true` is the current state. `last_error` is historical and stale.

## Confirmed root causes

### A. Incorrect semantic-subelement validation

Installed code:

- `rpc_server/snapshot_service.py`, `_collect_link_manifest()` around lines
  111-118, calls `validate_subelement_reference(target, subelement)` for every
  `PropertyLinkSub` subelement.
- `rpc_server/worker_protocol.py`, `validate_subelement_reference()` around
  lines 37-66, handles `FaceN`, `EdgeN`, and `VertexN`, but sends every other
  name to `target.Shape.getElement(name)`.

That resolver is wrong for Sketcher semantic subobjects such as `H_Axis`.
`target.getSubObject("H_Axis")` succeeds, while
`target.Shape.getElement("H_Axis")` necessarily fails.

### B. Readiness timeout is too close to normal startup time

`rpc_server/worker_manager.py`, `_probe_version()` around lines 184-192, uses a
fixed `timeout=5`. Normal startup consumes approximately 60-70% of this budget
on the tested system before contention is introduced.

### C. Successful discovery does not clear `_last_error`

`rpc_server/worker_manager.py`:

- `_last_error` is initialized around line 144.
- Failed discovery assigns `_last_error` around line 228.
- Successful discovery caches `_executable` and `_executable_version` around
  lines 219-220, but does not reset `_last_error`.
- `status()` always returns `_last_error` around line 547.

This directly explains the contradictory status response.

## Recommended implementation changes

### Priority 1: fix subelement resolution

Update `validate_subelement_reference()` so that semantic subobjects are
resolved through the document object, not only its `Shape`:

1. Retain the bounded `FaceN`/`EdgeN`/`VertexN` checks.
2. For other permitted names, call `target.getSubObject(name)`.
3. Accept a non-null returned subobject, including a returned `Part::TopoShape`.
4. Reject names only when both the recognized indexed-shape path and object
   subobject resolution fail.
5. Preserve protocol/path-safety validation; do not blindly accept arbitrary
   strings.

At minimum, add coverage for Sketcher `H_Axis`, `V_Axis`, and `RootPoint`.

### Priority 2: make executable discovery resilient

One or more of the following should be implemented:

- Increase the version-probe timeout to a value with operational headroom,
  such as 15 seconds.
- Make the timeout configurable.
- Probe and cache the executable before accepting expensive GUI work.
- Avoid changing `available` to false solely because a previously identified,
  compatible executable was slow to answer one health probe.

### Priority 3: clear or structure historical errors

On successful `discover_executable()` completion, set `_last_error = None`.
If historical diagnostics are desired, return them separately, for example as
`previous_error` plus a timestamp, rather than labeling them as the current
`last_error`.

### Agent-side mitigation until fixed

- Use `read_only=True` whenever requesting `execution_mode="worker"`.
- Split large boolean/collision sweeps into bounded batches.
- Do not immediately return to GUI execution after a timeout; first wait for a
  successful GUI synchronization/liveness check.
- Treat `available` as the current worker state. Do not report a non-null
  `last_error` as current when `available` is true.
- The bolt document should not be edited merely to remove `H_Axis`; those are
  valid modeling references. Fix the MCP validator instead.

## Suggested regression tests

1. Create a `Sketcher::SketchObject` and a PartDesign feature whose
   `ReferenceAxis` points to `H_Axis`.
2. Assert `validate_subelement_reference(sketch, "H_Axis")` succeeds.
3. Repeat for `V_Axis` and `RootPoint`.
4. Assert a genuinely unknown semantic name is rejected.
5. Assert out-of-range `FaceN`, `EdgeN`, and `VertexN` names remain rejected.
6. Build a snapshot containing the valid `ReferenceAxis` and assert snapshot
   creation succeeds.
7. Run a minimal `read_only=True`, `execution_mode="worker"` job against the
   snapshot and assert a worker job starts and completes.
8. Simulate one version-probe timeout followed by success; assert status changes
   from unavailable to available and `last_error` becomes null.
9. Simulate a version probe lasting more than five seconds under load and verify
   the revised readiness policy does not produce an avoidable false negative.

## Acceptance criteria

- The existing `M2_5x8PanHeadBolt` document snapshots without modifying its
  `ReferenceAxis` properties.
- The collision-check request can execute in isolated worker mode with
  `read_only=True`.
- `get_worker_status()` returns `available: true` and `last_error: null` after a
  successful recovery probe.
- Worker discovery remains successful under representative CPU load.
- Invalid indexed shape references are still rejected.
- GUI timeout safety behavior remains intact: new GUI work is rejected while
  timed-out GUI code is still running.

## Scope clarification

The later `Convert_TorusToBSplineSurface` OCCT exception is a separate geometry
operation issue caused by the collision-check transformation method. It is not
the cause of the worker fallback failure and should be tracked separately if it
needs correction.
