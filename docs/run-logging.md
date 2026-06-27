# Run logging and result metadata

Each scenario run creates a directory under `sim_runs/<run_id>/`:

| File | Contents |
| --- | --- |
| `run.log` | Timestamped text log (bridge, runner, export, spawn) |
| `run_events.yaml` | Structured events (`lifecycle`, `handoff`, `export`, `gazebo`, `log`) |
| `result.yaml` | Pass/fail, assertions, `input_hashes`, `metadata` |
| `telemetry.yaml` | Optional telemetry time series |

## Environment

| Variable | Role |
| --- | --- |
| `LOG_LEVEL` | Root log level (default `INFO`) |
| `LOG_FORMAT` | `text` or `json` for stdout |
| `SIM_RUNS_DIR` | Override `sim_runs/` root |
| `E2E_RUN_DIR` | Docker E2E parent dir; recorded in metadata |

## API

```python
from bridge.run_context import begin_run, finalize_run, record_lifecycle
from bridge.logging_config import configure_logging

configure_logging()
ctx = begin_run("my_scenario")
try:
    record_lifecycle("export_started")
    ...
finally:
    finalize_run()
```

`runner.run_test()` calls `begin_run` / `finalize_run` automatically.

## result.yaml metadata block

```yaml
metadata:
  write_policy: generated_only
  paths:
    export_urdf: /path/to/generated/.../arm_2dof.urdf
    spawn_urdf: /path/to/...
    world_sdf: /path/to/generated/empty_world/empty_world.sdf
  file_hashes:
    spawn_urdf: sha256...
  lifecycle_events: [...]
  e2e_run_dir: /workspace/sim_runs/e2e_20260529T...
  bridge_module: gz_cli
```

`input_hashes` merges scenario YAML hash, discovered URDF/SDF hashes, and `metadata.file_hashes`.
