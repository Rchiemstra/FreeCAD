# CI — robot simulation rig

Local CI scripts (no GitHub Actions on this branch):

```bash
bash scripts/ci/run_local.sh              # offline + lifecycle + Docker E2E
bash scripts/ci/run_local.sh offline      # fast
bash scripts/ci/run_local.sh lifecycle    # env/SDF smoke
bash scripts/ci/run_local.sh e2e          # slow (~5–15 min first build)
```

## Gates

| Job | Script | What it proves |
| --- | --- | --- |
| **Offline pytest** | `scripts/ci/run_offline_pytest.sh` | Bridge, runner, iteration, workbench logic |
| **Lifecycle smoke** | `scripts/ci/run_lifecycle_smoke.sh` | `empty_world` SDF + env module consistency |
| **Strict Docker E2E** | `scripts/ci/run_docker_e2e.sh` | RobotCAD export, spawn, `e2e_smoke` 4/4, version lock |

## Live tests (not in CI)

Host tests marked `@pytest.mark.gazebo`, `freecad`, or `needs_freecad` are **opt-in**:

```powershell
set RUN_GAZEBO_LIVE=1
pytest tests/test_bridge.py -m gazebo -v
```

CI sets `CI=true` and **forbids** `RUN_GAZEBO_LIVE=1` (`tests/conftest.py`).

## Readable failures

- Pytest uses `-v --tb=short` and explicit `-m "not gazebo …"`.
- Each script prints a banner (`═══ CI: … ═══`) before/after.
- E2E failures write logs under `sim_runs/e2e_*/console.log`.

## Requirements

- **offline / lifecycle:** Python 3.12 + pip
- **Docker E2E:** Docker engine with Linux containers, `robots/arm_2dof.FCStd` in repo
