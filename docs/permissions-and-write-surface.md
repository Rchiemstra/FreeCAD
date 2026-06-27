# Permissions and write surface

Phase 6 policy layer: **read operations are always allowed**; **write operations** must pass `bridge.permissions.assert_write_allowed()` before changing disk, FreeCAD documents, or Gazebo simulation state.

## Policy environment

| Variable | Values | Default |
| --- | --- | --- |
| `BRIDGE_WRITE_POLICY` | `allow`, `deny`, `generated_only` | `allow` (local); `generated_only` when `CI=true` and unset |
| `CI` | `true` / `1` | Unset locally |

- **`allow`** — all registered write operations permitted.
- **`deny`** — all writes blocked (read-only automation).
- **`generated_only`** — writes allowed only under `generated/`, `sim_runs/`, or system temp (spawn URDF prep).

Interactive MCP “prompt before write” is **not implemented**; use `deny` or `generated_only` in automation and `allow` on a trusted dev host.

## Read-only vs write-capable (bridge API)

### Read-only (no policy gate)

| Module | Functions | Touches |
| --- | --- | --- |
| `bridge.project` | `load_project` | Reads `project.yaml` |
| `bridge.validate` | `validate_urdf`, `validate_sdf` | Reads URDF/SDF |
| `bridge.freecad_bridge` | `check_robotcad`, `resolve_freecad_cmd` | RPC probe only |
| `bridge.gazebo_bridge` | `wait_for_ready`, `get_model_state`, `get_simulation_status`, `list_models` | MCP read |
| `bridge.handoff` | `resolve_robot_urdf` | Reads `robots/`, `generated/` |
| `bridge.mcp_status` | status helpers | MCP read |
| `runner.scenario` | `load_scenario`, `list_scenario_files` | Reads scenario YAML |
| `runner.result` | `load_result` | Reads `sim_runs/` |
| `runner.assertions` | `evaluate_*` | In-memory only |

### Write-capable (policy gated)

| Operation ID | Entry points | Side effects |
| --- | --- | --- |
| `cad.export_urdf` | `freecad_bridge.export_urdf`, `export_urdf_cmd` | `generated/<robot>/`, FreeCAD doc via RPC/Cmd |
| `cad.export_world_sdf` | `freecad_bridge.export_sdf_world` | `generated/<world>/` |
| `gazebo.spawn_model` | `gazebo_bridge.spawn_model`, `gazebo_gz_docker.spawn_prepared_xml` | Gazebo entity; may write temp URDF |
| `gazebo.sim_control` | `pause_simulation`, `resume_simulation`, `reset_simulation` | Gazebo world state |
| `runner.write_result` | `runner.result.write_result` | `sim_runs/<run_id>/` |
| `files.temp_urdf` | `gazebo_gz_docker.spawn_prepared_xml` | Temp spawn URDF when XML differs from disk |

`bridge.handoff.export_and_spawn` orchestrates write steps above; each step enforces policy independently.

## MCP / workbench paths

| Path | Read | Write |
| --- | --- | --- |
| FreeCAD MCP (`localhost:9875`) | `execute_code` queries, `check_robotcad` | Export snippets (via `export_urdf`) |
| Gazebo MCP (stdio) | `gazebo_get_*`, `gazebo_list_models` | `gazebo_spawn_sdf`, pause/unpause/reset |
| ROS MCP (rosbridge) | Topic/service introspection | **Not wired in v1** — no write tools |
| SimWorkbench UI | Status panel, list models | Play / export buttons → `handoff` / `export_urdf` |
| Docker E2E | N/A in bridge | Container scripts write `generated/` directly (bypass policy; CI artifact only) |

## Schema enforcement

JSON Schema (draft-07) in `config/schemas/`:

| File | Enforced in |
| --- | --- |
| `project.schema.yaml` | `bridge.project.load_project` |
| `scenario.schema.yaml` | `runner.scenario.load_scenario` |
| `result.schema.yaml` | `runner.result.write_result` |

Requires `jsonschema` (see `requirements-bridge.txt`). Validation runs on load/write; invalid files raise `ValueError` / `ScenarioLoadError`.

## Python API

```python
from bridge.permissions import assert_write_allowed, WriteOperation, effective_write_policy
from bridge.schema_validate import validate_instance

assert_write_allowed(WriteOperation.CAD_EXPORT_URDF, target=Path("generated/arm_2dof"))
validate_instance(data, "scenario")
```
